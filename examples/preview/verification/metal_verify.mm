#include "metal_verify.h"
#include <huxerui/avkit/video_texture.h>
#include "preview_sticker.h"
#include "bgfx_metal_texture.h"
#include "sticker_scene.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include <huxerui/macos/external_texture.h>

namespace avkit_preview {
namespace {

using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using huxerui::macos::MetalTexture;
namespace metal = huxerui::avkit::macos;

constexpr auto kSize = kPreviewStickerSize;
constexpr huxerui::Size kPreviewSize{640.0F, 360.0F};

using Pixel = PreviewPixel;

Pixel Background(int x, int y, bool alternate) {
  const auto red = static_cast<std::uint8_t>(20 + x / 8);
  const auto green = static_cast<std::uint8_t>(48 + y / 2);
  const auto blue = static_cast<std::uint8_t>(168 - y / 2);
  return alternate ? Pixel{blue, red, green, 208} : Pixel{red, green, blue, 208};
}

void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

template<class Generator>
ColorSurface Upload(id<MTLDevice> device, Generator pixel, RowOrigin origin = RowOrigin::TopLeft,
                     MTLPixelFormat format = MTLPixelFormatRGBA8Unorm) {
  auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                        width:kSize.width
                                                                       height:kSize.height
                                                                    mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  Require(texture != nil, "Could not allocate a source Metal texture");

  std::vector<Pixel> pixels;
  pixels.reserve(kSize.width * kSize.height);
  for (std::uint32_t row = 0; row < kSize.height; ++row) {
    const std::uint32_t y = origin == RowOrigin::BottomLeft ? kSize.height - 1 - row : row;
    for (std::uint32_t x = 0; x < kSize.width; ++x) {
      auto value = pixel(static_cast<int>(x), static_cast<int>(y));
      if (format == MTLPixelFormatBGRA8Unorm) std::swap(value.r, value.b);
      pixels.push_back(value);
    }
  }
  [texture replaceRegion:MTLRegionMake2D(0, 0, kSize.width, kSize.height)
             mipmapLevel:0
               withBytes:pixels.data()
             bytesPerRow:kSize.width * sizeof(Pixel)];
  return metal::RetainMetalTexture(texture, origin);
}

struct Fixture {
  explicit Fixture(PixelFormat format = PixelFormat::Rgba8Unorm)
      : target{kSize, format},
        backend(metal::CreateBgfxBackend(device, target)) {
    InstallStickerScene(scene);
  }

  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  const ImageDesc target;
  std::unique_ptr<RenderBackend> backend;
  SceneEvaluator scene;
  ColorSurface sticker = CreatePreviewSticker(device);

  RenderRequest Request(bool alternate = false, RowOrigin origin = RowOrigin::TopLeft,
                        huxerui::media::MediaTimestamp time = {}) {
    const auto format = alternate ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
    auto background = Upload(device, [alternate](int x, int y) { return Background(x, y, alternate); }, origin, format);
    return {scene.Evaluate(time, target), {std::move(background), sticker}};
  }
};

RenderTicket Submit(RenderBackend& backend, const RenderRequest& request) {
  auto result = backend.TrySubmit(request);
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  Require(result.Value().has_value(), "Composite submission hit unexpected backpressure");
  return *result.Value();
}

ColorSurface Complete(RenderBackend& backend, RenderTicket ticket) {
  backend.Pump();
  auto result = backend.TryTake(ticket);
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  Require(result.Value().has_value(), "Composite was not ready after its GPU completion wait");
  return std::move(result.Value().value());
}

void Publish(MetalTexture& mailbox, const ColorSurface& surface) {
  mailbox.Publish({
      .texture = metal::GetMetalTexture(surface),
      .origin = MetalTexture::Origin::TopLeft,
      .alpha = MetalTexture::Alpha::Premultiplied,
  });
}

std::vector<Pixel> ReadPixels(id<MTLTexture> texture) {
  Require(texture != nil && texture.width && texture.height, "Readback requires a nonempty texture");
  Require(texture.pixelFormat == MTLPixelFormatRGBA8Unorm || texture.pixelFormat == MTLPixelFormatBGRA8Unorm,
          "Readback requires an 8-bit RGBA or BGRA texture");
  id<MTLDevice> device = texture.device;
  const auto width = texture.width, height = texture.height;
  const NSUInteger pixel_bytes = width * sizeof(Pixel);
  const NSUInteger stride = (pixel_bytes + 255) & ~NSUInteger{255};
  const NSUInteger length = stride * height;
  id<MTLBuffer> buffer = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
  id<MTLCommandQueue> queue = [device newCommandQueue];
  id<MTLCommandBuffer> commands = [queue commandBuffer];
  id<MTLBlitCommandEncoder> encoder = [commands blitCommandEncoder];
  Require(buffer != nil && encoder != nil, "Could not prepare Metal readback");
  [encoder copyFromTexture:texture
               sourceSlice:0
               sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(width, height, 1)
                  toBuffer:buffer
         destinationOffset:0
    destinationBytesPerRow:stride
  destinationBytesPerImage:length];
  [encoder endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  Require(commands.status == MTLCommandBufferStatusCompleted, "Metal readback did not complete successfully");

  std::vector<Pixel> pixels(width * height);
  for (NSUInteger row = 0; row < height; ++row) {
    const auto* bytes = static_cast<const std::byte*>(buffer.contents) + row * stride;
    std::memcpy(pixels.data() + row * width, bytes, pixel_bytes);
  }
  if (texture.pixelFormat == MTLPixelFormatBGRA8Unorm) {
    for (auto& pixel : pixels) std::swap(pixel.r, pixel.b);
  }
  return pixels;
}

Pixel Expected(int x, int y, bool alternate) {
  const auto base = Background(x, y, alternate);
  const auto sticker = PreviewStickerPixel(x, y);
  const int remainder = 255 - sticker.a;
  const auto over = [remainder](int source, int destination) {
    return static_cast<std::uint8_t>(source + (destination * remainder + 127) / 255);
  };
  return {over(sticker.r, base.r), over(sticker.g, base.g), over(sticker.b, base.b), over(sticker.a, base.a)};
}

void CheckPixels(id<MTLTexture> texture, bool alternate, const std::string& label) {
  Require(texture.width == kSize.width && texture.height == kSize.height,
          "Readback texture dimensions do not match the plan");
  const auto pixels = ReadPixels(texture);
  const auto matches = [](int a, int b) { return std::abs(a - b) <= 1; };
  for (std::uint32_t y = 0; y < kSize.height; ++y) {
    for (std::uint32_t x = 0; x < kSize.width; ++x) {
      const auto expected = Expected(static_cast<int>(x), static_cast<int>(y), alternate);
      const auto actual = pixels[y * kSize.width + x];
      if (!matches(expected.r, actual.r) || !matches(expected.g, actual.g) ||
          !matches(expected.b, actual.b) || !matches(expected.a, actual.a)) {
        throw std::runtime_error(label + " pixel mismatch at (" + std::to_string(x) + ", " + std::to_string(y) +
                                 "). Expected RGBA " + std::to_string(expected.r) + "/" +
                                 std::to_string(expected.g) + "/" + std::to_string(expected.b) + "/" +
                                 std::to_string(expected.a) + ", got " + std::to_string(actual.r) + "/" +
                                 std::to_string(actual.g) + "/" + std::to_string(actual.b) + "/" +
                                 std::to_string(actual.a));
      }
    }
  }
}

id<MTLTexture> Acquire(const MetalTexture& mailbox) {
  MetalTexture::Origin origin{};
  id<MTLTexture> texture = mailbox.AcquireFrame(origin);
  Require(texture != nil && origin == MetalTexture::Origin::TopLeft, "Invalid published Metal snapshot");
  return texture;
}

void CheckSurface(const ColorSurface& surface, ImageDesc target) {
  Require(surface.pixels.image == target && surface.pixels.origin == RowOrigin::TopLeft &&
              surface.alpha == AlphaMode::Premultiplied,
          "The output metadata does not match the plan");
}

void Verify(PixelFormat format) {
  Fixture fixture(format);
  auto& backend = *fixture.backend;
  const auto request_a = fixture.Request();
  const auto request_b = fixture.Request(true, RowOrigin::BottomLeft);
  const auto ticket = Submit(backend, request_a);
  const auto pending = backend.TryTake(ticket);
  Require(pending.Succeeded() && !pending.Value(), "A pending ticket returned a readable surface");

  const auto first = Complete(backend, ticket);
  CheckSurface(first, fixture.target);
  CheckPixels(metal::GetMetalTexture(first), false, "A");
  auto opened = OpenBgfxPreviewTexture(fixture.target.size);
  Require(opened.Succeeded(), "Cannot open a reusable bgfx preview texture");
  auto preview = std::move(opened).Value();
  const auto mailbox = std::dynamic_pointer_cast<MetalTexture>(preview->Texture());
  Require(mailbox && mailbox->Revision() == 0, "Expected an empty Metal preview texture");
  Require(preview->Publish(backend, first).Succeeded() && mailbox->Revision() == 1, "Cannot publish preview A");
  id<MTLTexture> first_snapshot = Acquire(*mailbox);
  Require(first_snapshot != metal::GetMetalTexture(first), "Publication did not produce an independent snapshot");
  CheckPixels(first_snapshot, false, "published A");

  const SurfaceStorage* reusable_slot{};
  {
    const auto second = Complete(backend, Submit(backend, request_b));
    CheckSurface(second, fixture.target);
    Require(first.pixels.storage != second.pixels.storage, "A leased target was reused for B");
    CheckPixels(metal::GetMetalTexture(second), true, "B");
    CheckPixels(metal::GetMetalTexture(first), false, "leased A after B");
    Require(preview->Publish(backend, second).Succeeded() && preview->Texture() == mailbox && mailbox->Revision() == 2,
            "Preview B did not update the same texture");
    CheckPixels(Acquire(*mailbox), true, "published B");
    const auto blocked = backend.TrySubmit(request_a);
    Require(blocked.Succeeded() && !blocked.Value(), "The pool accepted a request with both targets leased");
    reusable_slot = second.pixels.storage.get();
  }

  const auto third = Complete(backend, Submit(backend, request_a));
  CheckSurface(third, fixture.target);
  Require(third.pixels.storage.get() == reusable_slot, "The pool did not reuse B's released target");
  CheckPixels(metal::GetMetalTexture(third), false, "A after reuse");
  CheckPixels(metal::GetMetalTexture(first), false, "leased A after reuse");
  Require(preview->Publish(backend, third).Succeeded() && preview->Texture() == mailbox && mailbox->Revision() == 3,
          "Republishing A did not update the same texture");
  CheckPixels(Acquire(*mailbox), false, "republished A");
  CheckPixels(first_snapshot, false, "retained HuxerUI snapshot");
  preview->Finish();
  Require(!preview->Publish(backend, third).Succeeded() && mailbox->Revision() == 3, "A finished preview accepted a frame");
  CheckPixels(Acquire(*mailbox), false, "last frame after finish");
}

void VerifyAnimatedSticker() {
  Fixture fixture(PixelFormat::Bgra8Unorm);
  fixture.sticker = CreatePreviewSticker(fixture.device, true);
  MetalTexture mailbox(kPreviewSize);
  const huxerui::media::MediaTimestamp times[] = {{}, {1, 2}, {0, 44100}};
  for (auto time : times) {
    const auto request = fixture.Request(false, RowOrigin::TopLeft, time);
    auto& backend = *fixture.backend;
    const auto surface = Complete(backend, Submit(backend, request));
    Publish(mailbox, surface);
    Require(HasPreviewStickerCue(Acquire(mailbox), time), "HuxerUI preview did not place the sticker at its media PTS");
  }
  mailbox.Finish();
}

} // namespace

ColorSurface CreatePreviewSticker(id<MTLDevice> device, bool cue) {
  return Upload(device, [cue](int x, int y) { return PreviewStickerPixel(x, y, cue); });
}

bool HasPreviewStickerCue(id<MTLTexture> texture, huxerui::media::MediaTimestamp time) {
  const auto pixels = ReadPixels(texture);
  const auto center = StickerCueCenter(time);
  const auto x = static_cast<NSUInteger>(center.x * texture.width);
  const auto y = static_cast<NSUInteger>(center.y * texture.height);
  return pixels[y * texture.width + x] == Pixel{16, 250, 32, 255};
}

huxerui::Bytes ReadPreviewPixels(id<MTLTexture> texture) {
  const auto pixels = ReadPixels(texture);
  const auto bytes = std::as_bytes(std::span(pixels));
  return {bytes.begin(), bytes.end()};
}

void PublishPreviewSurface(MetalTexture& mailbox, const ColorSurface& surface) { Publish(mailbox, surface); }

int VerifyMetalPreview() {
  @autoreleasepool {
    try {
      VerifyStickerScene();
      Verify(PixelFormat::Rgba8Unorm);
      Verify(PixelFormat::Bgra8Unorm);
      VerifyAnimatedSticker();
      std::cout << "Flecs sticker verified: external component and system, parent lifetimes, equivalent media PTS, "
                   "backward sampling, and animated Metal preview pixels.\n";
      std::cout << "Metal RGBA8 and BGRA8 composites verified: full-image pixels, orientation, premultiplied alpha, "
                   "GPU completion, two-slot reuse, and HuxerUI snapshots.\n";
      return 0;
    } catch (const std::exception& error) {
      std::cerr << "Metal composite verification failed: " << error.what() << '\n';
      return 1;
    }
  }
}

} // namespace avkit_preview
