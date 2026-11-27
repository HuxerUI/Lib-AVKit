#pragma once

#include <huxerui/avkit/render.h>

namespace huxerui::avkit::render::detail {

/// @brief Decodes a local still image for a clip or thumbnail.
/// @param file Image file readable on the calling sequence.
ImagePixels DecodeResourceImage(const File& file);

/// @brief Decodes color pixels from a versioned asset.
/// @param catalog Catalog providing the encoded image.
/// @param resource Exact identity of the image.
ImagePixels DecodeResourceImage(const ResourceCatalog& catalog, const ResourceRef& resource);

/// @brief Decodes the selected channel of a packaged image as R8Unorm.
/// @param catalog Catalog providing the encoded mask image.
/// @param mask Exact image reference and the requested coverage channel.
ImagePixels DecodeMaskImage(const ResourceCatalog& catalog, const MaskImage& mask);

} // namespace huxerui::avkit::render::detail
