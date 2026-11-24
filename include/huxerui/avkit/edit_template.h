#pragma once

/// @file
/// @brief Preparation and application of reusable edit recipes.
///
/// Link AVKit::Core. Plan against an empty timeline with imported assets and a matching canvas.
/// Applying a valid plan publishes the assembled editable tracks in a single undoable transaction.

#include <memory>
#include <string>
#include <vector>

#include <huxerui/avkit/project.h>

namespace huxerui::avkit {

/// @brief Clip layout and effects assembled from a named input slot.
struct TemplateClip {
  std::string slot;
  TimeRange source; ///< Trim offsets from the selected track's presentation start.
  TimeRange timeline; ///< Destination interval on the project clock.
  Rect destination{0, 0, 1, 1};
  std::vector<StickerSpec> stickers;
  std::optional<BackgroundReplacement> background;
  bool reverse{};
  bool include_original_audio{}; ///< Assemble linked audio from the same slot for this video clip.
};

/// @brief Template track and its clip definitions in assembly order.
struct TemplateTrack {
  media::TrackKind kind;
  std::vector<TemplateClip> clips;
};

/// @brief Versioned recipe with a canvas, input slots, and ordered tracks.
struct EditTemplateRecipe {
  ResourceRef reference;
  PixelSize canvas;
  std::vector<std::string> slots;
  std::vector<TemplateTrack> tracks;
};

/// @brief Explicit binding of an imported asset to a template input slot.
struct SlotBinding { std::string slot; AssetId asset; };

/// @brief Preferred slot bindings and remaining assets in selection order.
struct EditTemplateInputs {
  std::vector<AssetId> ordered_assets;
  std::vector<SlotBinding> bindings;
};

/// @brief Validated assembly tied to one exact project revision.
class EditTemplatePlan {
public:
  /// @brief Returns the candidate snapshot for inspection before applying it.
  const ProjectSnapshot& Prepared() const { return *prepared_; }

private:
  friend EditTemplatePlan PlanEditTemplate(const Project&, const EditTemplateRecipe&, const EditTemplateInputs&);
  friend std::shared_ptr<const ProjectSnapshot> ApplyEditTemplate(Project&, const EditTemplatePlan&);
  EditTemplatePlan(std::shared_ptr<const ProjectSnapshot> base, std::shared_ptr<const ProjectSnapshot> prepared);
  std::shared_ptr<const ProjectSnapshot> base_, prepared_;
};

/// @brief Resolves explicit bindings, fills remaining slots in selection order, and validates the assembly.
/// @return Prepared plan. The project retains its current revision until the plan is applied.
EditTemplatePlan PlanEditTemplate(const Project& project, const EditTemplateRecipe& recipe, const EditTemplateInputs& inputs);

/// @brief Applies an assembly as one edit against the original project revision.
/// @return The published project revision.
std::shared_ptr<const ProjectSnapshot> ApplyEditTemplate(Project& project, const EditTemplatePlan& plan);

} // namespace huxerui::avkit
