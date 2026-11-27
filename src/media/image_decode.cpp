#include "media/image_asset.h"

#include "project/resource_internal.h"

#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

#include <bimg/decode.h>
#include <bx/allocator.h>

namespace huxerui::avkit::render::detail {
namespace {

ImagePixels DecodeImage(std::span<const std::byte> bytes, std::optional<MaskChannel> mask = {}) {
  if (bytes.empty() || bytes.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument("Invalid image resource size");
  if (mask && *mask != MaskChannel::Grayscale && *mask != MaskChannel::Alpha)
    throw std::invalid_argument("Invalid mask channel");

  const bool coverage = mask.has_value();
  const bool gray = mask == MaskChannel::Grayscale;
  bx::DefaultAllocator allocator;
  std::unique_ptr<bimg::ImageContainer, decltype(&bimg::imageFree)> image(
      bimg::imageParse(&allocator, bytes.data(), static_cast<std::uint32_t>(bytes.size())), bimg::imageFree);
  if (!image || image->m_cubeMap || image->m_numLayers != 1 || image->m_depth > 1 ||
      !image->m_width || !image->m_height || image->m_width > 8192 || image->m_height > 8192 ||
      (coverage && image->m_parser != bimg::ImageParser::Png) ||
      (gray && (image->m_format != bimg::TextureFormat::R8 || image->m_hasAlpha)))
    throw std::invalid_argument("Cannot decode the requested color image or mask channel");

  const auto orientation = image->m_orientation;
  if (!gray && image->m_format != bimg::TextureFormat::RGBA8) {
    image.reset(bimg::imageConvert(&allocator, bimg::TextureFormat::RGBA8, *image, false));
    if (!image) throw std::invalid_argument("Cannot convert image resource to RGBA8");
  }

  PixelSize size{image->m_width, image->m_height};
  const auto count = std::size_t(size.width) * size.height;
  const auto* first = static_cast<const std::byte*>(image->m_data);
  if (!first || image->m_size < count * (gray ? 1 : 4))
    throw std::invalid_argument("Truncated image resource");

  auto output = std::make_shared<Bytes>();
  if (mask == MaskChannel::Alpha) {
    output->resize(count);
    for (std::size_t index = 0; index < count; ++index) (*output)[index] = first[index * 4 + 3];
  } else if (!coverage && orientation != bimg::Orientation::R0) {
    using Orientation = bimg::Orientation;
    const auto width = size.width, height = size.height;
    if (orientation == Orientation::R90 || orientation == Orientation::R270 ||
        orientation == Orientation::HFlipR90 || orientation == Orientation::HFlipR270)
      std::swap(size.width, size.height);
    output->resize(count * 4);
    for (std::uint32_t y = 0; y < size.height; ++y) {
      for (std::uint32_t x = 0; x < size.width; ++x) {
        std::uint32_t sx = x, sy = y;
        switch (orientation) {
          case Orientation::HFlip: sx = width - 1 - x; break;
          case Orientation::VFlip: sy = height - 1 - y; break;
          case Orientation::R180: sx = width - 1 - x; sy = height - 1 - y; break;
          case Orientation::R90: sx = y; sy = height - 1 - x; break;
          case Orientation::R270: sx = width - 1 - y; sy = x; break;
          case Orientation::HFlipR90: sx = width - 1 - y; sy = height - 1 - x; break;
          case Orientation::HFlipR270: sx = y; sy = x; break;
          default: break;
        }
        std::memcpy(output->data() + (std::size_t(y) * size.width + x) * 4,
                    first + (std::size_t(sy) * width + sx) * 4, 4);
      }
    }
  } else {
    output->assign(first, first + count * (coverage ? 1 : 4));
  }
  const auto format = coverage ? PixelFormat::R8Unorm : PixelFormat::Rgba8Unorm;
  return {{size, format}, BufferReference(*output, output), std::size_t(size.width) * (coverage ? 1 : 4),
          RowOrigin::TopLeft, coverage ? std::nullopt : std::optional{AlphaMode::Straight}};
}

} // namespace

ImagePixels DecodeResourceImage(const File& file) {
  auto bytes = file.ReadBytes();
  if (!bytes.Succeeded()) throw std::runtime_error(bytes.Error().message);
  return DecodeImage(bytes.Value());
}

ImagePixels DecodeResourceImage(const ResourceCatalog& catalog, const ResourceRef& resource) {
  const auto bytes = avkit::detail::ReadResourceBytes(catalog, resource);
  return DecodeImage(bytes);
}

ImagePixels DecodeMaskImage(const ResourceCatalog& catalog, const MaskImage& mask) {
  const auto bytes = avkit::detail::ReadResourceBytes(catalog, mask.resource);
  return DecodeImage(bytes, mask.channel);
}

} // namespace huxerui::avkit::render::detail

