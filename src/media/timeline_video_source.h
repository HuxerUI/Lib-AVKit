#pragma once

#include <huxerui/avkit/component.h>
#include <huxerui/avkit/timeline.h>

#include "analysis/tracking.h"

namespace huxerui::avkit::detail {

struct VideoClipSource {
  /// Scene entity whose video cursor owns the decoded frames.
  entityx::Entity clip;
  /// Imported asset retained by the owning timeline revision.
  const MovieAsset* movie;
  /// Mapping from project time to source time, including playback direction.
  ClipTiming placement;
  /// Derived project interval that includes any transition overlap.
  ClipPresentationRange visibility;
};

inline constexpr std::size_t kVideoPrerollLimit = 2;

/// Owns the video cursors and their preroll workers for a timeline revision.
class TimelineVideoSource {
public:
  /// @brief Creates decoder state for one revision on its sampling sequence.
  /// @param providers Candidate video decoders. Keep them alive through destruction.
  /// @param policy Selects the allowed providers.
  /// @param report Optional fallback callback. Keep it alive through destruction.
  /// @param device Render device used for hardware import. An empty pointer selects the software decoder.
  TimelineVideoSource(const CodecProviders& providers, CodecPolicy policy, const OnCodecFallback& report,
                      render::RenderBackend* device);

  ~TimelineVideoSource();

  /// @brief Installs the video sources projected into the current ECS world.
  /// @param sources Clip bindings and stable movie metadata for the revision.
  /// @param events Scene event manager used to notify systems when a source is invalidated.
  void SetSources(std::span<const VideoClipSource> sources, entityx::EventManager& events);

  /// @brief Checks whether a clip already has a reusable entry picture.
  /// @param clip Entity of the video clip.
  bool HasPrepared(entityx::Entity clip);

  /// @brief Releases inactive cursors while preserving candidates reserved for preroll.
  /// @param at Current project timestamp.
  /// @param events Receives source invalidation events when a cursor is released.
  void PrepareFrame(media::MediaTimestamp at, entityx::EventManager& events);

  /// @brief Samples and imports a picture, retrying via software at the same PTS when GPU import fails.
  /// @param clip Entity whose decoder state is used.
  /// @param source Timestamp in the original media clock.
  /// @param target Physical composition extent used for frame fitting.
  /// @param stop Cancels synchronous decode work.
  /// @return Borrowed picture also published on the clip, or empty when no picture covers the requested time.
  const media::VideoFrame* Sample(entityx::Entity clip, media::MediaTimestamp source,
                                  PixelSize target, std::stop_token stop);

  /// @brief Provides an independent source-time reader for analysis history and lookahead.
  /// @param clip Entity owning the analysis scope.
  /// @param image Selected picture at the analysis target time. Keep it alive while using the returned input.
  /// @param stop Cancels reads needed to build the analysis input.
  std::optional<SourceTracking::Input> TrackingInput(entityx::Entity clip, const media::VideoFrame& image,
                                                     std::stop_token stop);

  /// @brief Schedules bounded background decoding for upcoming clip entries.
  /// @param start Current position in project time.
  /// @param entries At most kVideoPrerollLimit clip entities selected for preparation.
  /// @param stop Cancellation shared with submitted work.
  /// @return Whether all selected entry pictures are already reusable.
  bool Preroll(media::MediaTimestamp start, std::span<const entityx::Entity> entries, std::stop_token stop);

  /// @brief Cancels queued preroll and discards cursors that cannot be reused across a seek.
  /// @param seek New project time, or empty to release cursors outside the retained entries.
  /// @param entries Clip entities chosen for the next preroll window.
  /// @param events Receives invalidation for each clip so analysis generations can advance.
  void Reset(std::optional<media::MediaTimestamp> seek, std::span<const entityx::Entity> entries,
             entityx::EventManager& events);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::avkit::detail
