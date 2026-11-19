#include <huxerui/avkit/render.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace huxerui::avkit::render {
namespace {

class CpuImageStorage final : public SurfaceStorage {
public:
  CpuImageStorage(ImageDesc image, ImagePixels pixels, RowOrigin origin)
      : image(image), pixels(std::move(pixels)), origin(origin) {}

  ImageDesc image;
  ImagePixels pixels;
  RowOrigin origin;
};

} // namespace

ImageSurface RetainImagePixels(ImageDesc image, ImagePixels pixels, RowOrigin origin) {
  const auto [width, height] = image.size;
  const auto channels = image.format == PixelFormat::R8Unorm ? 1 : 4;
  if (!width || !height || width > std::numeric_limits<std::size_t>::max() / channels)
    throw std::invalid_argument("Invalid image dimensions");
  const auto packed = std::size_t(width) * channels;
  const auto size = pixels.bytes.AsBytes().size();
  if (pixels.row_bytes < packed || size < packed || height - 1 > (size - packed) / pixels.row_bytes)
    throw std::invalid_argument("Pixel storage does not cover the specified rows");
  return {image, origin, std::make_shared<CpuImageStorage>(image, std::move(pixels), origin)};
}

std::optional<ImagePixels> GetImagePixels(const ImageSurface& surface) {
  const auto* storage = dynamic_cast<const CpuImageStorage*>(surface.storage.get());
  if (!storage || storage->image != surface.image || storage->origin != surface.origin) return std::nullopt;
  return storage->pixels;
}

} // namespace huxerui::avkit::render
