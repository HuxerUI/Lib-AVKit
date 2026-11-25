#include "timeline_verify.h"

#include "metal_verify.h"
#include "movie_renderer.h"
#include "workbench_platform.h"

#include <chrono>
#include <utility>

namespace avkit_preview {
namespace {

using namespace std::chrono_literals;
using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::media;
using huxerui::macos::MetalTexture;

render::ColorSurface DrawTimelineSample(TimelineSession& session, MovieRenderer& renderer, MediaTimestamp at,
                                        std::stop_token stop) {
  const auto frame = session.SampleVideo(at, render::PixelFormat::Bgra8Unorm, stop,
                                         [&](const auto& video) { return renderer.ImportFrame(video); }, 12ms);
  return renderer.Draw(frame);
}

} // namespace

void RenderTimelineSample(TimelineSession& session, MovieRenderer& renderer, MediaTimestamp at, MetalTexture& mailbox,
                          std::stop_token stop) {
  PublishPreviewSurface(mailbox, DrawTimelineSample(session, renderer, at, stop));
}

TimelinePlaybackStats PlayTimeline(TimelineSession& session, TimeRange range, MovieRenderer& renderer,
                                   MetalTexture& mailbox, MediaTimestamp at, bool muted, const TimelineSeek& seek,
                                   std::stop_token stop, TimelinePreviewMode mode) {
  const auto stats = PlayClockedTimeline(session, range, at, muted, seek, stop, mode,
      [&](MediaTimestamp time, std::stop_token token) { return DrawTimelineSample(session, renderer, time, token); },
      [&](const render::ColorSurface& surface, MediaTimestamp) { PublishPreviewSurface(mailbox, surface); },
      RunWorkbenchRenderIteration);
  if (mode == TimelinePreviewMode::Interactive) mailbox.Finish();
  return stats;
}

void ExportComposedTimeline(std::shared_ptr<const ProjectSnapshot> project, const File& file,
                            const MovieCodecProviders& providers, MovieRenderer& renderer, CodecPolicy policy,
                            OnCodecFallback on_fallback, std::shared_ptr<analysis::AnalysisProvider> analyzer,
                            const WorkbenchExportFrame& on_frame) {
  FileExportSettings settings;
  settings.policy = policy;
  settings.video = PreviewVideoConfig(project->canvas);
  settings.video.frame_rate_numerator = kTimelinePreviewRate;
  ExportRenderedTimeline(std::move(project), file, providers, renderer.Timeline(), settings,
                         std::move(on_fallback), std::move(analyzer), on_frame);
}

void ExportTimelineFile(std::string_view path, std::string_view destination, bool software_only,
                           std::shared_ptr<analysis::AnalysisProvider> analyzer) {
  @autoreleasepool {
    auto project = Project::Open(File(path)).Snapshot();
    MovieRenderer renderer(project->canvas);
    ExportComposedTimeline(std::move(project), File(destination), TimelineProviders(), renderer,
                           software_only ? CodecPolicy::SoftwareOnly : CodecPolicy::Auto, {}, std::move(analyzer));
  }
}

} // namespace avkit_preview
