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
#include <vector>

#include <huxerui/avkit/analysis.h>
#include <huxerui/avkit/animation.h>
#include <huxerui/avkit/render.h>
#include <huxerui/file.h>
#include <huxerui/media_container.h>

namespace huxerui::avkit {

/// @brief Rotates normalized canvas positions around the destination center in physical canvas space.
Transform2D CanvasRotation(Rect destination, PixelSize canvas, float degrees);

/// @brief Half-open media interval [start, end) with exact rational boundaries.
struct TimeRange {
  media::MediaTimestamp start, end;
  bool operator==(const TimeRange&) const = default;
};

/// @brief Stable project identifier for an imported asset.
struct AssetId { std::uint64_t value{}; bool operator==(const AssetId&) const = default; };
/// @brief Stable project identifier for a timeline track.
struct TrackId { std::uint64_t value{}; bool operator==(const TrackId&) const = default; };
/// @brief Stable project identifier for a placement of an asset track.
struct ClipId { std::uint64_t value{}; bool operator==(const ClipId&) const = default; };
/// @brief Stable project identifier for a sticker or transition instance.
struct InstanceId { std::uint64_t value{}; bool operator==(const InstanceId&) const = default; };

/// @brief Versioned asset-pack reference. Multiple instances can share one resource.
struct ResourceRef {
  std::string pack, revision, key;
  bool operator==(const ResourceRef&) const = default;
};

/// @brief A track ID in an imported file with its available presentation interval.
struct AssetTrack {
  std::uint32_t id;
  media::TrackKind kind;
  TimeRange presentation;
  /// Visible dimensions after applying the source orientation, when known.
  std::optional<PixelSize> display_size;
  bool operator==(const AssetTrack&) const = default;
};

/// @brief Source file and track metadata, retained when the source file becomes unavailable.
struct AssetReference {
  File file;
  std::vector<AssetTrack> tracks;
  bool operator==(const AssetReference&) const = default;
};

/// @brief Imported asset entry. Relinking preserves id and advances content_revision.
struct AssetInfo {
  AssetId id;
  AssetReference source;
  std::uint64_t content_revision{};
  bool operator==(const AssetInfo&) const = default;
};

/// @brief Trim and playback direction for a clip's selected source track.
struct TrackSource {
  AssetId asset;
  std::uint32_t track;
  TimeRange range;
  bool reverse{};
  bool operator==(const TrackSource&) const = default;
};

/// @brief Maps a source trim to an equal-duration project interval.
struct ClipPlacement {
  TrackSource source;
  TimeRange timeline;
  bool operator==(const ClipPlacement&) const = default;
};

/// @brief Maps a project subrange to its source interval.
/// @return Ascending source boundaries for either playback direction.
TimeRange MapClipToSource(const ClipPlacement& clip, TimeRange project);
/// @brief Maps a source subrange to its project interval.
/// @return Ascending project boundaries for either playback direction.
TimeRange MapClipToProject(const ClipPlacement& clip, TimeRange source);

/// @brief Text for a named slot in an animation resource. An empty string replaces the authored text.
struct StickerTextReplacement {
  std::string slot;
  std::string text;
  bool operator==(const StickerTextReplacement&) const = default;
};

/// @brief Versioned image resource for a named slot in an animation resource.
struct StickerImageReplacement {
  std::string slot;
  ResourceRef image;
  bool operator==(const StickerImageReplacement&) const = default;
};

/// @brief Per-instance values for named animation slots. Omitted slots use their authored content.
struct StickerReplacements {
  std::vector<StickerTextReplacement> texts;
  std::vector<StickerImageReplacement> images;
  bool Empty() const { return texts.empty() && images.empty(); }
  bool operator==(const StickerReplacements&) const = default;
};

/// @brief Sticker resource, layout, and optional tracking and animation settings.
struct StickerSpec {
  ResourceRef resource;
  /// Bounds in the owning clip, or in tracking anchor coordinates when an anchor is present.
  Rect destination;
  std::optional<analysis::TrackingAnchor> anchor;
  /// Positive optical-axis depth of a flat sticker in meters. Uses the owning clip's metric depth map.
  std::optional<float> depth_meters;
  /// When set, resolve the resource as an animation on the owning clip's project clock.
  std::optional<AnimationPlayback> playback;
  StickerReplacements replacements;
  bool operator==(const StickerSpec&) const = default;
};

/// @brief Independently editable use of a sticker resource.
struct StickerInstance {
  InstanceId id;
  StickerSpec spec;
  bool operator==(const StickerInstance&) const = default;
};

/// @brief How a key approaches the next value.
enum class Interpolation { Hold, Linear };

/// @brief Parameter value and interpolation at a local media time.
template<class T> struct Keyframe {
  media::MediaTimestamp time;
  T value;
  Interpolation interpolation{Interpolation::Linear};
  bool operator==(const Keyframe&) const = default;
};

/// @brief Masked or authored animation transition for an overlap on one visual track.
struct TransitionSpec {
  TimeRange range;
  ResourceRef mask; ///< Coverage resource for a masked transition.
  float feather{0.5F}; ///< Base feather in [0, 1], used when the curve is empty.
  bool inverted{};
  /// Times are relative to the transition start. A final key can sit at the exclusive end.
  std::vector<Keyframe<float>> feather_keys;
  /// Versioned animation with outgoing and incoming video slots. Leave the mask empty when set.
  std::optional<ResourceRef> animation;
  bool operator==(const TransitionSpec&) const = default;
};

/// @brief Transition instance between two adjacent visual clips.
struct TransitionInfo {
  InstanceId id;
  ClipId from, to;
  TransitionSpec spec;
  bool operator==(const TransitionInfo&) const = default;
};

/// @brief A versioned static coverage mask in oriented, pre-mirror source space.
struct StaticMask {
  ResourceRef resource;
  bool inverted{};
  bool operator==(const StaticMask&) const = default;
};

/// @brief Playback at the end of a clip mask sequence.
enum class MaskPlayback {
  HoldLast, ///< Retains the last frame for the rest of the clip.
  Loop,     ///< Restarts at the exact sequence duration.
};

/// @brief A versioned coverage sequence sampled from the clip's project-time start.
struct AnimatedMask {
  ResourceRef resource;
  MaskPlayback playback{MaskPlayback::HoldLast};
  bool inverted{};
  bool operator==(const AnimatedMask&) const = default;
};

/// @brief Clip coverage over a solid or transparent background.
struct BackgroundReplacement {
  Color color;
  /// The rectangle fills the clip destination. Coordinates refer to the oriented, pre-mirror source.
  Rect source_crop{0, 0, 1, 1};
  bool mirrored{};
  /// Stable portrait model ID. An empty value selects the provider's default.
  std::string model_id;
  /// Selects packaged coverage in place of portrait analysis while bound.
  /// Bind at most one packaged mask.
  std::optional<StaticMask> static_mask;
  std::optional<AnimatedMask> animated_mask;
  bool operator==(const BackgroundReplacement&) const = default;
};

/// @brief Clip audio gain and linear fades in project time.
struct AudioMix {
  float gain{1.0F};
  bool muted{};
  media::MediaTimestamp fade_in, fade_out;
  bool operator==(const AudioMix&) const = default;
};

enum class CanvasOverlayKind { Text, Subtitle, Sticker, PagText };

/// @brief Style overrides for a canvas text instance. Font size is relative to its bounds.
struct CanvasTextStyle {
  Color foreground = Color::White();
  Color background{0, 0, 0, 0};
  float size{1.0F};
  ResourceRef font;
  bool operator==(const CanvasTextStyle&) const = default;
};

/// @brief A visual overlay with its own project-time interval and normalized canvas bounds.
struct CanvasOverlay {
  InstanceId id;
  CanvasOverlayKind kind;
  TimeRange range;
  Rect destination;
  std::string text;
  ResourceRef resource;
  CanvasTextStyle style;
  float opacity{1.0F};
  float rotation_degrees{}; ///< Clockwise about the center of the destination in canvas pixels.
  bool operator==(const CanvasOverlay&) const = default;
};

/// @brief Clip placement, normalized canvas bounds, and clip-local stickers.
struct ClipInfo {
  ClipId id;
  ClipPlacement placement;
  Rect destination{0, 0, 1, 1};
  std::vector<StickerInstance> stickers;
  std::optional<BackgroundReplacement> background;
  /// On an audio clip, identifies the video clip that owns its original sound.
  std::optional<ClipId> audio_of;
  AudioMix audio_mix;
  /// Crop the source proportionally to cover the destination rectangle.
  bool fill{};
  float rotation_degrees{}; ///< Clockwise about the center of the destination in canvas pixels.
  bool operator==(const ClipInfo&) const = default;
};

/// @brief One visual or audio track and its clips in project time order.
struct TrackInfo {
  TrackId id;
  media::TrackKind kind;
  std::vector<ClipInfo> clips;
  bool operator==(const TrackInfo&) const = default;
};

/// @brief Committed project data. Track order defines compositing order from bottom to top.
/// A Project exposes snapshots through shared const handles.
struct ProjectSnapshot {
  PixelSize canvas;
  std::uint64_t revision{}, next_id{1};
  std::vector<AssetInfo> assets;
  std::vector<TrackInfo> tracks;
  std::vector<TransitionInfo> transitions;
  std::vector<CanvasOverlay> overlays;

  /// @brief Returns the project interval from zero to the last clip end.
  TimeRange Range() const;
  /// @brief Returns IDs of assets whose source files are unavailable.
  std::vector<AssetId> MissingAssets() const;
};

/// @brief Transaction-local draft operations. Use only during the Project::Edit callback.
class ProjectEdit {
public:
  ProjectEdit(const ProjectEdit&) = delete;
  ProjectEdit& operator=(const ProjectEdit&) = delete;

  /// @brief Imports a file and its track metadata into the draft.
  AssetId Import(AssetReference source);
  /// @brief Appends a video or audio track and returns its stable ID.
  TrackId AddTrack(media::TrackKind kind);
  /// @brief Creates a clip on the supplied track and returns its stable ID.
  ClipId InsertClip(TrackId track, ClipPlacement placement);
  /// @brief Changes the placement or trim of an existing clip.
  void SetClip(ClipId clip, ClipPlacement placement);
  /// @brief Sets normalized canvas bounds for a clip.
  void SetLayout(ClipId clip, Rect destination);
  /// @brief Sets clockwise picture rotation around the center of the destination.
  void SetClipRotation(ClipId clip, float degrees);
  /// @brief Removes a clip, its bound original audio, and its transitions.
  void RemoveClip(ClipId clip);
  /// @brief Chooses between fitting the complete image and covering the clip rectangle.
  void SetClipFill(ClipId clip, bool fill);
  /// @brief Sets the output canvas size in physical pixels.
  void SetCanvas(PixelSize canvas);
  /// @brief Attaches audio to its video clip, or exposes it as independent audio when detached.
  void BindAudio(ClipId audio, std::optional<ClipId> video);
  /// @brief Sets the mix parameters for an audio clip.
  void SetAudioMix(ClipId audio, AudioMix mix);
  /// @brief Inserts, updates, or removes a canvas overlay with a stable project identifier.
  InstanceId AddOverlay(CanvasOverlay overlay);
  void SetOverlay(CanvasOverlay overlay);
  void RemoveOverlay(InstanceId id);
  /// @brief Sets or removes a visual clip's background and its coverage source.
  void SetBackground(ClipId clip, std::optional<BackgroundReplacement> background);
  /// @brief Rebinds an existing packaged mask on this clip while preserving its playback and inversion.
  void SetBackgroundMaskResource(ClipId clip, ResourceRef resource);
  /// @brief Attaches an independently editable sticker to a visual clip.
  InstanceId AddSticker(ClipId clip, StickerSpec spec);
  /// @brief Sets an instance's bounds in its clip or anchor coordinates.
  void SetStickerLayout(InstanceId instance, Rect destination);
  /// @brief Replaces all slot values on an animated sticker. Pass an empty set to restore authored content.
  void SetStickerReplacements(InstanceId instance, StickerReplacements replacements);
  /// @brief Creates or updates the transition between adjacent visual clips.
  /// @return The stable new or existing transition ID.
  InstanceId SetTransition(ClipId from, ClipId to, TransitionSpec spec);
  /// @brief Changes the project interval and proportionally retimes the existing local keys.
  void SetTransitionRange(InstanceId instance, TimeRange range);
  /// @brief Replaces the feather value and its optional local keyframes.
  void SetTransitionFeather(InstanceId instance, float feather, std::vector<Keyframe<float>> keys = {});
  /// @brief Removes the transition from the draft.
  void RemoveTransition(InstanceId instance);
  /// @brief Rebinds an asset in place and updates its selected source track IDs by media kind.
  void Relink(AssetId asset, AssetReference replacement);

private:
  friend class Project;
  explicit ProjectEdit(ProjectSnapshot& draft) : draft_(draft) {}
  std::uint64_t AllocateId();
  ClipInfo& Clip(ClipId clip);
  StickerSpec& Sticker(InstanceId instance);

  ProjectSnapshot& draft_;
};

struct EditTemplateRecipe;
struct EditTemplateInputs;
class EditTemplatePlan;

/// @brief Owns project revisions and in-memory undo and redo history on one sequence.
class Project {
public:
  /// @brief Creates an empty project with the given physical canvas size.
  explicit Project(PixelSize canvas);
  /// @brief Starts an editable project from a validated snapshot with empty undo and redo history.
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
  void Save(const File& destination) const;
  /// @brief Opens a supported project document. Unavailable source paths remain available for relinking.
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
