#pragma once

/// @file
/// @brief Exact sampling of immutable timeline revisions for preview and MP4 export.
///
/// Link AVKit::Media. The session works in project time and retains the decoded pictures and render
/// plan needed by a compositor. Rendering and resource resolution belong to the caller.

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

/// @brief Retained picture or versioned resource bound to a color input slot.
using TimelineInput = std::variant<media::VideoFrame, ResourceRef>;

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
};

/// @brief Samples one immutable revision on a worker sequence.
/// Each instance owns its codec cursors, analysis generations, and ECS world.
class TimelineSession {
public:
  /// @brief Prepares project metadata and retains the selected providers.
  /// @param analysis Required for active clips with tracking anchors.
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
  /// @return Immutable plan and retained inputs. Required analysis is sampled at its exact source PTS.
  TimelineFrame SampleVideo(media::MediaTimestamp at, render::ImageFormat output = render::ImageFormat::Bgra8Unorm,
                            std::stop_token stop = {});
  /// @brief Returns the PCM format shared by the timeline's selected audio clips.
  media::AudioFormat PcmFormat() const;
  /// @brief Mixes an interval on the project PCM sample grid, filling gaps with silence.
  /// @param start Interval start aligned to the sample rate.
  /// @param samples Positive number of sample frames.
  media::AudioFrame SampleAudio(media::MediaTimestamp start, std::uint32_t samples);
  /// @brief Retires all sampling cursors and analysis state before a discontinuous seek.
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
/// @param analysis Provider for tracked stickers when required by the project.
void ExportTimeline(std::shared_ptr<const ProjectSnapshot> project, const File& destination,
                    const MovieCodecProviders& providers, const FileExportSettings& settings,
                    const ComposeTimelineFrame& compose, OnCodecFallback on_fallback = {},
                    std::shared_ptr<analysis::AnalysisProvider> analysis = {});

} // namespace huxerui::avkit
