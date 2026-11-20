#include "movie_renderer.h"
#include "bgfx_metal_texture.h"
#include "app_resources.h"
#include "metal_preview.h"
#include "render/bgfx/video_texture_internal.h"
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

#include <huxerui/avkit/video_texture.h>
#include <huxerui/macos/external_texture.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using namespace huxerui::media;
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
  Owned<CVPixelBufferRef> fitted;
  Owned<CVMetalTextureRef> view;
  ColorSurface surface;
};

const std::array kWipeFrames{
    &app::raw::masks_wipe_0_png, &app::raw::masks_wipe_1_png, &app::raw::masks_wipe_2_png, &app::raw::masks_wipe_3_png,
    &app::raw::masks_wipe_4_png, &app::raw::masks_wipe_5_png, &app::raw::masks_wipe_6_png, &app::raw::masks_wipe_7_png};
static_assert(kWipeFrames.size() == kPreviewWipeFrameCount);
constexpr MediaTimestamp kClipMaskFrameInterval{5, 48};
const std::array kClipMaskFrames{&app::raw::masks_static_window_png, &app::raw::masks_static_offset_png,
                                 &app::raw::masks_sequence_corner_png};
constexpr std::array kTrackingParts{analysis::TrackedPart::Face, analysis::TrackedPart::LeftHand,
                                    analysis::TrackedPart::RightHand};

} // namespace

struct MovieRenderer::Impl {
  explicit Impl(PixelSize size)
      : size_(size),
        target_{size, render::PixelFormat::Bgra8Unorm},
        backend_(metal::CreateBgfxBackend(device_, target_)),
        sticker_(CreatePreviewSticker(device_)),
        context_([CIContext contextWithMTLDevice:device_ options:@{kCIContextCacheIntermediates: @NO}]),
        srgb_(CGColorSpaceCreateWithName(kCGColorSpaceSRGB)) {
    CVMetalTextureCacheRef cache = nullptr;
    Check(CVMetalTextureCacheCreate(nullptr, nullptr, device_, nullptr, &cache) == kCVReturnSuccess,
          "Cannot create the Metal video texture cache");
    cache_.reset(cache);
    Check(context_ != nil && srgb_, "Cannot create the Metal video conversion context");
    InstallStickerScene(scene_);
  }

  ColorSurface Draw(const VideoFrame& input, bool cue) {
    return DrawSticker(ImportFrame(input).surface, input.pts, cue);
  }

  ImportedVideoFrame ImportFrame(const VideoFrame& input) {
    auto result = render::detail::ImportDecodedBgfxFrame(*backend_, input, {size_.width, size_.height},
                                                         TextureTransferPolicy::AllowCpuTransfer);
    if (!result.Succeeded() && input.Storage() == VideoStorage::Platform &&
        result.Error().code == RenderErrorCode::Unsupported) {
      throw PlatformVideoImportError(result.Error().message);
    }
    return Require(std::move(result));
  }

  ColorSurface Draw(const ColorSurface& input, MediaTimestamp time, bool cue) {
    return DrawSticker(input, time, cue);
  }

  ColorSurface Draw(CVPixelBufferRef image, huxerui::Rect crop, VideoRotation rotation, MediaTimestamp time, bool cue) {
    const auto imported = Import(image, crop, rotation);
    return DrawSticker(imported.surface, time, cue);
  }

  ColorSurface Draw(const TimelineFrame& frame) {
    @autoreleasepool {
      std::vector<ColorSurface> inputs;
      inputs.reserve(frame.inputs.size());
      for (const auto& input : frame.inputs) {
        if (const auto* picture = std::get_if<VideoFrame>(&input)) {
          inputs.push_back(ImportFrame(*picture).surface);
        } else if (const auto* surface = std::get_if<ColorSurface>(&input)) {
          inputs.push_back(*surface);
        } else {
          inputs.push_back(Sticker(std::get<ResourceRef>(input)));
        }
      }
      auto masks = frame.analysis_masks;
      masks.reserve(masks.size() + frame.static_masks.size() + frame.animated_masks.size() + frame.masks.size());
      for (const auto& resource : frame.static_masks) masks.push_back(StaticMaskTexture(resource));
      for (const auto& sample : frame.animated_masks) masks.push_back(AnimatedMaskTexture(sample));
      for (const auto& sample : frame.masks) masks.push_back(TransitionMaskTexture(sample));
      return Render({frame.plan, std::move(inputs), std::move(masks)});
    }
  }

private:
  const ColorSurface& Sticker(const ResourceRef& resource) {
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

  const ImageSurface& TransitionMaskTexture(const TransitionMaskSample& sample) {
    Check(sample.resource == PreviewWipeResource(), "The preview mask resource is unavailable");
    const auto index = sample.FrameIndex(kPreviewWipeFrameCount);
    auto& cached = wipe_frames_[index];
    if (!cached) {
      auto surface = LoadMask(*kWipeFrames[index]);
      const auto size = surface.image.size;
      if (wipe_size_) Check(*wipe_size_ == size, "Mask sequence frame sizes differ");
      cached = std::move(surface);
      wipe_size_ = size;
    }
    return *cached;
  }

  const ImageSurface& StaticMaskTexture(const ResourceRef& resource) {
    const auto& resources = PreviewStaticMaskResources();
    const auto found = std::ranges::find(resources, resource);
    if (found == resources.end()) throw std::runtime_error("The static mask resource is unavailable: " + resource.key);
    const auto index = static_cast<std::size_t>(found - resources.begin());
    const std::array assets{&app::raw::masks_static_window_png, &app::raw::masks_static_offset_png};
    auto& cached = static_masks_[index];
    if (!cached) cached = LoadMask(*assets[index]);
    return *cached;
  }

  const ImageSurface& AnimatedMaskTexture(const AnimatedMaskSample& sample) {
    Check(sample.resource == PreviewAnimatedMaskResource(), "The clip mask sequence is unavailable");
    const auto index = sample.FrameIndex(kClipMaskFrameInterval, kClipMaskFrames.size());
    if (!animated_masks_) {
      std::array<ImageSurface, kClipMaskFrames.size()> frames;
      for (std::size_t i = 0; i < frames.size(); ++i) {
        frames[i] = LoadMask(*kClipMaskFrames[i]);
        Check(frames[i].image.size == frames.front().image.size, "Clip mask sequence frame sizes differ");
      }
      animated_masks_ = std::move(frames);
    }
    return (*animated_masks_)[index];
  }

  ImageSurface LoadMask(const RawResource& asset) {
    if (!resources_) {
      auto* directory = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"HuxerUI" isDirectory:YES];
      Check(directory != nil, "Cannot locate the preview resource package");
      resources_.emplace(Resources::OpenPackage(File(directory.fileSystemRepresentation)));
    }
    const auto bytes = resources_->GetRawResource(asset).ReadBytes();
    const Owned<CFDataRef> encoded(CFDataCreateWithBytesNoCopy(nullptr, reinterpret_cast<const UInt8*>(bytes.data()),
                                                              bytes.size(), kCFAllocatorNull));
    const Owned<CGImageSourceRef> source(encoded ? CGImageSourceCreateWithData(encoded.get(), nullptr) : nullptr);
    const auto type = source ? CGImageSourceGetType(source.get()) : nullptr;
    Check(type && CFEqual(type, CFSTR("public.png")), "Expected a PNG mask resource");
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
    auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                   width:width height:height mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = device_.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:desc];
    Check(texture != nil, "Cannot allocate a Metal mask texture");
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:CFDataGetBytePtr(coverage.get())
               bytesPerRow:stride];
    return metal::RetainMetalDataTexture(texture);
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
    return {std::move(fitted), std::move(owned_view), base};
  }

  ColorSurface DrawSticker(const ColorSurface& base, MediaTimestamp time, bool cue) {
    if (cue && !cue_) cue_ = CreatePreviewSticker(device_, true);
    return Render({scene_.Evaluate(time, target_), {base, cue ? *cue_ : sticker_}});
  }

  ColorSurface Render(const RenderRequest& request) {
    auto ticket = Require(backend_->TrySubmit(request));
    Check(ticket.has_value(), "The composition target is busy");
    backend_->Pump();
    auto output = Require(backend_->TryTake(*ticket));
    Check(output.has_value(), "The composition target did not finish");
    return std::move(*output);
  }

public:
  VideoFrame EncoderFrame(const ColorSurface& output, const VideoEncoderConfig& config, MediaTimestamp pts,
                          std::optional<MediaTimestamp> duration) {
    auto picture = MakeBgfxEncoderFrame(*backend_, output, config, pts, duration);
    if (!picture.Succeeded() && config.input_storage == VideoStorage::Platform &&
        picture.Error().code == RenderErrorCode::Unsupported) {
      throw PlatformVideoTransferError(picture.Error().message);
    }
    return Require(std::move(picture));
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

  PixelSize size_;
  id<MTLDevice> device_ = MTLCreateSystemDefaultDevice();
  ImageDesc target_;
  SceneEvaluator scene_;
  std::unique_ptr<RenderBackend> backend_;
  ColorSurface sticker_;
  std::array<std::optional<ColorSurface>, kTrackingParts.size()> tracking_stickers_;
  std::optional<ColorSurface> cue_;
  CIContext* context_;
  Owned<CGColorSpaceRef> srgb_;
  Owned<CVMetalTextureCacheRef> cache_;
  Owned<CVPixelBufferPoolRef> bgra_;
  std::optional<Resources> resources_;
  std::optional<PixelSize> wipe_size_;
  std::array<std::optional<ImageSurface>, kPreviewWipeFrameCount> wipe_frames_;
  std::array<std::optional<ImageSurface>, 2> static_masks_;
  std::optional<std::array<ImageSurface, kClipMaskFrames.size()>> animated_masks_;
};

MovieRenderer::MovieRenderer(PixelSize size) : impl_(std::make_unique<Impl>(size)) {}
MovieRenderer::~MovieRenderer() = default;

ImportedVideoFrame MovieRenderer::ImportFrame(const VideoFrame& input) { return impl_->ImportFrame(input); }

ColorSurface MovieRenderer::Draw(const ColorSurface& input, MediaTimestamp time, bool cue) {
  return impl_->Draw(input, time, cue);
}

ColorSurface MovieRenderer::Draw(const VideoFrame& input, bool cue) { return impl_->Draw(input, cue); }

ColorSurface MovieRenderer::Draw(const TimelineFrame& frame) { return impl_->Draw(frame); }

ColorSurface MovieRenderer::Draw(CVPixelBufferRef input, huxerui::Rect crop, VideoRotation rotation,
                                  MediaTimestamp time, bool cue) {
  return impl_->Draw(input, crop, rotation, time, cue);
}

VideoFrame MovieRenderer::EncoderFrame(const ColorSurface& output, const VideoEncoderConfig& config,
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

const std::array<ResourceRef, 2>& PreviewStaticMaskResources() {
  static const std::array<ResourceRef, 2> resources{{
      {"avkit.preview", "1", "static-window"}, {"avkit.preview", "1", "static-offset"},
  }};
  return resources;
}

const ResourceRef& PreviewAnimatedMaskResource() {
  static const ResourceRef resource{"avkit.preview", "1", "clip-mask-sequence"};
  return resource;
}

} // namespace avkit_preview
