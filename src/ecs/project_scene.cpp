#include "project_scene.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "support/media_time.h"
#include "project/project_internal.h"

#include <huxerui/avkit/timeline.h>

namespace huxerui::avkit::detail {
namespace {

class ProjectScene {
public:
  explicit ProjectScene(RenderScene& scene) : scene(scene), entities(scene.Entities()) {}

  void Populate(const ProjectSnapshot& project) {
    bool inset = false;
    for (const auto& track : project.tracks) {
      if (track.kind != media::TrackKind::Video) continue;
      for (const auto& clip : track.clips) {
        const bool match_source = inset && clip.destination != Rect{0, 0, 1, 1};
        AddVideo(clip, detail::FindAsset(project, clip.placement.source.asset), match_source);
      }
      inset = true;
    }
    for (const auto& track : project.tracks) {
      if (track.kind != media::TrackKind::Audio) continue;
      for (const auto& clip : track.clips) AddAudio(clip);
    }
    for (const auto& transition : project.transitions) AddTransition(transition);
    for (const auto& overlay : project.overlays) {
      auto layer = entities.create();
      ProjectOverlay(layer, overlay, overlay.range, TakeOrder());
    }
  }

private:
  std::int32_t TakeOrder() {
    if (next_order == std::numeric_limits<std::int32_t>::max()) throw std::length_error("Too many visual layers");
    return next_order++;
  }

  entityx::Entity AddLayer(Rect destination, render::Blend blend, float rotation_degrees = 0) {
    auto layer = entities.create();
    layer.assign<Transform2D>(Transform2D{{destination.x, destination.y},
                                               {destination.width, destination.height}, rotation_degrees});
    layer.assign<LayerStyle>(TakeOrder(), blend);
    return layer;
  }

  void AddVideo(const TimelineClip& clip, const AssetSource& asset, bool match_source) {
    const auto blend = asset.still_image ? render::Blend::PremultipliedOver : render::Blend::Replace;
    auto layer = AddLayer(clip.destination, blend, clip.rotation_degrees);
    layer.assign<ClipId>(clip.id);
    layer.assign<ClipTiming>(clip.placement);
    if (clip.voice_effect) layer.assign<VoiceEffect>(*clip.voice_effect);
    layer.assign<VideoLayout>(clip.fill, match_source);
    if (asset.still_image)
      layer.assign<ProjectImageSource>(clip.placement.source.asset, *asset.tracks.front().display_size);
    else
      layer.assign<FileVideoClip>();
    if (const auto& background = clip.background) {
      layer.assign<BackgroundReplacement>(background->color);
      std::visit([&](const auto& mask) { layer.assign<std::decay_t<decltype(mask)>>(mask); }, background->mask);
    }
    if (clip.source_crop) layer.assign<SourceCrop>(*clip.source_crop);
    for (const auto& instance : clip.stickers) {
      auto entity = entities.create();
      const auto range = instance.range == TimeRange{} ? std::nullopt : std::optional{instance.range};
      ProjectOverlay(entity, instance, range, TakeOrder());
      entityx::Entity anchor = entityx::Entity{};
      if (instance.anchor) {
        anchor = entities.create();
        scene.SetSpatialParent(anchor, layer);
        entity.assign<TrackedSticker>(anchor, *instance.anchor);
      }
      scene.SetSpatialParent(entity, anchor ? anchor : layer);
      entity.assign<ClipAttachment>(layer);
      if (const auto& depth = instance.depth) entity.assign<StickerDepth>(*depth);
    }
  }

  entityx::Entity FindClip(ClipId id) {
    for (auto clip : entities.entities_with_components<ClipId>()) {
      if (*clip.component<ClipId>() == id) return clip;
    }
    throw std::logic_error("Missing projected clip entity");
  }

  void AddTransition(const TransitionInstance& transition) {
    auto from = FindClip(transition.from), to = FindClip(transition.to);
    const auto transition_root = [&](entityx::Entity clip) {
      if (const auto parent = clip.component<SpatialParent>()) return parent->entity;
      const auto root = entities.create();
      scene.SetSpatialParent(clip, root);
      return root;
    };
    const auto from_root = transition_root(from), to_root = transition_root(to);
    auto layer = AddLayer({0, 0, 1, 1}, render::Blend::PremultipliedOver);
    layer.assign<TransitionLink>(from, to, from_root, to_root);
    layer.assign<TimeRange>(transition.spec.range);
    std::visit([&](const auto& effect) { layer.assign<std::decay_t<decltype(effect)>>(effect); }, transition.spec.effect);
    layer.assign<InstanceId>(transition.id);
  }

  void AddAudio(const TimelineClip& clip) {
    const auto& source = clip.placement.source;
    entityx::Entity entity;
    media::MediaTimestamp offset;
    if (clip.audio_of) {
      entity = FindClip(*clip.audio_of);
      offset = Difference(source.range.start, entity.component<ClipTiming>()->source.range.start);
    } else {
      entity = entities.create();
      entity.assign<ClipId>(clip.id);
      entity.assign<ClipTiming>(clip.placement);
      if (clip.voice_effect) entity.assign<VoiceEffect>(*clip.voice_effect);
    }
    entity.assign<AudioSource>(source.asset, source.track, offset);
    entity.assign<AudioMix>(clip.audio_mix);
  }

  RenderScene& scene;
  entityx::EntityManager& entities;
  std::int32_t next_order{};
};

} // namespace

void ProjectOverlay(entityx::Entity layer, const OverlayInstance& overlay, std::optional<TimeRange> range,
                          std::int32_t order) {
  const auto& rect = overlay.destination;
  layer.replace<Transform2D>(Transform2D{{rect.x, rect.y}, {rect.width, rect.height}, overlay.rotation_degrees});
  layer.replace<LayerStyle>(order, render::Blend::PremultipliedOver, overlay.opacity);
  if (range) layer.replace<TimeRange>(*range);
  else if (layer.has_component<TimeRange>()) layer.remove<TimeRange>();
  layer.replace<InstanceId>(overlay.id);
  if (const auto* pag = std::get_if<PagOverlay>(&overlay.content)) {
    if (layer.has_component<StillImageSource>()) layer.remove<StillImageSource>();
    layer.replace<PagOverlay>(*pag);
  } else {
    if (layer.has_component<PagOverlay>()) layer.remove<PagOverlay>();
    layer.replace<StillImageSource>(std::get<StillImageSource>(overlay.content));
  }
}

void ProjectTimeline(RenderScene& scene, const ProjectSnapshot& project) {
  ProjectScene(scene).Populate(project);
}

} // namespace huxerui::avkit::detail
