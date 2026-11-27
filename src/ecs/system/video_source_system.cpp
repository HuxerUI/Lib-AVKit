#include "video_source_system.h"

#include <algorithm>
#include <array>

#include "ecs/scene_evaluation.h"
#include "media/timeline_video_source.h"
#include "support/media_time.h"

namespace huxerui::avkit::detail {
namespace {

std::vector<VideoClipSource> CollectSources(entityx::EntityManager& entities, const TimelineServices& sources) {
  std::vector<VideoClipSource> result;
  entities.each<FileVideoClip, ClipTiming, ClipPresentationRange>([&](entityx::Entity clip, const FileVideoClip&,
      const ClipTiming& timing, const ClipPresentationRange& visibility) {
    const auto& track = timing.source;
    const auto& movie = sources.movie(track.asset);
    ValidateMediaTrack(track, movie, media::TrackKind::Video);
    result.push_back({clip, &movie, timing, visibility});
  });
  return result;
}

bool CanPreroll(const VideoClipSource& clip, std::span<const VideoClipSource> sources) {
  if (clip.placement.source.reverse) return true;
  const auto at = clip.visibility.range.start;
  if (clip.visibility.starts_in_transition || !Before(clip.movie->video_range.start, SourceTime(clip.placement, at)))
    return false;
  return std::ranges::any_of(sources, [&](const VideoClipSource& other) {
    return Before(other.visibility.range.start, at) && Before(at, other.visibility.range.end);
  });
}

} // namespace

void VideoSourceSystem::update(entityx::EntityManager& entities, entityx::EventManager& events, entityx::TimeDelta) {
  auto root = SceneRoot(entities);
  const auto sources = root.component<const TimelineServices>();
  auto& video = sources->video;
  const auto clips = CollectSources(entities, *sources);
  video.SetSources(clips, events);
  const auto evaluation = root.component<const TimelineEvaluation>();
  const auto& frame = *root.component<const VideoFrameContext>();
  video.PrepareFrame(frame.time, events);
  entities.each<ClipSampleTime, FileVideoClip>([&](entityx::Entity clip, const ClipSampleTime& active, const FileVideoClip&) {
    if (const auto* picture = video.Sample(clip, active.source, frame.target.size, evaluation->stop);
        picture && evaluation->on_decode) evaluation->on_decode(*picture);
  });
}

void VideoPrerollSystem::update(entityx::EntityManager& entities, entityx::EventManager& events, entityx::TimeDelta) {
  auto root = SceneRoot(entities);
  const auto sources = root.component<const TimelineServices>();
  auto& video = sources->video;
  const auto clips = CollectSources(entities, *sources);
  video.SetSources(clips, events);
  auto request = root.component<VideoPrerollRequest>();
  std::array<const VideoClipSource*, kVideoPrerollLimit> entries{};
  for (const auto& clip : clips) {
    const auto start = clip.visibility.range.start;
    if (!Before(request->lookahead.start, start) || !Before(start, request->lookahead.end)) continue;
    if (request->seek ? !video.HasPrepared(clip.clip) : !CanPreroll(clip, clips)) continue;
    const auto slot = std::ranges::find_if(entries, [&](const auto* entry) {
      return !entry || Before(start, entry->visibility.range.start);
    });
    if (slot == entries.end()) continue;
    std::move_backward(slot, entries.end() - 1, entries.end());
    *slot = &clip;
  }
  std::array<entityx::Entity, kVideoPrerollLimit> selected;
  std::size_t count = 0;
  for (const auto* entry : entries) {
    if (!entry) break;
    selected[count++] = entry->clip;
  }
  const auto eligible = std::span(selected).first(count);
  if (request->seek) video.Reset(request->lookahead.start, eligible, events);
  else request->ready = video.Preroll(request->lookahead.start, eligible, request->stop);
}

} // namespace huxerui::avkit::detail
