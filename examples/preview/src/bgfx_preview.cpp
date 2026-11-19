#include "bgfx_preview.h"

#include <huxerui/avkit/bgfx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace avkit_preview {

namespace {

using namespace huxerui;
using namespace huxerui::avkit::render;

constexpr auto kRgba = avkit::render::PixelFormat::Rgba8Unorm;
#if defined(_WIN32)
constexpr ImageDesc kCanvas{{64, 64}, avkit::render::PixelFormat::Bgra8Unorm};
#else
constexpr ImageDesc kCanvas{{64, 64}, kRgba};
#endif

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

ImageSurface Rows(ImageDesc image, std::initializer_list<std::uint8_t> rows, std::size_t stride) {
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(rows);
  return RetainBgfxPixels(image, BufferReference(std::as_bytes(std::span(*bytes)), bytes), stride);
}

ColorSurface Render(RenderBackend& backend, const RenderRequest& request) {
  const auto submitted = backend.TrySubmit(request);
  if (!submitted.Succeeded()) throw std::runtime_error(submitted.Error().message);
  Require(submitted.Value().has_value(), "The preview could not acquire an output texture");
  const auto ticket = *submitted.Value();
  const auto pending = backend.TryTake(ticket);
  Require(pending.Succeeded() && !pending.Value(), "The preview published a texture before submitting its GPU work");
  backend.Pump();
  auto result = backend.TryTake(ticket);
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  Require(result.Value().has_value(), "The preview GPU work did not complete");
  return std::move(*result.Value());
}

} // namespace

huxerui::avkit::render::VideoPreviewImage CreateBgfxPreview() {
  BgfxPlatformHandles handles;
#if defined(__EMSCRIPTEN__)
  static char canvas[] = "#avkit-bgfx-canvas";
  handles.window = canvas;
#endif
  auto backend = CreateBgfxBackend(kCanvas, handles);
  const ColorSurface base{
      Rows({{1, 2}, kRgba}, {32, 64, 96, 0, 0, 0, 0, 0, 64, 96, 128, 0}, 8), AlphaMode::Opaque};
  const ColorSurface sticker{Rows({{1, 1}, kRgba}, {255, 0, 0, 128}, 4), AlphaMode::Straight};
  const auto composite = std::make_shared<RenderPlan>(RenderPlan{
      .target = kCanvas,
      .draws = {{0, Blend::Replace}, {1, Blend::PremultipliedOver, {0.25F, 0.25F, 0.5F, 0.5F}}},
  });
  auto first = Render(*backend, {composite, {base, sticker}, {}});
  const ColorSurface incoming{Rows({{1, 1}, kRgba}, {80, 120, 80, 255}, 4)};
  const auto mask = Rows({{4, 1}, avkit::render::PixelFormat::R8Unorm}, {0, 0, 0, 255}, 4);
  const auto masked = std::make_shared<RenderPlan>(RenderPlan{
      .target = kCanvas,
      .draws = {{2, Blend::Replace, {0, 0, 1, 1}, MaskedBlend{0, 0, 0}}},
      .subcompositions = {{{{1, Blend::Replace}}}},
  });
  RenderRequest request{masked, {incoming, first}, {mask}};
  const auto second = Render(*backend, request);
  const auto blocked = backend->TrySubmit(request);
  Require(blocked.Succeeded() && !blocked.Value(), "The preview reused a retained output texture");
  const auto* first_slot = first.pixels.storage.get();
  first = {};
  request.inputs.clear();
  const auto recycled = Render(*backend, {composite, {base, sticker}, {}});
  Require(recycled.pixels.storage.get() == first_slot, "The preview did not recycle its released output texture");

  const auto result = ReadBgfxPixels(*backend, second);
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  const auto bytes = result.Value().AsBytes();
  const auto [width, height] = kCanvas.size;
  const auto stride = width * 4;
  Bytes pixels(bytes.size());
  for (std::size_t y = 0; y < height; ++y) {
    const auto row = second.pixels.origin == RowOrigin::TopLeft ? y : height - 1 - y;
    std::ranges::copy(bytes.subspan(row * stride, stride), pixels.begin() + y * stride);
  }
  const auto verify = [&](std::size_t x, std::size_t y, const std::array<int, 4>& expected) {
    for (std::size_t channel = 0; channel < expected.size(); ++channel) {
      const auto index = kCanvas.format == avkit::render::PixelFormat::Bgra8Unorm && channel < 3 ? 2 - channel : channel;
      const auto actual = std::to_integer<int>(pixels[(y * width + x) * 4 + index]);
      if (std::abs(actual - expected[channel]) > 1) {
        throw std::runtime_error("Unexpected composite pixel at " + std::to_string(x) + "," + std::to_string(y) +
                                 " channel " + std::to_string(channel) + " value " + std::to_string(actual));
      }
    }
  };
  verify(8, 8, {32, 64, 96, 255});
  verify(8, 56, {64, 96, 128, 255});
  verify(56, 56, {80, 120, 80, 255});
  verify(24, 24, {144, 32, 48, 255});
  verify(40, 40, {160, 48, 64, 255});
  auto output = MakeBgfxPreviewImage(*backend, second, {0, 1});
  if (!output.Succeeded()) throw std::runtime_error(output.Error().message);
  return std::move(output).Value();
}

} // namespace avkit_preview
