#pragma once

/// @file
/// @brief Editable projects, immutable snapshots, source mapping, and persisted effect instances.
///
/// Link AVKit::Core. Edit a project on one owning sequence. Consumers can retain a committed
/// snapshot for independent playback, asset sampling, or export.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <huxerui/avkit/component.h>
#include <huxerui/avkit/tracking_anchor.h>
#include <huxerui/file.h>
#include <huxerui/media_container.h>

namespace huxerui::avkit {

class ResourceCatalog;

/// @brief A track ID in an imported file with its available presentation interval.
struct AssetTrack {
  /// Track identifier in the source container.
  std::uint32_t id;
  /// Media kind used to select a compatible timeline track.
  media::TrackKind kind;
  /// Available presentation interval on the source clock.
  TimeRange presentation;
  /// Visible dimensions after applying the source orientation, when known.
  std::optional<PixelSize> display_size;

  bool operator==(const AssetTrack&) const = default;
};

/// @brief Source file and track metadata, retained when the source file becomes unavailable.
struct AssetSource {
  /// Local file path, retained even if the file later becomes unavailable.
  File file;
  /// Imported tracks. The project accepts one track of each supported kind.
  std::vector<AssetTrack> tracks;
  /// A still uses its visual track duration as an insertion default and can be extended in an edit.
  bool still_image{};

  bool operator==(const AssetSource&) const = default;
};

/// @brief Transition instance between two adjacent visual clips.
struct TransitionInstance {
  /// Stable ID preserved when editing the overlap or its effect.
  InstanceId id;
  /// Outgoing and incoming clip IDs, respectively.
  ClipId from, to;
  /// Project-time overlap and the selected transition effect.
  TransitionSpec spec;

  bool operator==(const TransitionInstance&) const = default;
};

/// @brief Editor role for a canvas overlay. Content determines its render path.
enum class CanvasOverlayKind { Text, Subtitle, Sticker };

/// @brief A visual instance on the canvas, in a clip, or in a capture scene.
struct OverlayInstance {
  /// Stable ID used by edits, hit testing, and PAG caches.
  InstanceId id;
  /// Editor role used to group and present overlay controls.
  CanvasOverlayKind kind{CanvasOverlayKind::Sticker};
  /// Half-open interval on the owning clock. Empty spans the owning clip or live scene.
  TimeRange range;
  /// Placement rectangle in normalized canvas coordinates, before rotation.
  Rect destination;
  /// Still or PAG content. Text and subtitle overlays use PAG content.
  OverlayContent content;
  /// Opacity multiplier in [0, 1] applied during composition.
  float opacity{1.0F};
  float rotation_degrees{}; ///< Clockwise about the center of the destination in canvas pixels.
  /// Optional clip-local landmark anchor. When present, the destination is expressed in anchor space.
  std::optional<analysis::TrackingAnchor> anchor;
  /// Optional clip-local comparison against its metric depth map.
  std::optional<StickerDepth> depth;

  bool operator==(const OverlayInstance&) const = default;
};

/// @brief Authored background fill and its selected coverage source.
struct BackgroundReplacementSpec {
  /// Straight RGBA fill behind the covered foreground.
  Color color;
  /// Exactly one mask source, projected as a separate component on the clip entity.
  std::variant<PortraitMask, StaticMask, AnimatedMask> mask;

  bool operator==(const BackgroundReplacementSpec&) const = default;
};

/// @brief Clip placement, normalized canvas bounds, and clip-local stickers.
struct TimelineClip {
  /// Stable identifier for edits and runtime projection.
  ClipId id;
  /// Source trim and playback direction mapped to the project interval.
  ClipTiming placement;
  /// Normalized canvas rectangle for a video clip.
  Rect destination{0, 0, 1, 1};
  /// Clip-local layers in back-to-front order.
  std::vector<OverlayInstance> stickers;
  /// Color and coverage source applied to this video clip.
  std::optional<BackgroundReplacementSpec> background;
  /// Optional source-space crop and mirroring applied before placement.
  std::optional<SourceCrop> source_crop;
  /// On an audio clip, identifies the video clip that owns its original sound.
  std::optional<ClipId> audio_of;
  AudioMix audio_mix;
  /// Crop the source proportionally to cover the destination rectangle.
  bool fill{};
  float rotation_degrees{}; ///< Clockwise about the center of the destination in canvas pixels.
  std::optional<VoiceEffect> voice_effect; ///< Belongs to this clip's own or linked original audio.

  bool operator==(const TimelineClip&) const = default;
};

/// @brief One visual or audio track and its clips in project time order.
struct TimelineTrack {
  /// Stable identifier used when inserting clips.
  TrackId id;
  /// Kind shared by all clips on this track.
  media::TrackKind kind;
  /// Clips sorted by their start time on the project clock.
  std::vector<TimelineClip> clips;

  bool operator==(const TimelineTrack&) const = default;
};

/// @brief Committed project data. Track order defines compositing order from bottom to top.
/// A Project exposes snapshots through shared const handles.
struct ProjectSnapshot {
  /// Authored canvas dimensions in pixels. Preview and export can use different render target sizes.
  PixelSize canvas_size;
  /// Published revision and the next unused project identifier.
  std::uint64_t revision{}, next_id{1};
  /// Immutable catalog of media registered in this project.
  std::shared_ptr<const ResourceCatalog> resources;
  /// Project tracks. Video tracks compose in stored order.
  std::vector<TimelineTrack> tracks;
  /// Transitions between adjacent video clips.
  std::vector<TransitionInstance> transitions;
  /// Canvas layers drawn over the video tracks in stored order.
  std::vector<OverlayInstance> overlays;

  /// @brief Returns the full project interval, including overlays beyond the last clip.
  /// @return [0, end), where end is the latest clip or overlay end. An empty project has zero duration.
  TimeRange Range() const;

  /// @brief Returns keys of media whose source files are unavailable.
  std::vector<ResourceRef> MissingAssets() const;
};

/// @brief Transaction-local draft operations. Use only during the Project::Edit callback.
class ProjectEdit {
public:
  ProjectEdit(const ProjectEdit&) = delete;

  ProjectEdit& operator=(const ProjectEdit&) = delete;

  /// @brief Imports a file and its track metadata into the draft.
  /// @param source Local media file and inspected track metadata.
  /// @param resource Explicit key, such as a template media key. Empty allocates a project media key.
  /// @return Resource reference used by clips, templates, and asset sampling.
  /// @throws std::invalid_argument If the file is unavailable or the track metadata is invalid.
  ResourceRef Import(AssetSource source, ResourceRef resource = {});

  /// @brief Appends a video or audio track and returns its stable ID.
  /// @param kind Video for a composited track or audio for a mixed track.
  TrackId AddTrack(media::TrackKind kind);

  /// @brief Creates a clip on the supplied track and returns its stable ID.
  /// @param track ID of an existing track whose kind matches the source.
  /// @param placement Selected asset track, trim, direction, and project interval. The intervals must have equal duration.
  ClipId InsertClip(TrackId track, ClipTiming placement);

  /// @brief Places a still image on a visual track without opening a video decoder.
  /// @param track Destination visual track.
  /// @param asset Imported still image.
  /// @param start Position on the project clock.
  /// @param duration Positive interval. Omit to use the asset's configured insertion duration.
  /// @return Stable ID for this use of the image.
  ClipId InsertImage(TrackId track, ResourceRef asset, media::MediaTimestamp start,
                     std::optional<media::MediaTimestamp> duration = {});

  /// @brief Changes the placement or trim of an existing clip.
  /// Updates the project interval, direction, and source trim boundaries of linked original audio in the same edit.
  /// @param clip Stable ID of the clip to edit.
  /// @param placement Replacement source and project timing.
  void SetClip(ClipId clip, ClipTiming placement);

  /// @brief Trims a main-track clip and shifts following clips and their transitions in the same edit.
  /// @param clip Stable identifier on the first visual track.
  /// @param placement New trim and project placement.
  void RippleTrim(ClipId clip, ClipTiming placement);

  /// @brief Sets normalized canvas bounds for a clip.
  /// @param clip Video clip to position.
  /// @param destination Normalized canvas rectangle with positive extent. The canvas clips overflow.
  void SetLayout(ClipId clip, Rect destination);

  /// @brief Sets clockwise picture rotation around the center of the destination.
  /// @param clip Video clip whose rotation changes.
  /// @param degrees Finite clockwise angle in degrees, evaluated using the physical canvas aspect ratio.
  void SetClipRotation(ClipId clip, float degrees);

  /// @brief Removes a clip, its bound original audio, and its transitions.
  /// @param clip Stable clip ID. Removing linked audio also clears its owner's voice effect.
  void RemoveClip(ClipId clip);

  /// @brief Chooses between fitting the complete image and covering the clip rectangle.
  /// @param clip Video clip whose picture fit changes.
  /// @param fill True to cover the rectangle by cropping. False fits the complete picture.
  void SetClipFill(ClipId clip, bool fill);

  /// @brief Sets the authored canvas dimensions in pixels.
  /// @param canvas_size Positive authored width and height in pixels. Existing normalized layouts scale with the canvas.
  void SetCanvasSize(PixelSize canvas_size);

  /// @brief Attaches audio to its video clip, or exposes it as independent audio when detached.
  /// The edit transfers the voice effect between the original audio record and its video owner.
  /// @param audio Audio clip to attach or detach.
  /// @param video Video clip using the same asset, project interval, and playback direction. Pass empty to detach.
  void BindAudio(ClipId audio, std::optional<ClipId> video);

  /// @brief Sets the mix parameters for an audio clip.
  /// @param audio ID of the audio clip, including a clip that stores linked original sound.
  /// @param mix Gain, mute state, and fades. Each fade must fit within the clip duration.
  void SetAudioMix(ClipId audio, AudioMix mix);

  /// @brief Sets the voice effect on an independent audio clip or the owner of linked audio.
  /// @param clip Independent audio clip, a linked audio record, or the video clip that owns it.
  /// @param effect Processing to apply. Pass empty to clear it.
  void SetVoiceEffect(ClipId clip, std::optional<VoiceEffect> effect);

  /// @brief Appends a canvas overlay at the top of the overlay order.
  /// @param overlay Overlay content and placement. AVKit assigns its ID.
  /// @return Stable ID of the new instance.
  InstanceId AddOverlay(OverlayInstance overlay);

  /// @brief Replaces an existing canvas overlay in place.
  /// @param overlay Complete replacement value carrying the existing instance ID.
  void SetOverlay(OverlayInstance overlay);

  /// @brief Removes one canvas overlay from the draft.
  /// @param id Stable ID of the overlay to remove.
  void RemoveOverlay(InstanceId id);

  /// @brief Sets or removes a visual clip's background and its coverage source.
  /// @param clip Video clip that receives the background.
  /// @param background Replacement color and mask source. Pass empty to clear the effect.
  void SetBackground(ClipId clip, std::optional<BackgroundReplacementSpec> background);

  /// @brief Sets an explicit oriented source crop and horizontal mirroring for a video clip.
  /// @param clip Video clip whose source geometry changes.
  /// @param crop Normalized crop and mirror state before placement. Pass empty to clear the explicit crop.
  void SetSourceCrop(ClipId clip, std::optional<SourceCrop> crop);

  /// @brief Changes the resource of a packaged mask while preserving its channel, playback, and inversion.
  /// @param clip Video clip with a static or animated packaged mask.
  /// @param resource Versioned reference to a resource of the same mask kind.
  /// @throws std::invalid_argument If the clip has no packaged mask binding.
  void SetBackgroundMaskResource(ClipId clip, ResourceRef resource);

  /// @brief Attaches an independently editable sticker to a visual clip.
  /// @param clip Owning video clip.
  /// @param spec Content, clip-local placement, and optional timing, tracking, and depth settings.
  /// @return Stable ID of the new sticker instance.
  InstanceId AddSticker(ClipId clip, OverlayInstance spec);

  /// @brief Sets an instance's bounds in its clip or anchor coordinates.
  /// @param instance ID returned by AddSticker.
  /// @param destination Positive rectangle in normalized clip coordinates, or landmark anchor coordinates when tracked.
  void SetStickerLayout(InstanceId instance, Rect destination);

  /// @brief Replaces all slot values on an animated sticker. Pass an empty set to restore authored content.
  /// @param instance ID of an existing PAG sticker.
  /// @param replacements Complete set of named text and image overrides.
  /// @throws std::invalid_argument If the sticker does not have PAG content.
  void SetStickerReplacements(InstanceId instance, PagReplacements replacements);

  /// @brief Creates or updates a transition on one visual track.
  /// @param from Outgoing clip.
  /// @param to Adjacent incoming clip on the same track.
  /// @param spec Project-time overlap and the mask or PAG effect to use.
  /// @return The stable new or existing transition ID.
  InstanceId SetTransition(ClipId from, ClipId to, TransitionSpec spec);

  /// @brief Changes the overlap and scales mask key times if the duration changes.
  /// Moving the interval without resizing it keeps the existing local key times.
  /// @param instance Stable transition ID.
  /// @param range New half-open overlap on the project clock.
  void SetTransitionRange(InstanceId instance, TimeRange range);

  /// @brief Replaces the feather value and its optional local keyframes.
  /// @param instance ID of a mask transition.
  /// @param feather Base value in [0, 1] used when the curve is empty.
  /// @param keys Optional curve with strictly increasing times relative to the start of the transition.
  /// @throws std::invalid_argument If the transition does not have a mask effect.
  void SetTransitionFeather(InstanceId instance, float feather, std::vector<Keyframe<float>> keys = {});

  /// @brief Removes the transition from the draft.
  /// @param instance Stable ID of the transition to remove.
  void RemoveTransition(InstanceId instance);

  /// @brief Updates the stored location of an imported media file.
  /// @param asset Key shared by all uses of this media.
  /// @param file Available file at the new location of the original media.
  void Relink(const ResourceRef& asset, File file);

private:
  friend class Project;

  explicit ProjectEdit(ProjectSnapshot& draft) : draft_(draft) {}

  std::uint64_t AllocateId();
  TimelineClip& Clip(ClipId clip);
  OverlayInstance& Sticker(InstanceId instance);

  ProjectSnapshot& draft_;
};

struct EditTemplateRecipe;
struct EditTemplateInputs;
class EditTemplatePlan;

/// @brief Owns project revisions and in-memory undo and redo history on one sequence.
class Project {
public:
  /// @brief Creates an empty project with the given physical canvas size.
  /// @param canvas_size Positive authored width and height in pixels.
  explicit Project(PixelSize canvas_size);

  /// @brief Starts an editable project from a validated snapshot with empty undo and redo history.
  /// @param snapshot Initial project data with valid stable IDs and timeline layout.
  /// @return Editable project holding the supplied revision.
  /// @throws std::invalid_argument If the snapshot violates the project constraints.
  static Project FromSnapshot(ProjectSnapshot snapshot);

  Project(const Project&) = delete;

  Project& operator=(const Project&) = delete;

  Project(Project&&) noexcept = default;

  Project& operator=(Project&&) noexcept = default;

  /// @brief Returns a retained immutable view of the current revision.
  std::shared_ptr<const ProjectSnapshot> Snapshot() const { return current_; }

  /// @brief Runs changes against a draft and publishes a new revision after validation.
  /// @param changes Synchronous callback. The ProjectEdit reference is valid only during this call.
  /// @return The committed revision. A rejected or interrupted edit preserves the previous revision.
  std::shared_ptr<const ProjectSnapshot> Edit(const std::function<void(ProjectEdit&)>& changes);

  /// @brief Returns whether there is an edit to undo.
  bool CanUndo() const noexcept { return !undo_.empty(); }

  /// @brief Returns whether there is an undone edit to restore.
  bool CanRedo() const noexcept { return !redo_.empty(); }

  /// @brief Restores the previous edit as a new revision, or returns false if history is empty.
  bool Undo();

  /// @brief Restores the next undone edit as a new revision, or returns false if history is empty.
  bool Redo();

  /// @brief Saves the current revision and stable identifiers to a local project document.
  /// The writer publishes the completed document from a sibling temporary file.
  /// @param destination Local .avkit document to publish. The path must differ from each imported asset path.
  /// @throws std::runtime_error On an I/O failure before publication.
  void Save(const File& destination) const;

  /// @brief Opens a supported project document. Unavailable source paths remain available for relinking.
  /// @param source Local document using the current v1 layout.
  /// @return An editable project with the stored revision and an empty undo history.
  /// @throws std::runtime_error If the document cannot be read or parsed.
  static Project Open(const File& source);

private:
  friend EditTemplatePlan PlanEditTemplate(const Project&, const EditTemplateRecipe&, const EditTemplateInputs&);

  friend std::shared_ptr<const ProjectSnapshot> ApplyEditTemplate(Project&, const EditTemplatePlan&);

  explicit Project(ProjectSnapshot snapshot);

  std::shared_ptr<const ProjectSnapshot> Commit(ProjectSnapshot draft);

  bool Restore(std::vector<std::shared_ptr<const ProjectSnapshot>>& from,
               std::vector<std::shared_ptr<const ProjectSnapshot>>& to);

  std::shared_ptr<const ProjectSnapshot> current_;
  std::vector<std::shared_ptr<const ProjectSnapshot>> undo_, redo_;
  bool editing_{};
};

} // namespace huxerui::avkit
