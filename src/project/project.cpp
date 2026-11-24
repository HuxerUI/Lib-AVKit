#include "project_internal.h"

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

bool Versioned(const ResourceRef& resource) {
  return !resource.pack.empty() && !resource.revision.empty() && !resource.key.empty();
}

void ValidateTransition(const TransitionSpec& spec) {
  ValidateRange(spec.range);
  if (spec.animation) {
    Check(Versioned(*spec.animation) && spec.mask == ResourceRef{},
          "An animated transition needs a versioned animation resource and no mask");
    Check(spec.feather == 0.5F && !spec.inverted && spec.feather_keys.empty(),
          "An animated transition does not support coverage mask parameters");
    return;
  }
  Check(Versioned(spec.mask), "A transition needs a versioned mask resource");
  const auto coverage = [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; };
  Check(coverage(spec.feather), "Transition feather must be between zero and one");
  const auto duration = Difference(spec.range.end, spec.range.start);
  std::optional<media::MediaTimestamp> previous;
  for (const auto& key : spec.feather_keys) {
    Check(key.time.timescale && !Before(key.time, {}) && !Before(duration, key.time) &&
          (!previous || Before(*previous, key.time)), "Transition keys must increase within the local duration");
    Check(coverage(key.value) && (key.interpolation == Interpolation::Hold || key.interpolation == Interpolation::Linear),
          "Invalid transition feather key");
    previous = key.time;
  }
}

void ValidateAsset(const AssetReference& source) {
  Check(!source.tracks.empty(), "The asset has no tracks");
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

Transform2D CanvasRotation(Rect destination, PixelSize canvas, float degrees) {
  if (!canvas.width || !canvas.height || !std::isfinite(degrees))
    throw std::invalid_argument("Canvas rotation needs a finite angle and a positive canvas size");
  if (degrees == 0) return {};
  const auto radians = std::remainder(degrees, 360.0F) * 0.017453292519943295F;
  const auto cosine = std::cos(radians), sine = std::sin(radians);
  const auto aspect = float(canvas.width) / canvas.height;
  const auto center = Point{destination.x + destination.width / 2, destination.y + destination.height / 2};
  Transform2D transform{cosine, sine * aspect, -sine / aspect, cosine};
  const auto rotated = transform.Apply(center);
  transform.translate_x = center.x - rotated.x;
  transform.translate_y = center.y - rotated.y;
  return transform;
}

TimeRange MapClipToSource(const ClipPlacement& clip, TimeRange project) {
  return MapInterval(project, clip.timeline, clip.source.range, clip.source.reverse);
}

TimeRange MapClipToProject(const ClipPlacement& clip, TimeRange source) {
  return MapInterval(source, clip.source.range, clip.timeline, clip.source.reverse);
}

namespace detail {

const AssetInfo& FindAsset(const ProjectSnapshot& snapshot, AssetId id) {
  return Find(snapshot.assets, id, "Unknown asset identifier");
}

const AssetTrack& FindSourceTrack(const AssetReference& asset, std::uint32_t id) {
  return Find(asset.tracks, id, "Unknown source track identifier");
}

void ValidateProject(const ProjectSnapshot& snapshot) {
  Check(snapshot.canvas.width && snapshot.canvas.height && snapshot.next_id, "Invalid project canvas or identifier counter");
  std::unordered_set<std::uint64_t> ids;
  const auto unique_id = [&](auto id) {
    Check(id.value && id.value < snapshot.next_id && ids.insert(id.value).second, "Invalid or duplicate project identifier");
  };
  for (const auto& asset : snapshot.assets) {
    unique_id(asset.id);
    ValidateAsset(asset.source);
  }
  for (const auto& overlay : snapshot.overlays) {
    unique_id(overlay.id);
    ValidateRange(overlay.range);
    ValidateRect(overlay.destination);
    Check(std::isfinite(overlay.rotation_degrees), "Expected finite overlay rotation");
    const auto check_color = [](Color color) {
      for (const auto value : {color.red, color.green, color.blue, color.alpha})
        Check(value >= 0 && value <= 1, "Overlay color components must be in [0, 1]");
    };
    check_color(overlay.style.foreground);
    check_color(overlay.style.background);
    Check(overlay.style.size >= 0.25F && overlay.style.size <= 4.0F && overlay.opacity >= 0 && overlay.opacity <= 1 &&
          (overlay.style.font == ResourceRef{} || Versioned(overlay.style.font)), "Invalid overlay style");
    switch (overlay.kind) {
      case CanvasOverlayKind::Text:
      case CanvasOverlayKind::Subtitle:
        Check(!overlay.text.empty(), "Text overlays require content");
        break;
      case CanvasOverlayKind::Sticker:
      case CanvasOverlayKind::PagText:
        Check(Versioned(overlay.resource), "Canvas stickers need a versioned resource");
        break;
      default: throw std::invalid_argument("Unsupported canvas overlay kind");
    }
  }
  std::unordered_map<std::uint64_t, const ClipInfo*> video_clips;
  for (const auto& track : snapshot.tracks)
    if (track.kind == TrackKind::Video)
      for (const auto& clip : track.clips) video_clips.emplace(clip.id.value, &clip);
  std::unordered_set<std::uint64_t> linked_audio;
  std::unordered_map<std::uint64_t, const TransitionInfo*> transitions;
  for (const auto& transition : snapshot.transitions) {
    unique_id(transition.id);
    ValidateTransition(transition.spec);
    Check(transitions.emplace(transition.from.value, &transition).second, "A clip can have one outgoing transition");
  }
  for (const auto& track : snapshot.tracks) {
    unique_id(track.id);
    Check(track.kind == TrackKind::Video || track.kind == TrackKind::Audio, "Unsupported timeline track kind");
    const ClipInfo* previous = nullptr;
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
      const auto& source_track = FindSourceTrack(asset.source, source.track);
      Check(source_track.kind == track.kind && !Before(source.range.start, source_track.presentation.start) &&
                !Before(source_track.presentation.end, source.range.end), "Clip exceeds its compatible source track");
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
      if (clip.audio_of) {
        const auto owner = video_clips.find(clip.audio_of->value);
        Check(owner != video_clips.end() && linked_audio.insert(clip.audio_of->value).second,
              "Original sound must have one video owner");
        const auto& picture = owner->second->placement;
        Check(picture.timeline == timeline && picture.source.asset == source.asset && picture.source.reverse == source.reverse,
              "Original sound must follow its video clip");
      }
      Check(track.kind == TrackKind::Video || clip.stickers.empty(), "Stickers require a visual clip");
      if (clip.background) {
        const auto& effect = *clip.background;
        Check(track.kind == TrackKind::Video && detail::IsUnitRect(effect.source_crop),
              "Background replacement requires a visual clip and a normalized source crop");
        for (const auto channel : {effect.color.red, effect.color.green, effect.color.blue, effect.color.alpha})
          Check(channel >= 0 && channel <= 1, "Background color channels must be in [0, 1]");
        if (effect.static_mask) Check(Versioned(effect.static_mask->resource), "Static mask needs a versioned resource");
        if (effect.animated_mask) {
          const auto& mask = *effect.animated_mask;
          Check(!effect.static_mask, "A clip background accepts one packaged mask");
          Check(Versioned(mask.resource) && (mask.playback == MaskPlayback::HoldLast || mask.playback == MaskPlayback::Loop),
                "Animated mask needs a versioned resource and a valid playback mode");
        }
      }
      for (const auto& sticker : clip.stickers) {
        unique_id(sticker.id);
        const auto& resource = sticker.spec.resource;
        Check(Versioned(resource), "Sticker needs a versioned resource");
        ValidateRect(sticker.spec.destination);
        if (const auto depth = sticker.spec.depth_meters)
          Check(std::isfinite(*depth) && *depth > 0, "Sticker depth must be positive and finite");
        if (const auto playback = sticker.spec.playback)
          Check(*playback == AnimationPlayback::HoldLast || *playback == AnimationPlayback::Loop,
                "Sticker needs a valid animation playback mode");
        const auto& replacements = sticker.spec.replacements;
        if (!replacements.Empty()) {
          Check(sticker.spec.playback.has_value(), "Sticker replacements require an animation");
          std::unordered_set<std::string_view> slots;
          const auto check_slot = [&](const std::string& slot) {
            Check(!slot.empty() && slots.insert(slot).second, "Sticker replacement slots must be nonempty and unique");
          };
          for (const auto& text : replacements.texts) check_slot(text.slot);
          for (const auto& image : replacements.images) {
            check_slot(image.slot);
            Check(Versioned(image.image), "Sticker image replacement needs a versioned resource");
          }
        }
        if (const auto& anchor = sticker.spec.anchor) {
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

std::vector<AssetId> ProjectSnapshot::MissingAssets() const {
  std::vector<AssetId> missing;
  for (const auto& asset : assets) if (!asset.source.file.IsFile()) missing.push_back(asset.id);
  return missing;
}

std::uint64_t ProjectEdit::AllocateId() {
  const auto id = draft_.next_id;
  draft_.next_id = NextRevision(id);
  return id;
}

AssetId ProjectEdit::Import(AssetReference source) {
  ValidateAsset(source);
  Check(source.file.IsFile(), "Cannot import an unavailable file");
  const AssetId id{AllocateId()};
  draft_.assets.push_back({id, std::move(source)});
  return id;
}

TrackId ProjectEdit::AddTrack(TrackKind kind) {
  const TrackId id{AllocateId()};
  draft_.tracks.push_back({id, kind, {}});
  return id;
}

ClipId ProjectEdit::InsertClip(TrackId track, ClipPlacement placement) {
  auto& clips = Find(draft_.tracks, track, "Unknown timeline track").clips;
  const ClipId id{AllocateId()};
  clips.push_back({id, placement});
  return id;
}

ClipInfo& ProjectEdit::Clip(ClipId clip) {
  for (auto& track : draft_.tracks) {
    const auto found = std::ranges::find(track.clips, clip, &ClipInfo::id);
    if (found != track.clips.end()) return *found;
  }
  throw std::invalid_argument("Unknown clip identifier");
}

void ProjectEdit::SetClip(ClipId clip, ClipPlacement placement) {
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
void ProjectEdit::SetLayout(ClipId clip, Rect destination) { Clip(clip).destination = destination; }
void ProjectEdit::SetClipRotation(ClipId clip, float degrees) { Clip(clip).rotation_degrees = degrees; }
void ProjectEdit::RemoveClip(ClipId clip) {
  (void)Clip(clip);
  for (auto& track : draft_.tracks) {
    std::erase_if(track.clips, [clip](const auto& item) { return item.id == clip || item.audio_of == clip; });
  }
  std::erase_if(draft_.transitions, [clip](const auto& item) { return item.from == clip || item.to == clip; });
}
void ProjectEdit::SetClipFill(ClipId clip, bool fill) { Clip(clip).fill = fill; }
void ProjectEdit::SetCanvas(PixelSize canvas) { draft_.canvas = canvas; }
void ProjectEdit::BindAudio(ClipId audio, std::optional<ClipId> video) { Clip(audio).audio_of = video; }
void ProjectEdit::SetAudioMix(ClipId audio, AudioMix mix) { Clip(audio).audio_mix = mix; }

InstanceId ProjectEdit::AddOverlay(CanvasOverlay overlay) {
  overlay.id = {AllocateId()};
  const auto id = overlay.id;
  draft_.overlays.push_back(std::move(overlay));
  return id;
}

void ProjectEdit::SetOverlay(CanvasOverlay overlay) {
  Find(draft_.overlays, overlay.id, "Unknown canvas overlay identifier") = std::move(overlay);
}

void ProjectEdit::RemoveOverlay(InstanceId id) {
  const auto found = std::ranges::find(draft_.overlays, id, &CanvasOverlay::id);
  Check(found != draft_.overlays.end(), "Unknown canvas overlay identifier");
  draft_.overlays.erase(found);
}

void ProjectEdit::SetBackground(ClipId clip, std::optional<BackgroundReplacement> background) {
  Clip(clip).background = std::move(background);
}

void ProjectEdit::SetBackgroundMaskResource(ClipId clip, ResourceRef resource) {
  auto& background = Clip(clip).background;
  Check(background && (background->static_mask || background->animated_mask), "The clip has no packaged mask binding");
  if (background->static_mask) background->static_mask->resource = std::move(resource);
  else background->animated_mask->resource = std::move(resource);
}

InstanceId ProjectEdit::AddSticker(ClipId clip, StickerSpec spec) {
  auto& instances = Clip(clip).stickers;
  const InstanceId id{AllocateId()};
  instances.push_back({id, std::move(spec)});
  return id;
}

StickerSpec& ProjectEdit::Sticker(InstanceId instance) {
  for (auto& track : draft_.tracks) {
    for (auto& clip : track.clips) {
      const auto found = std::ranges::find(clip.stickers, instance, &StickerInstance::id);
      if (found != clip.stickers.end()) return found->spec;
    }
  }
  throw std::invalid_argument("Unknown sticker instance");
}

void ProjectEdit::SetStickerLayout(InstanceId instance, Rect destination) {
  Sticker(instance).destination = destination;
}

void ProjectEdit::SetStickerReplacements(InstanceId instance, StickerReplacements replacements) {
  Sticker(instance).replacements = std::move(replacements);
}

InstanceId ProjectEdit::SetTransition(ClipId from, ClipId to, TransitionSpec spec) {
  ValidateTransition(spec);
  const auto found = std::ranges::find_if(draft_.transitions, [&](const TransitionInfo& transition) {
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
  if (before != after) {
    auto keys = spec.feather_keys;
    for (auto& key : keys) key.time = detail::Retime(key.time, before, after);
    spec.feather_keys = std::move(keys);
  }
  spec.range = range;
}

void ProjectEdit::SetTransitionFeather(InstanceId instance, float feather, std::vector<Keyframe<float>> keys) {
  auto& spec = Find(draft_.transitions, instance, "Unknown transition instance").spec;
  spec.feather = feather;
  spec.feather_keys = std::move(keys);
}

void ProjectEdit::RemoveTransition(InstanceId instance) {
  const auto removed = std::erase_if(draft_.transitions, [&](const TransitionInfo& entry) { return entry.id == instance; });
  Check(removed != 0, "Unknown transition instance");
}

void ProjectEdit::Relink(AssetId id, AssetReference replacement) {
  ValidateAsset(replacement);
  Check(replacement.file.IsFile(), "The replacement file is unavailable");
  auto& asset = Find(draft_.assets, id, "Unknown asset identifier");
  for (auto& track : draft_.tracks) {
    for (auto& clip : track.clips) {
      auto& source = clip.placement.source;
      if (source.asset != id) continue;
      const auto kind = detail::FindSourceTrack(asset.source, source.track).kind;
      const auto match = std::ranges::find(replacement.tracks, kind, &AssetTrack::kind);
      Check(match != replacement.tracks.end(), "The replacement lacks a track used by the project");
      source.track = match->id;
    }
  }
  asset.source = std::move(replacement);
  asset.content_revision = NextRevision(draft_.revision);
}

Project::Project(PixelSize canvas) : Project(ProjectSnapshot{.canvas = canvas}) {}

Project::Project(ProjectSnapshot snapshot) {
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
    std::stable_sort(track.clips.begin(), track.clips.end(), [](const ClipInfo& a, const ClipInfo& b) {
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
