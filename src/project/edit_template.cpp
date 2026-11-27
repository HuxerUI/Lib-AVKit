#include <huxerui/avkit/edit_template.h>
#include <huxerui/avkit/resource_catalog.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

#include "support/media_time.h"
#include "project_internal.h"

namespace huxerui::avkit {
namespace {

media::MediaTimestamp AvailableDuration(const AssetSource& asset, const TemplateClip& clip, media::TrackKind kind) {
  const auto track = std::ranges::find(asset.tracks, kind, &AssetTrack::kind);
  if (track == asset.tracks.end()) throw std::invalid_argument("Incompatible template slot: " + clip.slot);
  auto limit = clip.source.end;
  if (!asset.still_image)
    limit = std::min(limit, detail::Difference(track->presentation.end, track->presentation.start), detail::Before);
  if (clip.include_original_audio && kind == media::TrackKind::Video) {
    const auto audio = std::ranges::find(asset.tracks, media::TrackKind::Audio, &AssetTrack::kind);
    if (audio != asset.tracks.end())
      limit = std::min(limit, detail::Difference(audio->presentation.end, audio->presentation.start), detail::Before);
  }
  const auto duration = detail::Difference(limit, clip.source.start);
  if (!detail::Before({}, duration)) throw std::invalid_argument("Template source trim is empty");
  // The common grid keeps cuts sample-aligned at both supported audio rates.
  const auto aligned = detail::FloorOnGrid(duration, 12);
  return aligned.ticks > 0 ? aligned : duration;
}

} // namespace

const EditTemplateRecipe& OpenEditTemplate(const ResourceCatalog& catalog, const ResourceRef& reference) {
  return catalog.Get<EditTemplateRecipe>(reference);
}

EditTemplatePlan::EditTemplatePlan(std::shared_ptr<const ProjectSnapshot> base,
                                   std::shared_ptr<const ProjectSnapshot> prepared)
    : base_(std::move(base)), prepared_(std::move(prepared)) {}

EditTemplatePlan PlanEditTemplate(const Project& project, const EditTemplateRecipe& recipe, const EditTemplateInputs& inputs) {
  const auto base = project.Snapshot();
  const auto& reference = recipe.reference;
  if (!base->tracks.empty() || base->canvas_size != recipe.canvas_size || (recipe.slots.empty() && recipe.resource_slots.empty()) || recipe.tracks.empty() ||
      reference.key.empty()) {
    throw std::invalid_argument("Template assembly needs a registered recipe and an empty project with the recipe canvas");
  }
  std::map<std::string, ResourceRef> bindings;
  for (const auto& slot : recipe.slots) {
    if (slot.empty() || !bindings.emplace(slot, ResourceRef{}).second) throw std::invalid_argument("Invalid template slot name");
  }
  for (const auto& slot : recipe.resource_slots) {
    if (slot.slot.empty() || !bindings.emplace(slot.slot, ResourceRef{}).second)
      throw std::invalid_argument("Invalid or duplicate template resource slot");
  }
  for (const auto& binding : inputs.bindings) {
    auto slot = bindings.find(binding.slot);
    if (slot == bindings.end() || !slot->second.key.empty() || binding.asset.key.empty())
      throw std::invalid_argument("Invalid explicit template slot binding");
    slot->second = binding.asset;
  }
  auto selected = inputs.ordered_assets.begin();
  for (const auto& slot : recipe.slots) {
    auto& bound = bindings.at(slot);
    if (bound.key.empty()) {
      while (selected != inputs.ordered_assets.end() &&
          std::ranges::any_of(bindings, [&](const auto& entry) { return entry.second == *selected; })) ++selected;
      if (selected == inputs.ordered_assets.end()) throw std::invalid_argument("Missing template slot: " + slot);
      bound = *selected++;
    }
    if (!detail::FindAsset(*base, bound).file.IsFile()) throw std::invalid_argument("Unavailable template slot: " + slot);
  }

  for (const auto& slot : recipe.resource_slots) {
    const auto bound = bindings.at(slot.slot);
    if (bound.key.empty() || !detail::FindAsset(*base, bound).file.IsFile())
      throw std::invalid_argument("Missing template resource slot: " + slot.slot);
  }

  const auto resolve_slot = [&](const TemplateClip& clip) -> const ResourceRef& {
    const auto binding = bindings.find(clip.slot);
    if (binding == bindings.end()) throw std::invalid_argument("Unknown template slot: " + clip.slot);
    return binding->second;
  };
  std::optional<media::MediaTimestamp> parallel_duration;
  if (recipe.placement == TemplatePlacement::Parallel) {
    for (const auto& track : recipe.tracks) {
      for (const auto& clip : track.clips) {
        const auto duration = AvailableDuration(detail::FindAsset(*base, resolve_slot(clip)), clip, track.kind);
        if (!parallel_duration || detail::Before(duration, *parallel_duration)) parallel_duration = duration;
      }
    }
  }

  Project draft(*base);
  auto prepared = draft.Edit([&](ProjectEdit& edit) {
    std::map<std::string, ClipId, std::less<>> clips;
    for (const auto& track : recipe.tracks) {
      const auto track_id = edit.AddTrack(track.kind);
      std::optional<TrackId> original_audio;
      media::MediaTimestamp next;
      for (const auto& clip : track.clips) {
        const auto& resource = resolve_slot(clip);
        const auto& asset = detail::FindAsset(*base, resource);
        const auto source = std::ranges::find(asset.tracks, track.kind, &AssetTrack::kind);
        if (source == asset.tracks.end()) throw std::invalid_argument("Incompatible template slot: " + clip.slot);
        auto timeline = clip.timeline;
        auto source_end = clip.source.end;
        if (recipe.placement != TemplatePlacement::Explicit) {
          const auto duration = parallel_duration ? *parallel_duration : AvailableDuration(asset, clip, track.kind);
          const auto start = parallel_duration ? media::MediaTimestamp{} : next;
          timeline = {start, detail::Add(start, duration)};
          next = timeline.end;
          source_end = detail::Add(clip.source.start, duration);
        }
        const TimeRange range{detail::Add(source->presentation.start, clip.source.start),
                              detail::Add(source->presentation.start, source_end)};
        const auto id = edit.InsertClip(track_id, {{resource, source->id, range, clip.reverse}, timeline});
        if (!clip.key.empty() && !clips.emplace(clip.key, id).second)
          throw std::invalid_argument("Duplicate template clip key: " + clip.key);
        edit.SetLayout(id, clip.destination);
        if (clip.source_crop) edit.SetSourceCrop(id, clip.source_crop);
        if (clip.background) edit.SetBackground(id, clip.background);
        for (const auto& sticker : clip.stickers) edit.AddSticker(id, sticker);
        if (clip.include_original_audio) {
          if (track.kind != media::TrackKind::Video) throw std::invalid_argument("Only video can request original audio");
          const auto sound = std::ranges::find(asset.tracks, media::TrackKind::Audio, &AssetTrack::kind);
          if (sound != asset.tracks.end()) {
            if (!original_audio) original_audio = edit.AddTrack(media::TrackKind::Audio);
            const TimeRange audio_range{detail::Add(sound->presentation.start, clip.source.start),
                                        detail::Add(sound->presentation.start, source_end)};
            const auto audio = edit.InsertClip(*original_audio, {{resource, sound->id, audio_range, clip.reverse}, timeline});
            edit.BindAudio(audio, id);
          }
        }
        if (clip.voice_effect) edit.SetVoiceEffect(id, clip.voice_effect);
      }
    }
    for (auto transition : recipe.transitions) {
      const auto from = clips.find(transition.from), to = clips.find(transition.to);
      if (from == clips.end() || to == clips.end()) throw std::invalid_argument("A template transition references an unknown clip");
      edit.SetTransition(from->second, to->second, std::move(transition.spec));
    }
    for (const auto& overlay : recipe.overlays) edit.AddOverlay(overlay);
  });
  return EditTemplatePlan(base, std::move(prepared));
}

std::shared_ptr<const ProjectSnapshot> ApplyEditTemplate(Project& project, const EditTemplatePlan& plan) {
  if (project.editing_ || project.Snapshot() != plan.base_) throw std::invalid_argument("Template plan is stale for this project");
  for (const auto& track : plan.prepared_->tracks) {
    for (const auto& clip : track.clips) {
      if (!detail::FindAsset(*plan.prepared_, clip.placement.source.asset).file.IsFile())
        throw std::invalid_argument("A template asset became unavailable after planning");
    }
  }
  return project.Commit(*plan.prepared_);
}

} // namespace huxerui::avkit
