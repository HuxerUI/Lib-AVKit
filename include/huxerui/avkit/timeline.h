#pragma once

/// @file
/// @brief Timeline sampling, playback, GPU composition, and MP4 export.
///
/// Link AVKit::Media to sample immutable project revisions on the project clock. Link
/// AVKit::Render for bgfx composition, managed preview, and rendered export.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <huxerui/avkit/component.h>
#include <huxerui/avkit/file_movie.h>
#include <huxerui/avkit/project.h>
#include <huxerui/avkit/render.h>

namespace huxerui::avkit {

namespace analysis { class AnalysisProvider; }
namespace render { class TimelinePreviewSession; }
namespace detail { struct TimelineSessionAccess; }

class RenderScene;

/// @brief Evaluated layout of an editable target at one project time.
struct TargetLayout {
  /// Maps the target's local coordinates to normalized project canvas coordinates.
  Affine2D transform;
  /// Content bounds in the target's local coordinates. The unit rectangle is the default.
  Rect local_bounds{0, 0, 1, 1};

  bool operator==(const TargetLayout&) const = default;
};

/// @brief Registers components and systems on a new EntityX scene.
using SceneModule = std::function<void(RenderScene&)>;

/// @brief Shared resource and extension configuration for capture, preview, and export.
struct SceneConfiguration {
  /// Immutable catalog installed on the root entity when the scene opens.
  std::shared_ptr<const ResourceCatalog> resources;
  /// Provider used by analysis systems installed for this source.
  std::shared_ptr<analysis::AnalysisProvider> analysis;
  /// Optional component and system registration, applied to every fresh scene.
  SceneModule module;
};

/// @brief Inspects a supported movie and returns its importable source tracks and ranges.
/// @param file Local video file to inspect.
/// @return Import metadata with available source intervals and oriented video dimensions.
AssetSource InspectTimelineMovie(const File& file);

/// @brief Inspects an image for native placement on a visual track.
/// @param file Local PNG or JPEG file.
/// @param default_duration Positive insertion duration. Each instance can override it.
/// @return Visual asset pointing to the original image.
AssetSource InspectTimelineImage(const File& file, media::MediaTimestamp default_duration = {3, 1});

/// @brief Inspects a local still image or movie using the same visual import entry.
/// @param file Selected visual file.
/// @param image_duration Default insertion duration when the file is an image.
AssetSource InspectTimelineVisual(const File& file, media::MediaTimestamp image_duration = {3, 1});

/// @brief Inspects a local audio file for use as an independent audio clip.
/// @param file Local prepared PCM WAVE file.
/// @return Import metadata for a single audio track.
AssetSource InspectTimelineAudio(const File& file);

/// @brief Compositor input for one exact project time.
struct TimelineFrame {
  /// Exact presentation time on the project or capture clock.
  media::MediaTimestamp pts;
  /// Tree retaining the evaluated draw inputs.
  std::shared_ptr<const render::RenderPlan> plan;
};

/// @brief Samples one immutable revision on a worker sequence.
/// Each instance owns its codec cursors, analysis generations, and ECS world.
class TimelineSession {
public:
  /// @brief Prepares project metadata and retains the selected providers.
  /// Video sampling uses the software provider. Open a session through TimelineRenderer for hardware import.
  /// @param project Retained snapshot whose sources and PCM timing are validated before sampling.
  /// @param providers Candidate platform and software decoders.
  /// @param policy Which providers can serve the timeline cursors.
  /// @param on_fallback Optional diagnostics delivered on the sampling sequence.
  /// @param analysis Required for tracking anchors and backgrounds that use portrait analysis.
  /// @param resources Catalog shared by scene evaluation and rendering. Required for packaged effects.
  /// @param output_size Optional physical composition size. Empty uses the authored canvas.
  TimelineSession(std::shared_ptr<const ProjectSnapshot> project, CodecProviders providers,
                  CodecPolicy policy = CodecPolicy::Auto, OnCodecFallback on_fallback = {},
                  std::shared_ptr<analysis::AnalysisProvider> analysis = {},
                  std::shared_ptr<const ResourceCatalog> resources = {}, std::optional<PixelSize> output_size = {});

  ~TimelineSession();

  TimelineSession(const TimelineSession&) = delete;

  TimelineSession& operator=(const TimelineSession&) = delete;

  /// @brief Installs a new revision and retires the old cursors and analysis generations.
  /// The current revision remains installed if validation or module setup throws.
  /// @param project Replacement snapshot. Registered scene modules are applied to its new ECS world.
  void ApplyProject(std::shared_ptr<const ProjectSnapshot> project);

  /// @brief Installs a scene module on this sequence and on each subsequently applied revision.
  /// The module can query clip IDs and attach custom components and systems through avkit/scene.h.
  /// @param module Callback invoked immediately and again whenever a project revision is installed.
  void InstallSceneModule(SceneModule module);

  /// @brief Samples one picture in the half-open project interval.
  /// @param at Exact time on the project clock.
  /// A rejected platform picture in a renderer-bound session retries from the same source PTS.
  /// @param output Optional color format. Uses the bound renderer format or BGRA8 by default.
  /// @param stop Cancels pending decode and analysis waits. Cancellation retires sampling state.
  /// @param continuous_step Positive media step for continuous playback. Omit on random access.
  /// @param analysis_wait Per-job mask or depth budget measured from submission. All visible jobs are submitted
  /// before waiting. A timeout keeps unmasked video or hides depth-bound stickers on the affected clip. Omit for offline sampling.
  /// @param on_decode Optional observer for decoded frames after successful texture preparation.
  /// @return Immutable plan and retained inputs. Required analysis is sampled at its exact source PTS.
  TimelineFrame SampleVideo(media::MediaTimestamp at, std::optional<render::PixelFormat> output = {},
                            std::stop_token stop = {},
                            std::optional<std::chrono::milliseconds> analysis_wait = {},
                            std::optional<media::MediaTimestamp> continuous_step = {},
                            const std::function<void(const media::VideoFrame&)>& on_decode = {});

  /// @brief Re-evaluates the most recent sample's draws using its retained draw inputs.
  /// The delta applies only to the returned plan. Use the original frame from the last SampleVideo call.
  /// @param frame The original frame returned by the most recent SampleVideo call.
  /// @param target Stable clip or overlay ID in the sampled project.
  /// @param delta Finite and invertible transform composed after the target's evaluated transform.
  /// Both the input and output of the delta use normalized project canvas coordinates.
  /// @return The recomposed frame with the original media time and retained inputs.
  /// @throws std::invalid_argument If the frame is stale or the transform is invalid.
  TimelineFrame RecomposeVideo(const TimelineFrame& frame, EditTarget target, Affine2D delta);

  /// @brief Requests up to two video entries strictly inside the lookahead, ordered by entry time.
  /// Selects reverse clips and trimmed forward clips that enter outside a transition while another picture remains visible.
  /// Call on the sampling sequence. Providers must support concurrent Probe and Open for independent sessions.
  /// Forward cursors must negotiate sequence handoff. Browser workers use the software provider.
  /// Returns without waiting for decode. Sampling decodes on demand if preparation is unavailable.
  /// @param lookahead In-range playback start and exclusive end on the project clock.
  /// @param stop Cancels the pending decode or discards its uncollected result.
  /// @return True if at least one entry was selected and every selected entry is cached.
  bool PrerollVideoEntries(TimeRange lookahead, std::stop_token stop = {});

  /// @brief Returns the PCM mix format selected from the imported audio tracks.
  media::AudioFormat PcmFormat() const;

  /// @brief Mixes an interval on the project PCM sample grid, filling gaps with silence.
  /// @param start Interval start aligned to the sample rate.
  /// @param samples Positive number of sample frames.
  /// @param stop Cancels decoder waits and block evaluation.
  /// @return Mixed PCM starting at the requested position and containing exactly samples sample frames.
  /// @throws std::out_of_range If the requested block does not fit the project interval.
  media::AudioFrame SampleAudio(media::MediaTimestamp start, std::uint32_t samples, std::stop_token stop = {});

  /// @brief Starts a new sampling epoch at an in-range project time.
  /// Retires audio and analysis state. Keeps reusable video at the target.
  /// @param at Destination time on the project clock.
  /// @param preroll_end Optional exclusive end of the new lookahead on the project clock. Keeps up to two prepared
  /// future entries, including live forward cursors, strictly between at and this end.
  void Seek(media::MediaTimestamp at, std::optional<media::MediaTimestamp> preroll_end = {});

  /// @brief Retires all sampling cursors and analysis state.
  void Reset();

private:
  friend class render::TimelinePreviewSession;
  friend struct detail::TimelineSessionAccess;

  TimelineSession(std::shared_ptr<const ProjectSnapshot> project, CodecProviders providers,
      CodecPolicy policy, OnCodecFallback on_fallback, std::shared_ptr<analysis::AnalysisProvider> analysis,
      std::shared_ptr<const ResourceCatalog> resources, std::optional<PixelSize> output_size,
      std::shared_ptr<render::RenderBackend> device, std::optional<render::PixelFormat> device_format);

  TimelineFrame SampleVideo(media::MediaTimestamp at, std::optional<render::PixelFormat> output,
      std::stop_token stop, std::optional<std::chrono::milliseconds> analysis_wait,
      std::optional<media::MediaTimestamp> continuous_step, const std::function<void(const media::VideoFrame&)>& on_decode,
      std::vector<detail::TargetLayoutEntry>* layouts);

  TimelineFrame RecomposeVideo(const TimelineFrame& frame, EditTarget target, Affine2D delta,
                               std::vector<detail::TargetLayoutEntry>* layouts);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

inline constexpr std::uint32_t kTimelinePreviewRate = 24;

struct TimelinePlaybackStats {
  /// Video frames published against the PCM clock, excluding immediate seek frames.
  std::size_t clocked_frames{};
  /// Rendered frames whose publication was skipped because the PCM clock had advanced too far.
  std::size_t late_video_frames{};
  /// Largest PCM-to-picture lag among clocked frames that were published.
  double maximum_video_lag_ms{};
  /// Underruns accumulated across the playback epochs created by seeks.
  std::uint64_t audio_underruns{};
};

using TimelineSeek = std::function<std::optional<media::MediaTimestamp>(media::MediaTimestamp)>;
using DrawTimelinePicture = std::function<render::Texture(media::MediaTimestamp, std::stop_token)>;
using PublishTimelinePicture = std::function<void(const render::Texture&, media::MediaTimestamp)>;
using TimelineIterationScope = bool (*)(const std::function<bool()>&);

/// @brief Advances preview video against consumed PCM and skips publication of frames that arrive too late.
/// @param session Timeline session sampled on the playback worker.
/// @param range Half-open project interval available to playback and seek.
/// @param at Initial position within the interval, with at least one PCM sample before its end.
/// @param muted True to mute output while the PCM clock continues advancing.
/// @param seek Optional callback that receives the last presented time and returns a requested seek position.
/// @param stop Stops the playback loop and cooperatively cancels video sampling.
/// @param draw Renders the requested project timestamp to a completed image.
/// @param publish Delivers a completed image and its timestamp when ready for display.
/// @param scope Optional wrapper for each playback iteration, used to bound platform resource lifetimes.
/// @param frame_rate Positive video presentation rate independent of the export rate.
/// @param keep_open_at_end Whether a seek-enabled loop continues accepting seeks at end of stream.
/// @return Playback lag and underrun counters for the completed run.
TimelinePlaybackStats PlayClockedTimeline(TimelineSession& session, TimeRange range,
    media::MediaTimestamp at, bool muted, const TimelineSeek& seek, std::stop_token stop,
    const DrawTimelinePicture& draw, const PublishTimelinePicture& publish,
    TimelineIterationScope scope = nullptr, std::uint32_t frame_rate = kTimelinePreviewRate, bool keep_open_at_end = true);

namespace render {

struct TimelineExportOptions;

/// @brief Composites evaluated timeline frames on the retained bgfx backend. Link AVKit::Render.
/// The backend and returned images belong to the owning render sequence.
/// Keep the renderer alive while its output leases are used.
class TimelineRenderer {
public:
  /// @brief Opens a compositor on the calling render sequence.
  /// On native render workers, supply a stoppable token to wait for the active renderer to close.
  /// Calls without a stoppable token and calls on Web report an occupied renderer immediately.
  /// @param target Fixed output extent and attachment format.
  /// @param stop Optional cancellation token for waiting on the active bgfx renderer.
  /// @param platform Optional platform context handles.
  /// @return The compositor, or an empty pointer if cancelled.
  /// @throws std::logic_error If the renderer is occupied and the call cannot wait.
  static std::unique_ptr<TimelineRenderer> Open(ImageInfo target, std::stop_token stop = {}, RenderPlatformHandles platform = {});

  ~TimelineRenderer();

  /// @brief Opens a timeline session using this compositor's device, extent, and attachment format.
  /// Sample and draw on the same render sequence. The session retains the device until it is closed.
  /// @param project Immutable timeline revision to sample.
  /// @param providers Candidate platform and software decoders.
  /// @param policy Decoder selection and fallback policy.
  /// @param scene Resources, analysis provider, and optional ECS extension.
  /// @param fallback Optional observer of decoder provider changes.
  /// @return Timeline session that can sample hardware pictures for this compositor.
  std::unique_ptr<TimelineSession> OpenTimeline(std::shared_ptr<const ProjectSnapshot> project,
      CodecProviders providers, CodecPolicy policy = CodecPolicy::Auto, SceneConfiguration scene = {},
      OnCodecFallback fallback = {}) const;

  /// @brief Returns the attachment format required by this compositor.
  PixelFormat Format() const;

  /// @brief Imports the current image of a platform texture on the GPU.
  /// @param texture SDK texture with a completed publication.
  /// @return Retained image in this renderer's graphics context.
  /// @throws std::runtime_error If the published image cannot be imported.
  Texture ImportTexture(const ExternalTexture& texture);

  /// @brief Uploads retained CPU pixels on this renderer's sequence.
  /// @param pixels Valid packed pixels and their row layout.
  Texture UploadTexture(const ImagePixels& pixels);

  /// @brief Uploads CPU inputs and composites the tree's images, textures, and evaluated PAG samples.
  /// @param frame Evaluated media time, draw plan, and retained image sources.
  /// @return Composite leased after GPU completion.
  /// @throws std::runtime_error If a required resource or a render result is unavailable.
  Texture Draw(const TimelineFrame& frame);

  /// @brief Prepares this renderer's completed output without changing the preview texture's revision.
  /// Commit on the application sequence with any accompanying UI state. Dropping the callback abandons the frame.
  /// An empty callback lets the caller publish directly with PublishPreview on the render sequence.
  /// @param preview Open preview publisher matching the composite's extent.
  /// @param output Completed RGB(A) image from this renderer.
  /// @return Commit callback, empty success when direct publication is needed, or a render diagnostic.
  Result<PreviewFrameCommit, RenderError> PreparePreview(PreviewTexture& preview, const Texture& output);

  /// @brief Publishes this renderer's completed output immediately on the render sequence.
  /// @param preview Open preview publisher matching the composite's extent.
  /// @param output Completed RGB(A) image from this renderer.
  /// @return Success after updating the preview texture or a render diagnostic.
  Result<void, RenderError> PublishPreview(PreviewTexture& preview, const Texture& output);

  /// @brief Packs an output lease for the negotiated encoder while preserving its timing.
  /// Throws PlatformEncoderInputError when the encoder cannot import the platform output.
  /// @param output Completed composite from this renderer.
  /// @param config The open encoder's negotiated format and storage.
  /// @param pts Exact presentation time on the output clock.
  /// @param duration Optional positive display duration.
  /// @return Retained picture ready for submission to the encoder.
  media::VideoFrame EncoderFrame(const Texture& output, const media::VideoEncoderConfig& config,
      media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration = {});

private:
  friend class TimelinePreviewSession;
  friend void ExportRenderedTimeline(TimelineRenderer& renderer, std::shared_ptr<const ProjectSnapshot> project,
      const File& destination, const CodecProviders& providers, const FileExportSettings& settings,
      const TimelineExportOptions& options);

  Texture Draw(const TimelineFrame& frame, std::vector<avkit::detail::TargetLayoutEntry>& layouts);

  struct Impl;

  explicit TimelineRenderer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

/// @brief One renderable publication from a managed preview session.
struct PreviewFrame {
  std::shared_ptr<ExternalTexture> texture;
  std::shared_ptr<const ProjectSnapshot> project;
  media::MediaTimestamp pts{};
  /// Monotonic publication identity, including edits at an unchanged PTS.
  std::uint64_t sequence{};
  bool playing{};
  /// Published texture dimensions in pixels.
  PixelSize texture_size{};
  /// Request ID returned by SetTransform or SetTransformDelta when this frame includes a temporary edit.
  std::optional<std::uint64_t> transform_request;
  /// Invalidated by a newer transport or project request.
  std::stop_token stop;
  /// Applies the prepared GPU output. Invoke together with the matching UI layout.
  PreviewFrameCommit commit;

  /// @brief Finds the topmost editable target whose transformed content bounds contain the point.
  /// @param point Position in normalized project canvas coordinates. Points outside the canvas return no hit.
  /// @param accepts Optional filter for application selection rules. Empty accepts every editable target.
  /// @return The first matching target in front-to-back draw order, or empty if none match.
  std::optional<EditTarget> HitTest(Point point, const std::function<bool(EditTarget)>& accepts = {}) const;

  /// @brief Finds a target's layout in this exact frame, including measured content bounds.
  /// @param target Stable project clip or overlay identifier.
  /// @return Layout for the drawn target, or empty if it has no editable layout in this frame.
  std::optional<TargetLayout> FindLayout(EditTarget target) const;

private:
  friend class TimelinePreviewSession;

  std::shared_ptr<const std::vector<avkit::detail::TargetLayoutEntry>> layouts_;
};

/// @brief The latest completed transport state and any worker error.
struct TimelinePreviewStatus {
  media::MediaTimestamp pts;
  bool playing{}, ready{};
  std::string error;
};

/// @brief Owns preview decoding, audio clock, rendering and transport requests on a worker.
class TimelinePreviewSession {
public:
  using Present = std::function<void(PreviewFrame)>;

  /// @param target Physical preview dimensions and color format.
  /// @param providers Decoder candidates for the session.
  /// @param present Worker callback that receives each prepared frame for the session.
  /// @param scene Resources, analysis and extensions reused across project revisions.
  /// @param fallback Optional decoder switch observer called on the worker.
  TimelinePreviewSession(ImageInfo target, CodecProviders providers, Present present, SceneConfiguration scene = {},
                         OnCodecFallback fallback = {});

  ~TimelinePreviewSession();

  /// @brief Installs a project revision and refreshes its current position and play state.
  /// @param project Immutable revision to display. The current position is clamped to its range.
  void ApplyProject(std::shared_ptr<const ProjectSnapshot> project);

  /// @brief Moves the playhead while preserving the requested play state.
  /// Clears the temporary transform from the previous position.
  /// @param at Project time, clamped to the available preview range when a project is installed.
  void Seek(media::MediaTimestamp at);

  /// @brief Starts or resumes playback from the current position and clears the temporary edit.
  void Play();

  /// @brief Stops playback and refreshes the current preview position as a paused frame.
  void Pause();

  /// @brief Previews an absolute transform on the paused frame, reusing its media sample.
  /// Replaces the previous temporary transform. A target without a layout in this frame keeps its original placement.
  /// @param target Stable project clip or overlay identifier.
  /// @param transform Finite and invertible mapping from the target's local coordinates to normalized project canvas coordinates.
  /// @return Session-local request ID for matching the presented frame, or zero if no paused project is active.
  /// @throws std::invalid_argument If the transform is nonfinite or singular.
  std::uint64_t SetTransform(EditTarget target, Affine2D transform);

  /// @brief Previews a temporary delta on the paused frame, reusing its media sample.
  /// Replaces the previous temporary transform. The original layout at the paused position is the base for each request.
  /// @param target Stable project clip or overlay identifier.
  /// @param delta Finite and invertible transform in normalized project canvas coordinates.
  /// @return Session-local request ID for matching the presented frame, or zero if no paused project is active.
  /// @throws std::invalid_argument If the delta is nonfinite or singular.
  std::uint64_t SetTransformDelta(EditTarget target, Affine2D delta);

  /// @brief Restores the authored layout at the current paused position.
  void ClearTransform();

  TimelinePreviewStatus Status() const;

  bool Closed() const;

  /// @brief Cancels and joins the worker. Call from outside the presentation callback.
  void Close();

private:
  friend void ExportProject(std::shared_ptr<const ProjectSnapshot>, const File&, const CodecProviders&,
                            const FileExportSettings&, const TimelineExportOptions&);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// @brief Resources, cancellation, and observations for one rendered export.
struct TimelineExportOptions {
  /// Shared scene configuration installed on every export attempt.
  SceneConfiguration scene;
  /// Receives provider switch diagnostics on the render sequence.
  OnCodecFallback on_fallback;
  /// Notifies the host of a prepared encoder frame's timeline position.
  std::function<void(media::MediaTimestamp)> on_frame;
  /// Observes the selected decoder or encoder video storage on the render sequence.
  std::function<void(CodecOperation, const media::VideoFrame&)> on_video_transfer;
  /// Cancellation shared by the timeline, renderer handoff, and codecs.
  std::stop_token stop;
  /// Optional scope for rendering each frame on the worker.
  TimelineIterationScope scope{};
  /// Active preview that yields the renderer until this export completes. Keep it alive for the call.
  TimelinePreviewSession* preview{};
};

/// @brief Exports one revision using the shared compositor and codec negotiation.
/// @param renderer Open renderer at the physical output size selected by settings.
/// @param project Immutable authored revision.
/// @param destination Local MP4 path separate from the source assets.
/// @param providers Candidate platform and software providers.
/// @param settings Output size, frame rate, and encoder configuration.
/// @param options Resources, reporting, and cancellation for the export session.
void ExportRenderedTimeline(TimelineRenderer& renderer, std::shared_ptr<const ProjectSnapshot> project,
    const File& destination, const CodecProviders& providers, const FileExportSettings& settings,
    const TimelineExportOptions& options = {});

/// @brief Opens the render target and exports a project with the same frame pipeline as preview.
/// @param project Immutable revision to render.
/// @param destination Output MP4 path.
/// @param providers Platform and software codec candidates.
/// @param settings Requested output dimensions, rate and codec policy.
/// @param options Scene configuration, progress, cancellation and optional preview handoff.
void ExportProject(std::shared_ptr<const ProjectSnapshot> project, const File& destination,
                   const CodecProviders& providers, const FileExportSettings& settings,
                   const TimelineExportOptions& options = {});

} // namespace render

} // namespace huxerui::avkit
