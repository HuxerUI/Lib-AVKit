#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include <huxerui/avkit/analysis.h>

namespace avkit_preview {

inline constexpr huxerui::avkit::PixelSize kPreviewStickerSize{320, 180};
inline constexpr std::array kPreviewTrackingParts{huxerui::avkit::analysis::TrackedPart::Face,
                                                  huxerui::avkit::analysis::TrackedPart::LeftHand,
                                                  huxerui::avkit::analysis::TrackedPart::RightHand};

struct PreviewPixel {
  std::uint8_t r, g, b, a;
  bool operator==(const PreviewPixel&) const = default;
};
static_assert(sizeof(PreviewPixel) == 4);

inline PreviewPixel PreviewStickerPixel(int x, int y, bool cue = false) {
  const int dx = x - 196;
  const int dy = y - 80;
  if (cue && std::abs(dx) < 15 && std::abs(dy) < 15) return {16, 250, 32, 255};
  const int distance = dx * dx + dy * dy;
  if (distance < 43 * 43) return {120, 59, 19, 128};
  if (distance < 46 * 46) return {88, 72, 43, 96};
  return {};
}

inline PreviewPixel PreviewTrackingStickerPixel(int x, int y, huxerui::avkit::analysis::TrackedPart part) {
  using enum huxerui::avkit::analysis::TrackedPart;
  const float u = (x + 0.5F) / kPreviewStickerSize.width, v = (y + 0.5F) / kPreviewStickerSize.height;
  if (part == Face) {
    const auto radius = std::hypot((u - (u < 0.5F ? 0.25F : 0.75F)) / 0.20F, (v - 0.5F) / 0.36F);
    if ((radius > 0.65F && radius < 1) || (std::abs(u - 0.5F) < 0.08F && std::abs(v - 0.5F) < 0.07F))
      return {252, 235, 30, 255};
    return {};
  }
  if (std::abs(u - 0.5F) + std::abs(v - 0.5F) > 0.46F) return {};
  return part == LeftHand ? PreviewPixel{35, 244, 255, 255} : PreviewPixel{254, 72, 192, 255};
}

} // namespace avkit_preview
