#include "movie_renderer.h"
#include "app_resources.h"
#include "metal_preview.h"
#include "sticker_scene.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>

#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>

#include <huxerui/macos/external_texture.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using namespace huxerui::media;
using huxerui::macos::PixelBufferTexture;
namespace metal = huxerui::avkit::macos;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

struct CFDeleter {
  void operator()(CFTypeRef value) const { CFRelease(value); }
};
template<class Ref> using Owned = std::unique_ptr<std::remove_pointer_t<Ref>, CFDeleter>;

struct ImportedPicture {
  Owned<CVPixelBufferRef> input, fitted;
  Owned<CVMetalTextureRef> view;
  RenderSurface surface;
};

const std::array kWipeFrames{
    &app::raw::masks_wipe_0_png, &app::raw::masks_wipe_1_png, &app::raw::masks_wipe_2_png, &app::raw::masks_wipe_3_png,
    &app::raw::masks_wipe_4_png, &app::raw::masks_wipe_5_png, &app::raw::masks_wipe_6_png, &app::raw::masks_wipe_7_png};
static_assert(kWipeFrames.size() == kPreviewWipeFrameCount);
constexpr std::array kTrackingParts{analysis::TrackedPart::Face, analysis::TrackedPart::LeftHand,
                                    analysis::TrackedPart::RightHand};

} // namespace

struct MovieRenderer::Impl {
  explicit Impl(PixelSize size)
      : size_(size),
        target_{size, render::ImageFormat::Bgra8Unorm},
        backend_(metal::CreateBgfxBackend(device_, target_)),
        sticker_(CreatePreviewSticker(device_)),
        context_([CIContext contextWithMTLDevice:device_ options:@{kCIContextCacheIntermediates: @NO}]),
        srgb_(CGColorSpaceCreateWithName(kCGColorSpaceSRGB)),
        bt709_(CGColorSpaceCreateWithName(kCGColorSpaceITUR_709)) {
    CVMetalTextureCacheRef cache = nullptr;
    Check(CVMetalTextureCacheCreate(nullptr, nullptr, device_, nullptr, &cache) == kCVReturnSuccess,
          "Cannot create the Metal video texture cache");
    cache_.reset(cache);
    Check(context_ != nil && srgb_ && bt709_, "Cannot create the Metal video conversion context");
    InstallStickerScene(scene_);
  }

  RenderSurface Draw(const VideoFrame& input, bool cue) {
    const auto image = Import(input);
    return DrawSticker(image.surface, input.pts, cue);
  }

  RenderSurface Draw(CVPixelBufferRef image, huxerui::Rect crop, VideoRotation rotation, MediaTimestamp time, bool cue) {
    const auto imported = Import(image, crop, rotation);
    return DrawSticker(imported.surface, time, cue);
  }

  RenderSurface Draw(const TimelineFrame& frame) {
    @autoreleasepool {
      std::vector<ImportedPicture> pictures;
      std::vector<RenderSurface> inputs;
      inputs.reserve(frame.inputs.size());
      for (const auto& input : frame.inputs) {
        if (const auto* picture = std::get_if<VideoFrame>(&input)) {
          pictures.push_back(Import(*picture));
          inputs.push_back(pictures.back().surface);
        } else {
          inputs.push_back(Sticker(std::get<ResourceRef>(input)));
        }
      }
      std::vector<DataSurface> masks;
      masks.reserve(frame.masks.size());
      for (const auto& sample : frame.masks) masks.push_back(Mask(sample));
      return Render({frame.plan, std::move(inputs), std::move(masks)});
    }
  }

private:
  const RenderSurface& Sticker(const ResourceRef& resource) {
    if (resource == PreviewStickerResource()) return sticker_;
    for (std::size_t index = 0; index < kTrackingParts.size(); ++index) {
      const auto part = kTrackingParts[index];
      if (resource != TrackingStickerResource(part)) continue;
      auto& texture = tracking_stickers_[index];
      if (!texture) texture = CreateTrackingSticker(device_, part);
      return *texture;
    }
    throw std::runtime_error("The preview sticker resource is unavailable");
  }

  const DataSurface& Mask(const TransitionMaskSample& sample) {
    Check(sample.resource == PreviewWipeResource(), "The preview mask resource is unavailable");
    const auto index = sample.FrameIndex(kPreviewWipeFrameCount);
    auto& cached = wipe_frames_[index];
    if (cached) return *cached;
    if (!resources_) {
      auto* directory = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"HuxerUI" isDirectory:YES];
      Check(directory != nil, "Cannot locate the preview resource package");
      resources_.emplace(Resources::OpenPackage(File(directory.fileSystemRepresentation)));
    }
    const auto bytes = resources_->GetRawResource(*kWipeFrames[index]).ReadBytes();
    const Owned<CFDataRef> encoded(CFDataCreateWithBytesNoCopy(nullptr, reinterpret_cast<const UInt8*>(bytes.data()),
                                                              bytes.size(), kCFAllocatorNull));
    const Owned<CGImageSourceRef> source(encoded ? CGImageSourceCreateWithData(encoded.get(), nullptr) : nullptr);
    const Owned<CGImageRef> image(source ? CGImageSourceCreateImageAtIndex(source.get(), 0, nullptr) : nullptr);
    Check(image && CGImageGetBitsPerComponent(image.get()) == 8 && CGImageGetBitsPerPixel(image.get()) == 8 &&
          CGImageGetAlphaInfo(image.get()) == kCGImageAlphaNone && CGImageGetColorSpace(image.get()) &&
          CGColorSpaceGetModel(CGImageGetColorSpace(image.get())) == kCGColorSpaceModelMonochrome,
          "Expected an 8-bit grayscale PNG mask");
    const auto width = CGImageGetWidth(image.get()), height = CGImageGetHeight(image.get());
    const auto stride = CGImageGetBytesPerRow(image.get());
    const Owned<CFDataRef> coverage(CGDataProviderCopyData(CGImageGetDataProvider(image.get())));
    Check(coverage && width && height && stride >= width && CFDataGetLength(coverage.get()) / stride >= height,
          "Mask has no complete grayscale payload");
    const PixelSize mask_size{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    if (wipe_size_) Check(*wipe_size_ == mask_size, "Mask sequence frame sizes differ");
    auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                   width:width height:height mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = device_.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:desc];
    Check(texture != nil, "Cannot allocate a Metal mask texture");
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:CFDataGetBytePtr(coverage.get())
               bytesPerRow:stride];
    cached = metal::RetainMetalDataTexture(texture);
    wipe_size_ = mask_size;
    return *cached;
  }

  ImportedPicture Import(const VideoFrame& input) {
    const auto fitted = Require(MakeExternalTexture(input, {size_.width, size_.height}));
    const auto mailbox = std::dynamic_pointer_cast<PixelBufferTexture>(fitted);
    Check(bool(mailbox), "Expected a CoreVideo-backed video texture");
    Owned<CVPixelBufferRef> image(mailbox->AcquireFrame());
    Check(bool(image), "Decoded video texture has no image");
    auto imported = Import(image.get(), {0, 0, 1, 1}, VideoRotation::None);
    imported.input = std::move(image);
    return imported;
  }

  ImportedPicture Import(CVPixelBufferRef image, huxerui::Rect crop, VideoRotation rotation) {
    const auto width = CVPixelBufferGetWidth(image), height = CVPixelBufferGetHeight(image);
    Owned<CVPixelBufferRef> fitted;
    if (CVPixelBufferGetPixelFormatType(image) != kCVPixelFormatType_32BGRA ||
        width != size_.width || height != size_.height || crop != huxerui::Rect{0, 0, 1, 1} || rotation != VideoRotation::None) {
      fitted = Allocate(bgra_, kCVPixelFormatType_32BGRA);
      CIImage* source = [CIImage imageWithCVPixelBuffer:image];
      const auto visible = CGRectMake(crop.x * width, (1 - crop.y - crop.height) * height,
                                      crop.width * width, crop.height * height);
      source = [source imageByCroppingToRect:visible];
      switch (rotation) {
        case VideoRotation::None: break;
        case VideoRotation::Clockwise90: source = [source imageByApplyingOrientation:6]; break;
        case VideoRotation::Clockwise180: source = [source imageByApplyingOrientation:3]; break;
        case VideoRotation::Clockwise270: source = [source imageByApplyingOrientation:8]; break;
      }
      const auto extent = source.extent;
      Check(!CGRectIsEmpty(extent) && !CGRectIsInfinite(extent), "Invalid video crop");
      const auto scale = std::min(size_.width / extent.size.width, size_.height / extent.size.height);
      const auto x = (size_.width - scale * extent.size.width) / 2 - scale * extent.origin.x;
      const auto y = (size_.height - scale * extent.size.height) / 2 - scale * extent.origin.y;
      source = [source imageByApplyingTransform:CGAffineTransformMake(scale, 0, 0, scale, x, y)];
      const auto bounds = CGRectMake(0, 0, size_.width, size_.height);
      auto black = [[CIImage imageWithColor:[CIColor colorWithRed:0 green:0 blue:0 alpha:1]] imageByCroppingToRect:bounds];
      [context_ render:[source imageByCompositingOverImage:black] toCVPixelBuffer:fitted.get() bounds:bounds
           colorSpace:srgb_.get()];
      image = fitted.get();
    }
    CVMetalTextureRef view = nullptr;
    Check(CVMetalTextureCacheCreateTextureFromImage(nullptr, cache_.get(), image, nullptr, MTLPixelFormatBGRA8Unorm,
              size_.width, size_.height, 0, &view) == kCVReturnSuccess,
          "Cannot import the video image into Metal");
    Owned<CVMetalTextureRef> owned_view(view);
    const auto base = metal::RetainMetalTexture(CVMetalTextureGetTexture(view));
    return {{}, std::move(fitted), std::move(owned_view), base};
  }

  RenderSurface DrawSticker(const RenderSurface& base, MediaTimestamp time, bool cue) {
    if (cue && !cue_) cue_ = CreatePreviewSticker(device_, true);
    return Render({scene_.Evaluate(time, target_), {base, cue ? *cue_ : sticker_}});
  }

  RenderSurface Render(const RenderRequest& request) {
    auto ticket = Require(backend_->TrySubmit(request));
    Check(ticket.has_value(), "The composition target is busy");
    backend_->Pump();
    auto output = Require(backend_->TryTake(*ticket));
    Check(output.has_value(), "The composition target did not finish");
    return std::move(*output);
  }

public:
  VideoFrame EncoderFrame(const RenderSurface& output, const VideoEncoderConfig& config, MediaTimestamp pts,
                          std::optional<MediaTimestamp> duration) {
    @autoreleasepool {
      const auto image = [CIImage imageWithMTLTexture:metal::GetMetalTexture(output)
                                             options:@{kCIImageColorSpace: (__bridge id)srgb_.get()}];
      Check(image != nil, "Cannot use the composition as an encoder input");
      auto pixels = Allocate(nv12_, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
      CVBufferSetAttachment(pixels.get(), kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
                            kCVAttachmentMode_ShouldPropagate);
      CVBufferSetAttachment(pixels.get(), kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
                            kCVAttachmentMode_ShouldPropagate);
      CVBufferSetAttachment(pixels.get(), kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                            kCVAttachmentMode_ShouldPropagate);
      auto* destination = [[CIRenderDestination alloc] initWithPixelBuffer:pixels.get()];
      // The CIImage made from a Metal texture maps its first row to Core Image y=0.
      destination.flipped = NO;
      destination.colorSpace = bt709_.get();
      NSError* error = nil;
      auto* task = [context_ startTaskToRender:image toDestination:destination error:&error];
      if (!task || ![task waitUntilCompletedAndReturnError:&error])
        throw std::runtime_error(error.localizedDescription.UTF8String ?: "Metal NV12 conversion did not finish");
      if (config.input_storage == VideoStorage::Platform && config.input.pixel_format == VideoPixelFormat::NV12) {
        auto mailbox = std::make_shared<PixelBufferTexture>(huxerui::Size{float(size_.width), float(size_.height)});
        mailbox->Publish(pixels.get());
        mailbox->Finish();
        return Require(MakeVideoFrame(mailbox, config.input, pts, duration));
      }
      Check(config.input_storage == VideoStorage::Cpu && config.input.pixel_format == VideoPixelFormat::I420,
            "The renderer cannot supply the encoder's negotiated storage and layout");
      return ReadI420(pixels.get(), config.input, pts, duration);
    }
  }

private:
  Owned<CVPixelBufferRef> Allocate(Owned<CVPixelBufferPoolRef>& pool, OSType type) const {
    if (!pool) {
      NSDictionary* attributes = @{
        (__bridge NSString*)kCVPixelBufferWidthKey: @(size_.width),
        (__bridge NSString*)kCVPixelBufferHeightKey: @(size_.height),
        (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(type),
        (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey: @YES
      };
      CVPixelBufferPoolRef value = nullptr;
      Check(CVPixelBufferPoolCreate(nullptr, nullptr, (__bridge CFDictionaryRef)attributes, &value) == kCVReturnSuccess,
            "Cannot create the video conversion buffer pool");
      pool.reset(value);
    }
    CVPixelBufferRef pixel = nullptr;
    Check(CVPixelBufferPoolCreatePixelBuffer(nullptr, pool.get(), &pixel) == kCVReturnSuccess,
          "Cannot allocate a video conversion buffer");
    return Owned<CVPixelBufferRef>(pixel);
  }

  VideoFrame ReadI420(CVPixelBufferRef image, const VideoFormat& format, MediaTimestamp pts,
                      std::optional<MediaTimestamp> duration) const {
    const auto width = size_.width, height = size_.height;
    auto luma = std::make_shared<Bytes>(std::size_t(width) * height);
    auto u = std::make_shared<Bytes>(luma->size() / 4);
    auto v = std::make_shared<Bytes>(u->size());
    Check(CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess,
          "Cannot read the completed NV12 buffer for software encoding");
    const auto* y = static_cast<const std::byte*>(CVPixelBufferGetBaseAddressOfPlane(image, 0));
    const auto* uv = static_cast<const std::byte*>(CVPixelBufferGetBaseAddressOfPlane(image, 1));
    const auto y_stride = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
    const auto uv_stride = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
    for (std::uint32_t row = 0; row < height; ++row)
      std::memcpy(luma->data() + std::size_t(row) * width, y + row * y_stride, width);
    for (std::uint32_t row = 0; row < height / 2; ++row) {
      for (std::uint32_t x = 0; x < width / 2; ++x) {
        const auto pixel = std::size_t(row) * (width / 2) + x;
        const auto source = row * uv_stride + 2 * x;
        (*u)[pixel] = uv[source];
        (*v)[pixel] = uv[source + 1];
      }
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    return {format, pts, duration,
            {{BufferReference(*luma, luma), width, 1},
             {BufferReference(*u, u), width / 2, 1},
             {BufferReference(*v, v), width / 2, 1}}};
  }

  PixelSize size_;
  id<MTLDevice> device_ = MTLCreateSystemDefaultDevice();
  ImageDesc target_;
  SceneEvaluator scene_;
  std::unique_ptr<RenderBackend> backend_;
  RenderSurface sticker_;
  std::array<std::optional<RenderSurface>, kTrackingParts.size()> tracking_stickers_;
  std::optional<RenderSurface> cue_;
  CIContext* context_;
  Owned<CGColorSpaceRef> srgb_, bt709_;
  Owned<CVMetalTextureCacheRef> cache_;
  Owned<CVPixelBufferPoolRef> bgra_, nv12_;
  std::optional<Resources> resources_;
  std::optional<PixelSize> wipe_size_;
  std::array<std::optional<DataSurface>, kPreviewWipeFrameCount> wipe_frames_;
};

MovieRenderer::MovieRenderer(PixelSize size) : impl_(std::make_unique<Impl>(size)) {}
MovieRenderer::~MovieRenderer() = default;

RenderSurface MovieRenderer::Draw(const VideoFrame& input, bool cue) { return impl_->Draw(input, cue); }

RenderSurface MovieRenderer::Draw(const TimelineFrame& frame) { return impl_->Draw(frame); }

RenderSurface MovieRenderer::Draw(CVPixelBufferRef input, huxerui::Rect crop, VideoRotation rotation,
                                  MediaTimestamp time, bool cue) {
  return impl_->Draw(input, crop, rotation, time, cue);
}

VideoFrame MovieRenderer::EncoderFrame(const RenderSurface& output, const VideoEncoderConfig& config,
                                       MediaTimestamp pts, std::optional<MediaTimestamp> duration) {
  return impl_->EncoderFrame(output, config, pts, duration);
}

VideoFrame MovieRenderer::EncodeFrame(const VideoFrame& input, const VideoEncoderConfig& config, bool cue) {
  @autoreleasepool {
    return EncoderFrame(Draw(input, cue), config, input.pts, input.duration);
  }
}

VideoEncoderConfig PreviewVideoConfig(PixelSize size) {
  VideoEncoderConfig config;
  config.input = {.width = size.width, .height = size.height, .visible = {0, 0, size.width, size.height},
      .primaries = ColorPrimaries::BT709, .transfer = ColorTransfer::BT709,
      .matrix = ColorMatrix::BT709, .range = ColorRange::Limited};
  return config;
}

const ResourceRef& PreviewStickerResource() {
  static const ResourceRef resource{"avkit.preview", "1", "sticker"};
  return resource;
}

const ResourceRef& TrackingStickerResource(analysis::TrackedPart part) {
  static const std::array<ResourceRef, kTrackingParts.size()> resources{{
      {"avkit.preview", "1", "glasses"}, {"avkit.preview", "1", "left_hand"}, {"avkit.preview", "1", "right_hand"}}};
  const auto item = std::ranges::find(kTrackingParts, part);
  Check(item != kTrackingParts.end(), "Unknown preview tracking sticker");
  return resources[item - kTrackingParts.begin()];
}

const ResourceRef& PreviewWipeResource() {
  static const ResourceRef resource{"avkit.preview", "1", "wipe"};
  return resource;
}

} // namespace avkit_preview
