#pragma once

#include <huxerui/avkit/timeline.h>

namespace huxerui::avkit::detail {

/// @brief Associates a frame layout with its project target in compositing order.
struct TargetLayoutEntry {
  EditTarget target;
  TargetLayout layout;
};

} // namespace huxerui::avkit::detail
