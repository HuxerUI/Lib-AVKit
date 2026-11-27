#pragma once

#include <entityx/Entity.h>

namespace huxerui::avkit::detail {

/// The clip's video sampling position changed or its cursor was retired.
struct VideoSourceInvalidatedEvent {
  entityx::Entity clip;
  bool source_changed{};
};

} // namespace huxerui::avkit::detail
