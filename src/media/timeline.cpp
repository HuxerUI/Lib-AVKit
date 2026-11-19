#include <huxerui/avkit/timeline.h>
#include <huxerui/avkit_ecs.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>

#include "analysis/tracking.h"
#include "project/project_internal.h"
#include "support/cancellation.h"
#include "support/media_time.h"

namespace huxerui::avkit {
namespace {

using namespace media;
using detail::Add;
using detail::Before;
using detail::CheckCancelled;
using detail::Difference;
using detail::ExactTicks;

TimeRange RangeOf(const MediaPresentationRange& range) { return {range.start, range.end}; }

bool Contains(TimeRange range, MediaTimestamp time) {
  return !Before(time, range.start) && Before(time, range.end);
}

MediaTimestamp SourceTime(const ClipPlacement& clip, MediaTimestamp time) {
  const auto offset = Difference(time, clip.timeline.start);
  return clip.source.reverse ? Difference(clip.source.range.end, offset) : Add(clip.source.range.start, offset);
}

struct VideoCursor {
  std::unique_ptr<MovieReader> reader;
  std::optional<VideoFrame> frame;
  std::optional<MediaTimestamp> last;
  std::optional<TimeRange> reverse_window;
  std::deque<VideoFrame> reverse_frames;
};

struct AudioCursor {
  std::unique_ptr<MovieReader> reader;
  std::optional<AudioFrame> frame;
  std::int64_t next{};
};

struct StickerLayer {
  const StickerSpec* spec;
  flecs::entity entity;
  flecs::entity anchor;
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
};

bool NeedsPicture(const VideoClip& clip, MediaTimestamp at) {
  return Contains(clip.clip->placement.timeline, at) &&
         (!clip.incoming || !Before(at, clip.incoming->spec.range.start)) &&
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

Rect VideoContent(const VideoFormat& format, PixelSize canvas) {
  // Match the integer placement of Lib-Media's Contain texture fit used by the timeline compositor.
  double width = double(format.visible.width) * format.aspect_width / format.aspect_height;
  double height = format.visible.height;
  if (format.rotation == VideoRotation::Clockwise90 || format.rotation == VideoRotation::Clockwise270)
    std::swap(width, height);
  const auto fit = std::min(canvas.width / width, canvas.height / height);
  const auto w = std::clamp(std::lround(width * fit), 1L, long(canvas.width));
  const auto h = std::clamp(std::lround(height * fit), 1L, long(canvas.height));
  return {float((canvas.width - w) / 2) / canvas.width, float((canvas.height - h) / 2) / canvas.height,
          float(w) / canvas.width, float(h) / canvas.height};
}

std::optional<Transform2D> AnchorTransform(const analysis::TrackingAnchor& anchor, const analysis::PersonSample& person,
                                           Rect content, PixelSize canvas) {
  const auto& part = anchor.part == analysis::TrackedPart::Face ? person.face
                     : anchor.part == analysis::TrackedPart::LeftHand ? person.left_hand : person.right_hand;
  if (!part || part->observed != person.target) return std::nullopt;
  const auto origin = detail::FindLandmark(*part, anchor.point);
  if (!origin) return std::nullopt;
  const Point position{content.x + origin->x * content.width, content.y + origin->y * content.height};
  if (!anchor.axis_point) return Transform2D{content.width, 0, 0, content.height, position.x, position.y};
  const auto axis = detail::FindLandmark(*part, *anchor.axis_point);
  if (!axis || *axis == *origin) return std::nullopt;
  const auto x = (axis->x - origin->x) * content.width;
  const auto y = (axis->y - origin->y) * content.height;
  return Transform2D{x, y, -y * canvas.height / canvas.width, x * canvas.width / canvas.height, position.x, position.y};
}

} // namespace

std::uint32_t TransitionMaskSample::FrameIndex(std::uint32_t frame_count) const {
  if (!frame_count || Before(offset, {}) || !Before(offset, duration))
    throw std::invalid_argument("Mask sampling requires a nonempty sequence and time within the transition");
  const auto scale = std::lcm<std::uint64_t>(offset.timescale, duration.timescale);
  if (scale > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("Incompatible transition clocks");
  const auto at = static_cast<std::uint64_t>(ExactTicks(offset, static_cast<std::uint32_t>(scale)));
  const auto length = static_cast<std::uint64_t>(ExactTicks(duration, static_cast<std::uint32_t>(scale)));
  const auto whole = length / frame_count, remainder = length % frame_count;
  std::uint32_t first = 0, last = frame_count - 1;
  while (first < last) {
    const auto middle = static_cast<std::uint32_t>((std::uint64_t(first) + last + 1) / 2);
    // Compare ticks to ceil(length * middle / frame_count) without multiplying the full length.
    const auto threshold = whole * middle + (remainder * middle + frame_count - 1) / frame_count;
    if (at >= threshold) first = middle;
    else last = middle - 1;
  }
  return first;
}

AssetReference InspectTimelineMovie(const File& file) {
  const auto movie = ImportMovie(file);
  return {movie.file, {{movie.video.id, TrackKind::Video, RangeOf(movie.video_range)},
                       {movie.audio.id, TrackKind::Audio, RangeOf(movie.audio_range)}}};
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
  }

  const MovieAsset& Movie(AssetId id) {
    const auto found = movies.find(id.value);
    if (found != movies.end()) return found->second;
    const auto& file = detail::FindAsset(*project, id).source.file;
    if (!file.IsFile()) throw std::runtime_error("Missing timeline asset: " + file.Path());
    return movies.emplace(id.value, ImportMovie(file)).first->second;
  }

  flecs::entity AddLayer(Rect destination, render::Blend blend) {
    if (next_order == std::numeric_limits<std::int32_t>::max()) throw std::length_error("Too many visual layers");
    return scene.World().entity()
        .set<LocalTransform2D>({{destination.x, destination.y}, {destination.width, destination.height}})
        .set<CompositeLayer>({0, next_order++, blend})
        .disable();
  }

  void AddVideo(const ClipInfo& clip, const MovieAsset& movie) {
    auto& node = pictures.emplace_back(VideoClip{&clip, &movie, AddLayer(clip.destination, render::Blend::Replace)});
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
      node.stickers.push_back({&instance.spec, entity, anchor});
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
    if (!cursor.reader || (cursor.last && Before(source, *cursor.last))) {
      cursor = {};
      cursor.reader = Open(*clip.movie, TrackKind::Video, source, clip.software_video);
      cursor.frame = cursor.reader->ReadVideo(stop);
    }
    cursor.last = source;
    while (cursor.frame && !Before(source, Add(cursor.frame->pts, *cursor.frame->duration)))
      cursor.frame = cursor.reader->ReadVideo(stop);
    return cursor.frame && !Before(source, cursor.frame->pts) ? &*cursor.frame : nullptr;
  }

  VideoFrame* ReversePicture(VideoClip& clip, MediaTimestamp source, std::stop_token stop) {
    auto& cursor = clip.cursor;
    if (!cursor.reverse_window || !Before(cursor.reverse_window->start, source) || Before(cursor.reverse_window->end, source)) {
      cursor.reverse_window.reset();
      cursor.reverse_frames.clear();
      cursor.reader.reset();
      const auto trim = clip.clip->placement.source.range.start;
      const auto earliest = Difference(source, {1, 4});
      auto begin = Before(earliest, trim) ? trim : earliest;
      cursor.reader = Open(*clip.movie, TrackKind::Video, begin, clip.software_video);
      while (auto picture = cursor.reader->ReadVideo(stop)) {
        if (!Before(picture->pts, source)) break;
        const auto end = Add(picture->pts, *picture->duration);
        if (cursor.reverse_frames.size() == 8) {
          cursor.reverse_frames.pop_front();
          if (Before(begin, cursor.reverse_frames.front().pts)) begin = cursor.reverse_frames.front().pts;
        }
        cursor.reverse_frames.push_back(std::move(*picture));
        if (!Before(end, source)) break;
      }
      cursor.reverse_window = TimeRange{begin, source};
    }
    const auto picture = std::ranges::find_if(cursor.reverse_frames, [&](const VideoFrame& frame) {
      return Before(frame.pts, source) && !Before(Add(frame.pts, *frame.duration), source);
    });
    return picture == cursor.reverse_frames.end() ? nullptr : &*picture;
  }

  render::ColorSurface PreparePicture(VideoClip& clip, VideoFrame*& picture, const ImportVideoFrame& import,
                                      std::stop_token stop) {
    try {
      return import(*picture);
    } catch (const PlatformVideoImportError& error) {
      auto replacement = clip.cursor.reader->RetryVideoFrame(*picture, error, stop);
      clip.software_video = true;
      if (clip.clip->placement.source.reverse) {
        auto& cursor = clip.cursor;
        cursor.reverse_frames.clear();
        cursor.reverse_window = TimeRange{replacement.pts, Add(replacement.pts, *replacement.duration)};
        cursor.reverse_frames.push_back(std::move(replacement));
        picture = &cursor.reverse_frames.front();
      } else {
        *picture = std::move(replacement);
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
                            const ImportVideoFrame& import) {
    CheckCancelled(stop);
    if (!Contains(range, at)) throw std::out_of_range("Sample time is outside the project interval");
    TimelineFrame frame{at, {}, {}};
    for (auto& clip : pictures) {
      if ((clip.cursor.reader || clip.cursor.reverse_window) && !NeedsPicture(clip, at)) {
        clip.cursor = {};
        if (clip.tracking) clip.tracking->Reset();
      }
      clip.subcomposition.reset();
    }
    std::uint32_t subcomposition_count = 0;
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
      if (import) frame.inputs.emplace_back(PreparePicture(clip, image, import, stop));
      else frame.inputs.emplace_back(*image);
      const analysis::PersonSample* primary = nullptr;
      Rect content;
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
            content = VideoContent(image->format, project->canvas);
          }
        }
      }
      for (const auto& sticker : clip.stickers) {
        if (sticker.anchor) {
          const auto transform = primary ? AnchorTransform(*sticker.spec->anchor, *primary, content, project->canvas)
                                         : std::nullopt;
          if (!transform) {
            sticker.entity.disable();
            continue;
          }
          sticker.anchor.set<Transform2D>(*transform);
        }
        sticker.entity.enable();
        const auto found = std::ranges::find_if(frame.inputs, [&](const TimelineInput& input) {
          const auto* resource = std::get_if<ResourceRef>(&input);
          return resource && *resource == sticker.spec->resource;
        });
        sticker.entity.get_mut<CompositeLayer>().input_slot = static_cast<std::uint32_t>(found - frame.inputs.begin());
        if (found == frame.inputs.end()) frame.inputs.emplace_back(sticker.spec->resource);
      }
    }
    for (auto& node : transitions) {
      const auto& spec = node.transition->spec;
      if (!node.visible) continue;
      if (!node.from->visible || !node.to->visible) throw std::runtime_error("Transition requires both source pictures");
      const auto offset = Difference(at, spec.range.start);
      const auto mask_slot = static_cast<std::uint32_t>(frame.masks.size());
      frame.masks.push_back({spec.mask, offset, Difference(spec.range.end, spec.range.start)});
      const auto first = static_cast<std::uint32_t>(frame.inputs.size());
      node.layer.get_mut<CompositeLayer>().input_slot = first + *node.from->subcomposition;
      node.layer.set<render::MaskedBlend>({first + *node.to->subcomposition, mask_slot, FeatherAt(spec, offset),
                                          spec.inverted, node.from->clip->destination});
    }
    CheckCancelled(stop);
    frame.plan = scene.Evaluate(at, {project->canvas, output}, subcomposition_count);
    return frame;
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
    if (clip.reverse) {
      MixReverse(clip, from, to, output);
      return;
    }
    ReadPcm(*clip.movie, clip.cursor, clip.source + (from - clip.start), to - from, [&](auto bytes) {
      const auto samples = bytes.size() / sizeof(std::int16_t);
      for (std::size_t sample = 0; sample < samples; ++sample) {
        std::int16_t value;
        std::memcpy(&value, bytes.data() + sample * sizeof(value), sizeof(value));
        output[sample] += value;
      }
      output = output.subspan(samples);
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
        for (std::size_t channel = 0; channel < channels; ++channel)
          output[std::size_t(sample) * channels + channel] += clip.reverse_pcm[std::size_t(offset - sample) * channels + channel];
      }
      output = output.subspan(std::size_t(count) * channels);
      source -= count;
      remaining -= count;
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

  std::shared_ptr<const ProjectSnapshot> project;
  MovieCodecProviders providers;
  CodecPolicy policy;
  OnCodecFallback report;
  std::shared_ptr<analysis::AnalysisProvider> analysis_provider;
  TimeRange range;
  AudioFormat pcm{48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Mono};
  SceneEvaluator scene;
  std::map<std::uint64_t, MovieAsset> movies;
  std::vector<VideoClip> pictures;
  std::vector<TransitionLayer> transitions;
  std::vector<AudioClip> sounds;
  std::int32_t next_order{};
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
                                           const ImportVideoFrame& import) {
  try {
    return impl_->SampleVideo(at, output, stop, import);
  } catch (const std::system_error& error) {
    if (error.code() == std::errc::operation_canceled) Reset();
    throw;
  }
}

AudioFormat TimelineSession::PcmFormat() const { return impl_->pcm; }

AudioFrame TimelineSession::SampleAudio(MediaTimestamp start, std::uint32_t samples) {
  return impl_->SampleAudio(start, samples);
}

void TimelineSession::Reset() {
  for (auto& clip : impl_->pictures) {
    clip.cursor = {};
    if (clip.tracking) clip.tracking->Reset();
  }
  for (auto& clip : impl_->sounds) {
    clip.cursor = {};
    clip.reverse_pcm = std::vector<std::int16_t>{};
  }
}

} // namespace huxerui::avkit
