#pragma once

/// @file
/// @brief Exact sampling of immutable timeline revisions for preview and MP4 export.
///
/// Link AVKit::Media. The session works in project time and retains the decoded pictures and render
/// plan needed by a compositor. Rendering and resource resolution belong to the caller.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <variant>
#include <vector>

#include <huxerui/avkit/file_movie.h>
#include <huxerui/avkit/project.h>

namespace huxerui::avkit {

/// @brief Inspects a supported MP4 or MOV and returns its importable source tracks and ranges.
AssetReference InspectTimelineMovie(const File& file);

/// @brief Animated sticker instance sampled at a clip-local project time.
struct AnimatedStickerSample {
  ResourceRef resource;
  media::MediaTimestamp elapsed;
  AnimationPlayback playback;
  InstanceId instance;
  StickerReplacements replacements;
  bool operator==(const AnimatedStickerSample&) const = default;
};

/// @brief Decoded picture, sticker resource sample, or prepared color surface bound to an input slot.
using TimelineInput = std::variant<media::VideoFrame, ResourceRef, render::ColorSurface, AnimatedStickerSample>;

/// @brief Prepares a decoded input on the compositor sequence.
/// Throw PlatformVideoImportError when a platform video texture cannot be shared with the compositor.
using ImportVideoFrame = std::function<render::ImportedVideoFrame(const media::VideoFrame&)>;

/// @brief Mask resource sampled at a transition-local time.
struct TransitionMaskSample {
  ResourceRef resource;
  media::MediaTimestamp offset, duration;
  /// @brief Selects a frame in a uniformly timed sequence across the half-open transition interval.
  /// @param frame_count Number of available mask frames. Must be positive.
  /// @return Zero-based mask frame index.
  std::uint32_t FrameIndex(std::uint32_t frame_count) const;
  bool operator==(const TransitionMaskSample&) const = default;
};

/// @brief Two completed clip subcompositions to supply to an authored transition at the same project time.
/// The compositor resolves the animation in place of the final draw's outgoing subcomposition slot.
struct TransitionAnimationSample {
  ResourceRef resource;
  InstanceId instance;
  media::MediaTimestamp offset, duration;
  std::uint32_t outgoing_pass, incoming_pass; ///< Indices in RenderPlan::subcompositions.
};

/// @brief Coverage sequence sampled at an elapsed time on the owning clip's project clock.
struct AnimatedMaskSample {
  ResourceRef resource;
  media::MediaTimestamp offset;
  MaskPlayback playback{MaskPlayback::HoldLast};
  /// @brief Selects the frame using the loaded resource's positive frame interval and count.
  std::uint32_t FrameIndex(media::MediaTimestamp frame_interval, std::uint32_t frame_count) const;
  bool operator==(const AnimatedMaskSample&) const = default;
};

/// @brief Compositor input for one exact project time.
struct TimelineFrame {
  media::MediaTimestamp pts;
  std::shared_ptr<const render::RenderPlan> plan;
  std::vector<TimelineInput> inputs; ///< External color slots referenced by the render plan.
  std::vector<TransitionMaskSample> masks; ///< Animated transition masks resolved by the compositor.
  /// Data slots contain analysis masks, depth maps, static masks, clip animations, and transition masks in order.
  std::vector<render::ImageSurface> analysis_masks;
  std::vector<ResourceRef> static_masks; ///< Unique static mask resources in slot order.
  std::vector<AnimatedMaskSample> animated_masks; ///< Clip-local mask samples in slot order.
  std::vector<render::ImageSurface> depth_maps; ///< Metric depth textures in slot order.
  std::vector<TransitionAnimationSample> transition_animations;
};

/// @brief Samples one immutable revision on a worker sequence.
/// Each instance owns its codec cursors, analysis generations, and ECS world.
class TimelineSession {
public:
  /// @brief Prepares project metadata and retains the selected providers.
  /// @param analysis Required for tracking anchors and backgrounds that use portrait analysis.
  TimelineSession(std::shared_ptr<const ProjectSnapshot> project, MovieCodecProviders providers,
                  CodecPolicy policy = CodecPolicy::Auto, OnCodecFallback on_fallback = {},
                  std::shared_ptr<analysis::AnalysisProvider> analysis = {});
  ~TimelineSession();
  TimelineSession(const TimelineSession&) = delete;
  TimelineSession& operator=(const TimelineSession&) = delete;

  /// @brief Installs a new revision and retires the old cursors and analysis generations.
  void ApplyProject(std::shared_ptr<const ProjectSnapshot> project);
  /// @brief Samples one picture in the half-open project interval.
  /// @param at Exact time on the project clock.
  /// @param output Requested color format for the render target.
  /// @param stop Cancels pending decode and analysis waits. Cancellation retires sampling state.
  /// @param import Optionally prepares each visible video input. A rejected platform frame is retried from its
  /// exact source PTS on the software provider for that clip. When omitted, the compositor must fit decoded
  /// inputs to the project canvas with ImageFit::Contain. The plan uses the corresponding UV mapping.
  /// @param analysis_wait Per-job mask or depth budget measured from submission. All visible jobs are submitted
  /// before waiting. A timeout keeps unmasked video or hides depth-bound stickers on the affected clip. Omit for offline sampling.
  /// @return Immutable plan and retained inputs. Required analysis is sampled at its exact source PTS.
  TimelineFrame SampleVideo(media::MediaTimestamp at, render::PixelFormat output = render::PixelFormat::Bgra8Unorm,
                            std::stop_token stop = {}, const ImportVideoFrame& import = {},
                            std::optional<std::chrono::milliseconds> analysis_wait = {});
  /// @brief Requests up to two video entries strictly inside the lookahead, ordered by entry time.
  /// Selects reverse clips and trimmed forward clips that enter outside a transition while another picture remains visible.
  /// Call on the sampling sequence. Providers must support concurrent Probe and Open for independent sessions.
  /// Forward cursors must negotiate sequence handoff. Browser workers use the software provider.
  /// Returns without waiting for decode. Sampling decodes on demand if preparation is unavailable.
  /// @param lookahead In-range playback start and exclusive end on the project clock.
  /// @param stop Cancels the pending decode or discards its uncollected result.
  /// @return True if at least one entry was selected and every selected entry is cached.
  bool PrerollVideoEntries(TimeRange lookahead, std::stop_token stop = {});
  /// @brief Returns the PCM format shared by the timeline's selected audio clips.
  media::AudioFormat PcmFormat() const;
  /// @brief Mixes an interval on the project PCM sample grid, filling gaps with silence.
  /// @param start Interval start aligned to the sample rate.
  /// @param samples Positive number of sample frames.
  media::AudioFrame SampleAudio(media::MediaTimestamp start, std::uint32_t samples);
  /// @brief Starts a new sampling epoch at an in-range project time.
  /// Retires audio and analysis state. Keeps reusable video at the target.
  /// @param preroll_end Optional exclusive end of the new lookahead on the project clock. Keeps up to two prepared
  /// future entries, including live forward cursors, strictly between at and this end.
  void Seek(media::MediaTimestamp at, std::optional<media::MediaTimestamp> preroll_end = {});
  /// @brief Retires all sampling cursors and analysis state.
  void Reset();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// @brief Produces one encoder-ready frame on the sampled project's timestamp and duration.
using ComposeTimelineFrame = std::function<media::VideoFrame(const TimelineFrame&, const media::VideoEncoderConfig&,
                                                            media::MediaTimestamp duration)>;

/// @brief Samples the project on the export video and PCM grids and publishes a finalized MP4.
/// @param destination New local path separate from project sources.
/// @param compose Compositor callback. A recoverable platform encoder failure can replay it from the start.
/// @param analysis Provider for tracked stickers and portrait background replacement required by the project.
/// @param import Optionally prepares video inputs with the same per-clip software retry as preview.
void ExportTimeline(std::shared_ptr<const ProjectSnapshot> project, const File& destination,
                    const MovieCodecProviders& providers, const FileExportSettings& settings,
                    const ComposeTimelineFrame& compose, OnCodecFallback on_fallback = {},
                    std::shared_ptr<analysis::AnalysisProvider> analysis = {}, const ImportVideoFrame& import = {});

} // namespace huxerui::avkit
