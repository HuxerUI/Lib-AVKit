#pragma once

#include <functional>
#include <stop_token>

#include <huxerui/avkit/timeline.h>

namespace avkit_preview {

inline constexpr std::uint32_t kTimelinePreviewRate = 24;
enum class TimelinePreviewMode { Interactive, Once };

struct TimelinePlaybackStats {
  std::size_t clocked_frames{};
  std::size_t late_video_frames{};
  double maximum_video_lag_ms{};
  std::uint64_t audio_underruns{};
};

using TimelineSeek = std::function<std::optional<huxerui::media::MediaTimestamp>(huxerui::media::MediaTimestamp)>;
using DrawTimelinePicture = std::function<huxerui::avkit::render::ColorSurface(huxerui::media::MediaTimestamp, std::stop_token)>;
using PublishTimelinePicture = std::function<void(const huxerui::avkit::render::ColorSurface&, huxerui::media::MediaTimestamp)>;
using TimelineIterationScope = bool (*)(const std::function<bool()>&);

TimelinePlaybackStats PlayClockedTimeline(huxerui::avkit::TimelineSession& session, huxerui::avkit::TimeRange range,
    huxerui::media::MediaTimestamp at, bool muted, const TimelineSeek& seek, std::stop_token stop,
    TimelinePreviewMode mode, const DrawTimelinePicture& draw, const PublishTimelinePicture& publish,
    TimelineIterationScope scope = nullptr);

} // namespace avkit_preview
