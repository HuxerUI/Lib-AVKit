# Lib-AVKit Architecture

The [repository overview](../README.md) lists the current targets and build options. This document describes the design, integration, and runtime contracts.

The Lib-AVKit design separates a headless C++ production engine, embeddable HuxerUI components, and a full editor. A project stores editable data, and EntityX evaluates each session's runtime state at the requested time. The primary image pipeline takes GPU textures as input and produces a composited texture. bgfx is the first rendering backend. Audio processing operates on PCM. Preview, recording, and offline export share effect definitions, media-time mappings, and render plans.

## Capture, preview, and project export

`CameraRecording` receives camera textures through Lib-Camera's frame processor. `Record` acquires confirmed microphone PCM through AudioCapture, aligns the clocks, and supervises the take until it finishes. Capture shows and encodes the same completed composite. The current preview can also be saved as a composed JPEG. The recorder can renegotiate a failed first platform texture transfer before admitting video and audio to the encoder.

`TimelinePreviewSession` owns playback, seek, frame sampling and renderer lifetime. Register the presentation callback when creating the session. Use `ApplyProject` to install a revision, `Seek` to move the playhead without changing the requested play state, and `Play` and `Pause` to control transport. `SetTransform` and `SetTransformDelta` change the layout of a paused frame using its retained media sample. The session combines pending commands and cancels superseded presentations. Each presentation carries the project revision, frame sequence, PTS, and the request ID of any applied temporary transform. Use `PreviewFrame::FindLayout` to query a target or `PreviewFrame::HitTest` to find the topmost editable target at a canvas position. Where the platform supports deferred publication, the host commits the prepared texture and uses its matching layout on the UI sequence. An in-playback seek resets the PCM epoch and reuses valid prepared decode windows.

`ExportProject` uses the selected project revision, resource catalog and scene module throughout sampling and encoder retry. It can suspend the supplied preview session while it owns the renderer and resume that session afterward. `ProbeVideoExport` checks the same platform and software encoder candidates that the MP4 writer negotiates. The low-level `RenderScene`, `TimelineSession` and `TimelineRenderer` are independently callable.

A still-image asset retains its file and oriented dimensions. The clip owns its presentation interval. Insertion defaults to three seconds, accepts a per-clip duration, and supports later trims. The v1 project document stores the still-image flag and the same clip interval used by playback and export.

A template recipe can be registered in `ResourceCatalog`. `OpenEditTemplate` exposes its user input slots. `CreateTemplateProject` imports bound user assets and local pack media, resolves a named clip's transitions and effect references, and returns an editable project. Templates preview and export through the same entry points as manually authored projects.


## Modules and dependencies

| Target | Responsibilities |
| --- | --- |
| `AVKit::Core` | Assets and resource packs, edit transactions, project snapshots, template assembly, ECS evaluation, media scheduling, audio processing, and production jobs. Standalone hosts can submit frames, sample at specific times, and export. |
| `AVKit::Bgfx` | Executes Core render plans and owns GPU programs, textures, attachments, synchronization, and submission queues. |
| `AVKit::Media` | C++ file media module. Defines the codec provider API, adapts Lib-Media, and coordinates import, decoded reads, seek, and MP4 export. |
| `AVKit::FFmpeg` | Software codec adapter. Each distribution platform declares the full configurations for which it supports fallback. |
| `AVKit::NCNN` | CPU tracking and portrait segmentation using versioned model resources. |
| `AVKit::PAG` | PAG animation sampling, editable text and image slots, and platform transition interop. |
| `AVKit::Render` | Composites sampled timeline frames and bound live render plans using bgfx and PAG. |
| `HuxerUI::AVKit` | HuxerUI application hook and the combined library targets. The preview application adds capture, media playback, the editor, and template selection. |

Core reuses the HuxerUI SDK's `File`, `FileReference`, `RawAsset`, `ImageAsset`, `Result`, and stream types. Media interfaces use Lib-Media's `MediaTimestamp`, `VideoFrame`, `AudioFrame`, `EncodedPacket`, and container types. The UI uses `Task` for device integration and asynchronous I/O, and `ExternalTexture` to publish composited frames. `Rect` describes layout, and `PixelSize` represents dimensions in whole physical pixels. The main application headers are separate from EntityX extensions and the PAG and bgfx implementations. [Lib-Media shared formats](../../Lib-Media/include/huxerui/media_format.h) · [Camera API](../../Lib-Camera/include/huxerui/camera.h) · [AudioCapture API](../../Lib-AudioCapture/include/huxerui/audiocapture.h)

## Production workflows

### Capture, voice effects, and recording

1. **Start capture.** Call `UseAvKitCapture` in a HuxerUI composition. Video and audio devices use `camera::CameraOptions` and `audio::CaptureOptions`, respectively. The host requests permission, the composition owns the capture lifecycle, and `Camera()` exposes existing controls such as focus and lighting.
2. **Select and edit a scene.** Register the scene assets in a `ResourceCatalog` before opening the session. In one transaction, `CaptureSession::EditAsync` can apply a scene template, add stickers manually, adjust existing beauty effects, and select voice effects and background music. Visual and audio effects share the same instance data format and validate their targets by domain. Applying a template publishes a new revision after its models and textures are ready. Override template node parameters by stable instance ID. User-added items have their own instances and layouts, so their effects can carry over when changing templates.
3. **Record the current scene.** `RecordVideoAsync` takes a destination and a `RecordingRequest`. An optional `RecordingAudio` specifies both the audio source and encoding settings. The microphone signal passes through voice effects, gain, and envelopes before mixing with the independent background music branch. Preview and recording use the same scene revision. JS and ECS can update sticker-local transforms each frame.
4. **Stop and import.** `PauseRecording` and `ResumeRecording` control the intervals included in the recording. `StopRecording` requests finalization, and the caller awaits the original recording task for its `MovieResult`. Local files and file references can be imported directly into an edit. To support changing the original beauty or voice effects after recording, retain the raw video, dry audio, and intervals in which each scene revision took effect. Single-file and segmented modes determine how the output is muxed.

Still photography uses the same capture session. `OriginalStill` returns a separate JPEG from Camera. `ProcessedStill` freezes the scene at shutter time, decodes the still JPEG, runs analysis and rendering again, and encodes the result. `PreviewSnapshot` uses a completed preview frame. Camera defines the flash, orientation, and capture dimensions for still photos. Once the file is ready, a separate Lib-Media `SaveImageAsync` or `SaveVideoAsync` call can publish it to the media library. [Camera photo capture contract](../../Lib-Camera/include/huxerui/camera.h#L481-L506) · [Media library publishing](../../Lib-Media/include/huxerui/media_library.h#L288-L314)

### One-click assembly and manual editing

1. **Import assets.** Inspect user video, still images, and audio in AVKit, then register the media with `ProjectEdit::Import`. Its returned `ResourceRef` selects the same catalog entry from a main-track clip, a picture-in-picture clip, or an audio clip. Each clip selects its own source track and trim. A template can bind those same keys or import a local pack file under its catalog key.
2. **Fill a template or arrange tracks directly.** For one-click assembly, `DescribeEditTemplate` exposes the canvas and slots. `PlanEditTemplateAsync` uses the user's selections and explicit bindings to plan shots, source ranges, transition handles, and music placement. `ApplyEditTemplate` commits the plan in one undoable transaction. For manual editing, use `Project::Edit` directly to add clips, subtitles, and layers, bind audio tracks, and set transitions. Both paths produce the same kind of project snapshot.
3. **Continue editing.** The main image and picture-in-picture can use separate `LayerLayout` values. One commit can swap their positions and stacking order. Pack assets can be added as stickers to the target image. Each instance owns its style, anchor, and animation timing. Apply beauty and visual effects to the relevant image, and voice effects to the original audio clip or voice track. Adjust background music gain and envelopes on its own track. The timeline requests thumbnails and waveforms in source time and edits transition ranges and parameter curves in project time.
4. **Preview and export.** `OpenEditorPreview` or `ApplyProject` uses the current project snapshot. `ExportAsync` selects the same revision and its current export interval. Export owns its runtime EntityX world and resources, samples video and PCM in project time, and sends them to the encoders. The user can keep editing the next revision.

## Resources, instances, and projects

`ResourceRef{key}` identifies one resource in the active catalog. Give pack entries unique keys at registration. Project imports allocate keys under `project/media/` or retain a registered pack-media key. The immutable media record stores the local path and source track metadata. Every clip that uses the media stores the same resource key and its own clip ID, source track, trim, and placement. `InstanceId` identifies an editable overlay or effect instance.

Register a local `File` or bundled `RawAsset` in `ResourceCatalog` under a unique key. Inspected project media stores its source file and track metadata together. A PAG registration stores the readable payload and editable `PagAsset` metadata under the same key. The catalog retains `MaskSequenceData` and `EditTemplateRecipe` records. `OpenRead` opens an independent stream, and `FindLocalFile` supplies paths to native loaders. For extracted packs, registration validates that the relative file path stays inside the pack and rejects duplicate keys across resource types. [Resource catalog API](../include/huxerui/avkit/resource_catalog.h)

A project snapshot owns its immutable media catalog. Opening a timeline merges that catalog with the supplied packaged resources once for the snapshot. The scene root owns a `ResourceCatalog` value with the same immutable resource records. Built-in and application systems use `SceneCatalog(root)` to resolve media, still images, PAG, and masks. Render plans retain the corresponding catalog snapshot through drawing. Relinking locates the original local media at a new path and publishes a new record under the existing key. Undo and redo restore the corresponding catalog snapshots. Media caches retain the exact immutable record and the relevant track and sampling options. Camera, decode, analysis, and compositor outputs remain frame data with their own texture leases. [Resource catalog API](../include/huxerui/avkit/resource_catalog.h)

### Shared declarations

The following C++20 declarations compose in document order and define the interface contracts for implementation. Identifiers are scoped to their project, session, or pack. A `TimeRange` is half-open, and time conversion uses `MediaTimestamp`. Validate specifications, coordinates, and parameters before committing a transaction.

```cpp
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <huxerui/data.h>
#include <huxerui/file.h>
#include <huxerui/geometry.h>
#include <huxerui/media_codec.h>
#include <huxerui/media_container.h>
#include <huxerui/paint.h>
#include <huxerui/resource.h>
#include <huxerui/stream.h>

namespace huxerui::avkit {

enum class ErrorCode { InvalidRequest, Unsupported, ClockUnavailable, Busy, StaleRevision,
                       Interrupted, Canceled, Processing, Io };
struct Error { ErrorCode code; std::string message; };
template<class T> using AvKitResult = huxerui::Result<T, Error>;
template<class T> class ResultJob {
public:
    virtual ~ResultJob() = default;
    virtual AvKitResult<std::optional<T>> TryTakeResult() = 0;
    virtual void Cancel() = 0;
};
struct PixelSize { std::uint32_t width, height; };
struct TimeRange { media::MediaTimestamp start, end; };

struct TrackId { std::uint64_t value; bool operator==(const TrackId&) const = default; };
struct ClipId { std::uint64_t value; bool operator==(const ClipId&) const = default; };
struct InstanceId { std::uint64_t value; bool operator==(const InstanceId&) const = default; };
struct ResourceRef {
    std::string key;
    bool operator==(const ResourceRef&) const = default;
};
enum class ResourceKind { SceneTemplate, EditTemplate, EffectPreset, TransitionPreset, Image,
                          Animation, Audio, Mask, Lut, Model, Font, Script, Shader };

enum class EffectDomain { Visual, Audio, Transition };
using ParameterValue = std::variant<bool, std::int64_t, double, std::string, Rect,
                                    media::MediaTimestamp, ResourceRef>;
struct Parameter { std::string name; ParameterValue value; };
struct ParameterDescriptor {
    Parameter default_value;
    std::optional<double> minimum, maximum;
    std::string unit;
    bool animatable{};
    std::optional<ResourceKind> resource_kind;
};
struct EffectDescriptor {
    std::string type, title;
    std::uint32_t schema_version;
    EffectDomain domain;
    std::vector<ParameterDescriptor> parameters;
};
struct EffectSpec {
    std::variant<std::string, ResourceRef> type_or_preset;
    std::vector<Parameter> parameters;
};
enum class Interpolation { Hold, Linear };
template<class T> struct Keyframe {
    media::MediaTimestamp time;
    T value;
    Interpolation interpolation{Interpolation::Linear};
};
struct ParameterCurve { std::string name; std::vector<Keyframe<ParameterValue>> keys; };
struct EffectInstance {
    InstanceId id;
    std::string type;
    std::uint32_t schema_version;
    std::vector<Parameter> parameters;
    std::vector<ParameterCurve> curves;
    std::optional<std::string> template_node;
    bool enabled{true};
};

struct LayerLayout {
    Rect rect{0.0F, 0.0F, 1.0F, 1.0F};
    std::optional<Rect> crop;
    ImageFit fit{ImageFit::Contain};
    std::int32_t order{};
    float rotation_radians{};
    float opacity{1.0F};
};
enum class TrackedPart { Face, Body, LeftHand, RightHand };
struct TrackingAnchor {
    TrackedPart part;
    std::string point;
    std::optional<std::string> axis_point;
};
struct OverlaySpec {
    ResourceRef resource;
    std::optional<TimeRange> range;
    LayerLayout layout;
    std::vector<Keyframe<LayerLayout>> layout_keys;
    std::optional<TrackingAnchor> anchor;
};
struct OverlayInstance { InstanceId id; OverlaySpec value; std::optional<std::string> template_node; };
struct VisualState {
    std::optional<ResourceRef> scene_template;
    LayerLayout layout;
    std::vector<Keyframe<LayerLayout>> layout_keys;
    std::vector<EffectInstance> effects;
    std::vector<OverlayInstance> overlays;
};
struct AudioState {
    double gain_db{};
    bool muted{};
    std::vector<Keyframe<double>> gain_keys;
    std::vector<EffectInstance> effects;
};
struct Ducking {
    double reduction_db;
    media::MediaTimestamp attack, release;
};

class EffectParameterEdit {
public:
    void SetParameter(Parameter parameter);
    void SetCurve(ParameterCurve curve);
};
class EffectChainEdit {
public:
    InstanceId Add(EffectSpec effect, std::optional<std::size_t> index = {});
    EffectParameterEdit& Parameters(InstanceId id);
    void SetEnabled(InstanceId id, bool enabled);
    void Move(InstanceId id, std::size_t index);
    void Remove(InstanceId id);
};
class VisualEdit {
public:
    void SetTemplate(std::optional<ResourceRef> scene);
    void SetLayout(LayerLayout layout, std::vector<Keyframe<LayerLayout>> keys = {});
    EffectChainEdit& Effects();
    InstanceId AddOverlay(OverlaySpec overlay);
    void SetOverlay(InstanceId id, OverlaySpec overlay);
    void RemoveOverlay(InstanceId id);
};
class AudioEdit {
public:
    EffectChainEdit& Effects();
    void SetGain(double gain_db, std::vector<Keyframe<double>> keys = {});
    void SetMuted(bool muted);
};

}
```

`VisualEdit` serves both capture scenes and edited images. `AudioEdit` applies to the microphone, background music, audio clips, and tracks. `EffectChainEdit` validates the effect domain and the instance's owner. A template supplies defaults, and users can override parameters on exposed nodes or add, move, and remove their own instances. Template nodes have stable local names. Each template controls which parameters it exposes, and the pack manifest maps PAG replacement slots. Regular stickers use canvas coordinates. With a `TrackingAnchor`, layout is evaluated in the anchor's local coordinates. Source `crop` uses normalized source-image coordinates with a top-left origin. [Vision analysis and tracking anchors](#vision-analysis-and-tracking-anchors)

A `ResourceRef` parameter can declare its required `resource_kind`. The editor uses it to filter choices, and template assembly and edit commits validate the kind of the resolved pack entry. Visual, audio, and transition effects share `EffectParameterEdit` for parameter and curve changes. An empty curve removes that parameter's keyframes and restores evaluation from the base value. Edit views are valid within the transaction callback. All paths validate parameter types, interpolation, and instance ownership. Use `SetParameter` to replace a mask. The project stores the version of the replacement resource.

### Project editing and timing

`ClipTiming::timeline` specifies a clip's position and duration in the project. A media source track has its own presentation interval, `ClipSource::range`, which maps into the project after container edits and audio trimming. Constant playback speed is the ratio of source duration to project duration, and `reverse` selects direction. Sampleable animation assets provide a video track represented by `ClipSource`. Still images and inline subtitles use the project interval directly. Clip animation is evaluated from the clip start. Track envelopes and transition ranges use project time, and transition parameter curves are evaluated from the transition start.

Reverse playback maps from the end of the source range. Video sampling uses the presentation sample immediately to the left of the mapped time and clips it to the source range. As a result, the first clip frame is the last image within that range. Sample thumbnails at an interior time in the source interval mapped from each visible cell. Query waveform data over the source range in ascending order, then draw it in the clip's playback direction.

```cpp
namespace huxerui::avkit {

enum class AssetKind { Video, Image, Audio, Subtitle, Animation };
enum class TrackKind { Visual, Audio, Subtitle };
enum class ImportMode { Reference, Copy };
using ImportSource = std::variant<File, FileReference, ImageAsset, ResourceRef>;
using ProjectSource = std::variant<File, FileReference>;
struct AssetTrack {
    std::uint32_t id;
    media::TrackKind kind;
    std::optional<TimeRange> presentation;
};
struct AssetSource {
    File file;
    std::vector<AssetTrack> tracks;
    bool still_image{};
};
struct ClipSource { ResourceRef asset; std::uint32_t track; TimeRange range; bool reverse{}; };
struct StillSource { ResourceRef asset; };
struct SubtitleCue { std::string text; };
struct ClipTiming {
    std::variant<ClipSource, StillSource, SubtitleCue> source;
    TimeRange timeline;
};
struct TimelineClip {
    ClipId id;
    ClipTiming placement;
    std::optional<VisualState> visual;
    std::optional<AudioState> audio;
    std::optional<ClipId> linked_clip;
};
struct TimelineTrack {
    TrackId id;
    TrackKind kind;
    std::vector<TimelineClip> clips;
    std::optional<AudioState> audio;
};
struct TransitionSpec { TimeRange range; EffectSpec effect; };
struct TransitionInstance { ClipId from, to; TimeRange range; EffectInstance effect; };
struct TrackDucking { TrackId voice, music; Ducking settings; };

class ProjectSnapshot {
public:
    std::uint64_t Revision() const;
    PixelSize canvas_size;
    TimeRange Range() const;
    std::shared_ptr<const ResourceCatalog> resources;
    std::span<const TimelineTrack> Tracks() const;
    std::span<const TransitionInstance> Transitions() const;
    std::span<const TrackDucking> DuckingRules() const;
};
class ProjectEdit {
public:
    TrackId AddTrack(TrackKind kind);
    void MoveTrack(TrackId track, std::size_t index);
    ClipId InsertClip(TrackId track, ClipTiming placement);
    void SetClip(ClipId clip, TrackId track, ClipTiming placement);
    ClipId SplitClip(ClipId clip, media::MediaTimestamp at);
    void LinkAudioVideo(ClipId video, ClipId audio);
    void Unlink(ClipId clip);
    VisualEdit& Visual(ClipId clip);
    AudioEdit& Audio(ClipId clip);
    AudioEdit& Audio(TrackId track);
    void SetTransition(ClipId from, ClipId to, std::optional<TransitionSpec> transition);
    void SetTransitionRange(ClipId from, ClipId to, TimeRange range);
    EffectParameterEdit& TransitionParameters(ClipId from, ClipId to);
    void SetDucking(TrackId voice, TrackId music, std::optional<Ducking> ducking);
    void RemoveClip(ClipId clip);
    void RemoveTrack(TrackId track);
    void Relink(const ResourceRef& asset, File file);
};
class Project {
public:
    std::shared_ptr<const ProjectSnapshot> Snapshot() const;
    AvKitResult<std::shared_ptr<const ProjectSnapshot>> Edit(
        const std::function<void(ProjectEdit&)>& changes);
    AvKitResult<void> Undo();
    AvKitResult<void> Redo();
};

}
```

A visual track can contain video, still images, and sampleable animations. The timeline track order and `LayerLayout::order` are stored separately. A single transaction can change the layout and order of the main image and picture-in-picture. A transition's entire range must be covered by both adjacent clips. An explicit transition accounts for visual overlap on the same track. Linked video and audio clips move, trim, or split at shared project boundaries while retaining each track's own source timing. Unlink the clips to edit them independently. Trimming, clip deletion, and asset relinking validate all affected transitions, source tracks, effects, and links.

`SetTransition` creates, replaces, or removes the transition between two adjacent clips. `TransitionParameters` edits its existing `EffectInstance`, and `SetTransitionRange` changes that instance's range. Parameters, ranges, and clip positions can change in one transaction. At commit time, validate the overlap against the resulting clip positions. A template can set parameter keyframes in the same transaction that creates the transition. Undo and redo restore ranges, curves, and resource references by project revision.

Transition curves use a local `MediaTimestamp` measured from the transition start. The final keyframe can be placed at the range end. Frame sampling uses the half-open interval. Moving a transition moves its curve with the start time. Changing the duration retimes keyframes by the ratio of the new duration to the old one. A feathering keyframe at `0.5s` in a one-second transition moves to `1s` when the transition is extended to two seconds. Retiming uses media-time precision and validates the converted times and keyframe order. Preview and export derive local time and normalized progress from the same transition range to evaluate parameter curves and animated masks, respectively.

Edit operations allocate IDs inside the callback and take effect at commit time. A failed transaction rolls back in full. The returned immutable snapshot provides a read model for tracks and instances for timeline reconstruction, autosave, and export. A saved project records asset references that can be reopened or managed copies. If access is lost, relink the original file by resource key. Each project snapshot owns its media catalog, and reopening restores its media keys and recorded paths. Specify a project revision and range when exporting an excerpt. [Lib-Media presentation intervals and trimming](../../Lib-Media/docs/media-container.md#track-presentation-intervals)

## Template assembly

Scene templates store nodes, versioned component parameters, relationships, exposed parameters, and resource references. Loading creates entities in the current world and rebuilds their relationships. Editing templates also describe user asset slots, trimming and ordering rules, tracks, transitions, and music. Capture and editing can reference the same scene template. A single resource manifest resolves versions of the pack's PNGs, PAG animations, music, masks, models, and JS.

```cpp
namespace huxerui::avkit {

struct MediaSlot {
    std::vector<AssetKind> accepts;
    std::uint32_t min_count, max_count;
    std::optional<media::MediaTimestamp> min_duration;
};
struct TextSlot { bool required{}; };
struct TemplateSlot { std::string id; std::variant<MediaSlot, TextSlot> input; };
struct EditTemplateInfo { PixelSize canvas_size; std::vector<TemplateSlot> slots; };
struct SlotAsset { ResourceRef asset; std::optional<std::uint32_t> track; std::optional<TimeRange> range; };
struct TemplateSlotBinding { std::string slot; std::variant<std::vector<SlotAsset>, std::string> value; };
struct EditTemplateInputs { std::vector<ResourceRef> ordered_assets; std::vector<TemplateSlotBinding> bindings; };
enum class SlotIssueCode { Missing, Incompatible, TooShort, TooMany };
struct SlotIssue { std::string slot; SlotIssueCode code; std::string message; };
class EditTemplatePlan;
struct EditTemplatePreparation {
    std::shared_ptr<const EditTemplatePlan> plan;
    std::vector<SlotIssue> issues;
};

class Engine;
AvKitResult<EditTemplatePreparation> PlanEditTemplate(
    Engine& engine, std::shared_ptr<const ProjectSnapshot> project, const ResourceRef& recipe,
    const EditTemplateInputs& inputs, std::stop_token stop = {});
AvKitResult<std::shared_ptr<const ProjectSnapshot>> ApplyEditTemplate(Project& project,
                                                                    const EditTemplatePlan& plan);

}
```

Explicit bindings take precedence. Fill the remaining slots in selection order, subject to their constraints. Planning uses the available source-track ranges to validate duration, frame dimensions, transition handles, and fallback rules for optional assets. Blocking issues return slot diagnostics and a null plan. Commit checks the assets, pack versions, target project identity, and revision. The first release assembles an empty timeline from assets already imported into the project and produces a complete project snapshot in one commit. Export can use the interval from `Range()`. Replacing clips uses the regular project editing interface. Preset beat markers and later highlight analysis can both supply segment selections to the plan.

A 12-second template can cut at seconds 4 and 8 with a 0.4-second overlap at each cut. The three clip ranges in project time are `[0, 4.2)`, `[3.8, 8.2)`, and `[7.8, 12)`. The plan selects each clip's source range independently. The template scales transition keyframes to the actual overlap duration and writes them to the same project effect instance within the assembly transaction. PAG video placeholders bind to their respective clips, and music becomes an editable audio track.

`EditTemplateRecipe` can include canvas overlays as well as track clips. Every asset, mask, font, and PAG replacement uses a key from the active catalog. Register pack entries with distinct keys before assembling the recipe. Template media slots keep their registered key when the local file becomes a project media record. [Template API](../include/huxerui/avkit/edit_template.h)

Frame scripts modify local parameters through declared template nodes. Core evaluates base parameters and keyframes, applies script changes, and then evaluates parent-child transforms. It calls scripts in scene or clip-local time. The first release uses stateless scripts with a bounded execution budget. Invalid properties or a timeout cause the current frame to use its base parameters. On targets without a script runtime, use the pack's declared alternative effect or return a capability error.

```js
export function onFrame({ timeSeconds }) {
    return [{
        node: "face-sticker",
        transform: { rotationZRadians: 0.16 * Math.sin(timeSeconds * 2 * Math.PI) },
    }];
}
```

### Template masks

Pack both static and animated masks as `ResourceKind::Mask`. Effects and transitions refer to them through `ResourceRef`, and multiple instances can share one resource entry. For an exposed mask parameter, select a replacement of kind `Mask`. Replacement and undo are part of project editing.

| Manifest or binding field | Meaning |
| --- | --- |
| Files and channel | A static resource contains one PNG. An animated resource contains a presentation-ordered sequence of PNGs with matching dimensions. The manifest selects grayscale or alpha, and decoded values represent coverage. `0` means uncovered and `1` means fully covered. An effect parameter controls inversion. |
| Sampling time | A sequence declares a positive `MediaTimestamp` frame interval. Regular effects sample from the time their instance becomes active and hold the final frame at the end. Looping resources take time modulo the sequence duration. Transitions map progress within the half-open overlap to the full sequence and select a frame by its interval. Preview, seek, and export use the same mapping. |
| Binding coordinates | The effect declares canvas, clip, or tracking anchor space and uses the corresponding layer layout and local transform. Template coverage is zero outside the image bounds. |

Applying a template validates mask versions, resource kinds, channels, and timing. The timeline binds the sampled pack mask by resource reference and frame index. The renderer caches the retained data image using that identity. A template can also declare a portrait segmentation dependency. Core retrieves that analysis mask by source frame. A circular window effect can multiply a circular mask resource by portrait coverage. Both inputs use the same sampler binding mechanism. [Data textures](#data-textures)

`TemplateClip::background` and `TimelineClip::background` store a `BackgroundReplacementSpec`. Project projection attaches a `BackgroundReplacement` component containing the fill color and exactly one source component to the same clip entity. Use `PortraitMask`, `StaticMask`, or `AnimatedMask` to select the source in the spec. The coverage system validates that each background effect has one source and publishes a frame-local `BackgroundMask` with its sampling and blend mode. A static mask's `MaskImage` selects grayscale or alpha coverage from the referenced PNG. Color and coverage use the same oriented source coordinates under `SourceCrop` and mirroring. Static and animated replacements use source-over blending. The portrait path uses the clip's blend mode. `ProjectEdit::SetBackgroundMaskResource` changes a packaged source reference while preserving the other mask settings. [Project API](../include/huxerui/avkit/project.h)

`TimelineFrame::data_inputs` includes unique static resource references for the requested frame. The compositor supplies analysis masks, depth maps, static masks, clip animations, and transition masks in slot order. The example registers 8-bit grayscale PNGs for its masks. Each renderer validates the mask format and caches the decoded R8Unorm image by catalog key. Preview and export use matching references and UV mappings. [Timeline API](../include/huxerui/avkit/timeline.h)

Assign an `AnimatedMask` to `BackgroundReplacementSpec::mask` to select a mask sequence and a per-clip `AnimationPlayback` policy. The coverage system divides the clip-local project time by the exact sequence interval and rounds down. Input preparation resolves the selected frame in the active catalog. The render plan retains the resulting coverage pixels or a GPU texture. The renderer reuses the uploaded mask when the same pixels appear again. Each frame declares its own `MaskImage` and channel. Color decodes are cached separately from coverage, and the coverage cache is scoped by catalog, resource key, and channel. The hold policy stops at the last frame and the loop policy wraps at the sequence duration. Seeking and descending project-time sampling evaluate the requested time directly. Changing a clip's source direction changes which video frame is sampled at that project time. [Timeline API](../include/huxerui/avkit/timeline.h)

## Thumbnails and waveforms

Core resolves assets and tracks from the project snapshot and samples in source-track presentation time. The timeline maps visible intervals to source time using each clip's trim, speed, and direction, then draws the results over the corresponding project intervals. Playback has its own `TimelineSession`. Asset jobs use separate cursors and share codec resources through a bounded background queue. Playback has priority over background sampling.

```cpp
namespace huxerui::avkit {

struct ThumbnailRequest {
    ResourceRef asset;
    std::optional<std::uint32_t> track;
    std::vector<media::MediaTimestamp> source_times;
    PixelSize size;
};
struct Thumbnail {
    media::MediaTimestamp pts;
    ImageAsset image;
};
using ThumbnailStrip = std::vector<std::optional<Thumbnail>>;

struct WaveformRequest {
    ResourceRef asset;
    std::uint32_t track;
    TimeRange source_range;
    media::MediaTimestamp bucket_interval;
};
struct WaveformPeak { float minimum, maximum; };
struct Waveform {
    media::MediaTimestamp start, bucket_interval;
    std::uint32_t channels;
    std::vector<WaveformPeak> peaks;
};

}
```

Specify a source track for video and sampleable animations. The decoder can group nearby requests and return an orientation-normalized `ImageAsset` and its actual presentation PTS for each item. Results follow request order. Empty presentation intervals in the container yield empty entries. Validate requests outside the presentation range before scheduling. For a still image, omit the track and request one thumbnail at time zero.

Waveform buckets align to the start of the source track's presentation interval. Return complete buckets covering the requested range and clip the drawing to the clip interval. Take the minimum and maximum PCM value per channel in each bucket. `peaks` interleaves channels within each bucket. Empty presentation intervals count as silence. When zooming out, merge adjacent buckets at compatible resolutions to preserve peaks.

Cache keys include the retained catalog media record, local file metadata, track, and sampling settings. Multiple clips using the same asset can reuse images and waveform buckets. A job captures the media record at launch. The UI accepts results by request generation for each visible item and replaces affected requests after asset relinking. Scrolling or zooming can cancel requests that leave the visible area. Shared sampling continues while other consumers still need it.

## Production sessions and output

Recording and export share container, video, and delivery settings. With `RecordingRequest::audio` omitted, the output is silent. When set, it specifies both the recording audio source and output settings such as AAC. `ExportRequest::audio` controls whether to encode the evaluated project audio. The media adapter determines the pixel layout, memory location, and negotiated format for encoding.

```cpp
namespace huxerui::avkit {

enum class CodecPolicy { Auto, SoftwareOnly };
enum class StreamDelivery { AfterFinish, Direct };
struct VideoColor {
    media::ColorPrimaries primaries{media::ColorPrimaries::BT709};
    media::ColorTransfer transfer{media::ColorTransfer::BT709};
    media::ColorMatrix matrix{media::ColorMatrix::BT709};
    media::ColorRange range{media::ColorRange::Limited};
    std::uint8_t bit_depth{8};
};
struct VideoOutput {
    media::CodecId codec{media::CodecId::H264};
    std::optional<PixelSize> size;
    std::optional<media::MediaTimestamp> frame_interval;
    std::optional<std::uint32_t> bitrate;
    std::optional<media::H264Profile> h264_profile;
    std::optional<VideoColor> color;
};
struct AudioOutput {
    media::CodecId codec{media::CodecId::Aac};
    std::optional<std::uint32_t> sample_rate, bitrate;
    std::optional<media::AudioChannelLayout> channel_layout;
};
struct MovieOutput {
    media::ContainerFormat container{media::ContainerFormat::Mp4};
    media::ContainerLayout layout{media::ContainerLayout::Default};
    VideoOutput video;
    StreamDelivery delivery{StreamDelivery::AfterFinish};
    CodecPolicy codec_policy{CodecPolicy::Auto};
};
enum class RecordingMode { SingleFile, Segmented };
enum class RecordingAudioSource { Microphone, Music, Mix };
struct RecordingAudio { RecordingAudioSource source; AudioOutput output; };
struct RecordingRequest {
    MovieOutput output;
    std::optional<RecordingAudio> audio;
    RecordingMode mode{RecordingMode::SingleFile};
};
struct ExportRequest { MovieOutput output; TimeRange range; std::optional<AudioOutput> audio; };
using MovieDestination = std::variant<File, RandomAccessStream, OutputStream>;
struct StreamReceipt { std::uint64_t bytes_written; };
struct MoviePart {
    std::variant<File, FileReference, StreamReceipt> artifact;
    media::MediaTimestamp timeline_start, duration;
};
struct MovieResult { std::vector<MoviePart> parts; std::optional<Error> interruption; };
enum class WorkPhase { Preparing, Running, Paused, Draining, Publishing, Completed, Failed, Canceled };
struct WorkProgress {
    WorkPhase phase;
    media::MediaTimestamp position;
    std::optional<media::MediaTimestamp> total;
    std::uint32_t attempt;
};
using OnProgress = std::function<void(WorkProgress)>;

class MovieJob : public ResultJob<MovieResult> {
public:
    virtual WorkProgress Progress() const = 0;
};

struct SceneSnapshot {
    std::uint64_t revision;
    VisualState visual;
    AudioState microphone;
    std::optional<ResourceRef> music;
    AudioState music_mix;
    std::optional<Ducking> ducking;
};
class CaptureEdit {
public:
    VisualEdit& Visual();
    AudioEdit& Microphone();
    void SetMusic(std::optional<ResourceRef> music);
    AudioEdit& Music();
    void SetDucking(std::optional<Ducking> ducking);
};

class RealtimeSession;
class TimelineSession;
class CodecProvider;
namespace render { class RenderBackend; }
namespace analysis { class AnalysisProvider; }

class Engine {
public:
    std::vector<EffectDescriptor> EffectTypes() const;
    AvKitResult<EditTemplateInfo> DescribeEditTemplate(const ResourceRef& recipe) const;
    AvKitResult<std::shared_ptr<Project>> CreateProject(PixelSize canvas, File asset_directory);
    AvKitResult<std::shared_ptr<Project>> OpenProject(ProjectSource source);
    AvKitResult<void> SaveProject(std::shared_ptr<const ProjectSnapshot> project, ProjectSource target);
    AvKitResult<ResourceRef> Import(Project& project, ImportSource source, ImportMode mode);
    AvKitResult<std::shared_ptr<RealtimeSession>> OpenRealtime(PixelSize canvas,
                                                               std::function<void()> wakeup = {});
    AvKitResult<std::shared_ptr<TimelineSession>> OpenTimeline(
        std::shared_ptr<const ProjectSnapshot> project, std::function<void()> wakeup = {});
    AvKitResult<std::shared_ptr<ResultJob<ThumbnailStrip>>> StartThumbnails(
        std::shared_ptr<const ProjectSnapshot> project, ThumbnailRequest request,
        std::function<void()> wakeup = {});
    AvKitResult<std::shared_ptr<ResultJob<Waveform>>> StartWaveform(
        std::shared_ptr<const ProjectSnapshot> project, WaveformRequest request,
        std::function<void()> wakeup = {});
    AvKitResult<std::shared_ptr<MovieJob>> StartExport(std::shared_ptr<const ProjectSnapshot> project,
                                                     MovieDestination target, ExportRequest request,
                                                     std::function<void()> wakeup = {});
};
AvKitResult<std::shared_ptr<Engine>> CreateEngine(
    std::shared_ptr<render::RenderBackend> renderer, std::vector<std::shared_ptr<CodecProvider>> codecs = {},
    std::vector<std::shared_ptr<analysis::AnalysisProvider>> analysis_providers = {});

}
```

`ResultJob` returns an empty value from `TryTakeResult` while work is pending and delivers a result or failure once. `Cancel` requests termination. Production jobs also expose progress through `MovieJob`. For regular files, stage the successfully muxed output before delivering it according to the destination's commit semantics. `AfterFinish` can also copy a muxed file to a provider or stream. `Direct` requires compatible write capabilities, and the receiver retains any prefix already written. The first release accepts a local `File` for segmented recording and returns the recording time offset of each completed segment. An interruption can return usable output with its cause. Return an error when no usable output exists. Recording follows the source cadence by default. The recording configuration can specify a fixed frame rate. Export requires a target frame interval or preset.

### HuxerUI entry points

The UI suite uses `Task` to bridge Core jobs, device controls, and asynchronous file providers. Codec and GPU work runs on its designated execution sequence. Edit callbacks run on the caller's side. The session serializes resource preparation and edit commits. A successful live edit returns a new scene revision. Video and DSP select revisions by their effective time. Canceling preparation can preserve the current scene. Asynchronous results from earlier revisions retire by generation.

```cpp
#include <huxerui/audiocapture.h>
#include <huxerui/camera.h>
#include <huxerui/media_playback.h>
#include <huxerui/task.h>

namespace huxerui::avkit {

enum class PhotoMode { OriginalStill, ProcessedStill, PreviewSnapshot };
struct PhotoRequest {
    PhotoMode mode{PhotoMode::OriginalStill};
    camera::PhotoOptions camera_options;
    std::optional<PixelSize> processed_size;
};
using UiMovieDestination = std::variant<File, FileReference, RandomAccessStream, OutputStream, AsyncOutputStream>;
enum class RecordingRoute { Auto, Camera, AvKit };

class CaptureSession {
public:
    camera::CameraSession Camera() const;
    std::optional<audio::CaptureStatus> MicrophoneStatus() const;
    std::shared_ptr<ExternalTexture> PreviewTexture() const;
    std::shared_ptr<const SceneSnapshot> SnapshotScene() const;
    Task<AvKitResult<std::shared_ptr<const SceneSnapshot>>> EditAsync(std::function<void(CaptureEdit&)> changes);
    Task<AvKitResult<ImageAsset>> CapturePhotoAsync(PhotoRequest request);
    Task<AvKitResult<MovieResult>> RecordVideoAsync(UiMovieDestination target, RecordingRequest request,
                                                  RecordingRoute route = RecordingRoute::Auto);
    WorkProgress RecordingProgress() const;
    std::optional<RecordingRoute> ActiveRecordingRoute() const;
    AvKitResult<void> PauseRecording();
    AvKitResult<void> ResumeRecording();
    AvKitResult<void> StopRecording();
};

class EditSession {
public:
    AvKitResult<void> ApplyProject(std::shared_ptr<const ProjectSnapshot> project);
    AvKitResult<void> Play();
    AvKitResult<void> Pause();
    AvKitResult<void> Seek(media::MediaTimestamp position);
    media::PlaybackSnapshot Playback() const;
    std::shared_ptr<ExternalTexture> PreviewTexture() const;
};

[[huxerui::composable]]
CaptureSession UseAvKitCapture(Engine& engine, camera::CameraOptions options,
                              std::optional<audio::CaptureOptions> microphone = {});
AvKitResult<std::shared_ptr<EditSession>> OpenEditorPreview(Engine& engine,
                                                          std::shared_ptr<const ProjectSnapshot> project);
Task<AvKitResult<ResourceRef>> ImportAsync(Engine& engine, std::shared_ptr<Project> project,
                                      ImportSource source, ImportMode mode);
Task<AvKitResult<std::shared_ptr<Project>>> OpenProjectAsync(Engine& engine, ProjectSource source);
Task<AvKitResult<void>> SaveProjectAsync(Engine& engine, std::shared_ptr<const ProjectSnapshot> project,
                                       ProjectSource target);
Task<AvKitResult<EditTemplatePreparation>> PlanEditTemplateAsync(
    Engine& engine, std::shared_ptr<const ProjectSnapshot> project, ResourceRef recipe, EditTemplateInputs inputs);
Task<AvKitResult<ThumbnailStrip>> LoadThumbnailsAsync(
    Engine& engine, std::shared_ptr<const ProjectSnapshot> project, ThumbnailRequest request);
Task<AvKitResult<Waveform>> LoadWaveformAsync(
    Engine& engine, std::shared_ptr<const ProjectSnapshot> project, WaveformRequest request);
Task<AvKitResult<MovieResult>> ExportAsync(Engine& engine, std::shared_ptr<const ProjectSnapshot> project,
                                         UiMovieDestination target, ExportRequest request, OnProgress progress = {});

}
```

## Media timing, DSP, and finalization

### Recording clock and audio

Each recording holds one microphone reader. Camera-managed recording uses H.264, optional AAC-LC, and the corresponding MP4 layout under Camera's public contract. A visual effect must return the matching frame within the synchronous frame callback. This route can accept processed PCM that preserves `PcmTiming::CaptureTime`. Changing the AudioCapture processor factory restarts capture. Live parameter changes use the current processor instance at block boundaries. AVKit manages mixing with background music, complex live scenes, and hardware and software codec selection. `Auto` validates active effects, audio sources, clocks, and output settings before opening the destination. [Camera-managed recording](../../Lib-Camera/include/huxerui/camera.h#L508-L542) · [AudioCapture PCM contract](../../Lib-AudioCapture/include/huxerui/audiocapture.h#L112-L148)

AVKit-managed camera and microphone recording uses Camera's `FrameObservation` for source generation, physical dimensions, orientation, mirroring, and an optional timestamp on AudioCapture's clock domain. Acquire an immutable platform snapshot within the observer before dispatching work. On macOS, Camera and AudioCapture map each session's CoreMedia time to the host clock. A standalone host can submit inputs it has already correlated. With no shared clock, `Auto` can delegate to Camera when that route satisfies the full request. Other requests that include a microphone return `ClockUnavailable`. Once recording starts, the route and output settings are fixed, and scene updates are validated against the active route's capabilities. [Camera frame observation](../../Lib-Camera/include/huxerui/camera.h) · [AudioCapture capture clock](../../Lib-AudioCapture/README.md#synchronize-with-camera) · [Apple capture session clock](https://developer.apple.com/documentation/avfoundation/avcapturesession/synchronizationclock)

At a given recording time, apply voice effects, gain, and envelopes to the microphone PCM. Decode music separately, sample it against the recording clock, then mix it in. Editing uses the same DSP on audio clips and tracks. The sidechain feeds the voice envelope into background music attenuation, with the connection stored explicitly in the project. Live pitch shifting preserves duration and compensates for algorithmic latency. Speed changes and reverse playback use the project-time mapping. Offline loudness processing can analyze audio before applying gain. Audio blocks advance continuously by output sample count, with drift correction where supported.

Pause and resume define active intervals on the recording clock. Video and PCM exclude the same gaps. Stopping sets the cutoff and closes video input for the recording. The audio reader closes input on its capture clock and reads to EOF. Core trims queued tail PCM to the active interval before draining DSP and codecs. The UI awaits the original recording task while muxing finishes. When a Core job or UI `Task` is canceled, adapters hold resources until retirement and handle delivered output according to the destination's semantics. Device or encoder loss reports an interruption. Segmented recording can continue with a new random-access segment. [AudioReader input finalization](../../Lib-AudioCapture/include/huxerui/audiocapture.h#L283-L319)

### File decoding, preview, and export

Map each asset to source time using the track's presentation interval. `MediaDemuxer` supplies compressed packets by sample description. Seeking selects an independently decodable point, decodes through preroll, and selects frames by PTS. Clips reading different positions in the same asset concurrently use separate cursors. An edit or seek advances the generation, and display and playback discard stale results. Audio DSP and temporal visual effects preroll from a fixed interval or a verifiable state. [Lib-Media demuxing and seeking](../../Lib-Media/docs/media-container.md#read-and-seek)

Preview selects the due image using the PCM position confirmed by the playback device. Use a monotonic clock when there is no audio track. Publish successive frames to an `ExternalTexture` of a fixed logical size. Offline export uses its own world and samples at a controlled frame rate and sample count. The job scheduler handles backpressure. Hand off render output according to the encoder's capabilities. For Lib-Media's `MakeVideoFrame`, serialize publication and snapshot acquisition to pair the correct frame with its time. [Lib-Media playback](../../Lib-Media/include/huxerui/media_playback.h#L220-L266) · [Texture to encoder frame](../../Lib-Media/include/huxerui/media_codec.h#L1074-L1095)

The muxer gets track descriptions and parameter sets from encoder format events. Use `ValidateOutput` to check destination capabilities. When `TryWrite` asks for a packet on a specific track, retain the original packet, advance that track, and retry. A regular MP4 can supply audio priming and padding in `EndTrack`. Choose fMP4 according to the edit intervals known at open time and the available capabilities. Encoded packets carry PTS, DTS, and duration. Publish the output after `Finish`. [Lib-Media muxing and finalization](../../Lib-Media/docs/media-container.md#write-and-finish)

## EntityX runtime and extensions

The project snapshot owns its media catalog, `TimelineTrack` entries, and `TimelineClip` entries. Clips carry stable IDs, authored ranges, resource keys, and edit parameters. The timeline projects each new revision into an EntityX scene. Capture uses the same overlay projection as the editor, including timed layers, typography, replacements, and stable instance IDs. `CameraRecording::SetOverlays` publishes a complete authored layer list for live frames. Components can be ordinary C++ structs, including types defined by an application. The entity manager and its entities belong to one execution sequence. ECS entities are session-local and project I/O continues to use stable project identifiers. [Scene API](../include/huxerui/avkit/scene.h) · [Scene components](../include/huxerui/avkit/component.h) · [EntityX 1.3.0](https://github.com/alecthomas/entityx/tree/1.3.0)

`avkit/component.h` groups AVKit's ECS-specific declarations by who supplies them and how long they live. Applications can attach authored inputs to their own entities. Source adapters publish the current picture or PCM block. The scene owns its relationships, root context, and evaluated layer state. Extensions can read the current state on the owning sequence. The internal root records retain borrowed services and per-evaluation requests for the durations documented in the header.

Project, timeline, and PAG headers share the component declarations. A drawable layer attaches its content as `StillImageSource` or `PagOverlay`. An optional `TimeRange` gates it on the scene clock, or on the owning clip's local project clock when the layer has `ClipAttachment`. A layer without a range follows its selected clock from zero. Spatial parenting, tracking, and depth are independent components. `avkit/types.h` supplies the stable IDs, intervals, and affine mapping shared with the render API.

`Transform2D` places a unit layer in its parent's coordinates using position, scale, rotation, and pivot. The tracking system writes the computed local `Affine2D` on its anchor entities. The spatial system resolves the same parent chain for the picture and its depth registration. When preview requests editor geometry, it measures the instance layout separately in output canvas pixels. A paused gesture supplies a stable project ID. The timeline resolves it to an entity and runs spatial compilation with a temporary entity transform against retained inputs. `Transform3D` describes a parent-relative placement using an XYZW unit quaternion.

The timing stage derives `ClipPresentationRange` from the authored `ClipTiming` and current transition links. The sampling stage uses that visibility to publish each active clip's source time in `ClipSampleTime`. The preroll stage uses the same ranges to choose the next two useful entries. The video source system resolves the selected asset and track from the clip components before sampling and invalidates the cursor and analysis state when its source changes. The decoder service owns the bounded workers, cursor handoff, and per-clip fallback. Visible clips publish `SampledVideo` for both main-track and picture-in-picture video. It borrows the decoded picture during evaluation and carries the source-to-texture UV mapping. The same entity retains the current `ImagePixels` or `Texture` through paused spatial recompilation until the next sample or seek. The picture's PTS can precede the requested source time when it covers that time. The clip sampling system derives source geometry from the crop and fit components and writes `render::SourceSampling`. Applications can opt into source-aspect fitting for their picture-in-picture policy. This uses the current frame geometry and applies the same policy in preview and export. The tracking system resolves sticker anchors. The coverage system writes sampled mask and depth components after submitting all visible jobs on their respective sessions. The bound device validates GPU analysis results before they enter the render tree. Preview omits an incompatible result on the affected clip, while offline export reports a diagnostic.

The camera adapter attaches the current imported `render::Texture` to one camera entity. `CameraFrameGeometry` carries crop, orientation, and mirroring, and the camera layout system fits the frame to the target. Each timeline video or still image clip has its own entity and uses `ClipSampleTime` to determine participation. Capture and editing share the overlay timing, image resource, and PAG systems. The timing system writes `LayerSampleTime` on active overlays, and the PAG system retains an evaluated `PagLayerSample` with the authored extent for canvas-fitted text. A timed `render::Texture` can also supply a clip's draw while `SampledVideo` provides its source geometry and analysis. Attached artwork is skipped if its owning clip or required tracking anchor has retired.

`RenderScene` schedules built-in and application systems together. The built-in registration functions live in `src/ecs/system`. Timeline and capture sessions supply frame inputs and evaluate their scenes through the same entry point. The timeline video source owns decode cursors and preroll workers. Seeking or retiring a cursor publishes a clip-scoped `VideoSourceInvalidatedEvent`, which the tracking and coverage systems use to retire analysis state. Those systems derive their requirements from the clip's background and sticker components. AVKit event types use the `Event` suffix. [Scene events](../src/ecs/scene_events.h) · [Timeline video source](../src/media/timeline_video_source.cpp) · [Built-in system registration](../src/ecs/system/scene_systems.cpp)

Source sampling runs before the default extension stage. Layer geometry, analysis, and overlay sampling then consume changes such as crop and PAG replacements on that same frame. Use `RegisterSystemAt` with `BeforeSampling` to change clip timing before sources are selected, or with `BeforeCompile` to consume or adjust resolved layer data. Each stage runs in registration order. [RenderScene API](../include/huxerui/avkit/scene.h)

Each EntityX world has one scene-owned root entity. Its `VideoFrameContext` supplies the exact media timestamp and target geometry. `SceneCatalog(root)` borrows its read-only catalog on the scene sequence. A configured analysis provider is a separate root component. The timeline adapter attaches its decode and analysis options for the duration of the update. The capture adapter supplies its camera frame directly on the camera entity. Timeline services share the root for the lifetime of that scene. Audio publishes a separate `AudioBlockEvaluation` with a sample count and format and runs the audio stages at that PCM clock. The live audio scene receives the microphone block on its own worker sequence with `LivePcmInput` and the active `VoiceEffect` component. Built-in systems query these components in their updates and own only their persistent state. [Scene evaluation](../src/ecs/scene_evaluation.h)

The spatial system reads drawable components and `LayerStyle` to build a full-canvas `CompositeNode` in `RenderPlan`. Camera and shared decoder textures use `TextureNode`. Decoded still images and fitted software video frames use `ImageNode` with retained `ImagePixels`. PAG compositions use `PagNode`. Background replacement and depth occlusion are separate operations on their corresponding inputs. `TransitionLink` supplies the scene relationships for the two sides. Each transition node owns two canvas-sized composites, including their attached artwork, and its evaluated mask or PAG sample. The renderer lowers the typed tree to GPU passes, uploads retained CPU inputs, and batches compatible adjacent PAG nodes at the target size in compositing order. [Render API](../include/huxerui/avkit/render.h) · [Timeline API](../include/huxerui/avkit/timeline.h)

Call `TimelineSession::InstallSceneModule` once per session to configure extensions on its entity manager and on subsequent project revisions. Register one EntityX system per concrete type. Query `VideoFrameContext` for exact media time during an update. An explicitly supplied positive `continuous_step` is passed to video systems in seconds as EntityX `TimeDelta`. Random-access evaluations, spatial recompilation, and separate audio or preroll stages receive zero delta. `RenderScene::Root()` exposes the root for scene-wide extension components, and `RenderScene::Frame()` reads its frame context. Components share EntityX's pool of 64 registered types across scenes in a process. Each scene owns its data on one execution sequence. AVKit makes upstream family ID allocation atomic so preview, capture, and export can register types on different sequences. [EntityX build integration](../cmake/AVKitEntityX.cmake)

```cpp
#include <huxerui/avkit/component.h>
#include <huxerui/avkit/scene.h>

using namespace huxerui::avkit;

struct Drift { float units_per_second; };

class DriftSystem final : public entityx::System<DriftSystem> {
public:
    void update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) override {
        entities.each<VideoFrameContext>([&](entityx::Entity, const VideoFrameContext& frame) {
            const auto seconds = double(frame.time.ticks) / frame.time.timescale;
            entities.each<Drift, Transform2D>([&](entityx::Entity, const Drift& drift, Transform2D& local) {
                local.translation.x = static_cast<float>(drift.units_per_second * seconds);
            });
        });
    }
};

void InstallMotion(RenderScene& scene) {
    scene.Entities().each<ClipId>([](entityx::Entity entity, const ClipId&) {
        entity.assign<Drift>(0.02F);
    });
    scene.RegisterSystem<DriftSystem>();
}
```

`RenderScene::SetSpatialParent` validates live entities and rejects cycles. `Destroy` detaches direct spatial children and keeps them live. Track membership and compositing order retain their own semantics. Paused transform edits compile against the retained sampled components, and audio mixing continues to sample its own PCM intervals. Seeking retires obsolete decode and analysis work. GPU executors release submitted surfaces according to the backend's completion rules.

### Linked audio and voice processing

When a video clip owns linked original audio, the timeline projects `AudioSource`, `AudioMix`, and any `VoiceEffect` onto that clip's entity. `AudioSource` records the selected track and its offset relative to the video's source trim. `ClipTiming` supplies the shared project interval, trim, and playback direction. The audio system reads those components for each PCM block. Extensions can adjust parameters in `BeforeAudio` and read the completed block in `Audio`. Capture, preview and export use that system. Decoder cursors and reverse audio windows belong to the media source service and are keyed by entity. Reverse windows start on a grid anchored to the trim so playback and seek decode the same source interval.

`ProjectEdit::SetVoiceEffect` accepts the owner of linked audio or an independent audio clip. The project stores the optional effect on the owner. The editable audio record keeps its existing identifier and remains hidden while linked. Detaching it transfers the effect to the independent audio clip. Preview and offline export sample the same mixer. The robot effect uses the clip-local output sample index and preserves the exact sample count. [Project API](../include/huxerui/avkit/project.h) · [Audio system](../src/ecs/system/audio_system.cpp)

## Vision analysis and tracking anchors

Core schedules analysis according to the current scene's required capabilities. Beauty effects, stickers, and virtual characters can share facial landmarks from the same source. Body pose estimation and portrait segmentation can use separate models. Models run on the analysis sequence and queue results for the owning world to accept. Rendering derives anchors and effect parameters from matched results. [External inference providers](#external-inference-providers)

**Describe submission mode and temporal context separately.** A single-image model can run asynchronously on a worker thread. Trackers and recurrent models can accept frames individually and depend on previous state. Other models require an explicit window of frames before and after the target. The model description declares the sampling interval, history and lookahead lengths, whether it is stateful, and whether it can handle input gaps. MediaPipe's live stream mode uses asynchronous callbacks. VideoPose3D illustrates a temporal model that consumes a sequence of body landmarks across frames. [MediaPipe Face Landmarker](https://developers.google.com/edge/mediapipe/solutions/vision/face_landmarker?tab=Python) · [VideoPose3D](https://github.com/facebookresearch/VideoPose3D)

```cpp
namespace huxerui::avkit::analysis {

struct FrameKey {
    std::uint64_t source, generation, sequence;
    media::MediaTimestamp pts;
    bool operator==(const FrameKey&) const = default;
};
struct AnalysisWindow {
    std::uint32_t past_frames{}, future_frames{};
    std::optional<media::MediaTimestamp> sample_interval;
    bool stateful{}, accepts_gaps{};
};
struct Landmark {
    std::string name;
    Point position;
    std::optional<float> confidence;
};
enum class TrackedSubject { Face, Body, Hand };
struct SubjectLandmarks {
    std::uint64_t subject_id;
    TrackedSubject subject;
    std::vector<Landmark> points;
};
template<class T> struct AnalysisResult {
    FrameKey target;
    AvKitResult<std::optional<T>> result;
};
using LandmarkResult = AnalysisResult<std::vector<SubjectLandmarks>>;

}
```

The analysis service submits leased input frames through a bounded queue. A result's `target` identifies the analyzed source frame, with `pts` in source media time. Core assigns source instances and frame sequence numbers. Two clips using the same resource key each have their own sampling instance. An absent result is pending. Success with an empty `optional` means the target was skipped. A result containing an empty landmark subject list means detection found no subjects. `AvKitResult` carries the reason for a failure. The result cache is keyed by analysis job and target frame. Model versions, parameters, and model subject IDs are scoped to their analysis session.

A source switch, seek, input geometry change, or model change assigns a new generation to the affected source and retires its previous state. Late results from retired generations are handled only to release resources. Results in the current generation can arrive out of order. Live tracking state advances forward only, and exact sampling looks up results by frame identifier.

| Use case | Sampling rules |
| --- | --- |
| Live preview and recording | Prefer the matching frame result within the latency budget. Stickers that permit approximation can use an unexpired anchor from the same source, generation, and subject, at or before the target frame, or predict from it over a short interval. Models that require consecutive samples reset and preroll across gaps according to their contract. Effects that need future frames use bounded frame buffering or declare a live alternative. With buffering, synchronize audio and video by original capture time and deliver the same composite to preview and recording. |
| File seeking, reverse playback, and offline export | Prepare the required window for the target frame. Preroll stateful analysis forward in source presentation order or restore a verified checkpoint. Reverse playback reads the corresponding results in project time. Export waits for the requested target-frame result and handles missing samples at file boundaries according to the model definition. |
| Still photos | Run detection again on pixels decoded from the separate JPEG. Models that depend on video state use their declared single-image path or return a capability result. |

Camera-managed recording must deliver the matching image within the synchronous frame callback. Templates that buffer frames while waiting for a matching result use the Core-managed recording route. Route negotiation validates each effect's analysis requirements. [Camera frame processing callback](../../Lib-Camera/include/huxerui/camera.h#L222-L240)

Analysis landmarks use normalized coordinates within the valid source image after orientation correction and before display mirroring, with a top-left origin. Undo model-input scaling, padding, and cropping when mapping landmarks into that space. Core computes final coordinates in the actual render order for mirroring, project cropping, picture-in-picture, and warping. 2D anchor points, expressions, skeletal poses, masks, and depth share the frame identifier. [Data textures](#data-textures) · [Camera frame geometry](../../Lib-Camera/include/huxerui/camera.h#L201-L219)

**Stickers are children of their anchors.** `TrackingAnchor` binds to the camera image or clip that owns the sticker and samples the selected person track by body part and semantic point name. Left and right are from the subject's perspective. A single point drives position, and local offset and size are evaluated in the owning image's normalized layout. With two points, the first point is the origin, the line between them is the horizontal axis, and their pixel distance is the local unit of length. Glasses can use the centers of both eyes. Treat missing, expired, or coincident points as tracking loss. Animation keyframes and JS update the sticker's local transform relative to the anchor.

### Cross-model person association

Core maintains person tracks per source and generation. A model uses `subject_id` to track within its own analysis session. `PersonTrackId` associates body parts within one generation of a source. Each part references an immutable landmark result and records its actual observation frame for checking the sampling interval and freshness.

```cpp
namespace huxerui::avkit::analysis {

struct PersonTrackId {
    std::uint64_t value;
    bool operator==(const PersonTrackId&) const = default;
};
struct TrackedPartSample {
    FrameKey observed;
    std::shared_ptr<const SubjectLandmarks> points;
};
struct TrackedPersonSample {
    FrameKey target;
    PersonTrackId person;
    std::optional<TrackedPartSample> face, body, left_hand, right_hand;
};

}
```

When only face effects are active, face tracks can establish `PersonTrackId`. For combined tracking, prefer deriving head and wrist ROIs from body tracks. Submit local inference with the target frame and person track, then validate the result against position and confidence at the target time. Grouped outputs from a joint model can be registered directly to the same track. Independent full-frame models use one-to-one matching by head position, wrist distance, and track continuity on the same source frame. Leave the part empty when candidates overlap or confidence is low. Match in a common 2D source coordinate space, then bring 3D data for virtual characters into character space using each model's calibration and skeletal retargeting. [MediaPipe Holistic outputs](https://developers.google.com/edge/mediapipe/solutions/vision/holistic_landmarker)

Validate asynchronous results against the track snapshot for their target frame. Published frames keep their frozen results. Live effects can reuse part data for the same person up to an age limit. Use a late result in subsequent frames only when its observation is newer than the current record for that part. Offline processing associates results in source presentation order and then samples them in project time. Scrubbing, reverse playback, and source switches follow the analysis cache and preroll rules.

The first release locks one primary subject per source that meets the effect requirements. Both initial selection and reselection use deterministic candidate ranking. Scenes and projects persist selection rules and semantic anchors, and person track IDs are assigned at runtime. Keep the selected track when a face or hand is temporarily missing. Hide a sticker once its sample expires, while a virtual character can continue using valid body poses. Reselect according to the rules once the whole track expires. Glasses read both eye positions from `TrackedPersonSample::face`. Expressions and skeletal drive data for virtual characters use the same source, person track, and actual observation frame.

## Sticker, effect, and transition libraries

**For assets authored primarily in After Effects, the first animation adapter uses open-source libpag and artists deliver assets through PAGExporter.** Core integrates libpag through an asset adapter. PNGs and transparent image sequences are basic assets. Add Lottie and ThorVG support as needed for existing assets. bgfx handles final compositing, and AVKit manages the timeline, tracking anchors, and sampling of user media. [PAG export options](https://pag.io/docs/en/pag-export.html)

| Content | Initial implementation and existing libraries |
| --- | --- |
| Static stickers, icons, and lightweight image sequences | Decode PNGs to RGBA and store static assets in a texture atlas. Sample sequences by time and draw textured quads with bgfx and ECS local transforms. Support face anchors and occlusion. |
| Vector stickers, complex After Effects animations, and graphics with replaceable text and images | PAGExporter exports vector or mixed PAG. Keep replaceable layers in the vector portion and prerender complex After Effects subcompositions as needed. Core renders a transparent image at the target time with libpag. Use sample assets to validate effect compatibility, replacement layers, and rendering cost. [PAG export options](https://pag.io/docs/en/pag-export.html) · [libpag C++ API](https://github.com/Tencent/libpag/blob/main/include/pag/pag.h) |
| Effects that depend on the input image | Implement skin smoothing, makeup, LUTs, blur, warping, and segmentation-based backgrounds in AVKit render plans. EntityX systems supply landmarks, masks, and animation parameters. bgfx passes sample camera or clip frames. Resource packs can reuse textures, LUTs, masks, and existing shaders. |
| Two-input video transitions | Start with fades, wipes, and mask reveals in bgfx, then port suitable `gl-transitions` source. AVKit supplies frames from both clips, normalized progress, and parameters through the existing shader build. [GL Transition specification](https://github.com/gl-transitions/gl-transitions#gl-transition-specification-v1) |
| Real-time particles | Use seeded ECS emitters and bgfx batched drawing for small particle systems. If the content team needs a dedicated particle editor, evaluate Effekseer's offscreen output, seeking, and platform texture integration with bgfx. [Effekseer rendering backends](https://github.com/effekseer/Effekseer#supported-platforms) |

### After Effects export conventions

- **Standalone overlays.** Use vector PAG for path animation. Complex particles, third-party plug-ins, and fixed lighting effects can export as mixed PAG or PNG sequences with transparency. Short sequences can be packed into atlases. When exporting a sequence from After Effects, choose `RGB + Alpha` and straight alpha. Record frame rate, original frame dimensions, crop position, and anchor in the pack manifest. ProRes 4444 can serve as a production delivery format with alpha. Lib-Media's macOS implementation preserves alpha through `Ayuv16`. For cross-platform packs, start with verifiable RGBA sequences. [Adobe rendering and alpha export](https://helpx.adobe.com/after-effects/desktop/render-and-export/basics-of-rendering-and-exporting/basics-rendering-and-exporting.html) · [Lib-Media ProRes 4444](../../Lib-Media/docs/media-codec.md#prores-422-and-alac)
- **Replaceable content.** Keep text, images, and video placeholders editable. The same PAG resource can include complex prerendered After Effects subcompositions. Replacement values and resources go into the project snapshot together. AVKit's text layout evaluates general titles and subtitles. Validate PAG with the actual fonts, masks, blend modes, and animation sampling used by the asset. [PAG mixed export](https://pag.io/docs/en/pag-export.html) · [PAG replaceable layers](https://github.com/Tencent/libpag/blob/main/include/pag/pag.h)
- **Animations applied to user footage.** After Effects can export animated masks, displacement maps, and transparent decoration layers. For an ink reveal, AVKit samples user clips A and B in sync over the transition overlap, blends them with the current mask frame, and adds the ink decoration. AVKit passes evaluate warping and blending that need the current image's pixels. [Template masks](#template-masks)

### Using user videos in PAG

| Integration | Input, sampling, and ownership |
| --- | --- |
| Enterprise `PAGMovie` | Create a `PAGMovie` from a local video path, set the source range, speed, and volume, and replace the placeholder with a `PAGImage`. The official examples cover Android and iOS. Check capabilities for the target platforms and selected version before using this interface. [Official PAG video replacement documentation](https://pag.io/docs/zh-CN/replace-video.html) |
| Per-frame replacement in the community edition | AVKit gets user video frames through Lib-Media or a software codec provider and composes each clip to a GPU texture. The PAG adapter imports the two textures with `PAGImage::FromTexture` and replaces the authored slots with `PAGFile::replaceImage`. Both source frames are sampled from the same project time. The adapter sets `PAGPlayer` progress and flushes into a GPU target. bgfx consumes the completed texture for final compositing. [libpag C++ API](https://github.com/Tencent/libpag/blob/v4.5.102/include/pag/pag.h) · [PAG team's per-frame replacement guidance](https://github.com/Tencent/libpag/discussions/1233) |

The manifest maps asset slot IDs to layers in the pinned PAG version. Bind layers separately when they need different videos or source times. `getVideoRanges` can help check required asset intervals, and the project retains the final clips and time mappings. In the overlap, the transition instance feeds the outgoing and incoming clips to two placeholders. PAG renders the layout, masks, and animations retained from After Effects. Full-sequence graphics can use several placeholders the same way, with the existing one-click assembly planner arranging clips, music, and output.

General transitions use bgfx two-input compositing. Transitions that mask or displace both video inputs inside PAG use a GPU adapter for the target platform. Validate two-stream decoding, texture sharing, synchronization, and output cost on each supported target. Project planning and hardware and software codecs use the existing AVKit paths.

A PAG transition uses `PagTransitionSource` to select a resource for two clips on the same track. The timeline supplies separate ordered subtrees for the outgoing and incoming clips and a `PagTransitionSample` with the local overlap time. The renderer composes both sides and binds the completed images to the authored slots on every sample. The private PAG adapter flushes at the requested progress and returns a premultiplied image for bgfx to sample in the transition's layer order. [Render API](../include/huxerui/avkit/render.h) · [PAG resource descriptor](../include/huxerui/avkit/resource_catalog.h)

The macOS libpag v4.5.102 framework uses OpenGL for external textures. AVKit issues two Metal blits to IOSurface-backed inputs, waits for completion, and imports the OpenGL views with `PAGImage::FromTexture`. PAG renders into the OpenGL view of an output IOSurface. After OpenGL completes, bgfx samples the Metal view of that same output. The bridge transfers no frame pixels through CPU memory. Leased outputs retain their storage and remain immutable. The adapter reports an error when GPU sharing is unavailable. Hardware and software codec selection stays independent of the PAG handoff. [libpag surface factory](https://github.com/Tencent/libpag/blob/v4.5.102/src/rendering/PAGSurfaceFactory.cpp) · [macOS GPU adapter](../platform/macos/src/pag_transition.mm)

### Render plan integration

Still images and PAG samples enter the same frame plan as other drawable sources. The PAG adapter resolves the resource and its named replacements, samples its local media time, and returns a retained `Texture` for bgfx composition.

The macOS and Android workbench renders PAG overlays with `PAGPlayer` into leased GPU textures. macOS shares an IOSurface between PAG's OpenGL view and the Metal compositor. Android binds the compositor's GLES texture in a shared EGL context and submits a GPU fence before sampling it from bgfx. Static point text keeps its texture across unchanged draws, and layer opacity is evaluated in the compositor. A failed graphics handoff falls back to the retained CPU frame path. Clip-local PAG animation keeps its project-time sample clock across video seeks and reverse playback.

A sticker's authored content is a `StillImageSource` or `PagOverlay`. PAG content binds a versioned `ResourceRef`, named replacements, and an `AnimationPlayback` mode. A `TimelineFrame` carries a `PagLayerSample` with the visible instance ID, replacement values, and elapsed time on its owning clip's project clock. Reversing the video source leaves that clock unchanged. The compositor resolves the sample to a leased color surface and uses the existing EntityX layer and source-over draw. [Project API](../include/huxerui/avkit/project.h) · [Timeline API](../include/huxerui/avkit/timeline.h)

`PagReplacements` stores named text overrides and versioned image references per instance. A `PagTextReplacement` may carry content, a packaged font, a font size multiplier, and foreground and background RGBA. Omitted fields preserve the asset defaults, and an explicit empty text value clears the slot. Templates can supply initial values through `TemplateClip::stickers` or `OverlayInstance::content`. `ProjectEdit::SetStickerReplacements` replaces the instance's values in an undoable edit. The project checks slot uniqueness and resource versions. The registered `PagAsset` maps the names to editable indices. [Project API](../include/huxerui/avkit/project.h)

`PagAsset` associates logical names with the editable text and image indices of a specific PAG revision. The catalog holds the metadata and readable PAG payload under the same key. The adapter uses local paths from `FindLocalFile` for compositions, fonts, and image replacements, and opens a stream for bundled resources. The timeline renderer prepares a fresh PAG instance when its resource or replacement values change. It owns font registration, animation sampling, and transition execution. [PAG resource descriptor](../include/huxerui/avkit/resource_catalog.h) · [libpag C++ API](https://github.com/Tencent/libpag/blob/v4.5.102/include/pag/pag.h)

PAG evaluation resolves replacement slots by their authored names. The retained sample carries the editable indices to the renderer. The renderer loads the font and image resources, applies replacements when the instance changes, and samples native animation timing with hold or loop behavior. Compatible consecutive overlays share an output-sized PAG composition. Rendering uses the current eight-bit compositor's sRGB code values.

Before compositing, normalize transparent layers to premultiplied linear-light values using their known channel order, transfer function, and alpha semantics. For premultiplied output with a nonlinear transfer function, recover straight RGB first, then apply the transfer conversion and premultiply in linear light. Set the RGB of fully transparent pixels to zero. Background-dependent blend modes receive the background frame as an additional pass input. Selected `gl-transitions` shaders must match the library's UV origin and straight RGBA contract. Reuse the existing cross-backend build and check both images, semitransparent edges, and sample positions. Project time and the overlap determine transition progress, and each clip samples its frame in its own source time.

Sample sticker sequences and vector animations at the given time using the resource's loop or cutoff rule. Live preview can skip frames under the session quality policy. Offline export waits for the target frame. For acceptance, compare sequential playback, random seeks, transparent edges, and sampled export frames for the same asset. Measure rasterization, upload, and GPU budgets for scenes with multiple stickers on target devices.

## C++ rendering contract

The public rendering contract is defined in `avkit/render.h`. `ImagePixels` retains CPU rows with their stride. `Texture` retains a sampleable GPU resource through completion. Both carry physical extent, format, row origin, and alpha information. Video preparation returns the drawable input and its source-to-texture UV mapping. Analysis reports that mapping and its valid region in `AnalysisImageCompletion`. [Render API](../include/huxerui/avkit/render.h)

ECS builds one immutable `RenderPlan` per frame. A `RenderNode` exposes the kind of operation, and each final derived type owns the fields and children that operation consumes. The root `CompositeNode` draws its children back to front at `RenderPlan::target`, which specifies the physical dimensions and format. `TimelineFrame` carries the PTS and the plan. The plan retains CPU pixels, GPU textures, and the exact resource catalog for PAG. `TimelineRenderer::Draw` lowers the tree to GPU passes, uploads CPU inputs on its render sequence, and batches eligible PAG layers before submission. [Render API](../include/huxerui/avkit/render.h) · [Timeline renderer](../include/huxerui/avkit/timeline.h)

Once `TrySubmit` issues a ticket, the backend retains the plan, inputs, and resources. Success with an empty ticket signals backpressure. The caller retains the request for retry. `Pump` advances submission and completion and reclaims resources. Success with an empty `TryTake` result means work is pending. A terminal result is delivered once. `Discard` drops delivery while GPU work holds its leases through completion. Rebuilding the device retires old tickets. Destroy resources on their owning execution sequence. Production sessions associate the request's media time with its ticket, and the backend executes the planned pixel operations.

### Data textures

Mask and depth operations retain R8Unorm or R32Float inputs as CPU pixels or GPU textures. Pass preparation uses the same upload path for color, coverage, and depth. File and resource caches retain decoded pixels by identity and revision, so an unchanged `BufferReference` can reuse its uploaded texture while cached. Submitted passes use GPU texture handles. Numeric sampling preserves the mask or depth values through upload and sampling. [Render API](../include/huxerui/avkit/render.h)

`AnalysisImageCompletion` pairs a CPU or GPU result with the exact request and source frame. `source_to_texture_uv` maps orientation-corrected, pre-mirror source coordinates to logical texture UVs. Coverage and depth sampling use the same clip crop, mirror, and picture-in-picture mapping as the color input, along with the analysis result's `valid_uv`. Samples outside that region are unavailable. Row origin is handled by the renderer. [Analysis API](../include/huxerui/avkit/analysis.h)

The first release stores foreground mask coverage in `R8Unorm`. For classification output, the adapter extracts foreground coverage. Matting output uses the matte. When detection finds no foreground, return zero coverage. The depth occlusion milestone uses `R32Float` to store optical-axis `Z` in meters, registered to the video camera. Zero, negative, and nonfinite values denote invalid samples. The sensor adapter handles synchronization and calibration. Virtual object projection and depth comparison use the same camera coordinates. Depth uses nearest-neighbor sampling. The planned 3D rasterizer uses `Depth32Float` for depth attachments.

Live effects wait for the corresponding frame result and bridge preparation within their budget. On timeout, apply the effect's declared fallback, such as restoring the original image for background replacement or hiding a virtual layer whose occlusion is unknown. Preview and recording share that choice with the composited frame. Offline processing waits for required frame results and GPU dependencies. A skipped or failed required result produces an analysis error.

The timeline background replacement milestone enables an optional solid background on an individual clip. Its crop is normalized in the oriented, pre-mirror source, and the selected rectangle fills the clip destination. The render plan maps the visible rectangle into the fitted source image and the local mask independently. bgfx interpolates numeric R8 coverage between texels, clamps the samples within the valid mask region, and uses zero outside it. The two surfaces may have different stored row origins. CPU providers return retained `ImagePixels`, which the renderer uploads while preparing the GPU passes. [Render API](../include/huxerui/avkit/render.h)

The video source prepares a shared hardware texture or fitted CPU pixels and records the source-to-texture UV mapping. The transform maps normalized, oriented source coordinates before clip mirroring into the prepared input. Use the identity for an image containing the entire oriented source and describe the visible placement for an image containing padding or a crop. [Timeline API](../include/huxerui/avkit/timeline.h)

The portrait mask capability currently accepts stateless single-frame providers. Each clip owns its analysis session. For an inferred mask, the coverage system passes `PortraitMask::model_id` to the provider when opening the session. An empty selection uses the provider default. Installing a new project revision retires the previous selection and its results. An explicit skipped completion keeps the cropped and mirrored source for this optional effect in preview and export. A valid all-zero texture displays the replacement color. The timeline submits every visible clip's mask request before collecting results. Clocked preview starts a 12 ms deadline at each submission and restores the original for the affected clip on timeout. A completion received within its deadline stays available until collection. Export waits for each current result and reports provider errors. A completion must match the job ID, clip, generation, and source PTS.

`StickerDepth` gives a flat sticker a positive, constant camera-space Z. `AnalysisCapability::DepthMap` and `AnalysisSession::SubmitDepth` return an `AnalysisImageCompletion` with the same identity and cancellation rules as portrait masks. The timeline uses one stateless depth session per affected clip and shares its depth map across that clip's stickers. A matching skipped result hides the depth-bound stickers in preview and export. Preview also hides them on timeout, and export waits for a matching result. A missing capability or provider error is reported to the caller. [Analysis API](../include/huxerui/avkit/analysis.h) · [Project API](../include/huxerui/avkit/project.h)

EntityX includes `render::DepthOcclusion` on each visible depth-bound sticker in the compiled plan. Its target-to-depth UV map follows the owning video's crop, mirror, and canvas placement. A sticker pixel is visible when its depth is at or in front of a valid scene sample inside the clip and depth regions. Invalid samples suppress that sticker pixel. Preview and export use the same image and depth slots. bgfx validates the data slot format and rejects `R32Float` inputs on devices without sampleable 2D float textures. [Render API](../include/huxerui/avkit/render.h)

### Shader programs and resource packs

Effect resources can declare shader names, parameter defaults, keyframe support, textures, masks, and LUTs. Project snapshots pin resource revisions or content digests. Asset packs provide the inputs needed by their effects, while the renderer owns compilation and caching of GPU programs.

bgfx uses `shaderc` pinned to the runtime version to build artifacts offline for each target. Cross builds compile the tool for the host from the same pinned sources. CI checks required shader variants, logical bindings, and platform artifacts. Shared source holds reusable YUV, color grading, warping, and filtering logic. Keep bgfx `.sc` entry points, `varying.def.sc`, and binding maps in the backend directory. A new backend can reuse plans, parameter structures, and algorithm functions and supply its own entry points and compiled artifacts. [bgfx shaderc documentation](https://bkaradzic.github.io/bgfx/tools.html#shader-compiler-shaderc)

### Render passes and framebuffers

The plan orders offscreen passes before composition to the final target. Each pass draws over transparent black at the target size. Transitions use distinct passes for the outgoing and incoming clip layers. A PAG transition samples those results and publishes another image for the final draws. Typed pass references stay independent of the number and order of external inputs.

The bgfx backend validates dimensions, sampling formats, and referenced slots before recording draws. Framebuffers and attachments are owned by the backend, and GPU completion and outstanding leases govern their reuse. Preview publishes the packed color output through HuxerUI. The encoder adapter converts that output to its negotiated Lib-Media input format while preserving the requested PTS and duration.

```text
Video A Y/UV → color normalization → horizontal smoothing → vertical smoothing → LUT ┐
Video B frame → color normalization ─────────────────────────────────────────────────┤
Mask and depth data textures ────────────────────────────────────────────────────────┤
Sticker atlases and subtitle glyphs ─────────────────────────────────────────────────┤
                                                                              Compositing pass
                                                                                     ↓
                                                                              RGB(A) output
```

The planned high-precision working space uses linear BT.709 RGB with premultiplied alpha. Negotiate the working format by configuration and capabilities. Select `RGBA16F` for higher precision and use `RGBA8` within its supported precision range. Interpret matrix, transfer function, range, and chroma location from source metadata, then apply the visible crop and orientation. Explicit asset interpretation settings fill missing color metadata. The output adapter sets transfer, range, and alpha semantics for the destination. Enable P010 and HDR working spaces and outputs only when input, format capabilities, and output all pass negotiation.

### External video frames

The file decode path accepts Lib-Media's `media::VideoFrame` and uses its `VideoFormat` geometry and color, `VideoPlane` row and pixel strides, and `BufferReference` storage leases. Platform-decoded frames use the public texture bridge. During the callback, the camera adapter imports the published image from `camera::PreviewOutput` as a `Texture` and attaches crop, orientation, and mirroring through `CameraFrameGeometry`. The decoder adapter returns a fitted image with a mapping from the oriented source coordinates to its UVs. Asset interpretation rules fill missing metadata such as chroma location.

`Texture` copies share private backing for the sampled image. The bridge acquires a readable producer snapshot, and the backend completes GPU writes before returning an output image. Output slots remain leased while any image copy refers to them. The backend owns device resources and retires uploads on its render sequence once their texture leases are released. Publish a completed image to an SDK `ExternalTexture` for preview, or construct a `media::VideoFrame` with the negotiated encoder layout and PTS. Serialize producer publication and encoder snapshot acquisition to associate them with the target frame.

`ImportVideoTexture` receives hardware-decoded frames through Lib-Media's `MakeExternalTexture` and retains a readable GPU texture through the bound render device. Failed sharing reports `Unsupported` for the per-clip retry. Software-decoded SDR I420 or NV12 pictures are fitted into CPU pixels, which pass preparation uploads explicitly. Both paths supply their source-to-texture UV mapping.

The current eight-bit SDR compositor uses sRGB code values in its color textures. The CPU import expands the signaled YUV range and applies the signaled matrix before converting display-referred BT.709 RGB to sRGB. It uses the normalized BT.1886 display curve with gamma 2.4, and the CPU encoder transfer uses its inverse. This matches the Apple Metal bridge's BT.709 export and keeps the MP4's BT.709 metadata. CPU input with unspecified transfer retains the existing code-value behavior. [ITU-T H.274 colour description](https://www.itu.int/epublications/publication/itu-t-h-274-v3-2023-09-versatile-supplemental-enhancement-information-messages-for-coded-video-bitstreams)

File preview prepares each video picture before admitting it to playback. File export retries an input when its compositor reports `PlatformVideoImportError`. `MovieReader::RetryVideoFrame` replaces only the video cursor with the configured software provider and decodes the same source presentation timestamp and duration. Playback retains prepared surfaces until their timestamps are presented or dropped. Later seeks keep the software video provider. Timeline preview and export use the same input preparation path to retry the affected clip. Other clips, audio, and the open encoder keep their existing sessions.

| Platform | Initial integration and handoff |
| --- | --- |
| Windows | D3D11 builds copy the SDK's immutable BGRA8 snapshot on a compatible adapter and wait for the copy. A BGRA8 output can publish to `D3D11Texture` and use Lib-Media's NV12 conversion. Windows builds hosted on other systems use Vulkan and the CPU transfer path. |
| macOS and iOS | Each platform's Metal bridge retains its SDK `MetalTexture` snapshots and wraps compatible CoreVideo BGRA storage. Core Image converts decoded SDR video to BGRA and renders the composite to pooled BT.709 NV12 for encoding. |
| Android | The background GLES compositor imports a retained `GlFrame` through EGLImage. It publishes an RGBA8 snapshot to HuxerUI and can provide opaque encoder frames to Lib-Media for delivery to the MediaCodec input Surface. A driver that rejects publication of a reused render target can receive a copy into a separate GL texture. A decoder texture that cannot be published reports a transfer failure. |
| Linux | With a current compatible GDK/EGL context, the OpenGL compositor samples compatible `linux::GlTexture` leases and publishes RGBA8 snapshots to HuxerUI. The independent Vulkan compositor uses pixel transfer. Lib-Media's Linux video path uses CPU planes. |
| Web | On the browser main thread, WebGL 2 uploads open `VideoFrame` objects and publishes RGBA8 composites through a canvas and `VideoFrameTexture`. Lib-Media accepts the resulting opaque frame for WebCodecs. |

`MovieReader` restarts a video cursor with the software provider after a recoverable decoder error. Capture imports the published SDK image on the compositor sequence. Preview updates a reusable HuxerUI `ExternalTexture`. A paused update can prepare publication on the render sequence and commit the image together with its artwork geometry on the application sequence. `MakeBgfxEncoderFrame` produces one `media::VideoFrame` with the supplied PTS and duration using the open encoder's negotiated format and input storage. A file or timeline compositor can report `PlatformEncoderInputError` so the writer can reopen the media and retry with a CPU encoder configuration. [Video texture API](../include/huxerui/avkit/timeline.h)

The existing Lib-Media `MakeExternalTexture` and `MakeVideoFrame` APIs can establish the first path. The public Windows and Android bridges create GPU snapshots and may convert color. Apple's `MetalTexture::Publish` also creates a GPU snapshot. Use the actual SDK contracts to determine capabilities and cost. If measurements call for a shorter handoff, negotiate a public platform frame API with retained leases in Lib-Media. The media adapter layer consumes that API.

## External inference providers

`CreateEngine` accepts in-process `AnalysisProvider` instances. Effect modules declare required capabilities and output versions, and templates pin the model's `ResourceRef`. Core resolves the model to a retained `RawAsset` and calls `Probe` for a candidate input format, storage mode, ROI, and output types to obtain its `AnalysisWindow`. During resource preparation, Core opens the selected session and checks the actual GPU import or CPU handoff path. A missing required capability returns a preparation diagnostic. Templates with declared alternatives can assemble according to their fallback rules.

```cpp
#include <typeindex>
#include <utility>

namespace huxerui::avkit::analysis {

struct AnalysisPort {
    std::string capability;
    std::uint32_t schema_version;
    std::type_index type;
    bool operator==(const AnalysisPort&) const = default;
};
class AnalysisValue {
public:
    template<class T>
    static AnalysisValue Make(std::string capability, std::uint32_t schema_version, T value) {
        return AnalysisValue({std::move(capability), schema_version, typeid(T)},
                             std::make_shared<const T>(std::move(value)));
    }

    const AnalysisPort& Port() const noexcept { return port_; }

    template<class T> std::shared_ptr<const T> Get() const {
        if (port_.type != typeid(T)) return {};
        return std::static_pointer_cast<const T>(value_);
    }

private:
    AnalysisValue(AnalysisPort port, std::shared_ptr<const void> value)
        : port_(std::move(port)), value_(std::move(value)) {}

    AnalysisPort port_;
    std::shared_ptr<const void> value_;
};

struct AnalysisConfig {
    ResourceRef model;
    RawAsset model_data;
    std::vector<AnalysisPort> outputs;
    std::variant<media::VideoFormat, render::ImageInfo> input_format;
    media::VideoStorage storage;
    bool uses_roi{};
};
struct AnalysisFrame {
    FrameKey key;
    render::RenderInput image;
    std::optional<Rect> roi;
};
struct AnalysisJob {
    std::uint64_t job_id;
    FrameKey target;
    std::span<const AnalysisFrame> frames;
};
struct LandmarkCompletion {
    std::uint64_t job_id;
    AnalysisResult<std::vector<AnalysisValue>> outputs;
};

class AnalysisSession {
public:
    virtual ~AnalysisSession() = default;
    virtual AvKitResult<media::SubmitStatus> TrySubmit(const AnalysisJob& job) = 0;
    virtual std::optional<LandmarkCompletion> TryTake() = 0;
    virtual void Cancel() = 0;
};
class AnalysisProvider {
public:
    virtual ~AnalysisProvider() = default;
    virtual AvKitResult<AnalysisWindow> Probe(const AnalysisConfig& config) const = 0;
    virtual AvKitResult<std::unique_ptr<AnalysisSession>> Open(
        const AnalysisConfig& config, std::function<void()> wakeup, std::stop_token stop = {}) = 0;
};

struct ExpressionWeight { std::string name; float weight; };
struct FaceExpression {
    std::uint64_t subject_id;
    std::vector<ExpressionWeight> weights;
};
enum class Pose3DOrigin { Camera, HipCenter };
enum class Pose3DUnit { Metres, Relative };
struct JointObservation {
    std::string name;
    std::array<float, 3> position;
    std::optional<float> confidence;
};
struct Pose3D {
    std::uint64_t subject_id;
    Pose3DOrigin origin;
    Pose3DUnit unit;
    std::vector<JointObservation> joints;
};
struct InstanceMask {
    std::uint64_t subject_id;
    render::PixelInput coverage;
    Affine2D source_to_texture_uv;
    Rect valid_uv{0, 0, 1, 1};
};

}
```

Core organizes the input window in source presentation order and attaches each frame's ROI in source-image coordinates. The job ID distinguishes multiple ROIs on one frame within a session, and `target` identifies the image being analyzed. Isolate stateful sessions by model configuration, source generation, and continuous ROI sequence. Upon accepting a submission, the session retains the inputs and leases it needs. On `WouldBlock`, the caller retains the original request. In an active session, every accepted job reports completion, a skip, or failure once. Use `wakeup` to schedule Core when output is ready or queue capacity returns.

When seeking, switching cameras, or stopping a job, Core retires the corresponding generation and calls `Cancel`. The provider stops accepting inputs and releases in-flight resources after the underlying work completes. Submitted GPU work holds leases through the last read. Validate late results by job and target frame before retiring them. Session wakeups schedule Core, and each world accepts results on its own execution sequence. Recording route negotiation checks the active session's temporal window and handoff requirements. Strict same-frame effects that wait for future frames or asynchronous GPU data use AVKit-managed recording.

Validate result ports by capability name, schema version, and C++ value type. A model can deliver several result types in one job. Packs and projects store capability names, versions, and model references, while `AnalysisValue` checks types in process. Core dispatches existing `AnalysisResult<T>` values by target frame. Landmark, expression, and pose ports can each provide a list of subjects. Whole-image instance segmentation can provide a list of `InstanceMask` results. Local inference can associate a person through its job. Core sets `MaskCoverage::person` after association.

Interpret `FaceExpression` weights using the semantic names and value ranges in the port schema. Express 3D points in camera axes aligned with the orientation-corrected image. X points right, Y points down, and Z points forward along the optical axis. `Pose3D` specifies origin and units. The character system retrieves these data by the same person track and actual observation frame, then retargets joints to local transforms in the character skeleton. For real-world occlusion, first calibrate 3D observations to the same camera space and metric scale as the depth map. [MediaPipe Face Landmarker](https://developers.google.com/edge/mediapipe/solutions/vision/face_landmarker) · [MediaPipe Pose Landmarker](https://developers.google.com/edge/mediapipe/solutions/vision/pose_landmarker)

## Core frame submission and timeline sampling

A standalone host can submit one video source and one microphone PCM stream to a live session. Core decodes background music from the pack. `LiveVideo` carries a media frame or a retained RGB surface. A media frame uses its own PTS, and a surface supplies `surface_pts`. Set `common_time` only after the adapter verifies a shared clock domain and origin. Switching sources advances the generation and retires video, audio, and analysis results together.

```cpp
namespace huxerui::avkit {

struct CaptureStamp {
    std::uint64_t source_generation;
    std::optional<media::MediaTimestamp> common_time;
};
struct LiveVideo {
    render::RenderInput image;
    std::optional<media::MediaTimestamp> surface_pts;
    CaptureStamp capture;
};
struct LiveAudio { media::AudioFrame frame; CaptureStamp capture; };
struct PreviewFrame { render::Texture surface; media::MediaTimestamp pts; };

class RealtimeRecording : public MovieJob {
public:
    virtual AvKitResult<void> Pause(std::optional<media::MediaTimestamp> at) = 0;
    virtual AvKitResult<void> Resume(std::optional<media::MediaTimestamp> at) = 0;
    virtual AvKitResult<void> Stop(std::optional<media::MediaTimestamp> cutoff,
                                    std::optional<Error> interruption = {}) = 0;
    virtual AvKitResult<void> FinishAudio(std::optional<Error> interruption = {}) = 0;
};
class RealtimeSession {
public:
    AvKitResult<std::shared_ptr<const SceneSnapshot>> Edit(
        const std::function<void(CaptureEdit&)>& changes, std::stop_token stop = {});
    std::shared_ptr<const SceneSnapshot> SnapshotScene() const;
    AvKitResult<media::SubmitStatus> TrySubmitVideo(const LiveVideo& input);
    AvKitResult<media::SubmitStatus> TrySubmitAudio(const LiveAudio& input);
    AvKitResult<std::optional<PreviewFrame>> TryTakePreview();
    AvKitResult<ImageAsset> ProcessStill(std::shared_ptr<const SceneSnapshot> scene, ImageAsset image,
                                        std::optional<PixelSize> processed_size, int jpeg_quality);
    AvKitResult<std::shared_ptr<RealtimeRecording>> StartRecording(
        MovieDestination target, RecordingRequest request, std::function<void()> wakeup = {});
};
class TimelineSession {
public:
    AvKitResult<void> ApplyProject(std::shared_ptr<const ProjectSnapshot> project);
    void Reset();
    AvKitResult<std::optional<PreviewFrame>> TryRenderVideo(
        media::MediaTimestamp time, std::optional<PixelSize> target_size = {});
    AvKitResult<std::optional<media::AudioFrame>> TryRenderAudio(TimeRange range, const media::AudioFormat& format);
};

}
```

The bounded queue retains an input's lease once it accepts the input. On `WouldBlock`, the caller keeps the original frame and retries. `TimelineSession` has one pending request per stream. Retry with the same parameters when the result is empty. `Reset` and a change of project revision retire old requests. Within a generation, sample audio in contiguous sample intervals. After a seek, restore the history required by DSP.

Recording with a microphone requires shared capture times for control points. Video with packaged music can generate continuous PCM against video time. `Stop` closes video input for recording and fixes the boundary. The active audio input can continue submitting PCM before that cutoff. `FinishAudio` reports that the reader has been drained. Preview can still accept the next frame. When there is no external microphone, the recorder finalizes the music branch over the active interval.

## Codec adapters and automatic fallback

### GPU composition and software codecs

The GPU performs video composition, image effects, scaling, and blending. Prefer importing hardware-decoded frames as platform textures. Upload the CPU planes from software decoding once upon entry to composition. Subsequent passes run on the GPU. For software encoding, the output adapter converts the final composite to a supported layout and reads it back asynchronously through a bounded queue. Audio DSP runs independently in PCM blocks. On GPU device loss, the render service rebuilds resources and renegotiates frame paths. This recovery is independent of codec switching.

Lib-Media platform adapters already include some software paths, such as GStreamer `x264enc` and `x265enc` on Linux, a shared Vorbis software decoder, and system software encoders in specific Android MediaCodec configurations. `hardware_acceleration` describes capabilities. Use the negotiated session to identify the actual implementation. `VideoStorage::Cpu` describes where the frame resides. Hardware decoding can also return CPU frames.

### C++ codec interfaces

| Interface | Contract |
| --- | --- |
| `CodecProvider` | Pass a Lib-Media configuration to the typed `Probe` and `Open` overloads. Entry preparation can probe and open independent decoders concurrently. Open confirms video storage and layout through `Negotiated()` and reports `Hardware`, `Software`, or `Unknown`. The first format event provides the session's stream description. |
| `VideoDecoderSession` | Submit `media::EncodedPacket` in decode order. Read `media::VideoDecodeOutput` for the frame format, pictures in presentation order, and EOS. Later format events can describe a change. |
| `VideoEncoderSession` | Submit `media::VideoFrame` and read `media::VideoEncodeOutput`. Configure the mux track from the announced bitstream description before writing access units. |
| `AudioDecoderSession` and `AudioEncoderSession` | Supply the exact PCM format in the configuration. Reuse `media::AudioFrame`, `AudioDecodeOutput`, and `AudioEncodeOutput`. The encode summary carries input sample count, priming, and tail padding per channel. |

When submission returns `WouldBlock`, the caller retains the same packet or frame and advances reading. `FinishInput` closes input and drains the reorder queue. `Close` retires the session. Previously delivered frames keep independent leases. `LibMediaCodecProvider` adapts platform sessions, and `FFmpegCodecProvider` implements software sessions with the in-process `libavcodec` send/receive API. Muxing, seek indexes, and the timeline stay in the media scheduling layer. The FFmpeg adapter converts extradata, packet representations, and frame layouts as required. [FFmpeg codec session contract](https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html)

`MovieReader` and `ExportProject` take the same `CodecProviders`. The file pipeline probes Lib-Media first in `Auto`. On macOS it requests platform NV12 for video. The FFmpeg candidate requests CPU I420 with the same codec settings and timing. The compositor receives the selected video encoder's input contract. Lib-Media exposes VideoToolbox capability and session results without identifying the hardware implementation used for an individual session. Its adapter reports `Unknown` for that implementation field. [Codec provider header](../include/huxerui/avkit/codec.h) · [File media header](../include/huxerui/avkit/file_movie.h)

The default `MovieReader` constructor opens both tracks. Its single-track constructor takes a `media::TrackKind` and an initial presentation time. A seek keeps that selection. A recoverable decoder failure reopens the affected track with the software provider at its resume position.

Distributions that promise software fallback ship verified implementations and publish separate coverage for decoding and encoding. The first acceptance configuration is H.264 8-bit SDR software decoding and encoding with AAC-LC output. FFmpeg can use OpenH264 or libx264 for H.264 software encoding. Select an implementation that meets the target profile and distribution configuration. Upstream FFmpeg lists libx264 and libx265 integration as GPL build options. Validate WebCodecs and WASM software builds separately for the web. Software decoding and encoding coverage depends on the implementations enabled in the distribution and verified against the full configuration. [FFmpeg encoders and build options](https://ffmpeg.org/general.html#x264) · [OpenH264 configuration](https://ffmpeg.org/ffmpeg-codecs.html#libopenh264)

### Switching and recovery

The default `Auto` policy prefers hardware implementations that can hand off GPU frames directly. It can also probe platform paths whose actual implementation is unknown. If configuration is rejected, hardware resources are exhausted, or a recoverable device or session failure occurs, the scheduler retires the old session and selects a matching, verified software implementation. Use `SoftwareOnly` to select software explicitly or verify the same project. Handle backpressure, temporary input gaps, and normal intermittent output under the session protocol. Record the cause, actual provider, and any extra frame transfer path for each fallback. While a software session is active, probe platform paths again at the next seek, asset switch, or new job.

| Scenario | Switching contract |
| --- | --- |
| Creation or configuration failure | Try a software session with the same settings, then negotiate frame storage. If no matching implementation exists, return the specific cause and preserve the user's codec, profile, bit depth, resolution, and color requirements. |
| File decoding fails midstream | Freeze the source's delivery position, advance the decoder generation, and drop late results from the retired session. Replay configuration and compressed packets in decode order from an independently recoverable random-access point. Prefer a bounded packet cache and seek the demuxer again if needed. Deduplicate delivered frames by source PTS and presentation order, then catch up to the current playback target. Offline processing waits for the first frame still to be delivered. |
| Preview decoder recovery | Keep the last valid image. Audio can continue within its buffered range. Once video recovers, catch up using the audio clock. If the product waits for video, pause and resume at the same playback position. Recovery latency depends on the random-access interval, device performance, and cache. |
| Export encoding failure | Recreate the encoder and replay the current attempt's inputs while the muxer has committed none of that encoder session's dependent data. Once bitstream configuration or samples have been committed, recreate the temporary destination from the same project and resource snapshot and complete the job in software from the start. If direct streaming has already published bytes, end the attempt as an interrupted stream. |
| AVKit-managed live recording encoding failure | Source frames arrive in real time. In segmented mode, start a software-encoded segment with the next available raw frame. The first encoded frame must be independently decodable, and the gap is recorded. For a continuous single file, finalize according to muxer and session capabilities and report the interruption cause. Camera-managed recording uses Camera's failure result. |

Switching encoders can change parameter sets, B-frame reordering, timestamps, and encoding latency. Segment-based export recovery applies to segments that can be validated and muxed independently. Each has its own random-access point, bitstream description, and time offset. The first release restarts the temporary single-file export job to preserve readability and timing precision.

## bgfx backend execution

- **Process-wide rendering service.** The service manages the bgfx instance, texture pool, and program library. It assigns views and submission capacity by session budget. Preview has priority, and export receives a quota so it can keep progressing. Platform integration selects the API and render sequences for thread-affine calls. Headless jobs use the initialization supported by the selected bgfx graphics API. If the host already has a bgfx instance, agree on a single process-wide owner and view allocation. [bgfx threading model](https://bkaradzic.github.io/bgfx/internals.html) · [bgfx initialization contract](https://bkaradzic.github.io/bgfx/bgfx.html)
- **One view per pass.** Set the view and framebuffer in plan order. Use `Sequential` for alpha blending and bind sampled, draw, and compute resources by declared format and usage. The backend tracks asynchronous frame uploads, dependencies across views, and resource destruction. The texture pool owns attachments. Framebuffer destruction follows that attachment ownership. [bgfx view and framebuffer API](https://bkaradzic.github.io/bgfx/bgfx.html)
- **Submission and GPU completion.** `bgfx::frame()` advances submission. The external frame bridge verifies platform texture imports, output handoff, and GPU completion mechanisms for each backend. First verify the path where the adapter allocates a compatible texture to use as the backend's target. Chain submissions where synchronization objects can be passed between components. On other combinations, the bridge waits for a verifiable completion condition on its own sequence. [bgfx frame and external texture API](https://bkaradzic.github.io/bgfx/bgfx.html)

The shared bgfx compositor and texture lifetime code live in `src/render/bgfx/`. One shader source set is compiled for Metal, GLES and WebGL, Direct3D, Vulkan, and desktop OpenGL. Each platform selects its graphics context and provides its own texture bridge under `platform/`. `BgfxPlatformHandles` supplies optional window and display handles for context creation. Web uses the window field for the canvas selector. Platform texture leases remain valid through GPU use and keep output slots busy until the caller releases them. The scaffold sets deployment targets of macOS 12, iOS 15, and Android API 23. Verify the pinned dependencies and GPU capabilities against the deployment targets before distribution. [bgfx C++ API](../include/huxerui/avkit/timeline.h) · [Renderer build](../cmake/AVKitBgfx.cmake) · [bgfx platform support](https://bkaradzic.github.io/bgfx/overview.html)

The portable entry point accepts retained pixel rows and previous GPU output leases. It composes into reusable GPU textures, and the output surface declares the renderer's row origin. Backend-owned output uses a one-pixel staging readback to establish completion. The platform bridges wait for GPU completion with the platform renderer. Explicit full-frame readback supports the bitmap and CPU encoder paths. [Compositor API](../include/huxerui/avkit/timeline.h)

## First milestone and acceptance checks

**Start with a minimal texture-in, texture-out path on macOS.** Pin bgfx, bx, bimg, and shaderc versions compatible with the target system and set up minimal `AVKit::Core` and `AVKit::Bgfx` targets. Use the bridge to prepare two source textures and a sampleable offscreen target on the same Metal device. The inputs are a base image with known colors and a semitransparent sticker. Verify external texture imports through bgfx's `createTexture2D`, execute one composition pass, and retrieve a leased GPU `Texture`. Show the same result in `examples/preview`. [bgfx external texture parameter](https://bkaradzic.github.io/bgfx/bgfx.html#_CPPv4N4bgfx15createTexture2DE8uint16_t8uint16_tb8uint16_tN13TextureFormat4EnumE8uint64_tPK6Memory8uint64_t)

The milestone has three acceptance checks.

- Core submits the plan and retrieves the frame in a headless session. Check dimensions, orientation, premultiplied alpha, and known output pixels.
- Repeatedly submit inputs in A, B, A order. When B completes, verify the pixels of the first A, which the consumer still holds. Both A outputs must agree. The texture pool reclaims resources after lease return and GPU completion.
- Use an actual GPU completion signal to confirm the target is ready to read, then publish BGRA8 or RGBA8 through `macos::MetalTexture::Publish`. Keep the `ExternalTexture` logical size fixed across updates. Record the SDK's GPU snapshot copy and synchronization points. Use explicit test readback for pixel checks.

The macOS implementation runs bgfx submission inline with one Metal frame in flight. The empty follow-up submission in `Pump` waits on the previous Metal command buffer's completion semaphore. `TryTake` returns the surface after that wait. The pool waits for the returned surface lease before recycling an output. The workbench's offscreen backend has three slots so a pending preview frame can coexist with both sides of a PAG transition. HuxerUI publication makes a synchronous GPU snapshot. [Pinned bgfx Metal queue](https://github.com/bkaradzic/bgfx/blob/0762725e7dbf2863d68ed92bf26348a0b117c79f/src/renderer_mtl.cpp#L5929-L6000)

| Follow-up stage | Acceptance checks |
| --- | --- |
| [File-based production](#macos-file-media-milestone) | Import one video and one audio track. Use one composition plan for preview, seek, MP4 export, and reimport. Verify audio and video duration, PTS, backpressure, temporary output after encoding failure, and software fallback. |
| [Capture and voice effects](#camera-and-microphone-recording) | Run Camera-managed recording and still capture. After integrating retained frames and a correlatable clock, run AVKit-managed recording. Check the single reader, voice effect switching, pause and resume consistency, trimming at stop, and muxed output. |
| [ECS scene evaluation](#macos-ecs-sticker-milestone) | Register an application-side component and system, reparent a sticker, sample equivalent and backward media timestamps, and check the resulting draw in preview, recording, and file export. |
| [Timeline and edit templates](#macos-timeline-and-edit-template-milestone) | Edit two clips and two sticker instances. Check undo, redo, reopening, and asset relinking. Assemble the same inputs twice and compare preview, seek, and export. |
| [Animated transitions](#macos-animated-mask-transition-milestone) | Change a transition's start and duration and compare instance identity, curve progress, mask sampling, and export pixels. |
| [Thumbnails and waveforms](#macos-thumbnail-and-waveform-milestone) | Scroll, zoom, and relink assets during playback to verify background cancellation and isolation of decode cursors and caches. Use trimmed and reversed clips from one asset to compare actual video PTS, peak buckets, and project-time mappings. |
| Sticker tracking and analysis scheduling | Use a mock provider to verify capability failures, history and lookahead windows, cancellation, and late results, then connect a real model. Check glasses and left and right hand anchors while two people cross. Use two clips of the same asset, seeking, and reverse playback to verify source isolation and history. Confirm expressions and 3D pose belong to the same person. |
| Data textures and template masks | Use a partial-frame mask to verify pixel alignment with mirroring, cropping, and picture-in-picture. Check no foreground, skipped frames, and delayed GPU synchronization. Test occlusion of near and far virtual objects against known camera depth. Replace and undo a static mask, then reopen the project. Compare animated mask pixels at the same transition progress during sequential playback, seek, and export. |
| PAG and advanced effects | Use reference frames from After Effects to validate PAG alpha, per-frame two-input rendering, seeking, and replaceable layers. Measure texture handoffs on each target device. Then validate matting, virtual characters, JS execution budgets, and concurrent batch jobs. |

## macOS file media milestone

The workbench selects one SDR H.264, HEVC, VP8, VP9, AV1, or ProRes 422 track from the combinations demuxed by Lib-Media in MP4, MOV, Matroska, WebM, MPEG-TS, and AVI. Each selected track needs a stable sample description and a finite presentation range. An audio track is optional. When present, the workbench selects AAC-LC, Opus, Vorbis, MP3, ALAC, or PCM at 44.1 or 48 kHz in mono or stereo. Lib-Media preserves the media-to-presentation edit mapping, and AVKit applies packet padding to decoded PCM. If Matroska or WebM gives only a shared segment duration, the reader fills the interval after the last audio packet with silence through that duration.

AVKit sizes each video decoder's queue before probing or opening it. The output byte limit covers at least 34 coded frames, charged at four bytes per pixel for opaque platform output or at the plane sizes for 8-bit I420 and NV12. AVKit sizes the platform and software configurations separately and retains the configured slot counts. These limits bound queue reservations. Actual allocation follows the decoder's queued frames and platform buffer pool.

The FFmpeg adapter decodes to I420 and reduces supported 10-bit SDR and 4:2:2 layouts to the compositor input profile. HDR tone mapping is outside this profile. Export writes H.264 and AAC-LC MP4. The FFmpeg AVC encoder uses `libx264` with zero B pictures, and its AAC packets carry the encoder PTS including priming. Lib-Media writes the MP4 edit with known leading and trailing trim. Timeline export uses its configured frame grid.

Preview and export evaluate the same scene and execute the bgfx pipeline at their respective output sizes. On macOS the video bridge fits the decoded picture and imports a Metal texture. Preview publishes the completed composite through the HuxerUI texture mailbox. The audio device clock selects due pictures. A seek retires the playback epoch and discards queued PCM and video, seeks to an earlier random-access point, and trims PCM to the target sample. The first published frame can span the target. Export converts the composite to an IOSurface-backed NV12 buffer for VideoToolbox, or reads its completed planes as I420 for FFmpeg. The example waits for the GPU conversion on the rendering sequence.

AVKit configures the file module and both codec provider targets for every platform. The build pins the Lib-Media Git revision and accepts an existing `HuxerUI::Media` target or a local override through `AVKIT_LIB_MEDIA_SOURCE_DIR`. Point `PKG_CONFIG_PATH` at a target FFmpeg build with `libavcodec` 60 or later, `libavutil` 58 or later, and `libswresample`. For a minimal FFmpeg build, enable these decoders along with the AAC and `libx264` encoders.

```text
h264,hevc,vp8,vp9,prores,aac,opus,vorbis,mp3,alac,pcm_u8,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_s16be,pcm_s24be,pcm_s32be,pcm_f32be
```

AV1 requires a Lib-Media decoder for that configuration or FFmpeg with `libdav1d` or `libaom-av1` enabled. The `libx264` encoder requires the FFmpeg GPL build option. The build and OS compatibility of those dependencies are part of the application's distribution configuration. [FFmpeg release archives](https://ffmpeg.org/download.html)

## Camera and microphone recording

`AVKit::Capture` supplies `CameraRecording` on macOS and Android. The workbench uses the same recording page on both platforms. The application requests permissions, opens Camera and AudioCapture, chooses the sticker and voice settings, and displays the preview. Both take creation and MP4 finalization belong to the recorder. Still photos save the composed preview frame through `CameraRecording::CapturePhoto`.

`CameraRecording::Connect` binds a Camera `FrameProcessor` and an observer. The observer contributes the source generation and capture timestamp correlated to the microphone clock. While Camera retains the input, the processor hands the texture to the AVKit render sequence. The scene systems fit the camera source using its crop, rotation, and mirror geometry and evaluate the sticker at the current media time. The renderer executes the compiled plan. One completed composite feeds the retained HuxerUI preview texture and the negotiated encoder transfer.

Recording and the editor each open one `BgfxPreviewTexture` for their canvas size. The render sequence publishes completed output leases to the same HuxerUI `ExternalTexture` for playback, paused seeks, and gesture refreshes. `Finish()` preserves the last frame. Metal publishes a GPU snapshot. When the Android host supports hardware bitmaps, the GLES adapter blits to an Android hardware buffer for presentation in the same Canvas as editor controls. The Android adapter can also publish a GLES layer for standalone hosts. [Preview texture API](../include/huxerui/avkit/timeline.h)

For a paused seek or gesture, the render worker prepares the completed picture and forwards it with the matching project time and transform. The application keeps the latest request and commits the texture update and its selection geometry on the same UI turn. Replaced or canceled requests discard their prepared frames. Recording publishes directly from its render worker.

The recorder requires a common capture clock on both inputs. macOS uses CoreMedia host time, and Android uses boot time when Camera reports real-time sensor timestamps. The first eligible video frame after the microphone starts defines time zero. Active intervals share the PCM sample grid. Pause closes an interval while camera preview continues, and resume starts another interval at the previous movie boundary. Stop seals the video duration and drains the microphone through the same cutoff. Robot voice applies ring modulation to the retained PCM without changing its sample count.

`WriteLiveMovie` supplies codec negotiation, AAC trim, and staged MP4 output. Recording and workbench export use `FileExportSettings::Sdr` for limited-range BT.709 and the platform AAC policy. On Android it selects FFmpeg for AAC to preserve the exact sample cutoff when the platform encoder omits priming and padding. The video encoder uses the platform provider with software fallback during negotiation. A failure after a live input starts does not replay consumed frames. [Camera recording API](../include/huxerui/avkit/camera_recording.h) · [File media API](../include/huxerui/avkit/file_movie.h)

## macOS ECS sticker milestone

`AVKit::Core` pins EntityX 1.3.0 and uses Lib-Media's `MediaTimestamp` for scene evaluation. File media and FFmpeg have separate CMake targets. [EntityX dependency](../cmake/AVKitEntityX.cmake)

`RenderScene` owns one EntityX scene. Application code attaches its own component and registers a system. Sampling publishes the exact `VideoFrameContext` before calling registered systems and projects bound `LayerStyle` entities into an ordered render plan. Spatial links compose local transforms before each draw. `SetSpatialParent` checks entity lifetimes and cycles. [Render scene](../include/huxerui/avkit/scene.h)

## macOS timeline and edit template milestone

`AVKit::Core` owns the project, revision history, and template planner. `Project::Edit` validates a transaction before publishing its snapshot. Visual track order defines compositing order. Overlays have independent `InstanceId` values and can share the same `ResourceRef`. `Project::Save` writes the current revision to a local `.avkit` document in the v1 format. The document stores each project media key, its absolute local path, and its source track metadata. Clip and effect records store resource keys. A background record keeps its color and one mask source tagged `p`, `s`, or `a`. A static mask record includes its grayscale or alpha channel. Unavailable paths remain in the project so `ProjectEdit::Relink` can update the location in one transaction. Undo and redo history lives in memory. [Project API](../include/huxerui/avkit/project.h)

`PlanEditTemplate` accepts a typed recipe and already-imported assets, resolves explicit bindings before selection order, and prepares an editable snapshot for an empty timeline. `ApplyEditTemplate` checks the exact base snapshot and asset availability, then commits the assembly as one undoable edit. The built-in example recipe assembles two shots, swaps the main and picture-in-picture sources at the cut, and follows the main shot with its selected audio track. [Template API](../include/huxerui/avkit/edit_template.h)

The macOS and Android workbench links a newly imported video's original sound to its `ClipId`. Trimming, moving, or reversing the video updates that sound in the same transaction. The clip's audio panel edits gain, mute, and fades. Detaching the sound exposes it as a separately editable audio clip. A template video can request original sound as part of its assembly.

Canvas overlays have stable instance IDs, project-time ranges, and normalized canvas bounds. Text, SRT cues, PAG text, and stickers share the same timeline and revision history. The workbench creates rows as overlapping instances require them. Each active PAG entity contributes an owned `PagLayerSample` with its local time. The renderer groups eligible layers within the same pass before bgfx composition. The project stores one content source per overlay. Template resources resolve through pack-relative paths or the HuxerUI resource package. [Project API](../include/huxerui/avkit/project.h) [Timeline API](../include/huxerui/avkit/timeline.h)

Within each pass, adjacent source-over PAG layers share a composition and output texture at the current preview or export canvas size. A single point-text overlay also uses a canvas-sized PAG target so bgfx can composite its glyphs without resampling a smaller layer texture. Text and stickers retain their own positions, transforms, opacity, and local time in that composition. The renderer reuses prepared text layers and can reuse the entire group output when its layers are unchanged. bgfx blends the group at its original position in the draw order. Other layers, masked and depth-tested draws, and pass boundaries interrupt batching. If the shared GPU target cannot be created, the renderer uses the individual sampling path.

The workbench authors titles and imported subtitle cues as `PagOverlay` instances referencing a packaged master. Its registered `PagAsset` declares the editable slot and enables `fit_text_to_canvas`. Template packs can declare the same behavior with their own resource keys and slot names. The renderer applies the slot overrides and lays out the glyphs at the current output size. The resulting glyph bounds drive the editor selection rectangle. Authored PAG compositions apply the same overrides to their editable text documents and preserve the asset's layer placement and animation. Foreground alpha multiplies the editable layer opacity in authored compositions, including the text document's background. The canvas-fitted point text renderer applies background alpha to its separate panel. The sticker catalog samples packaged animations for its picker previews. Timeline sampling evaluates each animation at its instance-local project time.

Independent music clips retain the original media file after import. The mixer selects a shared PCM rate and layout for playback and export. It reads each audio track through an independent decoder cursor, maps the source samples onto the project mix grid, and applies slot-local fades, gain, and voice effects. Waveform sampling still measures the source track on its own clock.

The workbench uses Lib-Media's `MediaLibraryPicker` when the host supports photo-library browsing. It imports confirmed references to project media storage in selection order. The desktop video and standalone audio entry points use the file picker. The export resolution menu scales the current canvas ratio to an even-sized 720p, 1080p, or 2160p output. It probes the software encoder before enabling the 2160p preset. The workbench chooses a preview texture size separately. Export renders a separate snapshot at the selected output size.

`AVKit::Media` samples immutable revisions through `TimelineSession`. It maps project times to half-open clip ranges, samples the source frames, and activates the corresponding EntityX layers. Each sticker's spatial parent is its clip. Active draws can share one resource input slot. The macOS preview and export pass the same plan and retained inputs to bgfx/Metal. Lib-Media's audio clock drives preview. Export samples a configured frame grid and reuses the existing `CodecProvider` negotiation and staged MP4 writer. Video clips in the file profile may include their original audio. Selected audio clips must share a PCM rate and channel layout. Periods without audio produce silent PCM on the project clock. The preview and CLI export run at 24 fps. [Timeline API](../include/huxerui/avkit/timeline.h)

`TimelineRenderer::Open` creates the platform bgfx compositor on the calling render sequence. All AVKit bgfx creation paths, including the standalone factory, share the same process-wide reservation. The backend holds it until context shutdown and resource cleanup finish. Recording and editing preview run on their respective render workers, and export opens its renderer after the app closes preview. Supply a stoppable token on a native render worker to wait for an occupied renderer. Calls without a stoppable token and calls on Web report contention immediately. Cancellation returns an empty renderer. The renderer releases its GPU resources on its owning sequence before returning the lease. Callers supply the output description and any required platform window or canvas handles.

Open `PreviewTexture` on the application context when platform presentation requires it. A preview texture exposes the HuxerUI texture and its close operation. The renderer imports a live camera input with `ImportTexture` and verifies ownership of completed composites passed to `PreparePreview` or `PublishPreview`. Paused preview prepares a commit on the render sequence and runs it on the application sequence with layer geometry for the same frame. Drop a pending commit when its preview request is cancelled. Recording publishes and encodes the same composite. The low-level bgfx entry points remain available for standalone render plans and texture interop. [Timeline renderer API](../include/huxerui/avkit/timeline.h) · [Preview and render API](../include/huxerui/avkit/render.h)

`TimelineRenderer::OpenTimeline` binds an independent sampling session to the compositor device and output format. Call `SampleVideo` on that session and pass its `TimelineFrame` to `Draw` on the same render sequence. The video source imports hardware frames on the bound device and retries rejected platform pictures in software at the same source PTS. A session constructed directly uses a software video provider. The image resource cache on the scene root decodes still images and file-backed coverage from the scene catalog. Mask sequences validate the dimensions of each sampled frame in that cache. PAG compositions retain the same catalog in the plan. The renderer uploads retained CPU pixels during pass preparation and reuses compatible textures. A standalone scene can evaluate its components for the same `Draw` entry point. Both paths can use `EncoderFrame` with the negotiated encoder contract. Application code owns the preview view, transport controls, and export progress. [Timeline API](../include/huxerui/avkit/timeline.h)

`PlayClockedTimeline` drives video from consumed PCM and handles seek, audio refill, and suppression of late frames. A timeline session can specify a physical output size independent of the authored canvas. Export uses its encoder configuration to select that size and the video rate. Cancellation is forwarded to timeline sampling and codec waits, and the MP4 is published after successful finalization. [Timeline API](../include/huxerui/avkit/timeline.h)

Video clips and canvas overlays can store clockwise rotation around their destination center in physical canvas coordinates. The preview session retains a paused sample while a canvas gesture is active. `TimelineSession::RecomposeVideo` applies a transform delta to the sampled scene without decoding again. The delta operates in normalized project canvas coordinates and is applied after the target's evaluated transform. `PreviewFrame::FindLayout` returns a `TargetLayout` for the drawn target. Its `transform` maps from local coordinates to normalized project canvas coordinates, and its `local_bounds` stores measured content in those same local coordinates. `PreviewFrame::HitTest` takes a point in normalized project canvas coordinates, walks the same frame's layouts from front to back, and tests the inverse-mapped point against each target's local bounds. The optional filter lets the editor restrict selection without reproducing draw order. The editor maps view coordinates to the fitted picture before querying the frame and handles selection buttons separately. `SetTransform` accepts a complete local-to-canvas matrix in the same coordinates as `TargetLayout::transform`. `SetTransformDelta` accepts a canvas-space delta relative to the original layout at the paused position. Either call replaces the previous temporary transform and returns a request ID that the host can match against the presented frame when displaying gesture feedback. `ClearTransform`, a seek, playback, or a project revision change removes the temporary transform. An `EditTarget` identifies either a timeline clip or an overlay instance and exposes the corresponding typed `ClipId` or `InstanceId`. Timeline selection and paused recomposition use the same target type. `ProjectSnapshot::canvas_size` stores the authored dimensions used for numeric editing. `PreviewFrame::texture_size` describes the published texture. In the app, `preview_view_size` is the outer view extent in logical UI units, and `preview_content_rect` is the fitted image area in the same units. Releasing the gesture commits one undoable edit. The same committed rotation is used by preview and export.

Each timeline cursor opens only its selected video or audio decoder directly at the mapped source time. Sampling retires stale cursors before opening replacements. Audio sampling releases a clip's decoder when it reaches the trim end, including when one PCM block spans a cut.

The macOS timeline preview fills the available capacity of its 250 ms PCM queue before deciding whether to sample another picture. At startup and after a seek, it publishes the requested picture, primes audio, and requests playback. It then schedules up to two upcoming video entries within a rolling 400 ms lookahead. The lookahead also gives the sampling sequence time to collect the result before entry. The preview submits and seals the final PCM block together.

After the PCM clock first advances, preview uses steady elapsed time to pace pictures between device completion updates. The estimate stays within accepted PCM and at most 150 ms ahead of the confirmed position. It resets on seek or when playback leaves the playing state. A batched update can leave multiple pictures due. The preview tries the next picture if it still has a frame interval to compose before the 100 ms lateness threshold. It rechecks the clock after composition and discards pictures that miss the threshold. Once audio drains, the last in-range picture can still be published.

## macOS animated mask transition milestone

`ProjectEdit::SetTransition` gives a pair of adjacent, overlapping video clips on one track a stable `InstanceId`. Each clip supplies its own source-time mapping. Before the transition range the outgoing clip supplies the picture. At the exclusive end the incoming clip takes over. The range must fit both clips, and the clips share a destination rectangle. Their stickers retain individual instance IDs and clip-local layouts. Other visual tracks retain their ordinary compositing order. [Project API](../include/huxerui/avkit/project.h)

`SetTransitionFeather` stores a base value and a typed curve with hold and linear interpolation. `SetTransitionRange` preserves the instance ID. Moving the range keeps the local key times. Resizing scales the keys by the exact duration ratio and validates the proposed range and keys in the transaction. A key may sit at the exclusive end. The project stores the transition instance, mask resource key, and curve.

The transition system maps the half-open local offset to the registered `MaskSequenceData` and retains the selected frame as a coverage input. Both source pictures and the feather curve use the same timeline evaluation. The macOS example packages eight grayscale PNG frames and caches the decoded coverage by version and frame index as `R8Unorm` data textures. The bgfx shader reads numeric coverage, applies the sampled feather and optional inversion, and blends both source images. Preview and export consume the same timeline plan. [Timeline API](../include/huxerui/avkit/timeline.h) · [Masked blend shader](../src/render/bgfx/shaders/avkit_fs_masked_blend.sc)

During the overlap, EntityX builds one subtree for each clip and its spatial children. The video keeps its destination rectangle. Sticker transforms resolve through the clip into canvas coordinates, so coverage outside the clip remains visible up to the canvas edge. The renderer independently composes the two sides to canvas-sized attachments, samples both completed images, and blends them above lower tracks. The mask uses clip-local coordinates and clamps to the edge outside the shared clip rectangle. bgfx reuses the offscreen framebuffers between submissions. [Render plan](../include/huxerui/avkit/render.h)

## macOS thumbnail and waveform milestone

`ClipSource::reverse` reverses a 1× clip. `MapClipToSource` and `MapClipToProject` return ascending time ranges in either direction. Reverse video uses the frame immediately to the left of the mapped time. Reverse audio reads a short source window in increasing presentation time and outputs its sample frames in reverse order with channels intact. [Project API](../include/huxerui/avkit/project.h)

`TimelineSession::PrerollVideoEntries` invokes the timing and preroll stages. The preroll system selects up to two entries in order of entry time. It prepares reverse clips and trimmed forward clips that enter outside a transition while another clip continues. A worker closes its reverse decoder after filling the window. For a forward entry, the worker decodes through the first visible picture and transfers the cursor after its last call when `CodecNegotiation::allow_sequence_handoff` is set. The sampling sequence then owns subsequent calls and close. Fallback events recorded by the worker are delivered on the sampling sequence when the result is collected. Later fallback events are also delivered there. The native Lib-Media video adapter and FFmpeg opt in. On Web, preparation uses the software provider when worker threads are available. A missing result uses ordinary sampling. A rejected platform frame retries in software at the exact source PTS. [Timeline API](../include/huxerui/avkit/timeline.h) · [Codec API](../include/huxerui/avkit/codec.h)

Each video cache keeps up to 64 CPU pictures within 64 MiB, or one picture if it exceeds the byte budget. Cached CPU planes own their bytes independently of the decoder. Each cache holds up to eight platform picture leases. A prepared forward cursor retains its recently decoded pictures for seeks and can advance through up to one second of source time on a forward seek. Reverse seeks can reuse CPU windows covering the target picture. `Seek` cancels pending jobs and releases future cursors unless the caller supplies the exclusive end of the new lookahead. Up to two prepared entries strictly within that window can be retained, including live forward decoders. The macOS preview uses its 400 ms lookahead for this call. A later lookahead update releases preparation for entries it no longer selects. A full reset or project revision change releases the windows. Analysis and composition run at the visible sample time.

`AssetSampler` opens video-only and audio-only cursors on its owning worker sequence. Thumbnail requests report actual decoded PTS. The example selects an interior source time for each thumbnail cell. Lib-Media fits supported platform frames to a small external texture. The software fallback produces a small RGBA bitmap directly. Waveform buckets align to the audio track start and contain each channel's minimum and maximum decoded PCM value. The example maps whole buckets back to the visible clip and draws them in playback order. The cache keys include the asset path, file metadata, relink revision, track, and sampling resolution. Coarse buckets can merge two cached half-size buckets while preserving their extrema. [Sampling API](../include/huxerui/avkit/asset_sampling.h)

The example maintains one active background request and one pending viewport. A scroll, zoom, or relink advances the generation and requests cooperative cancellation. The reader wakes a cancelled codec wait, and the view checks the generation again before displaying results. The timeline retains overlapping samples while the new viewport fills. The first available picture fills unresolved cells on the same clip, and later samples replace their own project-time cells. AVKit's `OpenCanvasThumbnailSource` fits platform frames through Lib-Media. On Android, AVKit transfers the resulting GLES texture to a hardware-backed bitmap on a sampling context independent of the preview renderer. The application paints the returned thumbnail in the timeline Canvas. Thumbnail sampling prefers a platform video decoder on the worker and retries a rejected frame in software at the same source PTS. Waveform sampling uses independent audio cursors. The preview owns its own `TimelineSession` and resumes at the current project position when a relinked revision replaces its sources.

### Tracking anchor milestone

`TimelineSession` and the rendered export entry points accept an in-process `AnalysisProvider` for exact file sampling. The first implementation probes body, face, and hand landmark capabilities against the decoded frame's format and storage before opening their model sessions. A face-only clip uses full-frame face tracks. Clips that use hand tracking derive regions from the body's `head`, `left_wrist`, and `right_wrist` landmarks. The provider returns each region index and a `face_center` or `wrist` landmark for association. Core checks confidence and position against that region and competing regions on the same frame. [Analysis API](../include/huxerui/avkit/analysis.h)

Stateless file models can request up to eight neighboring frames. A separate video decoder gathers their temporal windows in increasing source PTS order, including available frames outside the clip trim. Models with strict windows skip the target when a sample is missing. Each clip owns a generation and chooses its primary track from the source trim start when a valid reference is available. A missing part or temporarily absent primary hides its dependent stickers. A seek resets stateless reference sampling. The owner waits for the exact frame and job, and cancellation during inference retires the pending job and its sessions. Late completions release their retained resources through the provider.

A full-frame body provider can declare an ordered stateful session with no explicit neighbors. Core opens a separate video cursor at the clip's source trim start and advances it in source presentation order even when the first request is a seek or a reverse sample. It caches the selected person's anchor results, including absent results, under the exact source PTS in that clip. A seek reuses the completed prefix and advances it only when needed. A project revision or relink recreates the clip session. Export builds its own ordered prefix from the same trim. A cold seek near the end of a long clip may require substantial preroll.

A tracked sticker persists its subject part and semantic point names. The EntityX sticker layer is a spatial child of a tracking anchor within its clip. One point supplies a position. Two points also define the local pixel scale and direction. The Metal compositor applies the resulting affine draw in the fitted video coordinates, including in the clip offscreen passes. [Project API](../include/huxerui/avkit/project.h) · [Timeline API](../include/huxerui/avkit/timeline.h)

### YuNet face provider

`AVKit::NCNN` builds ncnn 20261020 for CPU. Supply the versioned FP32 graph and weights as `RawAsset` values. The factory opens the model and returns a diagnostic for missing assets or a load failure. Sessions accept unrotated SDR I420 CPU frames and report the face center and five named landmarks in visible-image coordinates. Use software decoding to supply these frames. [Provider API](../include/huxerui/avkit/ncnn.h) · [YuNet pack](../models/yunet/2023mar)

The first valid picture fixes the reference for the session. An empty or ambiguous reference leaves the session without a subject. Later samples need one eligible face and a close match to the eye-aligned luma patch from the reference. Each comparison uses that same reference, so requesting a target in a different playback order gives the same result for the same clip trim and input format. This selection heuristic is for a single subject. It can hide the sticker when the subject changes pose or lighting significantly. Run model inference and completion on the session's worker sequence. Cancellation skips the result before or after inference.

### ncnn body and hand provider

`CreateNcnnAnalysisProvider` can load the YuNet pack and the MediaPipe body and hand pack in one provider. The two detector and landmark pairs run in ncnn on SDR I420 CPU pictures. Body analysis detects people in the full picture and reports 33 source-space landmarks. Hand analysis searches around the body-derived wrist regions and reports up to 21 landmarks per valid hand. A YuNet session can also process the head regions when the project uses both face and hand stickers. [Model preparation](../models/mediapipe/body-hand) · [Provider API](../include/huxerui/avkit/ncnn.h)

Each clip initializes its body session at the source trim and selects its primary person once. The session uses source-time motion prediction and normalized torso pose in a one-to-one assignment. Torso chroma contributes a small cost and can corroborate a recovery inside the motion gate. A track that loses a reliable match hides immediately. It needs three consecutive compatible observations before becoming visible again. When trajectories merge, recovery requires additional motion or appearance evidence. If the evidence remains ambiguous, the anchor stays hidden.

The body provider uses the head to stabilize motion prediction when hip landmarks move abruptly. If the pose crop collapses or pulls the torso toward another detected person, it retries with an upright crop centered below the detected head. A replacement needs a valid torso separated from neighboring detections. A short-gap tentative match can restart from the last confirmed track, with a fresh confirmation count.

Hand analysis checks the coarse palm and refined wrist against the closest body region. When the first crop yields no confirmed hand, the provider tries smaller wrist-centered crops with alternate orientations. If a strong palm detection from a measured region still lacks a verified wrist, the provider can reuse its current-frame palm points for a tighter, slightly rotated landmark crop. The alternate crop requires higher landmark confidence. Both detections must remain close to the same body wrist. On the ordered path, a lone visible wrist that stays in nearly the same position while its side label flips is treated as ambiguous. A missing or ambiguous wrist hides only its own sticker while the body track stays confirmed.

The ordered sampler uses the current body wrist for the first search. If it yields no verified hand, a second search for that side can translate the preceding source frame's verified wrist by the head displacement and adjust the center toward a nearby current body wrist. It submits the narrower region as `predicted`. Verified wrists from the first search retain their ownership and compete with retry candidates. Both searches use the target picture, and the two source frames must be adjacent and belong to the same confirmed person and clip.

When the next frame has a usable body wrist, its retry may use the preceding frame's verified hand from either search. When the current body wrist itself is unavailable, a predicted search also requires the preceding frame to have had a usable body wrist. The provider verifies the palm and refined wrist in the target picture, and Core rejects predicted matches near competing regions. Preview, seeks, reverse playback, and export use the same source-ordered result.

If the selected hand has no usable temporal reference after the first search, Core can extend the retry region along the same person's target-frame elbow-to-wrist vector. Both body joints must pass the confidence check. The optional `AnalysisRegion::forearm` carries that vector so the provider can inspect both sides of the projected forearm while validating the palm and refined wrist near its tip. This uses the existing retry for the missing hand and preserves verified hands as competing regions.

### ncnn portrait segmentation

Set `NcnnAnalysisModels::portrait` to the MediaPipe Selfie Segmentation pack for default analysis. Set `NcnnAnalysisModels::portrait_wide` to the optional general PP-HumanSegV2-Lite pack for wide, full-body footage. Both use one unrotated SDR I420 CPU picture per job. The provider resizes the visible source to the pack's 256×256 planar RGB input and quantizes foreground probabilities into an `R8Unorm` image with top-left rows. The UV map is the identity because the grid covers the whole visible source. Use a software video decoder with this provider. [MediaPipe pack](../models/mediapipe/selfie-segmentation) · [PP-HumanSegV2-Lite pack](../models/paddle/pp-humansegv2-lite) · [Provider API](../include/huxerui/avkit/ncnn.h)

During editing, set `PortraitMask::model_id` to `analysis::kPPHumanSegV2LiteModelId` or `analysis::kMediaPipeSelfieModelId`, place the component value in `BackgroundReplacementSpec::mask`, and apply the spec with `ProjectEdit::SetBackground`. An empty model ID selects the provider default. The same provider can open both models for different clips of the same asset. An unavailable explicit model produces a diagnostic that names the requested ID.

On threaded builds, sessions of the same model share its inference worker. A timed-out preview job can retire while its ncnn extraction finishes, and a different model has its own worker. Cancelled jobs release their frame leases when retired. On Web builds without pthreads, inference runs inline and deadline checks can discard the result after inference completes. MediaPipe is tuned for close-range people. PP-HumanSegV2-Lite helps preserve people in wider shots. Both output single-frame foreground probabilities.

Prepare each pack with its own Python environment and output directory using PNNX 20261020. Verify both packs before collecting their four runtime files. Each generated pack keeps its own `LICENSE` and `SHA256SUMS` in its output directory.

```sh
pnnx="/path/to/pnnx"
models="$PWD/build/portrait-models"
python3 -m venv "$models/selfie-venv"
python3.11 -m venv "$models/wide-venv"
"$models/selfie-venv/bin/python" -m pip install onnx==1.22.0
"$models/wide-venv/bin/python" -m pip install paddle2onnx==1.3.1 onnx==1.19.1

"$models/selfie-venv/bin/python" models/mediapipe/selfie-segmentation/prepare.py "$pnnx" "$models/selfie"
"$models/wide-venv/bin/python" models/paddle/pp-humansegv2-lite/prepare.py "$pnnx" "$models/wide"
(cd "$models/selfie" && shasum -a 256 -c SHA256SUMS)
(cd "$models/wide" && shasum -a 256 -c SHA256SUMS)

pack="$models/runtime"
mkdir -p "$pack"
cp "$models/selfie/selfie.ncnn.param" "$models/selfie/selfie.ncnn.bin" "$pack/"
cp "$models/wide/pp_humansegv2_lite.ncnn.param" "$models/wide/pp_humansegv2_lite.ncnn.bin" "$pack/"
```
