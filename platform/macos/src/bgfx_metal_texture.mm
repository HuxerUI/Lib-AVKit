#include "bgfx_metal_texture.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>

#include <huxerui/macos/external_texture.h>

#include "texture_bridge.h"

namespace huxerui::avkit::macos {

namespace {

using namespace render;

using huxerui::macos::MetalTexture;
using huxerui::macos::PixelBufferTexture;

struct CFDeleter {
  void operator()(CFTypeRef value) const { CFRelease(value); }
};
template<class T> using Owned = std::unique_ptr<std::remove_pointer_t<T>, CFDeleter>;

media::VideoFrame ReadI420(CVPixelBufferRef image, const media::VideoFormat& format, media::MediaTimestamp pts,
                    std::optional<media::MediaTimestamp> duration) {
  const auto width = format.width, height = format.height;
  auto luma = std::make_shared<Bytes>(std::size_t(width) * height);
  auto u = std::make_shared<Bytes>(luma->size() / 4);
  auto v = std::make_shared<Bytes>(u->size());
  if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
    throw std::runtime_error("Cannot read the completed NV12 buffer for software encoding");
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

class PixelBufferPool {
public:
  Owned<CVPixelBufferRef> Allocate(PixelSize size, OSType format) {
    if (!pool_ || size_ != size || format_ != format) {
      NSDictionary* attributes = @{
        (__bridge NSString*)kCVPixelBufferWidthKey: @(size.width),
        (__bridge NSString*)kCVPixelBufferHeightKey: @(size.height),
        (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(format),
        (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey: @YES
      };
      CVPixelBufferPoolRef pool = nullptr;
      if (CVPixelBufferPoolCreate(nullptr, nullptr, (__bridge CFDictionaryRef)attributes, &pool) != kCVReturnSuccess)
        throw std::runtime_error("Cannot create the Metal video pixel buffer pool");
      pool_.reset(pool);
      size_ = size;
      format_ = format;
    }
    CVPixelBufferRef pixels = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pool_.get(), &pixels) != kCVReturnSuccess)
      throw std::runtime_error("Cannot allocate Metal video pixels");
    return Owned<CVPixelBufferRef>(pixels);
  }

private:
  Owned<CVPixelBufferPoolRef> pool_;
  PixelSize size_{};
  OSType format_{};
};

MTLPixelFormat MetalFormat(PixelFormat format) {
  switch (format) {
    case PixelFormat::Rgba8Unorm: return MTLPixelFormatRGBA8Unorm;
    case PixelFormat::Bgra8Unorm: return MTLPixelFormatBGRA8Unorm;
    case PixelFormat::R8Unorm: return MTLPixelFormatR8Unorm;
    case PixelFormat::R32Float: return MTLPixelFormatR32Float;
  }
  throw std::invalid_argument("Unsupported Metal texture format");
}

bool CanSample(id<MTLTexture> texture) {
  return texture != nil && texture.textureType == MTLTextureType2D && texture.sampleCount == 1 &&
         !texture.framebufferOnly &&
         (texture.usage == MTLTextureUsageUnknown || (texture.usage & MTLTextureUsageShaderRead) != 0);
}

class MetalStorage final : public SurfaceStorage {
public:
  explicit MetalStorage(id<MTLTexture> handle) : texture(handle) {}
  id<MTLTexture> texture;
  Owned<CVPixelBufferRef> pixels;
  Owned<CVMetalTextureRef> view;
};

id<MTLTexture> TextureFromSurface(const ImageSurface& surface) {
  auto storage = std::dynamic_pointer_cast<const MetalStorage>(surface.storage);
  return storage ? storage->texture : nil;
}

ImageSurface RetainTexture(id<MTLTexture> texture, PixelFormat format, RowOrigin origin) {
  if (!CanSample(texture) || texture.width > std::numeric_limits<std::uint16_t>::max() ||
      texture.height > std::numeric_limits<std::uint16_t>::max()) {
    throw std::invalid_argument("Expected a sampleable 2D Metal texture");
  }
  const ImageDesc image{{static_cast<std::uint32_t>(texture.width), static_cast<std::uint32_t>(texture.height)}, format};
  return {image, origin, std::make_shared<MetalStorage>(texture)};
}

class MetalPreviewTexture final : public BgfxPreviewTexture {
public:
  explicit MetalPreviewTexture(PixelSize size)
      : texture_(std::make_shared<MetalTexture>(Size{float(size.width), float(size.height)})) {}

  std::shared_ptr<ExternalTexture> Texture() const override { return texture_; }
  void Finish() noexcept override { texture_->Finish(); }

  void PublishFrame(const ColorSurface& surface) override { Publish(*texture_, surface); }

private:
  PreviewFrameCommit PrepareFrame(const ColorSurface& surface) override {
    return [texture = texture_, surface] { Publish(*texture, surface); };
  }

  static void Publish(MetalTexture& texture, const ColorSurface& surface) {
    texture.Publish({.texture = TextureFromSurface(surface.pixels),
        .origin = surface.pixels.origin == RowOrigin::TopLeft ? MetalTexture::Origin::TopLeft : MetalTexture::Origin::BottomLeft,
        .alpha = MetalTexture::Alpha::Premultiplied});
  }

  std::shared_ptr<MetalTexture> texture_;
};

class MetalTextureBridge final : public detail::BgfxTextureBridge {
public:
  explicit MetalTextureBridge(id<MTLDevice> device) : device_(device) {}

  std::shared_ptr<const SurfaceStorage> CreateOutput(ImageDesc image) const override {
    auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MetalFormat(image.format)
                                                                          width:image.size.width
                                                                         height:image.size.height
                                                                      mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
    if (texture == nil) throw std::runtime_error("Could not allocate a Metal render target");
    return std::make_shared<MetalStorage>(texture);
  }

  std::uint64_t GetTexture(const ImageSurface& surface) const override {
    id<MTLTexture> texture = TextureFromSurface(surface);
    const auto& image = surface.image;
    if (!CanSample(texture) || texture.device != device_ || texture.pixelFormat != MetalFormat(image.format) ||
        texture.width != image.size.width || texture.height != image.size.height) return 0;
    return reinterpret_cast<std::uint64_t>((__bridge void*)texture);
  }

  void WaitForCompletion() const override {
    // At one frame in flight, the pinned renderer waits for the previous Metal command buffer
    // before submitting the next frame.
    bgfx::frame(BGFX_FRAME_FLUSH);
  }

  detail::ImportedTexture Acquire(const ExternalTexture& source) override {
    using R = detail::ImportedTexture;
    @autoreleasepool {
      if (const auto* mailbox = dynamic_cast<const MetalTexture*>(&source)) {
        MetalTexture::Origin origin{};
        id<MTLTexture> texture = mailbox->AcquireFrame(origin);
        if (!texture || texture.device != device_) {
          return R::Failure({RenderErrorCode::Unsupported, "The published Metal texture uses another device or has no frame"});
        }
        return R::Success(RetainMetalTexture(
            texture, origin == MetalTexture::Origin::TopLeft ? RowOrigin::TopLeft : RowOrigin::BottomLeft));
      }
      const auto* mailbox = dynamic_cast<const PixelBufferTexture*>(&source);
      Owned<CVPixelBufferRef> pixels(mailbox ? mailbox->AcquireFrame() : nullptr);
      if (!pixels) return R::Failure({RenderErrorCode::Unsupported, "Expected a published Apple video texture"});
      const PixelSize size{static_cast<std::uint32_t>(CVPixelBufferGetWidth(pixels.get())),
                           static_cast<std::uint32_t>(CVPixelBufferGetHeight(pixels.get()))};
      if (CVPixelBufferGetPixelFormatType(pixels.get()) != kCVPixelFormatType_32BGRA) {
        auto rgba = input_pool_.Allocate(size, kCVPixelFormatType_32BGRA);
        auto* image = [CIImage imageWithCVPixelBuffer:pixels.get()];
        [Context() render:image toCVPixelBuffer:rgba.get() bounds:CGRectMake(0, 0, size.width, size.height)
               colorSpace:Srgb()];
        pixels = std::move(rgba);
      }

      if (!cache_) {
        CVMetalTextureCacheRef cache = nullptr;
        if (CVMetalTextureCacheCreate(nullptr, nullptr, device_, nullptr, &cache) != kCVReturnSuccess) {
          return R::Failure({RenderErrorCode::Unsupported, "Cannot create a CoreVideo Metal texture cache"});
        }
        cache_.reset(cache);
      }
      CVMetalTextureRef view = nullptr;
      if (CVMetalTextureCacheCreateTextureFromImage(nullptr, cache_.get(), pixels.get(), nullptr,
              MTLPixelFormatBGRA8Unorm, size.width, size.height, 0, &view) != kCVReturnSuccess) {
        return R::Failure({RenderErrorCode::Unsupported, "CoreVideo pixels cannot be shared with Metal"});
      }
      Owned<CVMetalTextureRef> owned_view(view);
      auto storage = std::make_shared<MetalStorage>(CVMetalTextureGetTexture(view));
      if (!CanSample(storage->texture)) {
        return R::Failure({RenderErrorCode::Unsupported, "The CoreVideo Metal image cannot be sampled"});
      }
      storage->view = std::move(owned_view);
      storage->pixels = std::move(pixels);
      return R::Success({{{size, PixelFormat::Bgra8Unorm}, RowOrigin::TopLeft, std::move(storage)}});
    }
  }

  detail::PublishedTexture Publish(const ColorSurface& surface, media::MediaTimestamp) override {
    using R = detail::PublishedTexture;
    try {
      MetalPreviewTexture preview(surface.pixels.image.size);
      preview.PublishFrame(surface);
      preview.Finish();
      return R::Success(preview.Texture());
    } catch (const std::exception& error) {
      return R::Failure({RenderErrorCode::Unsupported, error.what()});
    }
  }

  detail::EncoderPicture Encode(const ColorSurface& surface, const media::VideoEncoderConfig& config,
                                media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration) override {
    using R = detail::EncoderPicture;
    using namespace media;
    const auto& format = config.input;
    const bool cpu = config.input_storage == VideoStorage::Cpu;
    if ((cpu ? format.pixel_format != VideoPixelFormat::I420 : format.pixel_format != VideoPixelFormat::NV12) ||
        format.matrix != ColorMatrix::BT709 ||
        format.range != ColorRange::Limited || format.transfer != ColorTransfer::BT709 ||
        format.primaries != ColorPrimaries::BT709 || format.width % 2 || format.height % 2) {
      return R::Failure({RenderErrorCode::Unsupported, "The Apple GPU encoder transfer requires limited-range BT.709 NV12"});
    }
    @autoreleasepool {
      try {
        auto pixels = encoder_pool_.Allocate(surface.pixels.image.size, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
        CVBufferSetAttachment(pixels.get(), kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixels.get(), kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixels.get(), kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        auto* image = [CIImage imageWithMTLTexture:TextureFromSurface(surface.pixels)
                                          options:@{kCIImageColorSpace: (__bridge id)Srgb()}];
        auto* destination = [[CIRenderDestination alloc] initWithPixelBuffer:pixels.get()];
        Owned<CGColorSpaceRef> bt709(CGColorSpaceCreateWithName(kCGColorSpaceITUR_709));
        destination.colorSpace = bt709.get();
        destination.flipped = NO;
        NSError* error = nil;
        auto* task = image ? [Context() startTaskToRender:image toDestination:destination error:&error] : nil;
        if (!task || ![task waitUntilCompletedAndReturnError:&error]) {
          return R::Failure({RenderErrorCode::Unsupported, "Core Image could not render the encoder's NV12 frame"});
        }
        if (cpu) return R::Success(ReadI420(pixels.get(), format, pts, duration));
        auto mailbox = std::make_shared<PixelBufferTexture>(Size{float(format.width), float(format.height)});
        mailbox->Publish(pixels.get());
        mailbox->Finish();
        auto frame = MakeVideoFrame(mailbox, format, pts, duration);
        if (!frame.Succeeded()) return R::Failure({RenderErrorCode::Unsupported, frame.Error().message});
        return R::Success(std::move(frame).Value());
      } catch (const std::exception& error) {
        return R::Failure({RenderErrorCode::Unsupported, error.what()});
      }
    }
  }

private:
  CIContext* Context() {
    if (!context_) context_ = [CIContext contextWithMTLDevice:device_];
    return context_;
  }

  CGColorSpaceRef Srgb() {
    if (!srgb_) srgb_.reset(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    return srgb_.get();
  }

  id<MTLDevice> device_;
  CIContext* context_{};
  Owned<CGColorSpaceRef> srgb_;
  Owned<CVMetalTextureCacheRef> cache_;
  PixelBufferPool input_pool_;
  PixelBufferPool encoder_pool_;
};

} // namespace

std::unique_ptr<render::RenderBackend> CreateBgfxBackend(id<MTLDevice> device, render::ImageDesc target,
                                                       std::size_t output_slots) {
  if (device == nil) throw std::invalid_argument("A Metal device is required");
  bgfx::Init init;
  init.type = bgfx::RendererType::Metal;
  init.fallback = false;
  init.platformData.context = (__bridge void*)device;
  init.swapChain.maxFrameLatency = 1;
  return detail::CreateBgfxBackend(target, init, std::make_unique<MetalTextureBridge>(device), nullptr, output_slots);
}

render::ColorSurface RetainMetalTexture(id<MTLTexture> texture, render::RowOrigin origin) {
  PixelFormat format;
  switch (texture.pixelFormat) {
    case MTLPixelFormatRGBA8Unorm: format = PixelFormat::Rgba8Unorm; break;
    case MTLPixelFormatBGRA8Unorm: format = PixelFormat::Bgra8Unorm; break;
    default: throw std::invalid_argument("Expected RGBA8Unorm or BGRA8Unorm input");
  }
  return {RetainTexture(texture, format, origin), AlphaMode::Premultiplied};
}

render::ImageSurface RetainMetalDataTexture(id<MTLTexture> texture, render::RowOrigin origin) {
  if (texture.pixelFormat != MTLPixelFormatR8Unorm) throw std::invalid_argument("Expected R8Unorm mask input");
  return RetainTexture(texture, PixelFormat::R8Unorm, origin);
}

id<MTLTexture> GetMetalTexture(const render::ColorSurface& surface) {
  return TextureFromSurface(surface.pixels);
}

} // namespace huxerui::avkit::macos

namespace huxerui::avkit::render::detail {

std::unique_ptr<BgfxPreviewTexture> OpenPlatformBgfxPreviewTexture(PixelSize size) {
  return std::make_unique<macos::MetalPreviewTexture>(size);
}

std::unique_ptr<RenderBackend> CreatePlatformBgfxBackend(ImageDesc target, bgfx::Init init) {
  if (init.swapChain.nwh) {
    init.type = bgfx::RendererType::Metal;
    return CreateBgfxBackend(target, init);
  }
  @autoreleasepool {
    // PAG needs both clip surfaces while the previous composite may still await presentation.
    constexpr std::size_t kOutputSlots = 3;
    return macos::CreateBgfxBackend(MTLCreateSystemDefaultDevice(), target, kOutputSlots);
  }
}

} // namespace huxerui::avkit::render::detail
