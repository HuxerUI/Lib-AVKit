#include "transition_system.h"

#include "ecs/scene_evaluation.h"
#include "ecs/system/resource_system.h"

#include <huxerui/avkit/component.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "support/media_time.h"

namespace huxerui::avkit::detail {
namespace {

using media::MediaTimestamp;

float FeatherAt(const MaskTransition& spec, MediaTimestamp offset) {
  const auto& keys = spec.feather_keys;
  if (keys.empty()) return spec.feather;
  const auto upper = std::ranges::upper_bound(keys, offset, Before, &Keyframe<float>::time);
  if (upper == keys.begin()) return upper->value;
  const auto& lower = *(upper - 1);
  if (upper == keys.end() || lower.interpolation == Interpolation::Hold) return lower.value;
  const auto elapsed = Difference(offset, lower.time);
  const auto interval = Difference(upper->time, lower.time);
  const auto fraction = (double(elapsed.ticks) * interval.timescale) / (double(interval.ticks) * elapsed.timescale);
  return std::lerp(lower.value, upper->value, static_cast<float>(fraction));
}

} // namespace

void TransitionSystem::update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) {
  auto root = SceneRoot(entities);
  auto& resources = *root.component<ImageResources>();
  const auto& catalog = SceneCatalog(root);
  ClearComponents<render::MaskTransitionSample, render::PagTransitionSample>(entities);
  const auto has_picture = [](entityx::Entity clip) {
    return clip.has_component<SampledVideo>() ||
           (clip.has_component<ClipSampleTime>() && clip.has_component<ProjectImageSource>());
  };
  for (auto entity : entities.entities_with_components<ActiveTransition, TransitionLink, InstanceId>()) {
    const auto& active = *entity.component<ActiveTransition>();
    auto& link = *entity.component<TransitionLink>();
    if (!link.from || !link.to || !link.from_root || !link.to_root) {
      if (link.from_root && link.from_root.has_component<TransitionGroupRoot>()) link.from_root.remove<TransitionGroupRoot>();
      if (link.to_root && link.to_root.has_component<TransitionGroupRoot>()) link.to_root.remove<TransitionGroupRoot>();
      continue;
    }
    entity.component<LayerStyle>()->order = link.from.component<const LayerStyle>()->order;
    if (!has_picture(link.from) || !has_picture(link.to))
      throw std::runtime_error("Transition requires both source pictures");
    if (const auto pag = entity.component<PagTransitionSource>()) {
      const auto& asset = catalog.Get<PagAsset>(pag->resource);
      if (!asset.transition_slots) throw std::invalid_argument("A PAG transition needs two named image slots");
      entity.assign<render::PagTransitionSample>(pag->resource, *entity.component<InstanceId>(),
                                                 active.elapsed, active.duration);
      continue;
    }
    const auto& spec = *entity.component<MaskTransition>();
    const auto& sequence = catalog.Get<MaskSequenceData>(spec.resource);
    const auto index = UniformFrameIndex(active.elapsed, active.duration, static_cast<std::uint32_t>(sequence.frames.size()));
    const auto& bounds = *link.from.component<Transform2D>();
    const Rect region{bounds.translation.x, bounds.translation.y, bounds.scale.width, bounds.scale.height};
    entity.assign<render::MaskTransitionSample>(resources.LoadMaskFrame(catalog, spec.resource, index),
                                                FeatherAt(spec, active.elapsed), spec.inverted, region);
  }
}

} // namespace huxerui::avkit::detail
