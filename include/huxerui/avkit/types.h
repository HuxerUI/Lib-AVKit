#pragma once

/// @file
/// @brief Shared AVKit identifiers, canvas geometry, and media intervals.

#include <compare>
#include <cstdint>
#include <optional>
#include <string>

#include <huxerui/geometry.h>
#include <huxerui/media_format.h>

namespace huxerui::avkit {

/// @brief Physical image extent in pixels.
struct PixelSize {
  std::uint32_t width;
  std::uint32_t height;

  bool operator==(const PixelSize&) const = default;
};

/// @brief AVKit affine mapping for evaluated geometry and normalized texture coordinates.
struct Affine2D {
  /// Coefficients of the transformed x-axis (m11, m12) and y-axis (m21, m22).
  float m11{1}, m12{}, m21{}, m22{1};
  /// Offset added after the linear mapping.
  float translate_x{}, translate_y{};

  /// @brief Maps the unit rectangle onto the given bounds.
  /// @param bounds Destination origin and extent.
  static constexpr Affine2D FromRect(Rect bounds) noexcept {
    return {bounds.width, 0, 0, bounds.height, bounds.x, bounds.y};
  }

  bool operator==(const Affine2D&) const = default;

  /// @brief Tests for the exact identity mapping.
  bool IsIdentity() const noexcept { return *this == Affine2D{}; }

  /// @brief Applies the affine mapping to one position.
  /// @param point Position in the input coordinate space.
  Point Apply(Point point) const noexcept {
    return {m11 * point.x + m21 * point.y + translate_x, m12 * point.x + m22 * point.y + translate_y};
  }

  /// @brief Maps a position back into the input coordinate space.
  /// @param point Position in the output coordinate space.
  /// @return The input position, or empty if the determinant is nonfinite or too close to zero.
  std::optional<Point> Inverse(Point point) const noexcept;
};

/// @brief Rotates normalized canvas positions around the destination center in physical canvas space.
/// @param destination Bounds in normalized canvas coordinates. Their center is the pivot.
/// @param canvas Physical extent of the full canvas, used to account for its aspect ratio.
/// @param degrees Angle in degrees, clockwise in a canvas with a downward y-axis.
/// @throws std::invalid_argument If the canvas is empty or the angle is nonfinite.
Affine2D CanvasRotation(Rect destination, PixelSize canvas, float degrees);

/// @brief Fits a source's display aspect into normalized canvas bounds, centered on the bounds.
/// @param bounds Available destination rectangle in normalized canvas coordinates.
/// @param canvas Physical extent of the output canvas.
/// @param source Source display dimensions after accounting for orientation and pixel aspect ratio.
/// @return The fitted rectangle, or the supplied bounds when either extent is invalid.
Rect FitCanvasAspect(Rect bounds, PixelSize canvas, Size source);

/// @brief Half-open media interval [start, end) with exact rational boundaries.
struct TimeRange {
  /// Inclusive start in the owning timeline or source clock.
  media::MediaTimestamp start;
  /// Exclusive end in the same clock. Its time base can differ from the start.
  media::MediaTimestamp end;

  bool operator==(const TimeRange&) const = default;
};

/// @brief Stable project identifier for a timeline track.
struct TrackId {
  /// Persistent project value. Zero denotes an unassigned identifier.
  std::uint64_t value{};

  bool operator==(const TrackId&) const = default;
};
/// @brief Stable project identifier for a placement of an asset track.
struct ClipId {
  /// Persistent project value. Zero denotes an unassigned identifier.
  std::uint64_t value{};

  bool operator==(const ClipId&) const = default;
};
/// @brief Stable project identifier for a sticker or transition instance.
struct InstanceId {
  /// Persistent project value. Zero denotes an unassigned identifier.
  std::uint64_t value{};

  bool operator==(const InstanceId&) const = default;
};

/// @brief Stable project target for timeline selection and preview edits.
class EditTarget {
public:
  /// @brief Identifies a visual or audio clip on the timeline.
  /// @param id Stable ID of the authored clip.
  static constexpr EditTarget ForClip(ClipId id) noexcept { return {Kind::Clip, id.value}; }

  /// @brief Identifies an overlay such as a sticker, text, or subtitle.
  /// @param id Stable ID of the authored overlay instance.
  static constexpr EditTarget ForOverlay(InstanceId id) noexcept { return {Kind::Overlay, id.value}; }

  /// @brief Returns the typed clip ID when this target identifies a clip.
  constexpr std::optional<ClipId> AsClip() const noexcept {
    if (kind_ == Kind::Clip) return ClipId{value_};
    return std::nullopt;
  }

  /// @brief Returns the typed instance ID when this target identifies an overlay.
  constexpr std::optional<InstanceId> AsOverlay() const noexcept {
    if (kind_ == Kind::Overlay) return InstanceId{value_};
    return std::nullopt;
  }

  constexpr bool operator==(const EditTarget&) const noexcept = default;

private:
  enum class Kind { Clip, Overlay };

  constexpr EditTarget(Kind kind, std::uint64_t value) noexcept : kind_(kind), value_(value) {}

  Kind kind_;
  std::uint64_t value_;
};

/// @brief Reference to a resource in the active catalog.
struct ResourceRef {
  /// Logical key unique in the active catalog. Independent of a local file path.
  std::string key;

  auto operator<=>(const ResourceRef&) const = default;
};

} // namespace huxerui::avkit
