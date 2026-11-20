#pragma once

/// @file
/// @brief Time sampling modes for animated color assets.

namespace huxerui::avkit {

/// @brief Action at the end of a uniformly timed animation.
enum class AnimationPlayback {
  HoldLast, ///< Keeps the final frame for the remaining clip duration.
  Loop,     ///< Restarts on the next animation frame boundary.
};

} // namespace huxerui::avkit
