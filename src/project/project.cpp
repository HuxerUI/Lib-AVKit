#include "project_internal.h"

#include <huxerui/avkit/resource_catalog.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "support/media_time.h"
#include "support/geometry.h"

namespace huxerui::avkit {
namespace {

using detail::Before;
using detail::Difference;
using detail::Add;
using media::TrackKind;
using media::MediaTimestamp;

void Check(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}

template<class Range, class Id>
decltype(auto) Find(Range& entries, Id id, const char* message) {
  const auto found = std::ranges::find(entries, id, &Range::value_type::id);
  if (found == entries.end()) throw std::invalid_argument(message);
  return *found;
}

void ValidateRange(TimeRange range) {
  Check(range.start.timescale && range.end.timescale && !Before(range.start, {}) && Before(range.start, range.end),
        "Expected a nonempty, nonnegative time range");
}

TimeRange MapInterval(TimeRange interval, TimeRange from, TimeRange to, bool reverse) {
  ValidateRange(interval);
  Check(!Before(interval.start, from.start) && !Before(from.end, interval.end), "Interval exceeds the clip range");
  const auto start = Difference(interval.start, from.start), end = Difference(interval.end, from.start);
  return reverse ? TimeRange{Difference(to.end, end), Difference(to.end, start)}
                 : TimeRange{Add(to.start, start), Add(to.start, end)};
}

void ValidateRect(Rect rect) {
  Check(std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) && std::isfinite(rect.height) &&
            !rect.IsEmpty(), "Expected a finite destination with positive size");
}

bool HasResourceKey(const ResourceRef& resource) {
  return !resource.key.empty();
}

void ValidatePagReplacements(const PagReplacements& replacements) {
  std::unordered_set<std::string_view> slots;
  const auto check_slot = [&](const std::string& slot) {
    Check(!slot.empty() && slots.insert(slot).second, "PAG replacement slots must be nonempty and unique");
  };
  const auto check_color = [](const std::optional<Color>& color) {
    if (!color) return;
    for (const auto value : {color->red, color->green, color->blue, color->alpha})
      Check(value >= 0 && value <= 1, "PAG text colors must be in [0, 1]");
  };
  for (const auto& text : replacements.texts) {
    check_slot(text.slot);
    Check(!text.font || HasResourceKey(*text.font), "PAG text font needs a resource key");
    Check(!text.size_scale || (*text.size_scale >= 0.25F && *text.size_scale <= 4.0F), "Invalid PAG text size");
    check_color(text.foreground);
    check_color(text.background);
  }
  for (const auto& image : replacements.images) {
    check_slot(image.slot);
    Check(HasResourceKey(image.image), "PAG image replacement needs a resource key");
  }
}

void ValidateOverlayContent(const OverlayContent& content) {
  std::visit([](const auto& source) {
    Check(HasResourceKey(source.resource), "An overlay needs a resource key");
    if constexpr (std::is_same_v<std::decay_t<decltype(source)>, PagOverlay>) {
      Check(source.playback == AnimationPlayback::Loop || source.playback == AnimationPlayback::HoldLast,
            "Invalid PAG playback mode");
      ValidatePagReplacements(source.replacements);
    }
  }, content);
}

void ValidateTransition(const TransitionSpec& spec) {
  ValidateRange(spec.range);
  if (const auto* animation = std::get_if<PagTransitionSource>(&spec.effect)) {
    Check(HasResourceKey(animation->resource), "A PAG transition needs a resource key");
    return;
  }
  const auto& mask = std::get<MaskTransition>(spec.effect);
  Check(HasResourceKey(mask.resource), "A transition needs a mask resource key");
  const auto coverage = [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; };
  Check(coverage(mask.feather), "Transition feather must be between zero and one");
  const auto duration = Difference(spec.range.end, spec.range.start);
  std::optional<media::MediaTimestamp> previous;
  for (const auto& key : mask.feather_keys) {
    Check(key.time.timescale && !Before(key.time, {}) && !Before(duration, key.time) &&
          (!previous || Before(*previous, key.time)), "Transition keys must increase within the local duration");
    Check(coverage(key.value) && (key.interpolation == Interpolation::Hold || key.interpolation == Interpolation::Linear),
          "Invalid transition feather key");
    previous = key.time;
  }
}

void ValidateAsset(const AssetSource& source) {
  Check(!source.tracks.empty(), "The asset has no tracks");
  if (source.still_image)
    Check(source.tracks.size() == 1 && source.tracks.front().kind == TrackKind::Video &&
          source.tracks.front().display_size.has_value(), "A still image needs one visual track and its dimensions");
  std::unordered_set<std::uint32_t> ids;
  std::unordered_set<TrackKind> kinds;
  for (const auto& track : source.tracks) {
    Check((track.kind == TrackKind::Video || track.kind == TrackKind::Audio) &&
              track.id && ids.insert(track.id).second && kinds.insert(track.kind).second,
          "Expected a single source track per media kind");
    ValidateRange(track.presentation);
    if (track.display_size)
      Check(track.kind == TrackKind::Video && track.display_size->width && track.display_size->height,
            "Invalid source display dimensions");
  }
}

std::uint64_t NextRevision(std::uint64_t revision) {
  if (revision == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Project revision overflow");
  return revision + 1;
}

} // namespace

std::optional<Point> Affine2D::Inverse(Point point) const noexcept {
  const auto determinant = m11 * m22 - m12 * m21;
  if (!std::isfinite(determinant) || std::abs(determinant) <= 0.000001F) return std::nullopt;
  const auto x = point.x - translate_x, y = point.y - translate_y;
  return Point{(m22 * x - m21 * y) / determinant, (-m12 * x + m11 * y) / determinant};
}

Affine2D CanvasRotation(Rect destination, PixelSize canvas, float degrees) {
  if (!canvas.width || !canvas.height || !std::isfinite(degrees))
    throw std::invalid_argument("Canvas rotation needs a finite angle and a positive canvas size");
  if (degrees == 0) return {};
  const auto radians = std::remainder(degrees, 360.0F) * 0.017453292519943295F;
  const auto cosine = std::cos(radians), sine = std::sin(radians);
  const auto aspect = float(canvas.width) / canvas.height;
  const auto center = Point{destination.x + destination.width / 2, destination.y + destination.height / 2};
  Affine2D transform{cosine, sine * aspect, -sine / aspect, cosine};
  const auto rotated = transform.Apply(center);
  transform.translate_x = center.x - rotated.x;
  transform.translate_y = center.y - rotated.y;
  return transform;
}

Rect FitCanvasAspect(Rect bounds, PixelSize canvas, Size source) {
  if (!canvas.width || !canvas.height || !(source.width > 0 && source.height > 0)) return bounds;
  const auto ratio = source.width * canvas.height / (source.height * canvas.width);
  if (std::abs(bounds.width - bounds.height * ratio) < 1e-5F) return bounds;
  const auto width = std::min(bounds.width, bounds.height * ratio);
  const auto height = width / ratio;
  return {bounds.x + (bounds.width - width) / 2, bounds.y + (bounds.height - height) / 2, width, height};
}

TimeRange MapClipToSource(const ClipTiming& clip, TimeRange project) {
  return MapInterval(project, clip.timeline, clip.source.range, clip.source.reverse);
}

TimeRange MapClipToProject(const ClipTiming& clip, TimeRange source) {
  return MapInterval(source, clip.source.range, clip.timeline, clip.source.reverse);
}

namespace detail {

const AssetSource& FindAsset(const ProjectSnapshot& snapshot, const ResourceRef& reference) {
  if (const auto media = snapshot.resources->FindMedia(reference)) return *media;
  throw std::invalid_argument("Unknown media resource: " + reference.key);
}

const AssetTrack& FindSourceTrack(const AssetSource& asset, std::uint32_t id) {
  return Find(asset.tracks, id, "Unknown source track identifier");
}

void ValidateProject(const ProjectSnapshot& snapshot) {
  Check(snapshot.canvas_size.width && snapshot.canvas_size.height && snapshot.next_id, "Invalid project canvas or identifier counter");
  Check(bool(snapshot.resources), "A project snapshot needs a media catalog");
  std::unordered_set<std::uint64_t> ids;
  const auto unique_id = [&](auto id) {
    Check(id.value && id.value < snapshot.next_id && ids.insert(id.value).second, "Invalid or duplicate project identifier");
  };
  for (const auto& [key, media] : snapshot.resources->Media()) ValidateAsset(*media);
  for (const auto& overlay : snapshot.overlays) {
    unique_id(overlay.id);
    ValidateRange(overlay.range);
    Check(!overlay.anchor && !overlay.depth, "Canvas overlays require clip attachment for tracking or depth");
    ValidateRect(overlay.destination);
    Check(std::isfinite(overlay.rotation_degrees), "Expected finite overlay rotation");
    Check(overlay.opacity >= 0 && overlay.opacity <= 1, "Invalid overlay opacity");
    ValidateOverlayContent(overlay.content);
    Check(overlay.kind == CanvasOverlayKind::Sticker || std::holds_alternative<PagOverlay>(overlay.content),
          "A text overlay needs PAG content");
    switch (overlay.kind) {
      case CanvasOverlayKind::Text:
      case CanvasOverlayKind::Subtitle:
      case CanvasOverlayKind::Sticker:
        break;
      default: throw std::invalid_argument("Unsupported canvas overlay kind");
    }
  }
  std::unordered_map<std::uint64_t, const TimelineClip*> video_clips;
  for (const auto& track : snapshot.tracks)
    if (track.kind == TrackKind::Video)
      for (const auto& clip : track.clips) video_clips.emplace(clip.id.value, &clip);
  std::unordered_set<std::uint64_t> linked_audio;
  std::unordered_map<std::uint64_t, const TransitionInstance*> transitions;
  for (const auto& transition : snapshot.transitions) {
    unique_id(transition.id);
    ValidateTransition(transition.spec);
    Check(transitions.emplace(transition.from.value, &transition).second, "A clip can have one outgoing transition");
  }
  for (const auto& track : snapshot.tracks) {
    unique_id(track.id);
    Check(track.kind == TrackKind::Video || track.kind == TrackKind::Audio, "Unsupported timeline track kind");
    const TimelineClip* previous = nullptr;
    media::MediaTimestamp earlier_end;
    for (const auto& clip : track.clips) {
      unique_id(clip.id);
      const auto& source = clip.placement.source;
      const auto& timeline = clip.placement.timeline;
      ValidateRange(timeline);
      ValidateRange(source.range);
      if (previous) {
        const auto prior = previous->placement.timeline;
        Check(!Before(timeline.start, prior.start), "Timeline clips must be ordered by start time");
        if (Before(timeline.start, prior.end)) {
          const auto match = transitions.find(previous->id.value);
          Check(track.kind == TrackKind::Video && match != transitions.end() && match->second->to == clip.id,
                "Overlapping clips on a track need a transition");
          const auto& range = match->second->spec.range;
          Check(!Before(range.start, timeline.start) && !Before(prior.end, range.end) && !Before(timeline.end, prior.end),
                "Transition range must fit both adjacent clips in playback order");
          Check(!Before(timeline.start, earlier_end), "A transition cannot overlap a third clip on its track");
          Check(previous->destination == clip.destination, "Transitions require matching clip rectangles");
          transitions.erase(match);
        }
        earlier_end = prior.end;
      }
      previous = &clip;
      const auto& asset = FindAsset(snapshot, source.asset);
      const auto& source_track = FindSourceTrack(asset, source.track);
      Check(source_track.kind == track.kind && !Before(source.range.start, source_track.presentation.start) &&
                (asset.still_image || !Before(source_track.presentation.end, source.range.end)),
            "Clip exceeds its compatible source track");
      Check(Difference(timeline.end, timeline.start) == Difference(source.range.end, source.range.start),
            "This timeline supports source playback at 1x");
      ValidateRect(clip.destination);
      Check(std::isfinite(clip.rotation_degrees) && (track.kind == TrackKind::Video || !clip.rotation_degrees),
            "Expected finite rotation on visual clips");
      Check(track.kind == TrackKind::Video || !clip.fill, "Only visual clips have a picture fit mode");
      const auto& mix = clip.audio_mix;
      Check(std::isfinite(mix.gain) && mix.gain >= 0 && mix.gain <= 2 &&
            mix.fade_in.timescale && mix.fade_in.ticks >= 0 && mix.fade_out.timescale && mix.fade_out.ticks >= 0,
            "Invalid audio clip mix");
      Check(track.kind == TrackKind::Audio || (!clip.audio_of && mix == AudioMix{}), "Audio settings require an audio clip");
      if (clip.voice_effect)
        Check(*clip.voice_effect == VoiceEffect::Robot && !clip.audio_of,
              "Voice effects belong to the independent audio clip or the owner of linked audio");
      if (clip.audio_of) {
        const auto owner = video_clips.find(clip.audio_of->value);
        Check(owner != video_clips.end() && linked_audio.insert(clip.audio_of->value).second,
              "Original sound must have one video owner");
        const auto& picture = owner->second->placement;
        Check(picture.timeline == timeline && picture.source.asset == source.asset && picture.source.reverse == source.reverse,
              "Original sound must follow its video clip");
      }
      Check(track.kind == TrackKind::Video || clip.stickers.empty(), "Stickers require a visual clip");
      if (clip.source_crop)
        Check(track.kind == TrackKind::Video && detail::IsUnitRect(clip.source_crop->area),
              "Source cropping requires a visual clip and a normalized crop");
      if (clip.background) {
        const auto& effect = *clip.background;
        Check(track.kind == TrackKind::Video, "Background replacement requires a visual clip");
        for (const auto channel : {effect.color.red, effect.color.green, effect.color.blue, effect.color.alpha})
          Check(channel >= 0 && channel <= 1, "Background color channels must be in [0, 1]");
        if (const auto* mask = std::get_if<StaticMask>(&effect.mask))
          Check(HasResourceKey(mask->image.resource) &&
                (mask->image.channel == MaskChannel::Grayscale || mask->image.channel == MaskChannel::Alpha),
                "Static mask needs a resource key and a valid coverage channel");
        if (const auto* mask = std::get_if<AnimatedMask>(&effect.mask)) {
          Check(HasResourceKey(mask->resource) && (mask->playback == AnimationPlayback::HoldLast || mask->playback == AnimationPlayback::Loop),
                "Animated mask needs a resource key and a valid playback mode");
        }
      }
      for (const auto& sticker : clip.stickers) {
        unique_id(sticker.id);
        ValidateOverlayContent(sticker.content);
        ValidateRect(sticker.destination);
        if (const auto& depth = sticker.depth)
          Check(std::isfinite(depth->meters) && depth->meters > 0, "Sticker depth must be positive and finite");
        if (sticker.range != TimeRange{}) ValidateRange(sticker.range);
        Check(std::isfinite(sticker.rotation_degrees) && sticker.opacity >= 0 && sticker.opacity <= 1,
              "Invalid attached overlay rotation or opacity");
        if (const auto& anchor = sticker.anchor) {
          using enum analysis::TrackedPart;
          Check(anchor->part == Face || anchor->part == LeftHand || anchor->part == RightHand,
                "Sticker needs a supported tracked part");
          Check(!anchor->point.empty() &&
                    (!anchor->axis_point || (!anchor->axis_point->empty() && *anchor->axis_point != anchor->point)),
                "Sticker anchor needs distinct, nonempty landmark names");
        }
      }
    }
  }
  for (const auto& [id, clip] : video_clips)
    Check(!clip->voice_effect || linked_audio.contains(id), "A video clip needs linked audio to apply a voice effect");
  Check(transitions.empty(), "A transition must join adjacent overlapping clips on one visual track");
}

} // namespace detail

TimeRange ProjectSnapshot::Range() const {
  media::MediaTimestamp end;
  for (const auto& track : tracks) {
    if (!track.clips.empty() && Before(end, track.clips.back().placement.timeline.end))
      end = track.clips.back().placement.timeline.end;
  }
  for (const auto& overlay : overlays)
    if (Before(end, overlay.range.end)) end = overlay.range.end;
  return {{}, end};
}

std::vector<ResourceRef> ProjectSnapshot::MissingAssets() const {
  std::vector<ResourceRef> missing;
  if (!resources) return missing;
  for (const auto& [key, media] : resources->Media())
    if (!media->file.IsFile()) missing.push_back(key);
  return missing;
}

std::uint64_t ProjectEdit::AllocateId() {
  const auto id = draft_.next_id;
  draft_.next_id = NextRevision(id);
  return id;
}

ResourceRef ProjectEdit::Import(AssetSource source, ResourceRef resource) {
  ValidateAsset(source);
  Check(source.file.IsFile(), "Cannot import an unavailable file");
  if (resource.key.empty()) {
    do {
      resource.key = "project/media/" + std::to_string(AllocateId());
    } while (draft_.resources->FindMedia(resource));
  } else if (const auto registered = draft_.resources->FindMedia(resource)) {
    Check(*registered == source, "Media key already names a different source");
    return resource;
  }
  auto resources = std::make_shared<ResourceCatalog>(*draft_.resources);
  resources->Register(resource, std::move(source));
  draft_.resources = std::move(resources);
  return resource;
}

TrackId ProjectEdit::AddTrack(TrackKind kind) {
  const TrackId id{AllocateId()};
  draft_.tracks.push_back({id, kind, {}});
  return id;
}

ClipId ProjectEdit::InsertClip(TrackId track, ClipTiming placement) {
  auto& clips = Find(draft_.tracks, track, "Unknown timeline track").clips;
  const ClipId id{AllocateId()};
  clips.push_back({id, placement});
  return id;
}

TimelineClip& ProjectEdit::Clip(ClipId clip) {
  for (auto& track : draft_.tracks) {
    const auto found = std::ranges::find(track.clips, clip, &TimelineClip::id);
    if (found != track.clips.end()) return *found;
  }
  throw std::invalid_argument("Unknown clip identifier");
}

ClipId ProjectEdit::InsertImage(TrackId track, ResourceRef asset, MediaTimestamp start,
                                std::optional<MediaTimestamp> duration) {
  const auto& image = detail::FindAsset(draft_, asset);
  Check(image.still_image, "Image insertion requires a still image asset");
  const auto& source = image.tracks.front();
  const auto length = duration.value_or(Difference(source.presentation.end, source.presentation.start));
  return InsertClip(track, {{asset, source.id, {source.presentation.start, detail::Add(source.presentation.start, length)}},
                            {start, detail::Add(start, length)}});
}

void ProjectEdit::SetClip(ClipId clip, ClipTiming placement) {
  auto& picture = Clip(clip);
  const auto previous = picture.placement.source.range;
  for (auto& track : draft_.tracks) {
    if (track.kind != TrackKind::Audio) continue;
    for (auto& audio : track.clips) {
      if (audio.audio_of != clip) continue;
      audio.placement.timeline = placement.timeline;
      audio.placement.source.reverse = placement.source.reverse;
      auto& range = audio.placement.source.range;
      range.start = detail::Add(range.start, Difference(placement.source.range.start, previous.start));
      range.end = detail::Add(range.end, Difference(placement.source.range.end, previous.end));
    }
  }
  picture.placement = placement;
}

void ProjectEdit::RippleTrim(ClipId id, ClipTiming placement) {
  const auto primary = std::ranges::find(draft_.tracks, TrackKind::Video, &TimelineTrack::kind);
  Check(primary != draft_.tracks.end(), "The project has no main visual track");
  const auto clip = std::ranges::find(primary->clips, id, &TimelineClip::id);
  Check(clip != primary->clips.end(), "Ripple trim requires a clip on the main visual track");
  const auto shift = Difference(placement.timeline.end, clip->placement.timeline.end);
  SetClip(id, placement);
  for (auto following = clip + 1; following != primary->clips.end(); ++following) {
    auto next = following->placement;
    next.timeline = {Add(next.timeline.start, shift), Add(next.timeline.end, shift)};
    SetClip(following->id, next);
    for (const auto& transition : draft_.transitions) {
      if (transition.from == following->id) {
        const auto range = transition.spec.range;
        SetTransitionRange(transition.id, {Add(range.start, shift), Add(range.end, shift)});
      }
    }
  }
}

void ProjectEdit::SetLayout(ClipId clip, Rect destination) { Clip(clip).destination = destination; }

void ProjectEdit::SetClipRotation(ClipId clip, float degrees) { Clip(clip).rotation_degrees = degrees; }

void ProjectEdit::RemoveClip(ClipId clip) {
  if (const auto owner = Clip(clip).audio_of) Clip(*owner).voice_effect.reset();
  for (auto& track : draft_.tracks) {
    std::erase_if(track.clips, [clip](const auto& item) { return item.id == clip || item.audio_of == clip; });
  }
  std::erase_if(draft_.transitions, [clip](const auto& item) { return item.from == clip || item.to == clip; });
}

void ProjectEdit::SetClipFill(ClipId clip, bool fill) { Clip(clip).fill = fill; }

void ProjectEdit::SetCanvasSize(PixelSize canvas_size) { draft_.canvas_size = canvas_size; }

void ProjectEdit::BindAudio(ClipId audio, std::optional<ClipId> video) {
  auto& sound = Clip(audio);
  if (sound.audio_of == video) return;
  Check(!video || *video != audio, "Audio cannot be linked to itself");
  if (sound.audio_of) sound.voice_effect = std::exchange(Clip(*sound.audio_of).voice_effect, std::nullopt);
  sound.audio_of = video;
  if (video && sound.voice_effect) Clip(*video).voice_effect = std::exchange(sound.voice_effect, std::nullopt);
}

void ProjectEdit::SetAudioMix(ClipId audio, AudioMix mix) { Clip(audio).audio_mix = mix; }

void ProjectEdit::SetVoiceEffect(ClipId clip, std::optional<VoiceEffect> effect) {
  auto& item = Clip(clip);
  (item.audio_of ? Clip(*item.audio_of) : item).voice_effect = effect;
}

InstanceId ProjectEdit::AddOverlay(OverlayInstance overlay) {
  overlay.id = {AllocateId()};
  const auto id = overlay.id;
  draft_.overlays.push_back(std::move(overlay));
  return id;
}

void ProjectEdit::SetOverlay(OverlayInstance overlay) {
  Find(draft_.overlays, overlay.id, "Unknown canvas overlay identifier") = std::move(overlay);
}

void ProjectEdit::RemoveOverlay(InstanceId id) {
  const auto found = std::ranges::find(draft_.overlays, id, &OverlayInstance::id);
  Check(found != draft_.overlays.end(), "Unknown canvas overlay identifier");
  draft_.overlays.erase(found);
}

void ProjectEdit::SetBackground(ClipId clip, std::optional<BackgroundReplacementSpec> background) {
  Clip(clip).background = std::move(background);
}

void ProjectEdit::SetSourceCrop(ClipId clip, std::optional<SourceCrop> crop) {
  Clip(clip).source_crop = std::move(crop);
}

void ProjectEdit::SetBackgroundMaskResource(ClipId clip, ResourceRef resource) {
  auto& background = Clip(clip).background;
  Check(background.has_value(), "The clip has no packaged mask binding");
  if (auto* mask = std::get_if<StaticMask>(&background->mask)) mask->image.resource = std::move(resource);
  else if (auto* mask = std::get_if<AnimatedMask>(&background->mask)) mask->resource = std::move(resource);
  else throw std::invalid_argument("The clip has no packaged mask binding");
}

InstanceId ProjectEdit::AddSticker(ClipId clip, OverlayInstance spec) {
  auto& instances = Clip(clip).stickers;
  const InstanceId id{AllocateId()};
  spec.id = id;
  instances.push_back(std::move(spec));
  return id;
}

OverlayInstance& ProjectEdit::Sticker(InstanceId instance) {
  for (auto& track : draft_.tracks) {
    for (auto& clip : track.clips) {
      const auto found = std::ranges::find(clip.stickers, instance, &OverlayInstance::id);
      if (found != clip.stickers.end()) return *found;
    }
  }
  throw std::invalid_argument("Unknown sticker instance");
}

void ProjectEdit::SetStickerLayout(InstanceId instance, Rect destination) {
  Sticker(instance).destination = destination;
}

void ProjectEdit::SetStickerReplacements(InstanceId instance, PagReplacements replacements) {
  auto* pag = std::get_if<PagOverlay>(&Sticker(instance).content);
  Check(pag != nullptr, "Sticker replacements require PAG content");
  pag->replacements = std::move(replacements);
}

InstanceId ProjectEdit::SetTransition(ClipId from, ClipId to, TransitionSpec spec) {
  ValidateTransition(spec);
  const auto found = std::ranges::find_if(draft_.transitions, [&](const TransitionInstance& transition) {
    return transition.from == from && transition.to == to;
  });
  if (found != draft_.transitions.end()) {
    found->spec = std::move(spec);
    return found->id;
  }
  const InstanceId id{AllocateId()};
  draft_.transitions.push_back({id, from, to, std::move(spec)});
  return id;
}

void ProjectEdit::SetTransitionRange(InstanceId instance, TimeRange range) {
  ValidateRange(range);
  auto& spec = Find(draft_.transitions, instance, "Unknown transition instance").spec;
  const auto before = Difference(spec.range.end, spec.range.start);
  const auto after = Difference(range.end, range.start);
  if (auto* mask = std::get_if<MaskTransition>(&spec.effect); mask && before != after) {
    auto keys = mask->feather_keys;
    for (auto& key : keys) key.time = detail::Retime(key.time, before, after);
    mask->feather_keys = std::move(keys);
  }
  spec.range = range;
}

void ProjectEdit::SetTransitionFeather(InstanceId instance, float feather, std::vector<Keyframe<float>> keys) {
  auto& spec = Find(draft_.transitions, instance, "Unknown transition instance").spec;
  auto* mask = std::get_if<MaskTransition>(&spec.effect);
  Check(mask != nullptr, "Only mask transitions support feathering");
  mask->feather = feather;
  mask->feather_keys = std::move(keys);
}

void ProjectEdit::RemoveTransition(InstanceId instance) {
  const auto removed = std::erase_if(draft_.transitions, [&](const TransitionInstance& entry) { return entry.id == instance; });
  Check(removed != 0, "Unknown transition instance");
}

void ProjectEdit::Relink(const ResourceRef& asset, File file) {
  Check(file.IsFile(), "The original media is unavailable at the selected location");
  if (detail::FindAsset(draft_, asset).file == file) return;
  auto resources = std::make_shared<ResourceCatalog>(*draft_.resources);
  resources->Relink(asset, std::move(file));
  draft_.resources = std::move(resources);
}

Project::Project(PixelSize canvas_size) : Project(ProjectSnapshot{.canvas_size = canvas_size}) {}

Project Project::FromSnapshot(ProjectSnapshot snapshot) { return Project(std::move(snapshot)); }

Project::Project(ProjectSnapshot snapshot) {
  if (!snapshot.resources) snapshot.resources = std::make_shared<const ResourceCatalog>();
  detail::ValidateProject(snapshot);
  current_ = std::make_shared<const ProjectSnapshot>(std::move(snapshot));
}

std::shared_ptr<const ProjectSnapshot> Project::Edit(const std::function<void(ProjectEdit&)>& changes) {
  Check(!editing_ && bool(changes), "Expected an edit outside an active transaction");
  editing_ = true;

  struct ResetEditing { bool& value; ~ResetEditing() { value = false; } } reset{editing_};
  auto draft = *current_;
  ProjectEdit edit(draft);
  changes(edit);
  for (auto& track : draft.tracks) {
    std::stable_sort(track.clips.begin(), track.clips.end(), [](const TimelineClip& a, const TimelineClip& b) {
      return Before(a.placement.timeline.start, b.placement.timeline.start);
    });
  }
  return Commit(std::move(draft));
}

std::shared_ptr<const ProjectSnapshot> Project::Commit(ProjectSnapshot draft) {
  draft.revision = NextRevision(current_->revision);
  detail::ValidateProject(draft);
  auto committed = std::make_shared<const ProjectSnapshot>(std::move(draft));
  undo_.push_back(current_);
  redo_.clear();
  return current_ = std::move(committed);
}

bool Project::Restore(std::vector<std::shared_ptr<const ProjectSnapshot>>& from,
                      std::vector<std::shared_ptr<const ProjectSnapshot>>& to) {
  Check(!editing_, "Cannot restore history within an edit");
  if (from.empty()) return false;
  auto restored = *from.back();
  restored.revision = NextRevision(current_->revision);
  restored.next_id = current_->next_id;
  auto snapshot = std::make_shared<const ProjectSnapshot>(std::move(restored));
  to.push_back(current_);
  from.pop_back();
  current_ = std::move(snapshot);
  return true;
}

bool Project::Undo() { return Restore(undo_, redo_); }

bool Project::Redo() { return Restore(redo_, undo_); }

} // namespace huxerui::avkit
