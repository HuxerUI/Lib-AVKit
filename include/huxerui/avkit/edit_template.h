#pragma once

/// @file
/// @brief Preparation and application of reusable edit recipes.
///
/// Link AVKit::Core. Plan against an empty timeline with imported assets and a matching canvas.
/// Applying a valid plan publishes the assembled editable tracks in a single undoable transaction.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/avkit/project.h>

namespace huxerui::avkit {

/// @brief Clip layout and effects assembled from a named input slot.
struct TemplateClip {
  /// Recipe slot that supplies the clip's imported asset.
  std::string slot;
  TimeRange source; ///< Trim offsets from the selected track's presentation start.
  TimeRange timeline; ///< Destination interval on the project clock.
  /// Video placement in normalized canvas coordinates.
  Rect destination{0, 0, 1, 1};
  /// Clip-local drawable instances attached on assembly.
  std::vector<OverlayInstance> stickers;
  /// Optional background effect. Pack-relative masks resolve against the recipe.
  std::optional<BackgroundReplacementSpec> background;
  /// Sample source time in descending order.
  bool reverse{};
  bool include_original_audio{}; ///< Assemble linked audio from the same slot for this video clip.
  /// Applies to audio on this clip or its linked original sound.
  std::optional<VoiceEffect> voice_effect;
  /// Optional video crop and mirror in oriented source coordinates.
  std::optional<SourceCrop> source_crop;
  /// Optional stable local name referenced by transitions in this recipe.
  std::string key;
};

/// @brief Template track and its clip definitions in assembly order.
struct TemplateTrack {
  /// Track kind used to select each bound asset's source track.
  media::TrackKind kind;
  /// Clips assembled onto this track.
  std::vector<TemplateClip> clips;
};

/// @brief Placement of clips whose trim ends are capped by available source media.
enum class TemplatePlacement { Explicit, Sequence, Parallel };

/// @brief Transition between two named recipe clips.
struct TemplateTransition {
  std::string from, to;
  TransitionSpec spec;
};

/// @brief Asset in a pack that supplies a slot without asking the user to choose a file.
struct TemplateResourceSlot {
  /// Slot name referenced by the recipe's clips.
  std::string slot;
  /// Media file key in the template catalog.
  ResourceRef resource;
  /// Audio uses the audio importer. A visual slot accepts a movie or still image.
  media::TrackKind kind;
};

/// @brief Recipe with a canvas, input slots, ordered tracks, and canvas overlays.
struct EditTemplateRecipe {
  /// Catalog key of the recipe.
  ResourceRef reference;
  /// Authored canvas size of the prepared project, in pixels.
  PixelSize canvas_size;
  /// Unique input names in the order used for automatic binding.
  std::vector<std::string> slots;
  /// Tracks in assembly order, with video layers composed back to front.
  std::vector<TemplateTrack> tracks;
  /// Canvas overlays added after assembling the clips.
  std::vector<OverlayInstance> overlays;
  /// Explicit intervals are preserved. Sequence and Parallel fit the available source duration.
  TemplatePlacement placement{};
  /// Transitions reference local clip keys and use assembled project time.
  std::vector<TemplateTransition> transitions;
  /// Built-in file-backed slots, resolved from the catalog when creating the project.
  std::vector<TemplateResourceSlot> resource_slots;
};

/// @brief Retrieves a registered recipe by its exact resource identity.
/// @param catalog Published template and effect catalog.
/// @param reference Catalog key of the recipe.
const EditTemplateRecipe& OpenEditTemplate(const ResourceCatalog& catalog, const ResourceRef& reference);

/// @brief Creates an editable document from imported inputs in slot order.
/// @param catalog Published template and effect catalog.
/// @param reference Catalog key of the recipe.
/// @param assets Imported media to bind to the template's named slots.
Project CreateTemplateProject(const ResourceCatalog& catalog, const ResourceRef& reference,
                              std::vector<AssetSource> assets);

/// @brief Explicit binding of an imported asset to a template input slot.
struct TemplateSlotBinding {
  /// Input name declared by the recipe.
  std::string slot;
  /// Key of media imported into the target project.
  ResourceRef asset;
};

/// @brief Preferred slot bindings and remaining assets in selection order.
struct EditTemplateInputs {
  /// Candidate assets used to fill slots without explicit bindings.
  std::vector<ResourceRef> ordered_assets;
  /// Explicit assignments applied before automatic binding.
  std::vector<TemplateSlotBinding> bindings;
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
/// @param project Project with a matching canvas, imported sources, and an empty timeline.
/// @param recipe Versioned slot definitions, track layout, and overlay content.
/// @param inputs Explicit slot bindings and additional assets in selection order.
/// @return Prepared plan. The project retains its current revision until the plan is applied.
/// @throws std::invalid_argument If binding, timing, or project validation fails.
EditTemplatePlan PlanEditTemplate(const Project& project, const EditTemplateRecipe& recipe, const EditTemplateInputs& inputs);

/// @brief Applies an assembly as one edit against the original project revision.
/// @param project Project against which the plan was prepared.
/// @param plan Validated assembly tied to that exact base revision.
/// @return The published project revision.
/// @throws std::invalid_argument If the base revision is stale or any required file is unavailable.
std::shared_ptr<const ProjectSnapshot> ApplyEditTemplate(Project& project, const EditTemplatePlan& plan);

} // namespace huxerui::avkit
