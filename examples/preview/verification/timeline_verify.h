#pragma once

#include <memory>
#include <stop_token>
#include <string_view>

#if defined(__OBJC__)
#include "timeline_playback.h"
#include "workbench_player.h"

#include <huxerui/macos/external_texture.h>
#endif

namespace huxerui::avkit { struct MovieCodecProviders; struct ProjectSnapshot; }
namespace huxerui::avkit::analysis { class AnalysisProvider; }

namespace avkit_preview {

void ExportTimelineFile(std::string_view project, std::string_view destination, bool software_only,
                           std::shared_ptr<huxerui::avkit::analysis::AnalysisProvider> analysis = {});
void VerifyTimelinePreview(std::string_view first, std::string_view second, std::string_view directory);
void VerifyTimelineCodecs(std::string_view project, std::string_view directory);
void VerifyTimelineTransition(std::string_view first, std::string_view second, std::string_view directory);
void VerifyTimelineStrip(std::string_view first, std::string_view replacement, std::string_view directory);

#if defined(__OBJC__)
class MovieRenderer;
void RenderTimelineSample(huxerui::avkit::TimelineSession& session, MovieRenderer& renderer,
                          huxerui::media::MediaTimestamp at, huxerui::macos::MetalTexture& mailbox,
                          std::stop_token stop = {});
TimelinePlaybackStats PlayTimeline(huxerui::avkit::TimelineSession& session, huxerui::avkit::TimeRange range,
                                   MovieRenderer& renderer, huxerui::macos::MetalTexture& mailbox,
                                   huxerui::media::MediaTimestamp at, bool muted, const TimelineSeek& seek,
                                   std::stop_token stop, TimelinePreviewMode mode = TimelinePreviewMode::Interactive);
void ExportComposedTimeline(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project, const huxerui::File& file,
                            const huxerui::avkit::MovieCodecProviders& providers, MovieRenderer& renderer,
                            huxerui::avkit::CodecPolicy policy, huxerui::avkit::OnCodecFallback on_fallback = {},
                            std::shared_ptr<huxerui::avkit::analysis::AnalysisProvider> analysis = {},
                            const WorkbenchExportFrame& on_frame = {});
#endif

} // namespace avkit_preview
