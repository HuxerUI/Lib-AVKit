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

/// @brief Decoded picture, versioned resource, or prepared color surface bound to an input slot.
using TimelineInput = std::variant<media::VideoFrame, ResourceRef, render::ColorSurface>;

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

/// @brief Compositor input for one exact project time.
struct TimelineFrame {
  media::MediaTimestamp pts;
  std::shared_ptr<const render::RenderPlan> plan;
  std::vector<TimelineInput> inputs; ///< External color slots referenced by the render plan.
  std::vector<TransitionMaskSample> masks; ///< Data slots resolved by the compositor.
  /// Analysis masks occupy the first data slots, followed by the transition masks.
  std::vector<render::ImageSurface> analysis_masks;
};

/// @brief Samples one immutable revision on a worker sequence.
/// Each instance owns its codec cursors, analysis generations, and ECS world.
class TimelineSession {
public:
  /// @brief Prepares project metadata and retains the selected providers.
  /// @param analysis Required for active clips with tracking anchors or portrait background replacement.
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
  /// @param mask_wait Per-job preview budget measured from submission. All visible mask jobs are submitted
  /// before waiting. A timeout uses that clip's original image. Omit for offline sampling.
  /// @return Immutable plan and retained inputs. Required analysis is sampled at its exact source PTS.
  TimelineFrame SampleVideo(media::MediaTimestamp at, render::PixelFormat output = render::PixelFormat::Bgra8Unorm,
                            std::stop_token stop = {}, const ImportVideoFrame& import = {},
                            std::optional<std::chrono::milliseconds> mask_wait = {});
  /// @brief Returns the PCM format shared by the timeline's selected audio clips.
  media::AudioFormat PcmFormat() const;
  /// @brief Mixes an interval on the project PCM sample grid, filling gaps with silence.
  /// @param start Interval start aligned to the sample rate.
  /// @param samples Positive number of sample frames.
  media::AudioFrame SampleAudio(media::MediaTimestamp start, std::uint32_t samples);
  /// @brief Starts a new sampling epoch at an in-range project time.
  /// Retires audio and analysis state. A reverse clip can reuse its own cached CPU pictures at the target.
  void Seek(media::MediaTimestamp at);
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
