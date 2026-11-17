#include <huxerui/avkit/edit_template.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

#include "support/media_time.h"
#include "project_internal.h"

namespace huxerui::avkit {

EditTemplatePlan::EditTemplatePlan(std::shared_ptr<const ProjectSnapshot> base,
                                   std::shared_ptr<const ProjectSnapshot> prepared)
    : base_(std::move(base)), prepared_(std::move(prepared)) {}

EditTemplatePlan PlanEditTemplate(const Project& project, const EditTemplateRecipe& recipe, const EditTemplateInputs& inputs) {
  const auto base = project.Snapshot();
  const auto& reference = recipe.reference;
  if (!base->tracks.empty() || base->canvas != recipe.canvas || recipe.slots.empty() || recipe.tracks.empty() ||
      reference.pack.empty() || reference.revision.empty() || reference.key.empty()) {
    throw std::invalid_argument("Template assembly needs a versioned recipe and an empty project with the recipe canvas");
  }
  std::map<std::string, AssetId> bindings;
  for (const auto& slot : recipe.slots) {
    if (slot.empty() || !bindings.emplace(slot, AssetId{}).second) throw std::invalid_argument("Invalid template slot name");
  }
  for (const auto& binding : inputs.bindings) {
    auto slot = bindings.find(binding.slot);
    if (slot == bindings.end() || slot->second.value || !binding.asset.value)
      throw std::invalid_argument("Invalid explicit template slot binding");
    slot->second = binding.asset;
  }
  auto selected = inputs.ordered_assets.begin();
  for (const auto& slot : recipe.slots) {
    auto& bound = bindings.at(slot);
    if (!bound.value) {
      while (selected != inputs.ordered_assets.end() &&
          std::ranges::any_of(bindings, [&](const auto& entry) { return entry.second == *selected; })) ++selected;
      if (selected == inputs.ordered_assets.end()) throw std::invalid_argument("Missing template slot: " + slot);
      bound = *selected++;
    }
    if (!detail::FindAsset(*base, bound).source.file.IsFile()) throw std::invalid_argument("Unavailable template slot: " + slot);
  }

  Project draft(*base);
  auto prepared = draft.Edit([&](ProjectEdit& edit) {
    for (const auto& track : recipe.tracks) {
      const auto track_id = edit.AddTrack(track.kind);
      for (const auto& clip : track.clips) {
        const auto binding = bindings.find(clip.slot);
        if (binding == bindings.end()) throw std::invalid_argument("Unknown template slot: " + clip.slot);
        const auto& asset = detail::FindAsset(*base, binding->second);
        const auto source = std::ranges::find(asset.source.tracks, track.kind, &AssetTrack::kind);
        if (source == asset.source.tracks.end()) throw std::invalid_argument("Incompatible template slot: " + clip.slot);
        const TimeRange range{detail::Add(source->presentation.start, clip.source.start),
                              detail::Add(source->presentation.start, clip.source.end)};
        const auto id = edit.InsertClip(track_id, {{asset.id, source->id, range}, clip.timeline});
        edit.SetLayout(id, clip.destination);
        for (const auto& sticker : clip.stickers) edit.AddSticker(id, sticker);
      }
    }
  });
  return EditTemplatePlan(base, std::move(prepared));
}

std::shared_ptr<const ProjectSnapshot> ApplyEditTemplate(Project& project, const EditTemplatePlan& plan) {
  if (project.editing_ || project.Snapshot() != plan.base_) throw std::invalid_argument("Template plan is stale for this project");
  for (const auto& track : plan.prepared_->tracks) {
    for (const auto& clip : track.clips) {
      if (!detail::FindAsset(*plan.prepared_, clip.placement.source.asset).source.file.IsFile())
        throw std::invalid_argument("A template asset became unavailable after planning");
    }
  }
  return project.Commit(*plan.prepared_);
}

} // namespace huxerui::avkit
