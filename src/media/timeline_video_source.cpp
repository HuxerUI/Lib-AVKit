#include "timeline_video_source.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "ecs/scene_evaluation.h"
#include "ecs/scene_events.h"
#include "media/video_texture.h"
#include "support/cancellation.h"
#include "support/geometry.h"
#include "support/media_time.h"
#include "support/video_geometry.h"

namespace huxerui::avkit::detail {
namespace {

using namespace media;

TimeRange RangeOf(const MediaPresentationRange& range) { return {range.start, range.end}; }

bool Contains(TimeRange range, MediaTimestamp time) {
  return !Before(time, range.start) && Before(time, range.end);
}

struct VideoFrameCache {
  struct Entry {
    VideoFrame picture;
    std::size_t bytes;
  };

  bool Append(VideoFrame picture) {
    constexpr std::size_t kCpuByteLimit = 64 * 1024 * 1024;
    constexpr std::size_t kCpuFrameLimit = 64, kPlatformFrameLimit = 8;
    const auto storage = picture.Storage();
    const bool cpu = storage == VideoStorage::Cpu;
    const auto limit = cpu ? kCpuFrameLimit : kPlatformFrameLimit;
    std::size_t bytes = 0;
    for (const auto& plane : picture.planes) {
      const auto size = plane.data.AsBytes().size();
      if (size > kCpuByteLimit - bytes) {
        bytes = kCpuByteLimit + 1;
        break;
      }
      bytes += size;
    }
    const auto previous_size = entries.size();
    while (!entries.empty() && (entries.back().picture.Storage() != storage || entries.size() >= limit ||
                                retained_bytes > kCpuByteLimit || bytes > kCpuByteLimit - retained_bytes)) {
      retained_bytes -= entries.front().bytes;
      entries.pop_front();
    }
    const bool evicted = previous_size != entries.size();
    // Own cached CPU planes so reverse playback cannot exhaust a decoder's output pool.
    // A picture larger than the byte budget is kept alone without copying it.
    if (cpu && bytes <= kCpuByteLimit) {
      for (auto& plane : picture.planes) {
        const auto data = plane.data.AsBytes();
        const auto owned = std::make_shared<Bytes>(data.begin(), data.end());
        plane.data = BufferReference(*owned, owned);
      }
    }
    entries.push_back({std::move(picture), bytes});
    retained_bytes += bytes;
    return evicted;
  }

  std::deque<Entry> entries;
  std::size_t retained_bytes{};
};

struct VideoCursor {
  bool CoversReverse(MediaTimestamp source) const {
    return reverse_window && Before(reverse_window->start, source) && !Before(reverse_window->end, source);
  }

  VideoFrame* CachedReverse(MediaTimestamp source) {
    if (!CoversReverse(source)) return nullptr;
    const auto entry = std::ranges::find_if(reverse.entries, [&](const VideoFrameCache::Entry& entry) {
      const auto& picture = entry.picture;
      return Before(picture.pts, source) && !Before(Add(picture.pts, *picture.duration), source);
    });
    return entry == reverse.entries.end() ? nullptr : &entry->picture;
  }

  bool HasReverseCpuFrame(MediaTimestamp source) {
    const auto* frame = CachedReverse(source);
    return frame && frame->Storage() == VideoStorage::Cpu;
  }

  VideoFrame* CachedForward(MediaTimestamp source) {
    if (!forward) return nullptr;
    const auto found = std::ranges::find_if(forward->entries, [&](const VideoFrameCache::Entry& entry) {
      return Contains({entry.picture.pts, Add(entry.picture.pts, *entry.picture.duration)}, source);
    });
    return found == forward->entries.end() ? nullptr : &found->picture;
  }

  bool CanSeekForward(MediaTimestamp source) {
    return CachedForward(source) || (forward && reader && last && !Before(source, *last) &&
                                     !Before({1, 1}, Difference(source, *last)));
  }

  void ReadForward(std::stop_token stop) {
    frame = reader->ReadVideo(stop);
    if (forward && frame) forward->Append(*frame);
  }

  std::unique_ptr<MovieReader> reader;
  std::optional<VideoFrame> frame;
  std::optional<MediaTimestamp> last;
  std::optional<TimeRange> reverse_window;
  VideoFrameCache reverse;
  std::optional<VideoFrameCache> forward;
};

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)

struct VideoPrerollJob {
  struct Result {
    VideoCursor cursor;
    std::vector<CodecFallback> fallbacks;
  };

  const MovieAsset* movie;
  MediaTimestamp trim;
  MediaTimestamp source;
  MediaTimestamp entry;
  CodecPolicy policy;
  bool reverse;
  std::stop_token caller;
  std::stop_source cancel;
  std::optional<Result> result;
};

#endif

struct VideoPlayback {
  VideoClipSource source;
  VideoCursor cursor;
  bool software_video{};
  bool current{};
};

struct VideoPrerollSlot {
  VideoPlayback* clip{};
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
  std::shared_ptr<VideoPrerollJob> job;
#endif
};

} // namespace

struct TimelineVideoSource::Impl {
  Impl(const CodecProviders& codecs, CodecPolicy selection, const OnCodecFallback& fallback,
       render::RenderBackend* backend)
      : providers(codecs), policy(backend ? selection : CodecPolicy::SoftwareOnly), report(fallback), device(backend) {}

  ~Impl() {
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    for (auto& worker : preroll_workers) worker.request_stop();
#endif
  }

  std::unique_ptr<MovieReader> Open(const MovieAsset& movie, MediaTimestamp at, bool software_video) {
    return std::make_unique<MovieReader>(movie, providers, TrackKind::Video, at,
                                         software_video ? CodecPolicy::SoftwareOnly : policy, report);
  }

  void SetSources(std::span<const VideoClipSource> sources, entityx::EventManager& events) {
    for (const auto& source : sources) {
      auto [entry, inserted] = pictures.try_emplace(source.clip, VideoPlayback{source});
      auto& clip = entry->second;
      if (!inserted) {
        const auto& previous = clip.source;
        const bool changed = previous.movie != source.movie || previous.placement.source != source.placement.source;
        const bool entry_changed = changed || previous.placement.timeline != source.placement.timeline ||
                                   previous.visibility.range.start != source.visibility.range.start;
        if (entry_changed) {
          if (auto slot = FindPreroll(&clip); slot != prerolls.end()) CancelPreroll(*slot);
        }
        if (changed) {
          clip.cursor = {};
          clip.software_video = false;
          events.emit(VideoSourceInvalidatedEvent{source.clip, true});
        }
        clip.source = source;
      }
      clip.current = true;
    }
    for (auto entry = pictures.begin(); entry != pictures.end();) {
      auto& clip = entry->second;
      if (std::exchange(clip.current, false)) { ++entry; continue; }
      if (auto slot = FindPreroll(&clip); slot != prerolls.end()) CancelPreroll(*slot);
      events.emit(VideoSourceInvalidatedEvent{entry->first, true});
      entry = pictures.erase(entry);
    }
    CollectPreroll();
  }

  void PrepareFrame(MediaTimestamp at, entityx::EventManager& events) {
    for (auto& slot : prerolls) {
      if (slot.clip && !Before(at, PictureStart(*slot.clip))) CancelPreroll(slot);
    }
    for (auto& [entity, clip] : pictures) {
      if (!NeedsPicture(clip, at) && (clip.cursor.reader || clip.cursor.reverse_window) &&
          FindPreroll(&clip) == prerolls.end()) {
        clip.cursor = {};
        events.emit(VideoSourceInvalidatedEvent{entity});
      }
    }
  }

  const VideoFrame* Sample(entityx::Entity entity, MediaTimestamp source, PixelSize target, std::stop_token stop) {
    const auto clip = pictures.find(entity);
    if (clip == pictures.end()) return nullptr;
    auto* picture = Picture(clip->second, source, stop);
    if (!picture) return nullptr;
    const VideoTextureOptions options{target.width, target.height};
    const auto publish = [&](auto drawable, Affine2D uv) {
      if (!IsFinite(uv)) throw std::invalid_argument("Invalid prepared video UV mapping");
      const bool owns_drawable = !entity.has_component<render::Texture>() && !entity.has_component<render::ImagePixels>();
      entity.assign<SampledVideo>(picture, uv, owns_drawable ? DrawableStorage(drawable) : nullptr);
      if (owns_drawable) entity.assign<std::decay_t<decltype(drawable)>>(std::move(drawable));
    };
    if (picture->Storage() == VideoStorage::Platform) {
      if (!device) throw std::runtime_error("The software decoder must deliver CPU-accessible video frames");
      auto imported = ImportVideoTexture(*device, *picture, options);
      if (imported.Succeeded()) {
        auto prepared = std::move(imported).Value();
        if (!prepared.texture || !prepared.texture.Info().size.width || !prepared.texture.Info().size.height)
          throw std::runtime_error("The imported video texture is empty");
        publish(std::move(prepared.texture), prepared.source_to_texture_uv);
        return picture;
      }
      if (imported.Error().code != render::RenderErrorCode::Unsupported)
        throw std::runtime_error(imported.Error().message);
      picture = RetryPicture(clip->second, *picture, PlatformVideoImportError(imported.Error().message), stop);
    }
    auto pixels = FitCpuVideoFrame(*picture, options);
    if (!pixels.Succeeded()) throw std::runtime_error(pixels.Error().message);
    publish(std::move(pixels).Value(), FitVideoTexture(picture->format, options).SourceToTextureUv(options));
    return picture;
  }

  MediaTimestamp PictureStart(const VideoPlayback& clip) const { return clip.source.visibility.range.start; }

  MediaTimestamp EntrySource(const VideoPlayback& clip) const {
    return SourceTime(clip.source.placement, PictureStart(clip));
  }

  VideoFrame* CachedEntry(VideoPlayback& clip) const {
    const auto source = EntrySource(clip);
    return clip.source.placement.source.reverse ? clip.cursor.CachedReverse(source) : clip.cursor.CachedForward(source);
  }

  bool NeedsPicture(const VideoPlayback& clip, MediaTimestamp at) const {
    return Contains(clip.source.visibility.range, at);
  }

  VideoFrame* Picture(VideoPlayback& clip, MediaTimestamp source, std::stop_token stop) {
    if (clip.source.placement.source.reverse) return ReversePicture(clip, source, stop);
    auto& cursor = clip.cursor;
    if (auto* cached = cursor.CachedForward(source)) return cached;
    if (!cursor.reader || (cursor.last && Before(source, *cursor.last))) {
      cursor = {};
      cursor.reader = Open(*clip.source.movie, source, clip.software_video);
      cursor.ReadForward(stop);
    }
    cursor.last = source;
    while (cursor.frame && !Before(source, Add(cursor.frame->pts, *cursor.frame->duration)))
      cursor.ReadForward(stop);
    return cursor.frame && !Before(source, cursor.frame->pts) ? &*cursor.frame : nullptr;
  }

  VideoCursor ReadReverseWindow(const MovieAsset& movie, MediaTimestamp trim, MediaTimestamp source,
                                CodecPolicy video_policy, const OnCodecFallback& on_fallback, std::stop_token stop) {
    VideoCursor cursor;
    const auto earliest = Difference(source, {1, 4});
    auto begin = std::max(trim, earliest, Before);
    cursor.reader = std::make_unique<MovieReader>(movie, providers, TrackKind::Video, begin, video_policy, on_fallback);
    bool evicted = false;
    while (auto picture = cursor.reader->ReadVideoInternal(stop, trim)) {
      if (!Before(picture->pts, source)) break;
      const auto end = Add(picture->pts, *picture->duration);
      if (cursor.reverse.Append(std::move(*picture))) evicted = true;
      if (!Before(end, source)) break;
    }
    if (!cursor.reverse.entries.empty()) {
      const auto first = cursor.reverse.entries.front().picture.pts;
      begin = std::max(trim, evicted ? first : std::min(begin, first, Before), Before);
    }
    cursor.reverse_window = TimeRange{begin, source};
    return cursor;
  }

  VideoFrame* ReversePicture(VideoPlayback& clip, MediaTimestamp source, std::stop_token stop) {
    auto& cursor = clip.cursor;
    if (!cursor.CoversReverse(source)) {
      cursor = {};
      cursor = ReadReverseWindow(*clip.source.movie, clip.source.placement.source.range.start, source,
                                 clip.software_video ? CodecPolicy::SoftwareOnly : policy, report, stop);
    }
    return cursor.CachedReverse(source);
  }

  std::array<VideoPrerollSlot, kVideoPrerollLimit>::iterator FindPreroll(const VideoPlayback* clip) {
    return std::ranges::find(prerolls, clip, &VideoPrerollSlot::clip);
  }

  void CancelPreroll(VideoPrerollSlot& slot) {
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    if (const auto job = std::exchange(slot.job, {})) {
      job->cancel.request_stop();
      const std::lock_guard lock(preroll_mutex);
      std::erase(queued_preroll, job);
    }
#endif
    slot.clip = nullptr;
  }

  void CollectPreroll() {
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    for (auto& slot : prerolls) {
      if (!slot.job) continue;
      std::optional<VideoPrerollJob::Result> result;
      {
        std::lock_guard lock(preroll_mutex);
        result = std::exchange(slot.job->result, std::nullopt);
      }
      if (!result || slot.job->caller.stop_requested()) continue;
      if (result->cursor.reader) result->cursor.reader->SetCodecFallbackHandler(report);
      slot.clip->cursor = std::move(result->cursor);
      if (report) for (const auto& fallback : result->fallbacks) report(fallback);
    }
#endif
  }

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
  void DecodePreroll(VideoPrerollJob& job, std::stop_token stop) {
    std::stop_callback cancel_worker(stop, [&] { job.cancel.request_stop(); });
    std::stop_callback cancel_caller(job.caller, [&] { job.cancel.request_stop(); });
    const auto token = job.cancel.get_token();
    VideoPrerollJob::Result result;
    try {
      CheckCancelled(token);
      const auto collect_fallback = [&](const auto& fallback) { result.fallbacks.push_back(fallback); };
      auto& cursor = result.cursor;
      if (job.reverse) {
        cursor = ReadReverseWindow(*job.movie, job.trim, job.source, job.policy, collect_fallback, token);
        cursor.reader.reset();
        if (!cursor.CachedReverse(job.source)) return;
      } else {
        cursor.reader = std::make_unique<MovieReader>(*job.movie, providers, TrackKind::Video, job.source,
                                                       job.policy, collect_fallback);
        if (!cursor.reader->AllowsVideoSequenceHandoff()) return;
        cursor.forward.emplace();
        cursor.ReadForward(token);
        cursor.last = job.source;
        if (!cursor.CachedForward(job.source) || !cursor.reader->AllowsVideoSequenceHandoff()) return;
        cursor.reader->SetCodecFallbackHandler({});
      }
    } catch (const std::exception&) {
      // A missed speculative decode is retried by ordinary sampling when the clip becomes visible.
      return;
    }
    std::lock_guard lock(preroll_mutex);
    if (!token.stop_requested()) job.result.emplace(std::move(result));
  }

  void RunPreroll(std::stop_token stop) {
    std::unique_lock lock(preroll_mutex);
    while (preroll_ready.wait(lock, stop, [&] { return !queued_preroll.empty(); }) && !stop.stop_requested()) {
      auto job = std::move(queued_preroll.front());
      queued_preroll.pop_front();
      lock.unlock();
      DecodePreroll(*job, stop);
      job.reset();
      lock.lock();
    }
  }
#endif

  void QueuePreroll(VideoPrerollSlot& slot, std::stop_token stop) {
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    if (slot.job && !slot.job->caller.stop_requested()) return;
    auto& clip = *slot.clip;
#if defined(__EMSCRIPTEN__)
    // Browser video frame handles belong to the JavaScript worker that produced them.
    if (!providers.software) return;
    const auto video_policy = CodecPolicy::SoftwareOnly;
#else
    const auto video_policy = clip.software_video ? CodecPolicy::SoftwareOnly : policy;
#endif
    if (slot.job) CancelPreroll(slot);
    slot.clip = &clip;
    auto& worker = preroll_workers[&slot - prerolls.data()];
    if (!worker.joinable()) worker = std::jthread([this](auto token) { RunPreroll(token); });
    slot.job = std::make_shared<VideoPrerollJob>(VideoPrerollJob{clip.source.movie, clip.source.placement.source.range.start,
        EntrySource(clip), PictureStart(clip), video_policy, clip.source.placement.source.reverse, stop});
    {
      std::lock_guard lock(preroll_mutex);
      const auto position = std::ranges::upper_bound(queued_preroll, slot.job->entry, Before,
          [](const auto& job) { return job->entry; });
      queued_preroll.insert(position, slot.job);
    }
    preroll_ready.notify_one();
#endif
  }

  bool Preroll(MediaTimestamp start, std::span<const entityx::Entity> entries, std::stop_token stop) {
    for (auto& slot : prerolls) {
      if (!slot.clip || std::ranges::find(entries, slot.clip->source.clip) != entries.end()) continue;
      if (!NeedsPicture(*slot.clip, start)) slot.clip->cursor = {};
      CancelPreroll(slot);
    }
    bool ready = !entries.empty();
    for (const auto entity : entries) {
      auto& clip = pictures.at(entity);
      auto slot = FindPreroll(&clip);
      if (slot == prerolls.end()) slot = FindPreroll(nullptr);
      slot->clip = &clip;
      if (CachedEntry(clip)) continue;
      ready = false;
      QueuePreroll(*slot, stop);
    }
    return ready;
  }

  VideoFrame* RetryPicture(VideoPlayback& clip, const VideoFrame& rejected, const PlatformVideoImportError& error,
                           std::stop_token stop) {
    auto& cursor = clip.cursor;
    auto replacement = cursor.reader ? cursor.reader->RetryVideoFrame(rejected, error, stop)
        : MovieReader::RetryDetachedVideoFrame(*clip.source.movie, providers, rejected, error, report, stop);
    clip.software_video = true;
    if (clip.source.placement.source.reverse) {
      cursor.reverse = {};
      cursor.reverse_window = TimeRange{replacement.pts, Add(replacement.pts, *replacement.duration)};
      cursor.reverse.Append(std::move(replacement));
      return &cursor.reverse.entries.front().picture;
    }
    if (cursor.forward) {
      *cursor.forward = {};
      cursor.forward->Append(replacement);
    }
    cursor.last = replacement.pts;
    cursor.frame = std::move(replacement);
    return &*cursor.frame;
  }

  std::vector<VideoFrame> AnalysisImages(const VideoPlayback& clip, const VideoFrame& target,
                                         const analysis::AnalysisWindow& window, std::stop_token stop) {
    std::vector<VideoFrame> images;
    // Model context can cross the clip trim without moving the preview cursor.
    std::unique_ptr<MovieReader> reader;
    std::optional<VideoFrame> next;
    for (auto offset = -int(window.past_frames); offset <= int(window.future_frames); ++offset) {
      if (offset == 0) {
        images.push_back(target);
        continue;
      }
      const auto step = *window.sample_interval;
      if (step.ticks > std::numeric_limits<std::int64_t>::max() / std::abs(offset))
        throw std::overflow_error("Analysis sample interval exceeds the media clock");
      const auto at = Add(target.pts, {step.ticks * offset, step.timescale});
      if (!Contains(RangeOf(clip.source.movie->video_range), at)) continue;
      if (!reader) {
        reader = Open(*clip.source.movie, at, clip.software_video);
        next = reader->ReadVideo(stop);
      }
      while (next && !Before(at, Add(next->pts, *next->duration))) next = reader->ReadVideo(stop);
      if (!next || Before(at, next->pts) || (offset < 0 ? !Before(next->pts, target.pts) : !Before(target.pts, next->pts)))
        continue;
      if (images.empty() || Before(images.back().pts, next->pts)) images.push_back(*next);
    }
    return images;
  }

  std::optional<SourceTracking::Input> TrackingInput(entityx::Entity entity, const VideoFrame& image,
                                                     std::stop_token stop) {
    const auto binding = pictures.find(entity);
    if (binding == pictures.end()) return std::nullopt;
    auto& clip = binding->second;
    return SourceTracking::Input{
      [this, &clip, stop](const auto& target, const auto& window) { return AnalysisImages(clip, target, window, stop); },
      [this, &clip, &image, stop]() -> std::optional<VideoFrame> {
        const auto first = clip.source.placement.source.range.start;
        if (Contains({image.pts, Add(image.pts, *image.duration)}, first)) return std::nullopt;
        return Open(*clip.source.movie, first, clip.software_video)->ReadVideo(stop);
      },
      [this, &clip] {
        const auto trim = clip.source.placement.source.range;
        auto reader = std::shared_ptr<MovieReader>(Open(*clip.source.movie, trim.start, clip.software_video));
        return [reader = std::move(reader), end = trim.end](std::stop_token token) mutable -> std::optional<VideoFrame> {
          if (!reader) return std::nullopt;
          auto frame = reader->ReadVideo(token);
          if (!frame || !Before(Add(frame->pts, *frame->duration), end)) reader.reset();
          return frame;
        };
      },
    };
  }

  void Reset(std::optional<MediaTimestamp> seek, std::span<const entityx::Entity> entries, entityx::EventManager& events) {
    for (auto& slot : prerolls) CancelPreroll(slot);
    for (std::size_t i = 0; i < entries.size(); ++i) prerolls[i].clip = &pictures.at(entries[i]);
    for (auto& [entity, clip] : pictures) {
      if (FindPreroll(&clip) != prerolls.end()) {
        if (clip.cursor.HasReverseCpuFrame(EntrySource(clip))) clip.cursor.reader.reset();
      } else if (seek && NeedsPicture(clip, *seek)) {
        const auto source = SourceTime(clip.source.placement, *seek);
        const auto reuse = clip.source.placement.source.reverse ? clip.cursor.HasReverseCpuFrame(source)
                                                               : clip.cursor.CanSeekForward(source);
        if (!reuse) clip.cursor = {};
      } else {
        clip.cursor = {};
      }
      events.emit(VideoSourceInvalidatedEvent{entity});
    }
  }

  const CodecProviders& providers;
  CodecPolicy policy;
  const OnCodecFallback& report;
  render::RenderBackend* device;
  std::map<entityx::Entity, VideoPlayback> pictures;
  std::array<VideoPrerollSlot, kVideoPrerollLimit> prerolls;
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
  std::deque<std::shared_ptr<VideoPrerollJob>> queued_preroll;
  std::mutex preroll_mutex;
  std::condition_variable_any preroll_ready;
  std::array<std::jthread, kVideoPrerollLimit> preroll_workers;
#endif
};

TimelineVideoSource::TimelineVideoSource(const CodecProviders& providers, CodecPolicy policy,
                                         const OnCodecFallback& report, render::RenderBackend* device)
    : impl_(std::make_unique<Impl>(providers, policy, report, device)) {}

TimelineVideoSource::~TimelineVideoSource() = default;

void TimelineVideoSource::SetSources(std::span<const VideoClipSource> sources, entityx::EventManager& events) {
  impl_->SetSources(sources, events);
}

bool TimelineVideoSource::HasPrepared(entityx::Entity clip) {
  const auto entry = impl_->pictures.find(clip);
  return entry != impl_->pictures.end() && impl_->CachedEntry(entry->second);
}

void TimelineVideoSource::PrepareFrame(MediaTimestamp at, entityx::EventManager& events) {
  impl_->PrepareFrame(at, events);
}

const VideoFrame* TimelineVideoSource::Sample(entityx::Entity clip, MediaTimestamp source,
                                              PixelSize target, std::stop_token stop) {
  return impl_->Sample(clip, source, target, stop);
}

std::optional<SourceTracking::Input> TimelineVideoSource::TrackingInput(entityx::Entity clip, const VideoFrame& image,
                                                                        std::stop_token stop) {
  return impl_->TrackingInput(clip, image, stop);
}

bool TimelineVideoSource::Preroll(MediaTimestamp start, std::span<const entityx::Entity> entries, std::stop_token stop) {
  return impl_->Preroll(start, entries, stop);
}

void TimelineVideoSource::Reset(std::optional<MediaTimestamp> seek, std::span<const entityx::Entity> entries,
                                entityx::EventManager& events) {
  impl_->Reset(seek, entries, events);
}

} // namespace huxerui::avkit::detail
