#pragma once

/// @file
/// @brief Authored landmark bindings shared by effects and analysis.

#include <optional>
#include <string>

namespace huxerui::avkit::analysis {

/// @brief Parts available for sticker anchors, with hand sides from the subject's perspective.
/// Values are persisted in project documents.
enum class TrackedPart { Face = 0, LeftHand = 1, RightHand = 2 };

/// @brief Binds a sticker to semantic landmarks on the selected person.
struct TrackingAnchor {
  /// Detection landmark set used for both points.
  TrackedPart part;
  /// Origin landmark. If axis_point is present, the vector to it gives the direction and pixel scale.
  std::string point;
  /// Optional second landmark within the same part and person.
  std::optional<std::string> axis_point;

  bool operator==(const TrackingAnchor&) const = default;
};

} // namespace huxerui::avkit::analysis
