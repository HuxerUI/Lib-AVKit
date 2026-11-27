#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

#include <huxerui/geometry.h>
#include <huxerui/media_codec.h>

namespace huxerui::avkit::detail {

struct VideoTextureFit {
  media::VideoRectangle area;
  double crop_width{1}, crop_height{1};

  Affine2D SourceToTextureUv(media::VideoTextureOptions options) const {
    const auto x = static_cast<float>(area.width / (options.width * crop_width));
    const auto y = static_cast<float>(area.height / (options.height * crop_height));
    const auto left = float(area.x) / options.width;
    const auto top = float(area.y) / options.height;
    return {x, 0, 0, y, left - float((1 - crop_width) / 2) * x, top - float((1 - crop_height) / 2) * y};
  }
};

inline std::pair<double, double> VideoDisplaySize(const media::VideoFormat& format) {
  double width = double(format.visible.width) * format.aspect_width / format.aspect_height;
  double height = format.visible.height;
  if (format.rotation == media::VideoRotation::Clockwise90 || format.rotation == media::VideoRotation::Clockwise270)
    std::swap(width, height);
  return {width, height};
}

inline VideoTextureFit FitVideoTexture(const media::VideoFormat& format, media::VideoTextureOptions options) {
  const auto [width, height] = VideoDisplaySize(format);
  const auto scale_x = options.width / width, scale_y = options.height / height;
  VideoTextureFit fit{{0, 0, options.width, options.height}};
  if (options.fit == ImageFit::Cover) {
    const auto scale = std::max(scale_x, scale_y);
    fit.crop_width = scale_x / scale;
    fit.crop_height = scale_y / scale;
  } else {
    // Match the centered integer placement of Lib-Media's platform texture fit.
    const auto scale = std::min(scale_x, scale_y);
    fit.area.width = static_cast<std::uint32_t>(std::clamp(std::lround(width * scale), 1L, long(options.width)));
    fit.area.height = static_cast<std::uint32_t>(std::clamp(std::lround(height * scale), 1L, long(options.height)));
    fit.area.x = (options.width - fit.area.width) / 2;
    fit.area.y = (options.height - fit.area.height) / 2;
  }
  return fit;
}

} // namespace huxerui::avkit::detail
