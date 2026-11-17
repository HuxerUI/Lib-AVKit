#include "metal_preview.h"
#include "sticker_scene.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include <huxerui/avkit/macos/bgfx.h>
#include <huxerui/macos/external_texture.h>

namespace avkit_preview {
namespace {

using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using huxerui::macos::MetalTexture;

constexpr PixelSize kSize{320, 180};
constexpr huxerui::Size kPreviewSize{640.0F, 360.0F};

struct Pixel {
  std::uint8_t r, g, b, a;
  bool operator==(const Pixel&) const = default;
};
static_assert(sizeof(Pixel) == 4);

Pixel Background(int x, int y, bool alternate) {
  const auto red = static_cast<std::uint8_t>(20 + x / 8);
  const auto green = static_cast<std::uint8_t>(48 + y / 2);
  const auto blue = static_cast<std::uint8_t>(168 - y / 2);
  return alternate ? Pixel{blue, red, green, 208} : Pixel{red, green, blue, 208};
}

Pixel Sticker(int x, int y) {
  const int dx = x - 196;
  const int dy = y - 80;
  const int distance = dx * dx + dy * dy;
  if (distance < 43 * 43) return {120, 59, 19, 128};
  if (distance < 46 * 46) return {88, 72, 43, 96};
  return {};
}

void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

template<class Generator>
RenderSurface Upload(id<MTLDevice> device, Generator pixel, RowOrigin origin = RowOrigin::TopLeft,
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
  return macos::RetainMetalTexture(texture, origin);
}

struct Fixture {
  explicit Fixture(ImageFormat format = ImageFormat::Rgba8Unorm)
      : target{kSize, format},
        backend(macos::CreateBgfxBackend(device, target)) {
    InstallStickerScene(scene);
  }

  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  const ImageDesc target;
  std::unique_ptr<RenderBackend> backend;
  SceneEvaluator scene;
  RenderSurface sticker = CreatePreviewSticker(device);

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

RenderSurface Complete(RenderBackend& backend, RenderTicket ticket) {
  backend.Pump();
  auto result = backend.TryTake(ticket);
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  Require(result.Value().has_value(), "Composite was not ready after its GPU completion wait");
  return std::move(result.Value().value());
}

void Publish(MetalTexture& mailbox, const RenderSurface& surface) {
  mailbox.Publish({
      .texture = macos::GetMetalTexture(surface),
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
  const auto sticker = Sticker(x, y);
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

void CheckSurface(const RenderSurface& surface, ImageDesc target) {
  Require(surface.pixels.image == target && surface.pixels.origin == RowOrigin::TopLeft &&
              surface.alpha == AlphaMode::Premultiplied,
          "The output metadata does not match the plan");
}

void Verify(ImageFormat format) {
  Fixture fixture(format);
  auto& backend = *fixture.backend;
  const auto request_a = fixture.Request();
  const auto request_b = fixture.Request(true, RowOrigin::BottomLeft);
  const auto ticket = Submit(backend, request_a);
  const auto pending = backend.TryTake(ticket);
  Require(pending.Succeeded() && !pending.Value(), "A pending ticket returned a readable surface");

  const auto first = Complete(backend, ticket);
  CheckSurface(first, fixture.target);
  CheckPixels(macos::GetMetalTexture(first), false, "A");
  MetalTexture mailbox(kPreviewSize);
  Publish(mailbox, first);
  id<MTLTexture> first_snapshot = Acquire(mailbox);
  Require(first_snapshot != macos::GetMetalTexture(first), "Publication did not produce an independent snapshot");
  CheckPixels(first_snapshot, false, "published A");

  const SurfaceStorage* reusable_slot{};
  {
    const auto second = Complete(backend, Submit(backend, request_b));
    CheckSurface(second, fixture.target);
    Require(first.pixels.storage != second.pixels.storage, "A leased target was reused for B");
    CheckPixels(macos::GetMetalTexture(second), true, "B");
    CheckPixels(macos::GetMetalTexture(first), false, "leased A after B");
    Publish(mailbox, second);
    CheckPixels(Acquire(mailbox), true, "published B");
    const auto blocked = backend.TrySubmit(request_a);
    Require(blocked.Succeeded() && !blocked.Value(), "The pool accepted a request with both targets leased");
    reusable_slot = second.pixels.storage.get();
  }

  const auto third = Complete(backend, Submit(backend, request_a));
  CheckSurface(third, fixture.target);
  Require(third.pixels.storage.get() == reusable_slot, "The pool did not reuse B's released target");
  CheckPixels(macos::GetMetalTexture(third), false, "A after reuse");
  CheckPixels(macos::GetMetalTexture(first), false, "leased A after reuse");
  Publish(mailbox, third);
  CheckPixels(Acquire(mailbox), false, "republished A");
  CheckPixels(first_snapshot, false, "retained HuxerUI snapshot");
  mailbox.Finish();
}

void VerifyAnimatedSticker() {
  Fixture fixture(ImageFormat::Bgra8Unorm);
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

RenderSurface CreatePreviewSticker(id<MTLDevice> device, bool cue) {
  if (cue) return Upload(device, [](int x, int y) {
    if (std::abs(x - 196) < 15 && std::abs(y - 80) < 15) return Pixel{16, 250, 32, 255};
    return Sticker(x, y);
  });
  return Upload(device, Sticker);
}

RenderSurface CreateTrackingSticker(id<MTLDevice> device, analysis::TrackedPart part) {
  using enum analysis::TrackedPart;
  return Upload(device, [part](int x, int y) {
    const float u = (x + 0.5F) / kSize.width, v = (y + 0.5F) / kSize.height;
    if (part == Face) {
      const auto radius = std::hypot((u - (u < 0.5F ? 0.25F : 0.75F)) / 0.20F, (v - 0.5F) / 0.36F);
      if ((radius > 0.65F && radius < 1) || (std::abs(u - 0.5F) < 0.08F && std::abs(v - 0.5F) < 0.07F))
        return Pixel{252, 235, 30, 255};
      return Pixel{};
    }
    if (std::abs(u - 0.5F) + std::abs(v - 0.5F) > 0.46F) return Pixel{};
    return part == LeftHand ? Pixel{35, 244, 255, 255} : Pixel{254, 72, 192, 255};
  });
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

void PublishPreviewSurface(MetalTexture& mailbox, const RenderSurface& surface) { Publish(mailbox, surface); }

std::shared_ptr<huxerui::ExternalTexture> CreateMetalPreview() {
  @autoreleasepool {
    Fixture fixture;
    auto& backend = *fixture.backend;
    const auto surface = Complete(backend, Submit(backend, fixture.Request()));
    auto mailbox = std::make_shared<MetalTexture>(kPreviewSize);
    Publish(*mailbox, surface);
    mailbox->Finish();
    return mailbox;
  }
}

int VerifyMetalPreview() {
  @autoreleasepool {
    try {
      VerifyStickerScene();
      Verify(ImageFormat::Rgba8Unorm);
      Verify(ImageFormat::Bgra8Unorm);
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
