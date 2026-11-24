#include <huxerui/avkit/timeline.h>
#include <huxerui/avkit_ecs.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

#include "analysis/source_texture.h"
#include "analysis/tracking.h"
#include "project/project_internal.h"
#include "support/cancellation.h"
#include "support/geometry.h"
#include "support/media_time.h"
#include "support/video_geometry.h"

namespace huxerui::avkit {
namespace {

using namespace media;
using detail::Add;
using detail::Before;
using detail::CheckCancelled;
using detail::Difference;
using detail::ExactTicks;

constexpr std::size_t kVideoPrerollLimit = 2;

TimeRange RangeOf(const MediaPresentationRange& range) { return {range.start, range.end}; }

bool Contains(TimeRange range, MediaTimestamp time) {
  return !Before(time, range.start) && Before(time, range.end);
}

MediaTimestamp SourceTime(const ClipPlacement& clip, MediaTimestamp time) {
  const auto offset = Difference(time, clip.timeline.start);
  return clip.source.reverse ? Difference(clip.source.range.end, offset) : Add(clip.source.range.start, offset);
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

struct AudioCursor {
  std::unique_ptr<MovieReader> reader;
  std::optional<AudioFrame> frame;
  std::int64_t next{};
};

struct StickerLayer {
  const StickerSpec* spec;
  flecs::entity entity;
  flecs::entity anchor;
  InstanceId instance;
};

struct VideoClip {
  const ClipInfo* clip;
  const MovieAsset* movie;
  flecs::entity layer;
  std::vector<StickerLayer> stickers;
  VideoCursor cursor;
  bool visible{};
  const TransitionInfo* incoming{};
  const TransitionInfo* outgoing{};
  flecs::entity subcomposition_root;
  std::optional<std::uint32_t> subcomposition;
  std::unique_ptr<detail::SourceTracking> tracking;
  bool software_video{};
  std::unique_ptr<detail::SourceMask> mask;
  std::unique_ptr<detail::SourceDepth> depth;
};

struct VideoPrerollSlot {
  VideoClip* clip{};
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
  std::shared_ptr<VideoPrerollJob> job;
#endif
};

struct TransitionLayer {
  const TransitionInfo* transition;
  VideoClip* from;
  VideoClip* to;
  flecs::entity layer;
  bool visible{};
};

struct AudioClip {
  const MovieAsset* movie;
  std::int64_t start, end, source;
  AudioCursor cursor;
  bool reverse{};
  std::int64_t reverse_start{};
  std::vector<std::int16_t> reverse_pcm;
  float gain{1.0F};
  std::int64_t fade_in{}, fade_out{};

  double Volume(std::int64_t frame) const {
    const auto in = fade_in ? std::min(1.0, double(frame - start) / fade_in) : 1.0;
    const auto out = fade_out ? std::min(1.0, double(end - frame - 1) / fade_out) : 1.0;
    return gain * std::min(in, out);
  }
};

MediaTimestamp PictureStart(const VideoClip& clip) {
  return clip.incoming ? clip.incoming->spec.range.start : clip.clip->placement.timeline.start;
}

MediaTimestamp EntrySource(const VideoClip& clip) {
  return SourceTime(clip.clip->placement, PictureStart(clip));
}

VideoFrame* CachedEntry(VideoClip& clip) {
  const auto source = EntrySource(clip);
  return clip.clip->placement.source.reverse ? clip.cursor.CachedReverse(source) : clip.cursor.CachedForward(source);
}

bool NeedsPicture(const VideoClip& clip, MediaTimestamp at) {
  return !Before(at, PictureStart(clip)) && Before(at, clip.clip->placement.timeline.end) &&
         (!clip.outgoing || Before(at, clip.outgoing->spec.range.end));
}

float FeatherAt(const TransitionSpec& spec, MediaTimestamp offset) {
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

Transform2D UvRect(Rect rect) {
  return {rect.width, 0, 0, rect.height, rect.x, rect.y};
}

Rect TransformBounds(Rect rect, const Transform2D& transform) {
  const auto first = transform.Apply({rect.x, rect.y});
  const auto second = transform.Apply({rect.x + rect.width, rect.y});
  const auto third = transform.Apply({rect.x, rect.y + rect.height});
  const auto fourth = transform.Apply({rect.x + rect.width, rect.y + rect.height});
  const auto [left, right] = std::minmax({first.x, second.x, third.x, fourth.x});
  const auto [top, bottom] = std::minmax({first.y, second.y, third.y, fourth.y});
  return {left, top, right - left, bottom - top};
}

Transform2D DrawToSource(const BackgroundReplacement& effect) {
  auto transform = UvRect(effect.source_crop);
  if (effect.mirrored) {
    transform.translate_x += transform.m11;
    transform.m11 = -transform.m11;
  }
  return transform;
}

Transform2D InvertUv(const Transform2D& uv) {
  const auto determinant = double(uv.m11) * uv.m22 - double(uv.m12) * uv.m21;
  if (!std::isfinite(determinant) || determinant == 0)
    throw std::invalid_argument("Depth occlusion requires an invertible video UV mapping");
  Transform2D inverse{float(uv.m22 / determinant), float(-uv.m12 / determinant),
                      float(-uv.m21 / determinant), float(uv.m11 / determinant)};
  const auto origin = inverse.Apply({-uv.translate_x, -uv.translate_y});
  inverse.translate_x = origin.x;
  inverse.translate_y = origin.y;
  return inverse;
}

std::optional<Transform2D> AnchorTransform(const analysis::TrackingAnchor& anchor, const analysis::PersonSample& person,
                                           const Transform2D& source_to_clip, PixelSize canvas) {
  const auto& part = anchor.part == analysis::TrackedPart::Face ? person.face
                     : anchor.part == analysis::TrackedPart::LeftHand ? person.left_hand : person.right_hand;
  if (!part || part->observed != person.target) return std::nullopt;
  const auto origin = detail::FindLandmark(*part, anchor.point);
  if (!origin) return std::nullopt;
  const auto position = source_to_clip.Apply(*origin);
  if (!anchor.axis_point)
    return Transform2D{std::hypot(source_to_clip.m11, source_to_clip.m12), 0, 0,
                       std::hypot(source_to_clip.m21, source_to_clip.m22), position.x, position.y};
  const auto axis = detail::FindLandmark(*part, *anchor.axis_point);
  if (!axis || *axis == *origin) return std::nullopt;
  const auto dx = axis->x - origin->x, dy = axis->y - origin->y;
  const auto x = dx * source_to_clip.m11 + dy * source_to_clip.m21;
  const auto y = dx * source_to_clip.m12 + dy * source_to_clip.m22;
  return Transform2D{x, y, -y * canvas.height / canvas.width, x * canvas.width / canvas.height, position.x, position.y};
}

} // namespace

std::uint32_t TransitionMaskSample::FrameIndex(std::uint32_t frame_count) const {
  return detail::UniformFrameIndex(offset, duration, frame_count);
}

std::uint32_t AnimatedMaskSample::FrameIndex(MediaTimestamp frame_interval, std::uint32_t frame_count) const {
  if (!frame_count || !offset.timescale || !frame_interval.timescale || offset.ticks < 0 || frame_interval.ticks <= 0 ||
      (playback != MaskPlayback::HoldLast && playback != MaskPlayback::Loop))
    throw std::invalid_argument("Invalid clip mask sequence sample");
  const auto common = std::lcm<std::uint64_t>(offset.timescale, frame_interval.timescale);
  if (common > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("Incompatible mask sequence clocks");
  const auto scale = static_cast<std::uint32_t>(common);
  const auto index = ExactTicks(offset, scale) / ExactTicks(frame_interval, scale);
  return static_cast<std::uint32_t>(playback == MaskPlayback::Loop ? index % frame_count :
                                    std::min(index, std::int64_t(frame_count - 1)));
}

AssetReference InspectTimelineMovie(const File& file) {
  const auto movie = ImportMovie(file);
  const auto& desc = movie.video.descriptions.front();
  auto width = desc.visible ? desc.visible->width : desc.coded_width.value_or(0);
  auto height = desc.visible ? desc.visible->height : desc.coded_height.value_or(0);
  if (const auto& aspect = desc.pixel_aspect_ratio; aspect && aspect->height) {
    const auto oriented = std::llround(double(width) * aspect->width / aspect->height);
    if (oriented > 0 && oriented <= std::numeric_limits<std::uint32_t>::max()) width = static_cast<std::uint32_t>(oriented);
  }
  if (movie.video.rotation == VideoRotation::Clockwise90 || movie.video.rotation == VideoRotation::Clockwise270)
    std::swap(width, height);
  const auto size = width && height ? std::optional{PixelSize{width, height}} : std::nullopt;
  AssetReference result{movie.file, {{movie.video.id, TrackKind::Video, RangeOf(movie.video_range), size}}};
  if (movie.HasAudio()) result.tracks.push_back({movie.audio.id, TrackKind::Audio, RangeOf(movie.audio_range)});
  return result;
}

AssetReference InspectTimelineAudio(const File& file) {
  const auto movie = ImportMovieAudio(file);
  return {movie.file, {{movie.audio.id, TrackKind::Audio, RangeOf(movie.audio_range)}}};
}

struct TimelineSession::Impl {
  Impl(std::shared_ptr<const ProjectSnapshot> snapshot, MovieCodecProviders codecs, CodecPolicy selection,
       OnCodecFallback fallback, std::shared_ptr<analysis::AnalysisProvider> analyzer)
      : project(std::move(snapshot)), providers(std::move(codecs)), policy(selection), report(std::move(fallback)),
        analysis_provider(std::move(analyzer)) {
    if (!project) throw std::invalid_argument("Timeline sampling requires a project revision");
    detail::ValidateProject(*project);
    range = project->Range();
    for (const auto& track : project->tracks) {
      for (const auto& clip : track.clips) {
        const auto& source = clip.placement.source;
        const auto& movie = Movie(source.asset);
        const bool video = track.kind == TrackKind::Video;
        const auto& actual = video ? movie.video : movie.audio;
        const auto& available = video ? movie.video_range : movie.audio_range;
        if (source.track != actual.id || Before(source.range.start, available.start) ||
            Before(available.end, source.range.end))
          throw std::runtime_error("Relink timeline source with changed tracks or duration: " + movie.file.Path());
        if (video) AddVideo(clip, movie);
        else AddAudio(clip, movie);
      }
    }
    for (const auto& transition : project->transitions) AddTransition(transition);
    for (const auto& overlay : project->overlays) {
      auto layer = AddLayer(overlay.destination, render::Blend::PremultipliedOver);
      layer.get_mut<CompositeLayer>().opacity = overlay.opacity;
      if (overlay.rotation_degrees) layer.set<Transform2D>(CanvasRotation(overlay.destination, project->canvas, overlay.rotation_degrees));
      overlays.push_back(layer);
    }
  }

  ~Impl() {
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    for (auto& worker : preroll_workers) worker.request_stop();
#endif
  }

  const MovieAsset& Movie(AssetId id) {
    const auto found = movies.find(id.value);
    if (found != movies.end()) return found->second;
    const auto& asset = detail::FindAsset(*project, id).source;
    const auto& file = asset.file;
    if (!file.IsFile()) throw std::runtime_error("Missing timeline asset: " + file.Path());
    const bool video = std::ranges::any_of(asset.tracks, [](const auto& track) { return track.kind == TrackKind::Video; });
    return movies.emplace(id.value, video ? ImportMovie(file) : ImportMovieAudio(file)).first->second;
  }

  flecs::entity AddLayer(Rect destination, render::Blend blend) {
    if (next_order == std::numeric_limits<std::int32_t>::max()) throw std::length_error("Too many visual layers");
    return scene.World().entity()
        .set<LocalTransform2D>({{destination.x, destination.y}, {destination.width, destination.height}})
        .set<CompositeLayer>({0, next_order++, blend})
        .disable();
  }

  void AddVideo(const ClipInfo& clip, const MovieAsset& movie) {
    const bool packaged_mask = clip.background && (clip.background->static_mask || clip.background->animated_mask);
    const auto blend = packaged_mask ? render::Blend::PremultipliedOver : render::Blend::Replace;
    auto& node = pictures.emplace_back(VideoClip{&clip, &movie, AddLayer(clip.destination, blend)});
    if (clip.rotation_degrees) node.layer.set<Transform2D>(CanvasRotation(clip.destination, project->canvas, clip.rotation_degrees));
    if (clip.background && !packaged_mask)
      node.mask = std::make_unique<detail::SourceMask>(analysis_provider, clip.id.value, clip.background->model_id);
    if (std::ranges::any_of(clip.stickers, [](const auto& sticker) { return sticker.spec.depth_meters.has_value(); }))
      node.depth = std::make_unique<detail::SourceDepth>(analysis_provider, clip.id.value);
    std::vector<analysis::TrackedPart> parts;
    for (const auto& instance : clip.stickers) {
      auto entity = AddLayer(instance.spec.destination, render::Blend::PremultipliedOver);
      flecs::entity anchor;
      if (instance.spec.anchor) {
        parts.push_back(instance.spec.anchor->part);
        anchor = scene.World().entity().child_of(node.layer);
        scene.SetSpatialParent(anchor, node.layer);
      }
      scene.SetSpatialParent(entity, anchor ? anchor : node.layer);
      node.stickers.push_back({&instance.spec, entity, anchor, instance.id});
    }
    if (!parts.empty()) node.tracking = std::make_unique<detail::SourceTracking>(analysis_provider, clip.id.value, parts);
  }

  void AddTransition(const TransitionInfo& transition) {
    const auto find = [&](ClipId id) {
      return std::ranges::find_if(pictures, [&](const VideoClip& clip) { return clip.clip->id == id; });
    };
    auto& from = *find(transition.from);
    auto& to = *find(transition.to);
    from.outgoing = &transition;
    to.incoming = &transition;
    for (auto* clip : {&from, &to}) {
      if (clip->subcomposition_root) continue;
      clip->subcomposition_root = scene.World().entity();
      scene.SetSpatialParent(clip->layer, clip->subcomposition_root);
    }
    auto layer = AddLayer({0, 0, 1, 1}, render::Blend::PremultipliedOver);
    layer.get_mut<CompositeLayer>().order = from.layer.get<CompositeLayer>().order;
    transitions.push_back({&transition, &from, &to, layer});
  }

  void AddAudio(const ClipInfo& clip, const MovieAsset& movie) {
    if (sounds.empty()) pcm = movie.pcm;
    if (movie.pcm != pcm) throw std::invalid_argument("Timeline audio clips require matching PCM rates and layouts");
    const auto& placement = clip.placement;
    (void)ExactTicks(placement.source.range.end, pcm.sample_rate);
    sounds.push_back({&movie, ExactTicks(placement.timeline.start, pcm.sample_rate),
                      ExactTicks(placement.timeline.end, pcm.sample_rate),
                      ExactTicks(placement.source.range.start, pcm.sample_rate), {}, placement.source.reverse});
    auto& audio = sounds.back();
    audio.gain = clip.audio_mix.muted ? 0.0F : clip.audio_mix.gain;
    audio.fade_in = detail::AtScale(clip.audio_mix.fade_in, pcm.sample_rate).ticks;
    audio.fade_out = detail::AtScale(clip.audio_mix.fade_out, pcm.sample_rate).ticks;
  }

  std::unique_ptr<MovieReader> Open(const MovieAsset& movie, TrackKind track, MediaTimestamp at,
                                    bool software_video = false) {
    return std::make_unique<MovieReader>(movie, providers, track, at,
                                         software_video ? CodecPolicy::SoftwareOnly : policy, report);
  }

  VideoFrame* Picture(VideoClip& clip, MediaTimestamp at, std::stop_token stop) {
    const auto source = SourceTime(clip.clip->placement, at);
    if (clip.clip->placement.source.reverse) return ReversePicture(clip, source, stop);
    auto& cursor = clip.cursor;
    if (auto* cached = cursor.CachedForward(source)) return cached;
    if (!cursor.reader || (cursor.last && Before(source, *cursor.last))) {
      cursor = {};
      cursor.reader = Open(*clip.movie, TrackKind::Video, source, clip.software_video);
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

  VideoFrame* ReversePicture(VideoClip& clip, MediaTimestamp source, std::stop_token stop) {
    auto& cursor = clip.cursor;
    if (!cursor.CoversReverse(source)) {
      cursor = {};
      cursor = ReadReverseWindow(*clip.movie, clip.clip->placement.source.range.start, source,
                                 clip.software_video ? CodecPolicy::SoftwareOnly : policy, report, stop);
    }
    return cursor.CachedReverse(source);
  }

  std::array<VideoClip*, kVideoPrerollLimit> NextVideoEntries(TimeRange lookahead, const auto& eligible) {
    std::array<VideoClip*, kVideoPrerollLimit> entries{};
    for (auto& clip : pictures) {
      const auto start = PictureStart(clip);
      if (!Before(lookahead.start, start) || !Before(start, lookahead.end) || !eligible(clip)) continue;
      const auto position = std::ranges::find_if(entries, [&](const auto* entry) {
        return !entry || Before(start, PictureStart(*entry));
      });
      if (position == entries.end()) continue;
      std::move_backward(position, entries.end() - 1, entries.end());
      *position = &clip;
    }
    return entries;
  }

  bool CanPrerollEntry(const VideoClip& clip) {
    if (clip.clip->placement.source.reverse) return true;
    if (clip.incoming || !Before(clip.movie->video_range.start, EntrySource(clip))) return false;
    const auto at = PictureStart(clip);
    return std::ranges::any_of(pictures, [&](const VideoClip& other) {
      return Before(PictureStart(other), at) && NeedsPicture(other, at);
    });
  }

  auto FindPreroll(const VideoClip* clip) {
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
    slot.job = std::make_shared<VideoPrerollJob>(VideoPrerollJob{clip.movie, clip.clip->placement.source.range.start,
        EntrySource(clip), PictureStart(clip), video_policy, clip.clip->placement.source.reverse, stop});
    {
      std::lock_guard lock(preroll_mutex);
      const auto position = std::ranges::upper_bound(queued_preroll, slot.job->entry, Before,
          [](const auto& job) { return job->entry; });
      queued_preroll.insert(position, slot.job);
    }
    preroll_ready.notify_one();
#endif
  }

  bool PrerollVideoEntries(TimeRange lookahead, std::stop_token stop) {
    CheckCancelled(stop);
    if (!Contains(range, lookahead.start) || !Before(lookahead.start, lookahead.end) || Before(range.end, lookahead.end))
      throw std::out_of_range("Video preroll interval is outside the project interval");
    CollectPreroll();
    const auto entries = NextVideoEntries(lookahead, [&](const VideoClip& clip) { return CanPrerollEntry(clip); });
    for (auto& slot : prerolls) {
      if (!slot.clip || std::ranges::find(entries, slot.clip) != entries.end()) continue;
      if (!NeedsPicture(*slot.clip, lookahead.start)) slot.clip->cursor = {};
      CancelPreroll(slot);
    }
    bool ready = entries.front() != nullptr;
    for (auto* clip : entries) {
      if (!clip) break;
      auto slot = FindPreroll(clip);
      if (slot == prerolls.end()) slot = FindPreroll(nullptr);
      slot->clip = clip;
      if (CachedEntry(*clip)) continue;
      ready = false;
      QueuePreroll(*slot, stop);
    }
    return ready;
  }

  render::ImportedVideoFrame PreparePicture(VideoClip& clip, VideoFrame*& picture, const ImportVideoFrame& import,
                                             std::stop_token stop) {
    try {
      return import(*picture);
    } catch (const PlatformVideoImportError& error) {
      auto replacement = clip.cursor.reader ? clip.cursor.reader->RetryVideoFrame(*picture, error, stop)
          : MovieReader::RetryDetachedVideoFrame(*clip.movie, providers, *picture, error, report, stop);
      clip.software_video = true;
      if (clip.clip->placement.source.reverse) {
        auto& cursor = clip.cursor;
        cursor.reverse = {};
        cursor.reverse_window = TimeRange{replacement.pts, Add(replacement.pts, *replacement.duration)};
        cursor.reverse.Append(std::move(replacement));
        picture = &cursor.reverse.entries.front().picture;
      } else {
        auto& cursor = clip.cursor;
        if (cursor.forward) {
          *cursor.forward = {};
          cursor.forward->Append(replacement);
        }
        cursor.last = replacement.pts;
        cursor.frame = std::move(replacement);
        picture = &*cursor.frame;
      }
    }
    return import(*picture);
  }

  std::vector<VideoFrame> AnalysisImages(const VideoClip& clip, const VideoFrame& target,
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
      if (!Contains(RangeOf(clip.movie->video_range), at)) continue;
      if (!reader) {
        reader = Open(*clip.movie, TrackKind::Video, at, clip.software_video);
        next = reader->ReadVideo(stop);
      }
      while (next && !Before(at, Add(next->pts, *next->duration))) next = reader->ReadVideo(stop);
      if (!next || Before(at, next->pts) || (offset < 0 ? !Before(next->pts, target.pts) : !Before(target.pts, next->pts)))
        continue;
      if (images.empty() || Before(images.back().pts, next->pts)) images.push_back(*next);
    }
    return images;
  }

  TimelineFrame SampleVideo(MediaTimestamp at, render::PixelFormat output, std::stop_token stop,
                            const ImportVideoFrame& import, std::optional<std::chrono::milliseconds> analysis_wait) {
    last_video_plan.reset();
    CheckCancelled(stop);
    if (!Contains(range, at)) throw std::out_of_range("Sample time is outside the project interval");
    TimelineFrame frame{at, {}, {}};
    CollectPreroll();
    for (auto& slot : prerolls) {
      if (slot.clip && !Before(at, PictureStart(*slot.clip))) CancelPreroll(slot);
    }
    for (auto& clip : pictures) {
      if ((clip.cursor.reader || clip.cursor.reverse_window) && !NeedsPicture(clip, at) && FindPreroll(&clip) == prerolls.end()) {
        clip.cursor = {};
        if (clip.tracking) clip.tracking->Reset();
        if (clip.mask) clip.mask->Reset();
        if (clip.depth) clip.depth->Reset();
      }
      clip.subcomposition.reset();
    }
    std::uint32_t subcomposition_count = 0;
    for (std::size_t index = 0; index < overlays.size(); ++index) {
      const auto& overlay = project->overlays[index];
      auto layer = overlays[index];
      const bool visible = Contains(overlay.range, at);
      layer.add_if(!visible, flecs::Disabled);
      if (!visible) continue;
      layer.get_mut<CompositeLayer>().input_slot = static_cast<std::uint32_t>(frame.inputs.size());
      frame.inputs.emplace_back(CanvasOverlaySample{overlay, Difference(at, overlay.range.start)});
    }
    for (auto& node : transitions) {
      const bool active = Contains(node.transition->spec.range, at);
      if (active != node.visible) {
        node.visible = active;
        node.layer.add_if(!active, flecs::Disabled);
      }
      if (active) {
        node.from->subcomposition = subcomposition_count++;
        node.to->subcomposition = subcomposition_count++;
      }
    }
    struct ClipSample {
      VideoClip& clip;
      const VideoFrame& image;
      Transform2D draw_to_source;
      render::SourceSampling sampling;
      std::unique_ptr<detail::SourceMask::Request> request;
      std::unique_ptr<detail::SourceDepth::Request> depth_request;
    };
    std::vector<ClipSample> samples;
    const auto bind_sticker = [&](const StickerLayer& sticker, const VideoClip& clip) {
      std::optional<AnimatedStickerSample> animated;
      if (const auto playback = sticker.spec->playback) {
        const auto elapsed = Difference(at, clip.clip->placement.timeline.start);
        animated = AnimatedStickerSample{sticker.spec->resource, elapsed, *playback,
                                         sticker.instance, sticker.spec->replacements};
      }
      const auto found = std::ranges::find_if(frame.inputs, [&](const TimelineInput& input) {
        if (animated) {
          const auto* sample = std::get_if<AnimatedStickerSample>(&input);
          return sample && *sample == *animated;
        }
        const auto* resource = std::get_if<ResourceRef>(&input);
        return resource && *resource == sticker.spec->resource;
      });
      sticker.entity.get_mut<CompositeLayer>().input_slot = static_cast<std::uint32_t>(found - frame.inputs.begin());
      if (found != frame.inputs.end()) return;
      if (animated) frame.inputs.emplace_back(std::move(*animated));
      else frame.inputs.emplace_back(sticker.spec->resource);
    };
    for (auto& clip : pictures) {
      if (clip.subcomposition) clip.subcomposition_root.set<SubcompositionRoot>({*clip.subcomposition});
      else if (clip.subcomposition_root && clip.subcomposition_root.has<SubcompositionRoot>())
        clip.subcomposition_root.remove<SubcompositionRoot>();
      auto* image = NeedsPicture(clip, at) ? Picture(clip, at, stop) : nullptr;
      const bool visible = image != nullptr;
      if (clip.visible != visible) {
        clip.visible = visible;
        clip.layer.add_if(!clip.visible, flecs::Disabled);
        if (!visible) {
          for (const auto& sticker : clip.stickers) sticker.entity.disable();
        }
      }
      if (!image) continue;
      clip.layer.get_mut<CompositeLayer>().input_slot = static_cast<std::uint32_t>(frame.inputs.size());
      Transform2D content;
      if (import) {
        auto prepared = PreparePicture(clip, image, import, stop);
        content = prepared.source_to_texture_uv;
        if (!detail::IsFinite(content)) throw std::invalid_argument("Invalid imported video texture UV mapping");
        frame.inputs.emplace_back(std::move(prepared.surface));
      } else {
        frame.inputs.emplace_back(*image);
        if (clip.clip->background || clip.tracking || clip.depth || clip.clip->fill) {
          const VideoTextureOptions options{project->canvas.width, project->canvas.height, ImageFit::Contain};
          content = detail::FitVideoTexture(image->format, options).SourceToTextureUv(options);
        }
      }
      const analysis::PersonSample* primary = nullptr;
      auto source_to_clip = content;
      std::optional<Transform2D> cover;
      if (clip.clip->fill) {
        const auto& dest = clip.clip->destination;
        const auto dimension = [](std::uint32_t canvas, float fraction) {
          return static_cast<std::uint32_t>(std::clamp(double(canvas) * fraction, 1.0,
                                                     double(std::numeric_limits<std::uint32_t>::max())));
        };
        const VideoTextureOptions options{
            dimension(project->canvas.width, dest.width), dimension(project->canvas.height, dest.height), ImageFit::Cover};
        cover = detail::FitVideoTexture(image->format, options).SourceToTextureUv(options);
      }
      if (clip.clip->background) {
        const auto& effect = *clip.clip->background;
        auto draw_to_source = DrawToSource(effect);
        if (cover) draw_to_source = detail::Compose(draw_to_source, InvertUv(*cover));
        source_to_clip = InvertUv(draw_to_source);
        samples.push_back({clip, *image, draw_to_source, {detail::Compose(content, draw_to_source)}});
      } else if (cover) {
        const auto draw_to_source = InvertUv(*cover);
        source_to_clip = *cover;
        samples.push_back({clip, *image, draw_to_source, {detail::Compose(content, draw_to_source)}});
      } else if (clip.depth) {
        samples.push_back({clip, *image, InvertUv(content), {}});
      }
      if (clip.tracking) {
        const auto& tracking = clip.tracking->Sample(*image, [&](const auto& target, const auto& window) {
          return AnalysisImages(clip, target, window, stop);
        }, [&]() -> std::optional<VideoFrame> {
          const auto first = clip.clip->placement.source.range.start;
          if (Contains({image->pts, Add(image->pts, *image->duration)}, first)) return std::nullopt;
          return Open(*clip.movie, TrackKind::Video, first, clip.software_video)->ReadVideo(stop);
        }, [&] {
          const auto trim = clip.clip->placement.source.range;
          auto reader = std::shared_ptr<MovieReader>(Open(*clip.movie, TrackKind::Video, trim.start, clip.software_video));
          return [reader = std::move(reader), end = trim.end](std::stop_token token) mutable -> std::optional<VideoFrame> {
            if (!reader) return std::nullopt;
            auto frame = reader->ReadVideo(token);
            if (!frame || !Before(Add(frame->pts, *frame->duration), end)) reader.reset();
            return frame;
          };
        }, stop);
        if (tracking.primary) {
          const auto person = std::ranges::find(tracking.people, *tracking.primary, &analysis::PersonSample::person);
          if (person != tracking.people.end()) {
            primary = &*person;
          }
        }
      }
      for (const auto& sticker : clip.stickers) {
        if (sticker.anchor) {
          const auto transform = primary ? AnchorTransform(*sticker.spec->anchor, *primary, source_to_clip, project->canvas)
                                         : std::nullopt;
          if (!transform) {
            sticker.entity.disable();
            continue;
          }
          sticker.anchor.set<Transform2D>(*transform);
        }
        sticker.entity.enable();
        if (!sticker.spec->depth_meters) bind_sticker(sticker, clip);
      }
    }
    for (auto& item : samples) {
      if (item.clip.mask) item.request = item.clip.mask->Submit(item.image, stop, analysis_wait);
      if (item.clip.depth) item.depth_request = item.clip.depth->Submit(item.image, stop, analysis_wait);
    }
    for (auto& item : samples) {
      if (!item.clip.mask) continue;
      if (auto mask = item.clip.mask->Collect(std::move(item.request))) {
        item.sampling.background = render::BackgroundMask{static_cast<std::uint32_t>(frame.analysis_masks.size()),
            detail::Compose(mask->source_to_texture_uv, item.draw_to_source), mask->valid_uv,
            item.clip.clip->background->color};
        frame.analysis_masks.push_back(std::move(mask->surface));
      }
    }
    for (auto& item : samples) {
      if (!item.clip.depth) continue;
      auto depth = item.clip.depth->Collect(std::move(item.depth_request));
      const auto& destination = item.clip.clip->destination;
      const auto slot = static_cast<std::uint32_t>(frame.analysis_masks.size() + frame.depth_maps.size());
      if (depth) {
        const auto rotation = CanvasRotation(destination, project->canvas, item.clip.clip->rotation_degrees);
        const auto target_to_source = detail::Compose(item.draw_to_source,
            InvertUv(detail::Compose(rotation, UvRect(destination))));
        const auto target_to_depth = detail::Compose(depth->source_to_texture_uv, target_to_source);
        frame.depth_maps.push_back(std::move(depth->surface));
        for (const auto& sticker : item.clip.stickers) {
          if (!sticker.spec->depth_meters || sticker.entity.has(flecs::Disabled)) continue;
          sticker.entity.set<render::DepthOcclusion>({slot, target_to_depth, depth->valid_uv, TransformBounds(destination, rotation),
                                                      *sticker.spec->depth_meters});
          bind_sticker(sticker, item.clip);
        }
      } else {
        for (const auto& sticker : item.clip.stickers)
          if (sticker.spec->depth_meters) sticker.entity.disable();
      }
    }
    const auto first_static = frame.analysis_masks.size() + frame.depth_maps.size();
    const auto bind_packaged = [](ClipSample& item, auto& samples, const auto& sample, std::size_t first, bool inverted) {
      const auto found = std::ranges::find(samples, sample);
      const auto slot = static_cast<std::uint32_t>(first + (found - samples.begin()));
      if (found == samples.end()) samples.push_back(sample);
      item.sampling.background = render::BackgroundMask{slot, item.draw_to_source, {0, 0, 1, 1},
                                                        item.clip.clip->background->color, inverted};
    };
    for (auto& item : samples) {
      if (!item.clip.clip->background) continue;
      if (const auto& mask = item.clip.clip->background->static_mask)
        bind_packaged(item, frame.static_masks, mask->resource, first_static, mask->inverted);
    }
    const auto first_animation = first_static + frame.static_masks.size();
    for (auto& item : samples) {
      const auto& background = item.clip.clip->background;
      if (background && background->animated_mask) {
        const auto& mask = background->animated_mask;
        const auto offset = Difference(at, item.clip.clip->placement.timeline.start);
        const AnimatedMaskSample sample{mask->resource, offset, mask->playback};
        bind_packaged(item, frame.animated_masks, sample, first_animation, mask->inverted);
      }
      if (background || item.clip.clip->fill)
        item.clip.layer.set<render::SourceSampling>(std::move(item.sampling));
    }
    for (auto& node : transitions) {
      const auto& spec = node.transition->spec;
      if (!node.visible) continue;
      if (!node.from->visible || !node.to->visible) throw std::runtime_error("Transition requires both source pictures");
      const auto offset = Difference(at, spec.range.start);
      const auto duration = Difference(spec.range.end, spec.range.start);
      const auto first = static_cast<std::uint32_t>(frame.inputs.size());
      node.layer.get_mut<CompositeLayer>().input_slot = first + *node.from->subcomposition;
      if (const auto& animation = spec.animation) {
        frame.transition_animations.push_back({*animation, node.transition->id, offset, duration,
                                              *node.from->subcomposition, *node.to->subcomposition});
        continue;
      }
      const auto mask_slot = static_cast<std::uint32_t>(first_animation + frame.animated_masks.size() + frame.masks.size());
      frame.masks.push_back({spec.mask, offset, duration});
      node.layer.set<render::MaskedBlend>({first + *node.to->subcomposition, mask_slot, FeatherAt(spec, offset),
                                          spec.inverted, node.from->clip->destination});
    }
    CheckCancelled(stop);
    frame.plan = scene.Evaluate(at, {project->canvas, output}, subcomposition_count);
    last_video_plan = frame.plan;
    last_video_time = at;
    return frame;
  }

  std::shared_ptr<const render::RenderPlan> RecomposeVideo(const TimelineFrame& frame, const VisualTransformOverride& change) {
    if (!frame.plan || frame.plan != last_video_plan.lock() || frame.pts != last_video_time)
      throw std::invalid_argument("Recomposition requires the most recently sampled video frame");
    if (!detail::IsFinite(change.transform) || !change.transform.Inverse(Point{}))
      throw std::invalid_argument("Recomposition requires an invertible finite transform");
    flecs::entity layer;
    std::vector<std::pair<flecs::entity, render::DepthOcclusion>> depth;
    if (const auto* id = std::get_if<ClipId>(&change.target)) {
      const auto clip = std::ranges::find_if(pictures, [&](const VideoClip& entry) { return entry.clip->id == *id; });
      if (clip != pictures.end() && clip->visible) {
        layer = clip->layer;
        for (const auto& sticker : clip->stickers)
          if (const auto* value = sticker.entity.try_get<render::DepthOcclusion>()) depth.emplace_back(sticker.entity, *value);
      }
    } else {
      const auto instance = std::get<InstanceId>(change.target);
      const auto overlay = std::ranges::find(project->overlays, instance, &CanvasOverlay::id);
      if (overlay != project->overlays.end() && Contains(overlay->range, frame.pts))
        layer = overlays[overlay - project->overlays.begin()];
    }
    if (!layer || change.transform.IsIdentity()) return frame.plan;
    const auto* current = layer.try_get<Transform2D>();
    const auto original = current ? std::optional(*current) : std::nullopt;
    const auto restore = [&] {
      if (original) layer.set<Transform2D>(*original);
      else layer.remove<Transform2D>();
      for (const auto& [entity, value] : depth) entity.set<render::DepthOcclusion>(value);
    };
    try {
      layer.set<Transform2D>(original ? detail::Compose(change.transform, *original) : change.transform);
      for (const auto& [entity, value] : depth) {
        auto transformed = value;
        transformed.target_to_depth_uv = detail::Compose(value.target_to_depth_uv, InvertUv(change.transform));
        transformed.target_region = TransformBounds(value.target_region, change.transform);
        entity.set<render::DepthOcclusion>(transformed);
      }
      auto plan = scene.Evaluate(frame.pts, frame.plan->target, frame.plan->subcompositions.size());
      restore();
      return plan;
    } catch (...) {
      restore();
      throw;
    }
  }

  template<class Consume>
  void ReadPcm(const MovieAsset& movie, AudioCursor& cursor, std::int64_t source, std::int64_t count, Consume consume) {
    const auto channels = pcm.ChannelCount();
    if (!cursor.reader) cursor.reader = Open(movie, TrackKind::Audio, {source, pcm.sample_rate});
    for (auto remaining = count; remaining > 0;) {
      if (!cursor.frame || source - cursor.frame->pts.ticks >= cursor.frame->sample_count)
        cursor.frame = cursor.reader->ReadAudio();
      if (!cursor.frame) throw std::runtime_error("Timeline audio ended before its selected source range");
      const auto& frame = *cursor.frame;
      const auto offset = source - frame.pts.ticks;
      if (offset < 0 || offset >= frame.sample_count) throw std::runtime_error("Noncontiguous timeline audio");
      const auto take = std::min<std::int64_t>(remaining, frame.sample_count - offset);
      const auto bytes = frame.planes.front().AsBytes().subspan(std::size_t(offset) * channels * sizeof(std::int16_t),
                                                                std::size_t(take) * channels * sizeof(std::int16_t));
      consume(bytes);
      source += take;
      remaining -= take;
    }
    cursor.next = source;
  }

  void Mix(AudioClip& clip, std::int64_t from, std::int64_t to, std::span<std::int64_t> output) {
    if (!clip.gain) return;
    if (clip.reverse) {
      MixReverse(clip, from, to, output);
      return;
    }
    const auto channels = pcm.ChannelCount();
    auto at = from;
    ReadPcm(*clip.movie, clip.cursor, clip.source + (from - clip.start), to - from, [&](auto bytes) {
      const auto samples = bytes.size() / sizeof(std::int16_t);
      for (std::size_t sample = 0; sample < samples; ++sample) {
        std::int16_t value;
        std::memcpy(&value, bytes.data() + sample * sizeof(value), sizeof(value));
        output[sample] += std::lround(value * clip.Volume(at + sample / channels));
      }
      output = output.subspan(samples);
      at += samples / channels;
    });
  }

  void MixReverse(AudioClip& clip, std::int64_t from, std::int64_t to, std::span<std::int64_t> output) {
    const auto channels = pcm.ChannelCount();
    auto source = clip.source + (clip.end - from) - 1;
    for (auto remaining = to - from; remaining > 0;) {
      if (source < clip.reverse_start || source - clip.reverse_start >= std::int64_t(clip.reverse_pcm.size() / channels)) {
        const auto count = std::min<std::int64_t>(pcm.sample_rate / 4, source - clip.source + 1);
        const auto start = source + 1 - count;
        std::vector<std::int16_t> pcm_window(std::size_t(count) * channels);
        AudioCursor cursor;
        auto* destination = pcm_window.data();
        ReadPcm(*clip.movie, cursor, start, count, [&](auto bytes) {
          std::memcpy(destination, bytes.data(), bytes.size());
          destination += bytes.size() / sizeof(std::int16_t);
        });
        clip.reverse_start = start;
        clip.reverse_pcm = std::move(pcm_window);
      }
      const auto offset = source - clip.reverse_start;
      const auto count = std::min(remaining, offset + 1);
      for (std::int64_t sample = 0; sample < count; ++sample) {
        const auto gain = clip.Volume(from + sample);
        for (std::size_t channel = 0; channel < channels; ++channel)
          output[std::size_t(sample) * channels + channel] +=
              std::lround(clip.reverse_pcm[std::size_t(offset - sample) * channels + channel] * gain);
      }
      output = output.subspan(std::size_t(count) * channels);
      source -= count;
      remaining -= count;
      from += count;
    }
  }

  AudioFrame SampleAudio(MediaTimestamp at, std::uint32_t count) {
    if (!count || !Contains(range, at) || Before(range.end, Add(at, {count, pcm.sample_rate})))
      throw std::out_of_range("Sample interval is outside the project audio range");
    const auto start = ExactTicks(at, pcm.sample_rate), end = start + count;
    const auto channels = pcm.ChannelCount();
    std::vector<std::int64_t> sum(std::size_t(count) * channels);
    for (auto& clip : sounds) {
      if (!clip.cursor.reader) continue;
      const auto from = std::max(start, clip.start);
      if (from >= std::min(end, clip.end) || clip.cursor.next != clip.source + (from - clip.start)) clip.cursor = {};
    }
    for (auto& clip : sounds) {
      const auto from = std::max(start, clip.start), to = std::min(end, clip.end);
      if (from >= to) {
        if (!clip.reverse_pcm.empty()) clip.reverse_pcm = std::vector<std::int16_t>{};
        continue;
      }
      Mix(clip, from, to, std::span(sum).subspan(std::size_t(from - start) * channels));
      if (to == clip.end) {
        clip.cursor = {};
        clip.reverse_pcm = std::vector<std::int16_t>{};
      }
    }
    auto samples = std::make_shared<std::vector<std::int16_t>>(sum.size());
    std::ranges::transform(sum, samples->begin(), [](const auto value) {
      return static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
    });
    return {pcm, {start, pcm.sample_rate}, count, {BufferReference(std::as_bytes(std::span(*samples)), samples)}};
  }

  void Reset(std::optional<MediaTimestamp> seek, std::optional<MediaTimestamp> preroll_end = {}) {
    last_video_plan.reset();
    if (seek) CollectPreroll();
    for (auto& slot : prerolls) CancelPreroll(slot);
    if (seek && preroll_end) {
      const auto entries = NextVideoEntries({*seek, *preroll_end}, [](VideoClip& clip) { return CachedEntry(clip) != nullptr; });
      for (std::size_t i = 0; i < entries.size(); ++i) prerolls[i].clip = entries[i];
    }
    for (auto& clip : pictures) {
      if (FindPreroll(&clip) != prerolls.end()) {
        if (clip.cursor.HasReverseCpuFrame(EntrySource(clip))) clip.cursor.reader.reset();
      } else if (seek && NeedsPicture(clip, *seek)) {
        const auto source = SourceTime(clip.clip->placement, *seek);
        const auto reuse = clip.clip->placement.source.reverse ? clip.cursor.HasReverseCpuFrame(source)
                                                               : clip.cursor.CanSeekForward(source);
        if (!reuse) clip.cursor = {};
      } else {
        clip.cursor = {};
      }
      if (clip.tracking) clip.tracking->Reset();
      if (clip.mask) clip.mask->Reset();
      if (clip.depth) clip.depth->Reset();
    }
    for (auto& clip : sounds) {
      clip.cursor = {};
      clip.reverse_pcm = std::vector<std::int16_t>{};
    }
  }

  std::shared_ptr<const ProjectSnapshot> project;
  MovieCodecProviders providers;
  CodecPolicy policy;
  OnCodecFallback report;
  std::shared_ptr<analysis::AnalysisProvider> analysis_provider;
  TimeRange range;
  AudioFormat pcm{48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Mono};
  SceneEvaluator scene;
  std::weak_ptr<const render::RenderPlan> last_video_plan;
  MediaTimestamp last_video_time;
  std::map<std::uint64_t, MovieAsset> movies;
  std::vector<VideoClip> pictures;
  std::array<VideoPrerollSlot, kVideoPrerollLimit> prerolls;
  std::vector<TransitionLayer> transitions;
  std::vector<flecs::entity> overlays;
  std::vector<AudioClip> sounds;
  std::int32_t next_order{};
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
  std::deque<std::shared_ptr<VideoPrerollJob>> queued_preroll;
  std::mutex preroll_mutex;
  std::condition_variable_any preroll_ready;
  std::array<std::jthread, kVideoPrerollLimit> preroll_workers;
#endif
};

TimelineSession::TimelineSession(std::shared_ptr<const ProjectSnapshot> project, MovieCodecProviders providers,
                                 CodecPolicy policy, OnCodecFallback on_fallback,
                                 std::shared_ptr<analysis::AnalysisProvider> analysis)
    : impl_(std::make_unique<Impl>(std::move(project), std::move(providers), policy, std::move(on_fallback),
                                   std::move(analysis))) {}

TimelineSession::~TimelineSession() = default;

void TimelineSession::ApplyProject(std::shared_ptr<const ProjectSnapshot> project) {
  impl_ = std::make_unique<Impl>(std::move(project), impl_->providers, impl_->policy, impl_->report, impl_->analysis_provider);
}

TimelineFrame TimelineSession::SampleVideo(MediaTimestamp at, render::PixelFormat output, std::stop_token stop,
                                           const ImportVideoFrame& import,
                                           std::optional<std::chrono::milliseconds> analysis_wait) {
  try {
    return impl_->SampleVideo(at, output, stop, import, analysis_wait);
  } catch (const std::system_error& error) {
    if (error.code() == std::errc::operation_canceled) Reset();
    throw;
  }
}

std::shared_ptr<const render::RenderPlan> TimelineSession::RecomposeVideo(
    const TimelineFrame& frame, const VisualTransformOverride& change) {
  return impl_->RecomposeVideo(frame, change);
}

bool TimelineSession::PrerollVideoEntries(TimeRange lookahead, std::stop_token stop) {
  return impl_->PrerollVideoEntries(lookahead, stop);
}

AudioFormat TimelineSession::PcmFormat() const { return impl_->pcm; }

AudioFrame TimelineSession::SampleAudio(MediaTimestamp start, std::uint32_t samples) {
  return impl_->SampleAudio(start, samples);
}

void TimelineSession::Seek(MediaTimestamp at, std::optional<MediaTimestamp> preroll_end) {
  if (!Contains(impl_->range, at)) throw std::out_of_range("Seek time is outside the project interval");
  if (preroll_end && (!Before(at, *preroll_end) || Before(impl_->range.end, *preroll_end)))
    throw std::out_of_range("Seek lookahead end is outside the remaining project interval");
  impl_->Reset(at, preroll_end);
}

void TimelineSession::Reset() { impl_->Reset(std::nullopt); }

} // namespace huxerui::avkit
