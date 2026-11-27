#pragma once

/// @file
/// @brief Built-in components for AVKit scene and timeline systems.
/// The groups below distinguish inputs supplied by callers from state managed by scene systems.

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

#include <entityx/Entity.h>

#include <huxerui/avkit/render.h>
#include <huxerui/avkit/tracking_anchor.h>
#include <huxerui/avkit/types.h>
#include <huxerui/color.h>
#include <huxerui/file.h>
#include <huxerui/geometry.h>

namespace huxerui::avkit {

namespace analysis {
class AnalysisProvider;
struct TrackedPersonSample;
}

struct MovieAsset;
class ResourceCatalog;

/// @name Shared effect values
/// Used by project and render APIs and by component fields.
/// @{

/// @brief Action at the end of an animation or mask sequence.
enum class AnimationPlayback {
  HoldLast, ///< Keeps the final frame for the remaining clip duration.
  Loop,     ///< Restarts on the next animation frame boundary.
};

/// @brief Overrides for a named PAG text slot. Omitted fields keep the asset defaults.
struct PagTextReplacement {
  /// Name declared by the PAG asset.
  std::string slot;
  std::optional<std::string> text; ///< An empty string clears the slot.
  /// Packaged font face. An empty value keeps the asset's font.
  std::optional<ResourceRef> font;
  std::optional<float> size_scale; ///< Multiplier applied to the slot's default font size.
  /// Text fill color and opacity in RGBA.
  std::optional<Color> foreground;
  /// Text background color and opacity in RGBA.
  std::optional<Color> background;

  bool operator==(const PagTextReplacement&) const = default;
};

/// @brief Versioned image resource for a named slot in an animation resource.
struct PagImageReplacement {
  /// Name of the editable image slot.
  std::string slot;
  /// Versioned replacement image resolved when the composition is opened.
  ResourceRef image;

  bool operator==(const PagImageReplacement&) const = default;
};

/// @brief Per-instance values for named animation slots. Omitted slots use their authored content.
struct PagReplacements {
  /// Text overrides keyed by editable slot name.
  std::vector<PagTextReplacement> texts;
  /// Image overrides keyed by editable slot name.
  std::vector<PagImageReplacement> images;

  /// @brief Reports whether the instance uses only authored slot values.
  bool Empty() const { return texts.empty() && images.empty(); }

  bool operator==(const PagReplacements&) const = default;
};

/// @brief Channel decoded as R8Unorm coverage from a packaged image.
enum class MaskChannel { Grayscale, Alpha };

/// @brief A packaged image and the channel supplying its coverage.
struct MaskImage {
  /// Key of the image in the active catalog.
  ResourceRef resource;
  MaskChannel channel{MaskChannel::Grayscale};

  bool operator==(const MaskImage&) const = default;
};

/// @brief Selects the portrait model that supplies foreground coverage.
struct PortraitMask {
  /// Empty selects the analysis provider's default portrait model.
  std::string model_id;

  bool operator==(const PortraitMask&) const = default;
};

/// @brief A static coverage mask in oriented, pre-mirror source space.
struct StaticMask {
  MaskImage image;
  /// Inverts coverage inside the valid mask region.
  bool inverted{};

  bool operator==(const StaticMask&) const = default;
};

/// @brief A coverage sequence sampled from the clip's project-time start.
struct AnimatedMask {
  /// Versioned sequence descriptor and its R8Unorm frames.
  ResourceRef resource;
  /// Hold the final frame or wrap at the sequence boundary.
  AnimationPlayback playback{AnimationPlayback::HoldLast};
  /// Inverts coverage inside the valid mask region.
  bool inverted{};

  bool operator==(const AnimatedMask&) const = default;
};

/// @brief How a key approaches the next value.
enum class Interpolation { Hold, Linear };

/// @brief Parameter value and interpolation at a local media time.
template<class T> struct Keyframe {
  /// Exact time relative to the start of the effect.
  media::MediaTimestamp time;
  /// Parameter value at this exact time.
  T value;
  /// Interpolation from this key to the next key.
  Interpolation interpolation{Interpolation::Linear};

  bool operator==(const Keyframe&) const = default;
};

/// @brief A mask wipe and its optional transition-local feather curve.
struct MaskTransition {
  /// Mask sequence sampled by transition progress.
  ResourceRef resource;
  float feather{0.5F}; ///< Base feather in [0, 1], used when the curve is empty.
  /// Inverts the sampled coverage before crossfading.
  bool inverted{};
  /// Feather curve in strictly increasing local time. A final key can sit at the exclusive end.
  std::vector<Keyframe<float>> feather_keys;

  bool operator==(const MaskTransition&) const = default;
};

/// @brief A PAG asset with outgoing and incoming image slots.
struct PagTransitionSource {
  /// PAG asset with two distinct named image slots.
  ResourceRef resource;

  bool operator==(const PagTransitionSource&) const = default;
};

/// @brief Authored transition over a project-time overlap.
struct TransitionSpec {
  /// Half-open overlap on the project clock.
  TimeRange range;
  /// The effect evaluated across the overlap.
  std::variant<MaskTransition, PagTransitionSource> effect;

  bool operator==(const TransitionSpec&) const = default;
};

/// @}

/// @name Authored layer components
/// Applications and scene modules can set these on their layers. Project projection sets them for project-owned entities.
/// ClipId and InstanceId are shared identifiers also attached directly for entity lookup.
/// An optional TimeRange limits a drawable layer on its scene clock or, with ClipAttachment,
/// on the owning clip's local project clock.
/// A layer can use Affine2D directly as an explicit local matrix. The tracking system sets it on its own anchors.
/// @{

/// @brief Editable placement of a unit layer in its parent's normalized coordinates.
struct Transform2D {
  /// Translation of the unit layer before pivot rotation, in parent coordinates.
  Point translation;
  /// Layer width and height in normalized parent coordinates.
  Size scale{1, 1};
  /// Clockwise angle in degrees. The resolver accounts for the physical canvas aspect ratio.
  float rotation_degrees{};
  Point pivot{0.5F, 0.5F}; ///< Rotation center in unit-layer coordinates.

  bool operator==(const Transform2D&) const = default;
};

/// @brief Editable 3D placement in parent coordinates, with an XYZW unit quaternion.
struct Transform3D {
  /// XYZ position in parent coordinates.
  std::array<float, 3> translation{};
  /// Unit quaternion in XYZW order. The default is identity.
  std::array<float, 4> rotation{0, 0, 0, 1};
  /// Scale along the local X, Y, and Z axes.
  std::array<float, 3> scale{1, 1, 1};

  bool operator==(const Transform3D&) const = default;
};

/// @brief Compositing attributes, with a distinct order within the destination pass.
struct LayerStyle {
  /// Back-to-front draw order. Each visible draw in a pass needs a distinct order.
  std::int32_t order;
  /// Composite operation applied to the destination.
  render::Blend blend{render::Blend::PremultipliedOver};
  /// Multiplier in [0, 1] applied after sampling.
  float opacity{1.0F};
};

/// @brief Excludes a layer from final composition for this sample.
struct HiddenLayer {};

/// @brief Trim and playback direction for a clip's selected source track.
struct ClipSource {
  /// Imported asset to resolve in the current project.
  ResourceRef asset;
  /// Track identifier in that asset's container.
  std::uint32_t track;
  /// Half-open trim on the source presentation clock.
  TimeRange range;
  /// Reads the trim in descending source time while project time advances.
  bool reverse{};

  bool operator==(const ClipSource&) const = default;
};

/// @brief Maps a source trim to an equal-duration project interval.
struct ClipTiming {
  /// Source selection, trim, and direction.
  ClipSource source;
  /// Half-open placement interval on the project clock.
  TimeRange timeline;

  bool operator==(const ClipTiming&) const = default;
};

/// @brief Maps the endpoints of a project subrange into source time.
/// @param clip Trim and project placement, including the source playback direction.
/// @param project Interval contained in the clip's project placement.
/// @return Ascending half-open source boundaries for either playback direction.
/// @throws std::invalid_argument If the interval is invalid or outside the clip.
TimeRange MapClipToSource(const ClipTiming& clip, TimeRange project);

/// @brief Maps the endpoints of a source subrange into project time.
/// @param clip Trim and project placement, including the source playback direction.
/// @param source Interval contained in the clip's source trim.
/// @return Ascending half-open project boundaries for either playback direction.
/// @throws std::invalid_argument If the interval is invalid or outside the trim.
TimeRange MapClipToProject(const ClipTiming& clip, TimeRange source);

/// @brief Marks a file-backed video clip whose track is selected by ClipTiming.
struct FileVideoClip {};

/// @brief Static source and display geometry of a visual clip, assigned by project projection.
struct ProjectImageSource {
  ResourceRef resource; ///< Key of the still media in the scene catalog.
  PixelSize size; ///< Oriented display size of the still source.
};

/// @brief Video-specific layout options applied to the layer bounds.
struct VideoLayout {
  /// Keeps the layer bounds and center-crops the picture to cover them.
  bool fill{};
  /// Fits the layer bounds to the sample's display aspect ratio before composition.
  bool match_source_aspect{};
};

/// @brief Explicit crop and horizontal mirroring in oriented source coordinates.
struct SourceCrop {
  /// Crop in normalized oriented source coordinates, before mirroring.
  Rect area{0, 0, 1, 1};
  /// Reflects sampling horizontally within the crop.
  bool mirrored{};

  bool operator==(const SourceCrop&) const = default;
};

/// @brief Background fill on a visual entity with exactly one mask source component.
struct BackgroundReplacement {
  /// Straight RGBA fill behind the covered foreground.
  Color color;

  bool operator==(const BackgroundReplacement&) const = default;
};

/// @brief Requests source-space geometry for analysis even if the clip has no crop or mask.
struct SourceGeometryRequired {};

/// @brief A PAG resource and the authored bindings for one instance.
struct PagOverlay {
  /// Versioned composition resolved on the render sequence.
  ResourceRef resource;
  /// Instance-specific values for named editable slots.
  PagReplacements replacements;
  /// How local sample time behaves past the composition's duration.
  AnimationPlayback playback{AnimationPlayback::Loop};

  bool operator==(const PagOverlay&) const = default;
};

/// @brief A packaged still image supplied to a layer.
struct StillImageSource {
  ResourceRef resource;

  bool operator==(const StillImageSource&) const = default;
};

/// @brief The drawable content of one authored overlay. Projection attaches the selected component.
using OverlayContent = std::variant<StillImageSource, PagOverlay>;

/// @brief Owning clip clock for an attached layer, independent of its spatial parent.
struct ClipAttachment {
  /// Owning clip. Its active range gates visibility for this overlay.
  entityx::Entity clip;
};

/// @brief Attachment of a layer to a tracked landmark and its spatial anchor entity.
struct TrackedSticker {
  /// Spatial entity whose transform is resolved from the current observation.
  entityx::Entity anchor;
  /// Body part and landmarks used to place the anchor.
  analysis::TrackingAnchor binding;
};

/// @brief Camera-space depth of a sticker, in meters.
struct StickerDepth {
  /// Positive distance along the camera optical axis.
  float meters;

  bool operator==(const StickerDepth&) const = default;
};

/// @brief Audio track sampled on its owning clip's clock.
struct AudioSource {
  /// Imported asset supplying PCM after decode.
  ResourceRef asset;
  /// Source audio track identifier.
  std::uint32_t track;
  media::MediaTimestamp source_offset; ///< Audio source start relative to the clip source start.

  bool operator==(const AudioSource&) const = default;
};

/// @brief Clip audio gain and linear fades in project time.
struct AudioMix {
  /// Nonnegative linear sample gain. One preserves the source level.
  float gain{1.0F};
  /// Contributes silence for this clip while its timing remains active.
  bool muted{};
  /// Durations of the linear fades at the clip start and end.
  media::MediaTimestamp fade_in, fade_out;

  bool operator==(const AudioMix&) const = default;
};

/// @brief Duration-preserving voice processing on a file audio source or live PCM input.
enum class VoiceEffect { Robot = 1 };

/// @}

/// @name Source adapter inputs
/// The camera and audio adapters set these as input arrives. External producers can set them on their own source entities.
/// Attach an imported render::Texture directly to a layer to take precedence over decoded video or PAG.
/// @{

/// @brief Source geometry paired with the camera entity's render::Texture component.
struct CameraFrameGeometry {
  /// Normalized camera crop in input image coordinates.
  Rect crop;
  /// Orientation applied when fitting the camera image to the target.
  media::VideoRotation rotation;
  /// Horizontally reflects the oriented camera image.
  bool mirrored{};
};

/// @brief A retained input block aligned to the current audio evaluation.
struct LivePcmInput {
  /// Captured PCM block with a timestamp in the capture clock.
  media::AudioFrame frame;
};

/// @}

/// @name Scene-managed relationships
/// RenderScene and project projection maintain these relationships. External systems can inspect them.
/// @{

/// @brief A spatial relationship with independent entity lifetimes.
/// Configure it through RenderScene::SetSpatialParent so the hierarchy remains acyclic.
struct SpatialParent {
  /// Live parent entity whose transform is inherited.
  entityx::Entity entity;
};

/// @brief Two clip entities and the roots of their offscreen transition passes.
struct TransitionLink {
  /// Outgoing and incoming clip entities, respectively.
  entityx::Entity from, to;
  /// Spatial roots for both clip passes, including each clip's attached layers.
  entityx::Entity from_root, to_root;
};

/// @}

/// @name Scene root components
/// The scene publishes these evaluation-wide components on its root. External systems can inspect the current context.
/// @{

/// @brief Exact media time and output geometry on the scene's root entity.
/// RenderScene initializes it and updates it before running video systems.
struct VideoFrameContext {
  /// Exact time on the active timeline or capture clock.
  media::MediaTimestamp time;
  /// Physical size and pixel format of this evaluation's render target.
  render::ImageInfo target;
  /// Positive media duration for a caller-declared continuous update. Leave empty for random-access evaluation.
  std::optional<media::MediaTimestamp> continuous_step;
};

/// @brief One audio evaluation on the scene's root, independent of the video clock.
/// AVKit installs it for BeforeAudio and Audio. The audio system fills output.
struct AudioBlockEvaluation {
  /// Start of the block, aligned to the PCM sample grid.
  media::MediaTimestamp start;
  /// Number of sample frames requested per channel.
  std::uint32_t samples;
  /// PCM format shared by the inputs and the mix.
  media::AudioFormat format;
  /// Retained result assigned by the audio system.
  media::AudioFrame output;
  /// Cancels decoder waits for this block.
  std::stop_token stop{};
};

/// @}

/// @name System results on layers
/// Systems refresh these for the current sample. Treat them as read-only when extending the built-in timeline.
/// @{

/// @brief Effective video interval after the timeline resolves transition overlaps.
struct ClipPresentationRange {
  /// Effective project interval after applying transition overlaps.
  TimeRange range;
  /// Indicates that transition sampling owns the clip's initial pictures.
  bool starts_in_transition{};
};

/// @brief Requested source time and project-local elapsed time of a visible clip.
struct ClipSampleTime {
  /// Requested position on the source clock, with reverse playback already applied.
  media::MediaTimestamp source;
  /// Elapsed project time since the start of the clip, independent of the source direction.
  media::MediaTimestamp elapsed;
};

/// @brief Frame-local video metadata. Drawable storage is attached as Texture or ImagePixels.
/// The picture's PTS can differ from ClipSampleTime::source when it covers the requested time.
struct SampledVideo {
  /// Borrowed during source evaluation for tracking, coverage, and clip fitting. Cleared after evaluation.
  const media::VideoFrame* picture;
  /// Logical UV mapping supplied by the video importer.
  Affine2D source_to_texture_uv;
  /// Identity of storage installed by the video source. External drawables are left alone.
  const void* owned_drawable{};
};

/// @brief Prepared pixels of a visible project image or packaged still overlay.
struct SampledStillImage {
  render::ImagePixels pixels;
};

/// @brief Elapsed playback time of an active overlay. The layer timing system computes it for the current frame.
struct LayerSampleTime {
  /// Local evaluation time from the layer's time origin.
  media::MediaTimestamp elapsed;
};

/// @brief Editable indices in the order of the authored text and image replacements.
struct PagEditableIndices {
  std::vector<int> text, image;
};

/// @brief An owned PAG input for one layer at its evaluated time.
struct PagLayerSample {
  /// Stable authored instance ID for resource caching, when available.
  std::optional<InstanceId> instance;
  /// Exact time elapsed on the layer's local clock.
  media::MediaTimestamp elapsed;
  /// Owned composition and replacements for this sample.
  PagOverlay source;
  std::optional<Size> layout_extent; ///< Original normalized extent for canvas-fitted point text.
  /// Editable indices resolved by scene evaluation. An independent caller may leave them empty.
  std::optional<PagEditableIndices> slots;
};

/// @brief Video placement derived from the current picture and target geometry.
struct FittedVideoTransform {
  /// Resolved transform after fitting the source display aspect.
  Transform2D value;
};

/// @brief Frame-local source geometry used by tracking and depth coverage.
struct ClipSourceMapping {
  /// Maps normalized draw coordinates to the oriented source before mirroring.
  Affine2D draw_to_source;
  /// Inverse mapping used to place tracking results within the clip.
  Affine2D source_to_clip;
};

/// @brief Current subject selected by the tracking system for this clip.
struct PrimarySubject {
  const analysis::TrackedPersonSample* person; ///< Borrowed for the current evaluation.
};

/// @brief Local clock of a visible transition.
struct ActiveTransition {
  /// Local overlap time and full overlap duration.
  media::MediaTimestamp elapsed, duration;
};

/// @brief Starts an isolated transition side for this entity's spatial descendants.
struct TransitionGroupRoot {};

/// @brief Sampled depth for an occluded sticker, before the source clip is positioned.
struct SampledStickerDepth {
  /// Clip supplying the camera-space depth image and its geometry.
  entityx::Entity source;
  /// Retained R32Float image registered to the source clip.
  render::PixelInput depth;
  /// Maps clip-local draw coordinates into logical depth texture UVs.
  Affine2D clip_to_depth_uv;
  /// Valid depth coverage before row-origin handling.
  Rect valid_uv{0, 0, 1, 1};
  /// Positive optical-axis depth at which to place the layer.
  float layer_depth_meters;
};

/// @}

namespace detail {

struct TargetLayoutEntry;

class TimelineAudioSource;
class TimelineVideoSource;

/// @name Internal root components
/// AVKit owns these components. Their references remain valid for the lifetimes stated below.
/// @{

/// @brief Borrowed services attached for the lifetime of a timeline scene.
struct TimelineServices {
  /// Session-owned video cursors and preroll worker.
  TimelineVideoSource& video;
  /// Session-owned PCM source cursors.
  TimelineAudioSource& audio;
  /// Optional render device retained by the session for import and analysis texture validation.
  render::RenderBackend* device;
  /// Resolves inspected media for the current revision.
  std::function<const MovieAsset&(const ResourceRef&)> movie;
};

/// @brief Request and readiness result attached while the video preroll systems execute.
struct VideoPrerollRequest {
  /// Project interval used to select upcoming source entries.
  TimeRange lookahead;
  /// Retires or reuses prepared entries from the previous playback position.
  bool seek{};
  /// Filled by the preroll system when the selected entries are cached.
  bool ready{};
  /// Cancels work associated with this request.
  std::stop_token stop;
};

/// @brief Sampling options attached for one timeline video evaluation.
struct TimelineEvaluation {
  /// Token for decode, analysis, and scene evaluation.
  std::stop_token stop;
  /// Per-job analysis deadline. Empty waits for the matching result.
  std::optional<std::chrono::milliseconds> analysis_budget;
  /// Optional observation of the exact decoded picture selected after import and fallback.
  const std::function<void(const media::VideoFrame&)>& on_decode;
};

/// @brief Options and result attached for one spatial compilation.
struct RenderPlanCompilation {
  /// Entity and project-space transform delta for a paused recompose. Empty entity means authored layout.
  entityx::Entity target;
  Affine2D delta;
  /// Optional layout output in draw order. PAG fills in measured content bounds during rendering.
  std::vector<TargetLayoutEntry>* layouts{};
  /// Tree written by the spatial system. RenderScene finalizes its resource retention before publication.
  std::shared_ptr<render::RenderPlan> plan;
};

/// @}

} // namespace detail
} // namespace huxerui::avkit
