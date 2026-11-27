#pragma once

#include <huxerui/avkit/project.h>
#include <huxerui/avkit/scene.h>

namespace huxerui::avkit::detail {

/// @brief Installs the authored components of one canvas overlay onto an ECS entity.
/// @param layer Entity receiving the overlay components.
/// @param overlay Resource, transform, opacity, and replacement settings to project.
/// @param range Optional visibility interval in the owning timeline.
/// @param order Draw order within the owning track or scene.
void ProjectOverlay(entityx::Entity layer, const OverlayInstance& overlay, std::optional<TimeRange> range,
                          std::int32_t order);

/// @brief Projects a validated snapshot into the scene used by the timeline systems.
/// @param scene Destination ECS scene.
/// @param project Revision supplying the clip, overlay, and transition entities.
void ProjectTimeline(RenderScene& scene, const ProjectSnapshot& project);

} // namespace huxerui::avkit::detail
