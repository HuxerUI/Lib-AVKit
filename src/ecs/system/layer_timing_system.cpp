#include "layer_timing_system.h"

#include "ecs/scene_evaluation.h"
#include "support/media_time.h"

namespace huxerui::avkit::detail {

void LayerTimingSystem::update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) {
  const auto at = SceneRoot(entities).component<VideoFrameContext>()->time;
  ClearComponents<LayerSampleTime>(entities);
  for (auto layer : entities.entities_with_components<LayerStyle>()) {
    if (!layer.has_component<PagOverlay>() && !layer.has_component<StillImageSource>() &&
        !layer.has_component<render::Texture>() && !layer.has_component<render::ImagePixels>()) continue;
    auto local = at;
    if (const auto attachment = layer.component<ClipAttachment>()) {
      if (!attachment->clip || (!attachment->clip.has_component<SampledVideo>() &&
          !attachment->clip.has_component<ProjectImageSource>())) continue;
      const auto clock = attachment->clip.component<const ClipSampleTime>();
      if (!clock) continue;
      local = clock->elapsed;
    }
    if (const auto range = layer.component<TimeRange>()) {
      if (Before(local, range->start) || !Before(local, range->end)) continue;
      local = Difference(local, range->start);
    }
    layer.assign<LayerSampleTime>(local);
  }
}

} // namespace huxerui::avkit::detail
