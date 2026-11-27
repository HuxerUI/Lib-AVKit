#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <huxerui/avkit/component.h>

namespace huxerui::avkit::detail {

inline bool IsUnitRect(const Rect& rect) {
  return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 &&
         rect.x + rect.width <= 1 && rect.y + rect.height <= 1;
}

inline bool IsFinite(const Affine2D& transform) {
  return std::isfinite(transform.m11) && std::isfinite(transform.m12) &&
         std::isfinite(transform.m21) && std::isfinite(transform.m22) &&
         std::isfinite(transform.translate_x) && std::isfinite(transform.translate_y);
}

inline bool IsInvertible(const Affine2D& transform) {
  return IsFinite(transform) &&
      double(transform.m11) * transform.m22 != double(transform.m12) * transform.m21;
}

inline Affine2D Compose(const Affine2D& outer, const Affine2D& inner) {
  const auto origin = outer.Apply({inner.translate_x, inner.translate_y});
  return {outer.m11 * inner.m11 + outer.m21 * inner.m12, outer.m12 * inner.m11 + outer.m22 * inner.m12,
          outer.m11 * inner.m21 + outer.m21 * inner.m22, outer.m12 * inner.m21 + outer.m22 * inner.m22,
          origin.x, origin.y};
}

inline Affine2D UvRect(Rect rect) {
  return {rect.width, 0, 0, rect.height, rect.x, rect.y};
}

inline Affine2D LocalMatrix(const Transform2D& placement, PixelSize canvas) {
  const Rect bounds{placement.translation.x, placement.translation.y, placement.scale.width, placement.scale.height};
  if (placement.rotation_degrees == 0) return UvRect(bounds);
  const Point center{bounds.x + bounds.width * placement.pivot.x, bounds.y + bounds.height * placement.pivot.y};
  return Compose(CanvasRotation({center.x, center.y, 0, 0}, canvas, placement.rotation_degrees), UvRect(bounds));
}

inline Rect TransformBounds(Rect rect, const Affine2D& transform) {
  const auto first = transform.Apply({rect.x, rect.y});
  const auto second = transform.Apply({rect.x + rect.width, rect.y});
  const auto third = transform.Apply({rect.x, rect.y + rect.height});
  const auto fourth = transform.Apply({rect.x + rect.width, rect.y + rect.height});
  const auto [left, right] = std::minmax({first.x, second.x, third.x, fourth.x});
  const auto [top, bottom] = std::minmax({first.y, second.y, third.y, fourth.y});
  return {left, top, right - left, bottom - top};
}

inline Affine2D InverseAffine(const Affine2D& transform) {
  const auto determinant = double(transform.m11) * transform.m22 - double(transform.m12) * transform.m21;
  if (!std::isfinite(determinant) || determinant == 0)
    throw std::invalid_argument("Expected an invertible affine mapping");
  Affine2D inverse{float(transform.m22 / determinant), float(-transform.m12 / determinant),
                   float(-transform.m21 / determinant), float(transform.m11 / determinant)};
  const auto origin = inverse.Apply({-transform.translate_x, -transform.translate_y});
  inverse.translate_x = origin.x;
  inverse.translate_y = origin.y;
  return inverse;
}

} // namespace huxerui::avkit::detail
