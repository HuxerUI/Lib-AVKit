#include "timeline_system.h"

#include "ecs/scene_evaluation.h"

#include <huxerui/avkit/component.h>

#include <algorithm>

#include "support/media_time.h"

namespace huxerui::avkit::detail {
namespace {

using media::MediaTimestamp;

bool Contains(TimeRange range, MediaTimestamp at) {
  return !Before(at, range.start) && Before(at, range.end);
}

} // namespace

void ClipPresentationRangeSystem::update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) {
  for (auto clip : entities.entities_with_components<ClipPresentationRange>())
    if (!clip.has_component<ClipTiming>() || (!clip.has_component<FileVideoClip>() && !clip.has_component<ProjectImageSource>())) clip.remove<ClipPresentationRange>();
  entities.each<ClipTiming, FileVideoClip>([](entityx::Entity clip, const ClipTiming& timing, const FileVideoClip&) {
    clip.replace<ClipPresentationRange>(timing.timeline);
  });
  entities.each<ClipTiming, ProjectImageSource>([](entityx::Entity clip, const ClipTiming& timing, const ProjectImageSource&) {
    clip.replace<ClipPresentationRange>(timing.timeline);
  });
  entities.each<TransitionLink, TimeRange>([](entityx::Entity, const TransitionLink& link, const TimeRange& range) {
    auto from = link.from, to = link.to;
    if (!from || !to || !link.from_root || !link.to_root) return;
    auto outgoing = from.component<ClipPresentationRange>();
    auto incoming = to.component<ClipPresentationRange>();
    if (!outgoing || !incoming) return;
    outgoing->range.end = std::min(outgoing->range.end, range.end, Before);
    incoming->range.start = range.start;
    incoming->starts_in_transition = true;
  });
}

void TimelineSystem::update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) {
  auto root = SceneRoot(entities);
  const auto at = root.component<VideoFrameContext>()->time;
  ClearComponents<ClipSampleTime, ActiveTransition, TransitionGroupRoot>(entities);
  entities.each<ClipTiming, ClipPresentationRange>(
      [&](entityx::Entity clip, const ClipTiming& timing, const ClipPresentationRange& visibility) {
        if (Contains(visibility.range, at))
          clip.assign<ClipSampleTime>(SourceTime(timing, at), Difference(at, timing.timeline.start));
      });
  for (auto entity : entities.entities_with_components<TransitionLink, TimeRange>()) {
    auto& link = *entity.component<TransitionLink>();
    if (!link.from || !link.to || !link.from_root || !link.to_root ||
        !link.from.has_component<ClipPresentationRange>() || !link.to.has_component<ClipPresentationRange>()) continue;
    const auto& range = *entity.component<TimeRange>();
    if (!Contains(range, at)) continue;
    entity.assign<ActiveTransition>(Difference(at, range.start),
                                      Difference(range.end, range.start));
    link.from_root.assign<TransitionGroupRoot>();
    link.to_root.assign<TransitionGroupRoot>();
  }
}

} // namespace huxerui::avkit::detail
