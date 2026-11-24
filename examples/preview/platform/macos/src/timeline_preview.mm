#include "timeline_preview.h"

#include "metal_preview.h"
#include "movie_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace avkit_preview {
namespace {

using namespace std::chrono_literals;
using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::media;
using huxerui::macos::MetalTexture;

struct PreviewOptions {
  std::shared_ptr<const ProjectSnapshot> project;
  MediaTimestamp start;
  std::shared_ptr<analysis::AnalysisProvider> analysis;
  CodecPolicy policy;
};

std::optional<PreviewOptions> preview;
std::mutex seek_mutex;
std::optional<bool> pending_seek;
std::shared_ptr<const ProjectSnapshot> pending_project;
std::jthread worker;

render::ColorSurface DrawTimelineSample(TimelineSession& session, MovieRenderer& renderer, MediaTimestamp at,
                                        std::stop_token stop) {
  const auto frame = session.SampleVideo(at, render::PixelFormat::Bgra8Unorm, stop,
                                         [&](const auto& video) { return renderer.ImportFrame(video); }, 12ms);
  return renderer.Draw(frame);
}

} // namespace

MovieCodecProviders TimelineProviders() { return {CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()}; }

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
      [](const std::function<bool()>& iteration) {
        @autoreleasepool { return iteration(); }
      });
  if (mode == TimelinePreviewMode::Interactive) mailbox.Finish();
  return stats;
}

void ConfigureTimelinePreview(std::string_view path, std::string_view seconds,
                              std::shared_ptr<analysis::AnalysisProvider> analysis, bool software_only) {
  auto project = Project::Open(File(path)).Snapshot();
  std::size_t length = 0;
  const auto value = std::stod(std::string(seconds), &length);
  const auto end = project->Range().end;
  if (length != seconds.size() || !std::isfinite(value) || value < 0 ||
      value >= double(end.ticks) / end.timescale || value >= double(std::numeric_limits<std::int64_t>::max()) / 1000000)
    throw std::invalid_argument("Preview start is outside the timeline");
  preview = PreviewOptions{std::move(project), {std::llround(value * 1000000), 1000000}, std::move(analysis),
                           software_only ? CodecPolicy::SoftwareOnly : CodecPolicy::Auto};
}

bool HasTimelinePreview() { return preview.has_value(); }

std::shared_ptr<ExternalTexture> CreateTimelinePreview() {
  if (!preview) return {};
  const auto options = *preview;
  const auto canvas = options.project->canvas;
  auto mailbox = std::make_shared<MetalTexture>(huxerui::Size{float(canvas.width), float(canvas.height)});
  worker = std::jthread([mailbox, options](std::stop_token stop) {
    @autoreleasepool {
      try {
        MovieRenderer renderer(options.project->canvas);
        TimelineSession session(options.project, TimelineProviders(), options.policy, {}, options.analysis);
        const auto range = options.project->Range();
        PlayTimeline(session, range, renderer, *mailbox, options.start, false, [&](MediaTimestamp presented) -> std::optional<MediaTimestamp> {
          std::optional<bool> command;
          std::shared_ptr<const ProjectSnapshot> replacement;
          {
            std::scoped_lock lock(seek_mutex);
            command = std::exchange(pending_seek, std::nullopt);
            replacement = std::exchange(pending_project, {});
          }
          if (replacement) session.ApplyProject(std::move(replacement));
          else if (!command) return std::nullopt;
          if (!command) return presented;
          const auto end = range.end.Rescale(session.PcmFormat().sample_rate).value();
          return MediaTimestamp{*command ? end.ticks / 2 : 0, end.timescale};
        }, stop);
      } catch (const std::exception& error) {
        if (!stop.stop_requested()) std::cerr << "Timeline preview: " << error.what() << '\n';
        mailbox->Finish();
      }
    }
  });
  return mailbox;
}

void SeekTimelinePreview(bool midpoint) {
  std::scoped_lock lock(seek_mutex);
  pending_seek = midpoint;
}

void ReplaceTimelinePreviewProject(std::shared_ptr<const ProjectSnapshot> project) {
  std::scoped_lock lock(seek_mutex);
  pending_project = std::move(project);
}

void StopTimelinePreview() {
  worker.request_stop();
  if (worker.joinable()) worker.join();
}

void ExportComposedTimeline(std::shared_ptr<const ProjectSnapshot> project, const File& file,
                            const MovieCodecProviders& providers, MovieRenderer& renderer, CodecPolicy policy,
                            OnCodecFallback on_fallback, std::shared_ptr<analysis::AnalysisProvider> analyzer,
                            const WorkbenchExportFrame& on_frame) {
  FileExportSettings settings;
  settings.policy = policy;
  settings.video = PreviewVideoConfig(project->canvas);
  settings.video.frame_rate_numerator = kTimelinePreviewRate;
  ExportTimeline(std::move(project), file, providers, settings,
      [&](const TimelineFrame& frame, const VideoEncoderConfig& bound, MediaTimestamp duration) {
        @autoreleasepool {
          if (on_frame) on_frame(frame.pts);
          return renderer.EncoderFrame(renderer.Draw(frame), bound, frame.pts, duration);
        }
      }, std::move(on_fallback), std::move(analyzer), [&](const auto& video) { return renderer.ImportFrame(video); });
}

void ExportTimelinePreview(std::string_view path, std::string_view destination, bool software_only,
                           std::shared_ptr<analysis::AnalysisProvider> analyzer) {
  @autoreleasepool {
    auto project = Project::Open(File(path)).Snapshot();
    MovieRenderer renderer(project->canvas);
    ExportComposedTimeline(std::move(project), File(destination), TimelineProviders(), renderer,
                           software_only ? CodecPolicy::SoftwareOnly : CodecPolicy::Auto, {}, std::move(analyzer));
  }
}

} // namespace avkit_preview
