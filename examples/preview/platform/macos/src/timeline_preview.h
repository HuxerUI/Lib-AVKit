#pragma once

#include <memory>
#include <stop_token>
#include <string_view>

#include <huxerui/external_texture.h>

#if defined(__OBJC__)
#include <huxerui/avkit/timeline.h>
#include <huxerui/macos/external_texture.h>
#endif

namespace huxerui::avkit { struct ProjectSnapshot; }
namespace huxerui::avkit::analysis { class AnalysisProvider; }

namespace avkit_preview {

void ConfigureTimelinePreview(std::string_view path, std::string_view seconds,
                              std::shared_ptr<huxerui::avkit::analysis::AnalysisProvider> analysis = {});
bool HasTimelinePreview();
std::shared_ptr<huxerui::ExternalTexture> CreateTimelinePreview();
void SeekTimelinePreview(bool midpoint);
void ReplaceTimelinePreviewProject(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project);
void StopTimelinePreview();
void ExportTimelinePreview(std::string_view project, std::string_view destination, bool software_only);
void VerifyTimelinePreview(std::string_view first, std::string_view second, std::string_view directory);
void VerifyTimelineCodecs(std::string_view project, std::string_view directory);
void VerifyTimelineTransition(std::string_view first, std::string_view second, std::string_view directory);

#if defined(__OBJC__)
class MovieRenderer;
inline constexpr std::uint32_t kTimelinePreviewRate = 24;
using TimelineSeek = std::function<std::optional<huxerui::media::MediaTimestamp>(huxerui::media::MediaTimestamp)>;

struct TimelinePlaybackStats {
  std::size_t clocked_frames{};
  double maximum_video_lag_ms{};
  std::uint64_t audio_underruns{};
};

huxerui::avkit::MovieCodecProviders TimelineProviders();
void RenderTimelineSample(huxerui::avkit::TimelineSession& session, MovieRenderer& renderer,
                          huxerui::media::MediaTimestamp at, huxerui::macos::MetalTexture& mailbox,
                          std::stop_token stop = {});
TimelinePlaybackStats PlayTimeline(huxerui::avkit::TimelineSession& session, huxerui::avkit::TimeRange range,
                                   MovieRenderer& renderer, huxerui::macos::MetalTexture& mailbox,
                                   huxerui::media::MediaTimestamp at, bool muted, const TimelineSeek& seek,
                                   std::stop_token stop);
void ExportComposedTimeline(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project, const huxerui::File& file,
                            const huxerui::avkit::MovieCodecProviders& providers, MovieRenderer& renderer,
                            huxerui::avkit::CodecPolicy policy, huxerui::avkit::OnCodecFallback on_fallback = {});
#endif

} // namespace avkit_preview
