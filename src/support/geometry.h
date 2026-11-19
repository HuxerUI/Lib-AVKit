#pragma once

#include <cmath>

#include <huxerui/geometry.h>

namespace huxerui::avkit::detail {

inline bool IsUnitRect(const Rect& rect) {
  return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 &&
         rect.x + rect.width <= 1 && rect.y + rect.height <= 1;
}

inline bool IsFinite(const Transform2D& transform) {
  return std::isfinite(transform.m11) && std::isfinite(transform.m12) &&
         std::isfinite(transform.m21) && std::isfinite(transform.m22) &&
         std::isfinite(transform.translate_x) && std::isfinite(transform.translate_y);
}

inline Transform2D Compose(const Transform2D& outer, const Transform2D& inner) {
  const auto origin = outer.Apply({inner.translate_x, inner.translate_y});
  return {outer.m11 * inner.m11 + outer.m21 * inner.m12, outer.m12 * inner.m11 + outer.m22 * inner.m12,
          outer.m11 * inner.m21 + outer.m21 * inner.m22, outer.m12 * inner.m21 + outer.m22 * inner.m22,
          origin.x, origin.y};
}

} // namespace huxerui::avkit::detail
