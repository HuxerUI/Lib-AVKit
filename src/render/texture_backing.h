#pragma once

#include <huxerui/avkit/render.h>

#include <utility>

namespace huxerui::avkit::detail {

using render::AlphaMode;
using render::ImageInfo;
using render::PixelFormat;
using render::Texture;
using render::RowOrigin;

/// @brief Keeps CPU rows or a device image alive until all users release it.
class TextureBacking {
public:
  virtual ~TextureBacking() = default;
};

inline bool IsColorImageFormat(PixelFormat format) {
  return format == PixelFormat::Rgba8Unorm || format == PixelFormat::Bgra8Unorm;
}

/// @brief Creates images and exposes their backing to internal render adapters.
struct TextureAccess {
  static Texture Make(ImageInfo info, RowOrigin origin, std::shared_ptr<const TextureBacking> backing,
                          std::optional<AlphaMode> alpha = {}) {
    Texture image;
    if (!backing) return image;
    image.info_ = info;
    image.origin_ = origin;
    if (IsColorImageFormat(info.format)) image.alpha_ = alpha.value_or(AlphaMode::Premultiplied);
    image.backing_ = std::move(backing);
    return image;
  }

  static const std::shared_ptr<const TextureBacking>& Backing(const Texture& image) { return image.backing_; }
};

} // namespace huxerui::avkit::detail
