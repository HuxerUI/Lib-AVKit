#include "project_internal.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "support/media_time.h"

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
  }
}

std::uint64_t NextRevision(std::uint64_t revision) {
  if (revision == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Project revision overflow");
  return revision + 1;
}

} // namespace

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
          Check(previous->destination == clip.destination, "Mask transitions require matching clip rectangles");
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
      Check(track.kind == TrackKind::Video || clip.stickers.empty(), "Stickers require a visual clip");
      for (const auto& sticker : clip.stickers) {
        unique_id(sticker.id);
        const auto& resource = sticker.spec.resource;
        Check(Versioned(resource), "Sticker needs a versioned resource");
        ValidateRect(sticker.spec.destination);
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

void ProjectEdit::SetClip(ClipId clip, ClipPlacement placement) { Clip(clip).placement = placement; }
void ProjectEdit::SetLayout(ClipId clip, Rect destination) { Clip(clip).destination = destination; }

InstanceId ProjectEdit::AddSticker(ClipId clip, StickerSpec spec) {
  auto& instances = Clip(clip).stickers;
  const InstanceId id{AllocateId()};
  instances.push_back({id, std::move(spec)});
  return id;
}

void ProjectEdit::SetStickerLayout(InstanceId instance, Rect destination) {
  for (auto& track : draft_.tracks) {
    for (auto& clip : track.clips) {
      const auto found = std::ranges::find(clip.stickers, instance, &StickerInstance::id);
      if (found == clip.stickers.end()) continue;
      found->spec.destination = destination;
      return;
    }
  }
  throw std::invalid_argument("Unknown sticker instance");
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
