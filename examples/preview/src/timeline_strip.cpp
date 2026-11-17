#include "timeline_strip.h"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "support/cancellation.h"
#include "support/media_time.h"

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::media;
using huxerui::avkit::detail::Add;
using huxerui::avkit::detail::AtScale;
using huxerui::avkit::detail::Before;
using huxerui::avkit::detail::CheckCancelled;
using huxerui::avkit::detail::Difference;

std::optional<TimeRange> Intersection(TimeRange a, TimeRange b) {
  const auto start = Before(a.start, b.start) ? b.start : a.start;
  const auto end = Before(a.end, b.end) ? a.end : b.end;
  return Before(start, end) ? std::optional{TimeRange{start, end}} : std::nullopt;
}

MediaTimestamp Midpoint(TimeRange range) {
  const auto duration = Difference(range.end, range.start);
  if (duration.ticks % 2 == 0) return Add(range.start, {duration.ticks / 2, duration.timescale});
  if (duration.timescale > std::numeric_limits<std::uint32_t>::max() / 2)
    throw std::overflow_error("Thumbnail midpoint is outside the media clock");
  return Add(range.start, {duration.ticks, duration.timescale * 2});
}

void ReadStrip(AssetSampler& sampler, const ProjectSnapshot& project, StripResult& strip, std::stop_token stop) {
  for (const auto& track : project.tracks) {
    for (const auto& clip : track.clips) {
      CheckCancelled(stop);
      const auto& placement = clip.placement;
      const auto visible = Intersection(placement.timeline, strip.viewport.range);
      if (!visible) continue;
      if (track.kind == TrackKind::Video) {
        ThumbnailRequest request{placement.source.asset, placement.source.track, {}, {96, 54}};
        const auto interval = strip.viewport.thumbnail_step;
        const auto delta = Difference(visible->start, placement.timeline.start);
        const auto nearest = AtScale(delta, interval.timescale);
        const auto floor = nearest.ticks - Before(delta, nearest);
        auto at = Add(placement.timeline.start, {floor / interval.ticks * interval.ticks, interval.timescale});
        const auto first = strip.thumbnails.size();
        for (; Before(at, visible->end);) {
          const auto next = Add(at, interval);
          const auto cell = Intersection({at, next}, placement.timeline).value();
          const auto source = Midpoint(MapClipToSource(placement, cell));
          request.source_times.push_back(source);
          strip.thumbnails.push_back({track.id, clip.id, cell, source, {}});
          at = next;
        }
        auto pictures = sampler.ReadThumbnails(project, request, stop);
        for (std::size_t index = 0; index < pictures.size(); ++index)
          strip.thumbnails[first + index].thumbnail = std::move(pictures[index]);
        continue;
      }

      const auto wave = sampler.ReadWaveform(project, {placement.source.asset, placement.source.track,
          MapClipToSource(placement, *visible), strip.viewport.bucket_step}, stop);
      auto at = wave.start;
      const auto first = strip.waveform.size();
      for (std::size_t index = 0; index < wave.peaks.size(); index += wave.channels) {
        const auto next = Add(at, wave.bucket_interval);
        if (const auto source = Intersection({at, next}, placement.source.range)) {
          if (const auto overlap = Intersection(MapClipToProject(placement, *source), *visible)) {
            strip.waveform.push_back({track.id, clip.id, *overlap, *source,
                {wave.peaks.begin() + index, wave.peaks.begin() + index + wave.channels}});
          }
        }
        at = next;
      }
      if (placement.source.reverse) std::reverse(strip.waveform.begin() + first, strip.waveform.end());
    }
  }
}

} // namespace

struct StripWorker::Impl {
  struct Work {
    std::uint64_t generation;
    std::shared_ptr<const ProjectSnapshot> project;
    StripViewport viewport;
    Publish publish;
  };

  Impl(MovieCodecProviders providers, EncodeThumbnail encoder)
      : sampler(std::move(providers), std::move(encoder), CodecPolicy::SoftwareOnly),
        thread([this](std::stop_token stop) { Run(stop); }) {}

  ~Impl() {
    thread.request_stop();
    {
      std::scoped_lock lock(mutex);
      active.request_stop();
      pending.reset();
    }
    changed.notify_all();
  }

  void Run(std::stop_token stop) {
    for (;;) {
      std::optional<Work> work;
      std::stop_token request;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stop.stop_requested() || pending.has_value(); });
        if (stop.stop_requested()) return;
        work = std::move(pending);
        pending.reset();
        active = std::stop_source{};
        request = active.get_token();
        ++stats.started;
      }

      auto result = std::make_shared<StripResult>(StripResult{work->generation, work->project->revision, work->viewport});
      try { ReadStrip(sampler, *work->project, *result, request); }
      catch (const std::exception& error) {
        result->error = error.what();
        result->thumbnails.clear();
        result->waveform.clear();
      }
      {
        std::scoped_lock lock(mutex);
        if (work->generation != generation || request.stop_requested()) {
          ++stats.cancelled;
          continue;
        }
        ++stats.delivered;
      }
      if (work->publish) work->publish(std::move(result));
    }
  }

  AssetSampler sampler;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::optional<Work> pending;
  std::stop_source active;
  std::uint64_t generation{};
  StripWorkerStats stats;
  std::jthread thread;
};

StripWorker::StripWorker(MovieCodecProviders providers, EncodeThumbnail encoder)
    : impl_(std::make_unique<Impl>(std::move(providers), std::move(encoder))) {}
StripWorker::~StripWorker() = default;

std::uint64_t StripWorker::Schedule(std::shared_ptr<const ProjectSnapshot> project, StripViewport viewport, Publish publish) {
  if (!project || !Before(viewport.range.start, viewport.range.end) || !Before({}, viewport.thumbnail_step) || !Before({}, viewport.bucket_step))
    throw std::invalid_argument("Timeline strip needs a visible interval and positive sampling steps");
  std::scoped_lock lock(impl_->mutex);
  impl_->active.request_stop();
  impl_->pending = Impl::Work{++impl_->generation, std::move(project), viewport, std::move(publish)};
  impl_->changed.notify_one();
  return impl_->generation;
}

void StripWorker::Cancel(std::uint64_t generation) {
  std::scoped_lock lock(impl_->mutex);
  if (generation != impl_->generation) return;
  ++impl_->generation;
  impl_->pending.reset();
  impl_->active.request_stop();
}

StripWorkerStats StripWorker::Stats() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->stats;
}

StripViewport VisibleStrip(const ProjectSnapshot& project, std::int64_t left, unsigned zoom) {
  zoom = std::min(zoom, 2u);
  const auto end = project.Range().end;
  const auto ticks = AtScale(end, kStripClock).ticks;
  const auto length = std::int64_t(4 * kStripClock >> zoom);
  left = std::clamp(left, std::int64_t{}, std::max<std::int64_t>(0, ticks - length));
  const auto right = MediaTimestamp{left + length, kStripClock};
  return {{{left, kStripClock}, Before(end, right) ? end : right}, {1, 2u << zoom}, {1, 25u << zoom}};
}

} // namespace avkit_preview
