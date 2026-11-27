#include "coverage_system.h"

#include <huxerui/avkit/component.h>

#include <set>
#include <stdexcept>

#include "ecs/scene_evaluation.h"
#include "ecs/scene_events.h"
#include "ecs/system/resource_system.h"
#include "render/backend.h"
#include "support/geometry.h"
#include "support/media_time.h"

namespace huxerui::avkit::detail {

void CoverageSystem::configure(entityx::EventManager& events) {
  events.subscribe<VideoSourceInvalidatedEvent>(*this);
}

void CoverageSystem::receive(const VideoSourceInvalidatedEvent& event) {
  if (const auto mask = masks_.find(event.clip); mask != masks_.end()) mask->second.source.Reset();
  if (const auto depth = depths_.find(event.clip); depth != depths_.end()) depth->second.Reset();
}

void CoverageSystem::update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) {
  auto root = SceneRoot(entities);
  const auto evaluation = root.component<const TimelineEvaluation>();
  const auto& catalog = SceneCatalog(root);
  const auto provider = SceneAnalysisProvider(root);
  auto& resources = *root.component<ImageResources>();
  ClearComponents<render::BackgroundMask, SampledStickerDepth>(entities);
  for (auto entity : entities.entities_with_components<BackgroundReplacement>()) {
    const int sources = entity.has_component<PortraitMask>() + entity.has_component<StaticMask>() +
                        entity.has_component<AnimatedMask>();
    if (sources != 1) throw std::invalid_argument("Background replacement requires exactly one mask source");
  }

  const auto stop = evaluation->stop;
  const auto budget = evaluation->analysis_budget;
  const auto* device = root.component<const TimelineServices>()->device;
  const auto can_use = [&](const render::PixelInput& input) {
    const auto* texture = std::get_if<render::Texture>(&input);
    if (!texture || !device || device->CanSample(*texture)) return true;
    if (budget) return false;
    throw std::runtime_error("The analysis texture cannot be sampled on the render device");
  };
  std::set<entityx::Entity> depth_sources;
  for (auto sticker : entities.entities_with_components<ClipAttachment, StickerDepth>()) {
    const auto clip = sticker.component<ClipAttachment>()->clip;
    if (clip && clip.has_component<ClipId>()) depth_sources.insert(clip);
  }
  std::erase_if(depths_, [&](const auto& entry) { return !depth_sources.contains(entry.first); });
  std::erase_if(masks_, [](const auto& entry) {
    const auto clip = entry.first;
    if (!clip) return true;
    const auto id = clip.template component<const ClipId>();
    const auto portrait = clip.template component<const PortraitMask>();
    return !id || *id != entry.second.id || !clip.template has_component<BackgroundReplacement>() ||
           !portrait || portrait->model_id != entry.second.model_id;
  });
  std::vector<std::pair<entityx::Entity, std::unique_ptr<SourceMask::Request>>> masks;
  std::vector<std::pair<entityx::Entity, std::unique_ptr<SourceDepth::Request>>> depths;
  for (auto entity : entities.entities_with_components<SampledVideo, ClipId, ClipSourceMapping>()) {
    const auto& sample = *entity.component<SampledVideo>();
    if (!sample.picture) continue;
    const auto id = *entity.component<ClipId>();
    if (const auto portrait = entity.component<const PortraitMask>();
        portrait && entity.has_component<BackgroundReplacement>() && entity.has_component<render::SourceSampling>()) {
      const auto& model_id = portrait->model_id;
      auto mask = masks_.find(entity);
      if (mask == masks_.end()) {
        mask = masks_.emplace(entity, PortraitMaskSession{id, model_id, SourceMask(provider, id.value, model_id)}).first;
      }
      masks.emplace_back(entity, mask->second.source.Submit(*sample.picture, stop, budget));
    }
    if (depth_sources.contains(entity)) {
      const auto depth = depths_.try_emplace(entity, provider, id.value).first;
      depths.emplace_back(entity, depth->second.Submit(*sample.picture, stop, budget));
    }
  }

  for (auto& [entity, request] : masks) {
    if (auto mask = masks_.at(entity).source.Collect(std::move(request)); mask && can_use(mask->input)) {
      const auto& input = *entity.component<ClipSourceMapping>();
      const auto& background = *entity.component<const BackgroundReplacement>();
      const auto style = entity.component<const LayerStyle>();
      entity.assign<render::BackgroundMask>(std::move(mask->input),
          Compose(mask->source_to_texture_uv, input.draw_to_source), mask->valid_uv, background.color, false,
          style ? std::optional{style->blend} : std::nullopt);
    }
  }

  for (auto& [clip, request] : depths) {
    auto depth = depths_.at(clip).Collect(std::move(request));
    if (!depth || !can_use(depth->input)) continue;
    const auto clip_to_depth = Compose(depth->source_to_texture_uv, clip.component<ClipSourceMapping>()->draw_to_source);
    entities.each<ClipAttachment, StickerDepth>([&](entityx::Entity entity, const ClipAttachment& attachment,
                                                    const StickerDepth& spec) {
      if (attachment.clip == clip)
        entity.assign<SampledStickerDepth>(clip, depth->input, clip_to_depth, depth->valid_uv, spec.meters);
    });
  }

  const auto sample_packaged = [](entityx::Entity entity, render::PixelInput image, bool inverted) {
    const auto& background = *entity.component<const BackgroundReplacement>();
    const auto& mapping = *entity.component<const ClipSourceMapping>();
    entity.assign<render::BackgroundMask>(std::move(image), mapping.draw_to_source, Rect{0, 0, 1, 1},
                                          background.color, inverted, render::Blend::PremultipliedOver);
  };
  for (auto entity : entities.entities_with_components<BackgroundReplacement, StaticMask, render::SourceSampling,
                                                       ClipSourceMapping, ClipSampleTime>()) {
    const auto& mask = *entity.component<const StaticMask>();
    sample_packaged(entity, resources.LoadMask(catalog, mask.image), mask.inverted);
  }
  for (auto entity : entities.entities_with_components<BackgroundReplacement, AnimatedMask, render::SourceSampling,
                                                       ClipSourceMapping, ClipSampleTime>()) {
    const auto& mask = *entity.component<const AnimatedMask>();
    const auto elapsed = entity.component<const ClipSampleTime>()->elapsed;
    const auto& sequence = catalog.Get<MaskSequenceData>(mask.resource);
    const auto index = ClipMaskFrameIndex(elapsed, sequence.frame_interval, static_cast<std::uint32_t>(sequence.frames.size()),
                                         mask.playback);
    sample_packaged(entity, resources.LoadMaskFrame(catalog, mask.resource, index), mask.inverted);
  }
}

} // namespace huxerui::avkit::detail
