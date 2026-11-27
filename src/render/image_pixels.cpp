#include <huxerui/avkit/render.h>

#include <limits>

namespace huxerui::avkit::render {

bool ImagePixels::Validate() const noexcept {
  const auto [width, height] = info.size;
  const auto pixel_bytes = info.format == PixelFormat::R8Unorm ? 1 : 4;
  if (!width || !height || width > std::numeric_limits<std::size_t>::max() / pixel_bytes) return false;
  const auto packed = std::size_t(width) * pixel_bytes;
  const auto size = bytes.AsBytes().size();
  return row_bytes >= packed && size >= packed && height - 1 <= (size - packed) / row_bytes;
}

} // namespace huxerui::avkit::render
