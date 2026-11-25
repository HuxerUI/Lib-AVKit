#include "workbench_player.h"

#include "timeline_playback.h"
#include "timeline_resources.h"
#include "workbench_platform.h"
#include "workbench_render.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#include <huxerui/avkit/bgfx.h>
#include <huxerui/avkit/video_texture.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using namespace std::chrono_literals;
using media::MediaTimestamp;

double Seconds(MediaTimestamp time) { return double(time.ticks) / time.timescale; }

MediaTimestamp LastFrame(MediaTimestamp end) {
  auto last = end.Rescale(kTimelinePreviewRate).value();
  if (last.Compare(end) >= 0) --last.ticks;
  return last;
}

std::unique_ptr<TimelineRenderer> OpenWorkbenchRenderer(PixelSize size, const OverlayAssets& assets) {
  const ImageDesc target{size, PixelFormat::Rgba8Unorm};
  return std::make_unique<TimelineRenderer>(target, CreateBgfxBackend(target), PreviewTimelineResources(assets));
}

std::unique_ptr<BgfxPreviewTexture> OpenWorkbenchPreview(PixelSize size) {
  auto result = OpenBgfxPreviewTexture(size);
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return std::move(result).Value();
}

class VideoTransferLog {
public:
  ImportedVideoFrame Import(TimelineRenderer& renderer, const media::VideoFrame& input) {
    auto frame = renderer.ImportFrame(input);
    const bool platform = input.Storage() == media::VideoStorage::Platform;
    if (!std::exchange(imported_[platform], true)) {
      LogWorkbench(WorkbenchLogLevel::Info, "%s", platform ? "Imported a decoder frame on the GPU" :
                   "Uploaded an FFmpeg decoder frame");
    }
    return frame;
  }

  void Encoded(const media::VideoFrame& frame) {
    const bool platform = frame.Storage() == media::VideoStorage::Platform;
    if (!std::exchange(encoded_[platform], true)) {
      LogWorkbench(WorkbenchLogLevel::Info, "%s at PTS %.6f",
          platform ? "Submitted a GPU texture to the platform encoder" : "Prepared CPU I420 for the FFmpeg encoder",
          Seconds(frame.pts));
    }
  }

private:
  std::array<bool, 2> imported_{};
  std::array<bool, 2> encoded_{};
};

void ReportFallback(const CodecFallback& fallback) {
  constexpr std::array operations{"Video decode", "Audio decode", "Video encode", "Audio encode"};
  LogWorkbench(WorkbenchLogLevel::Warning, "%s, %s to %s, PTS %.3f: %s [%s]",
      operations[static_cast<std::size_t>(fallback.operation)], fallback.from.c_str(), fallback.to.c_str(),
      fallback.resume_at ? Seconds(*fallback.resume_at) : -1, fallback.cause.message.c_str(),
      fallback.cause.platform_code.c_str());
}

} // namespace

struct WorkbenchPlayer::Impl {
  struct Request {
    std::shared_ptr<const ProjectSnapshot> project;
    double seconds;
    bool play;
    std::optional<WorkbenchDrag> drag;
    std::stop_source cancel;
  };

  Impl(PixelSize size, OverlayAssets resources, Present present)
      : canvas(size), assets(std::move(resources)), preview(OpenWorkbenchPreview(size)), on_frame(std::move(present)),
        worker([this](std::stop_token stop) { Run(stop); }) {}

  ~Impl() { Close(); }

  void Close() {
    std::stop_source current(std::nostopstate);
    {
      std::scoped_lock lock(mutex);
      if (closed) return;
      closed = true;
      current = cancel;
    }
    worker.request_stop();
    current.request_stop();
    ready.notify_all();
    worker.join();
  }

  void Run(std::stop_token stop) {
    RunWorkbenchRenderIteration([&] {
      std::optional<WorkbenchRenderLease> lease;
      std::unique_ptr<TimelineRenderer> renderer;
      std::unique_ptr<TimelineSession> timeline;
      std::shared_ptr<const ProjectSnapshot> active;
      std::optional<TimelineFrame> retained;
      VideoTransferLog transfer;
      while (RunWorkbenchRenderIteration([&] {
        std::unique_lock lock(mutex);
        if (!ready.wait(lock, stop, [&] { return pending.has_value(); }) || stop.stop_requested()) return false;
        auto request = std::move(*pending);
        pending.reset();
        lock.unlock();
        const auto token = request.cancel.get_token();
        try {
          if (token.stop_requested()) return true;
          const auto range = request.project->Range();
          if (range.end.ticks <= 0) return true;
          if (!renderer) {
            if (!lease.emplace(stop)) return false;
            renderer = OpenWorkbenchRenderer(canvas, assets);
          }
          if (!timeline) timeline = std::make_unique<TimelineSession>(request.project, TimelineProviders(),
                                                                     CodecPolicy::Auto, ReportFallback);
          else if (active != request.project) timeline->ApplyProject(request.project);
          if (active != request.project) retained.reset();
          active = request.project;
          const auto last = LastFrame(range.end);
          const MediaTimestamp at{std::clamp<std::int64_t>(std::llround(request.seconds * kTimelinePreviewRate),
                                                         0, last.ticks), kTimelinePreviewRate};
          const auto sample = [&](MediaTimestamp time, std::stop_token cancel_token) {
            return timeline->SampleVideo(time, renderer->Format(), cancel_token,
                [&](const media::VideoFrame& video) { return transfer.Import(*renderer, video); }, 12ms);
          };
          const auto draw = [&](MediaTimestamp time, std::stop_token cancel_token) {
            return renderer->Draw(sample(time, cancel_token));
          };
          bool announced_playback = false;
          const auto publish = [&](const ColorSurface& surface, MediaTimestamp time, bool playing) {
            if (token.stop_requested()) return;
            PreviewFrameCommit commit;
            if (!playing) {
              auto prepared = preview->Prepare(renderer->Backend(), surface);
              if (!prepared.Succeeded()) throw std::runtime_error(prepared.Error().message);
              commit = std::move(prepared).Value();
            }
            if (token.stop_requested()) return;
            if (!commit) {
              const auto result = preview->Publish(renderer->Backend(), surface);
              if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
            }
            const auto seconds = Seconds(time);
            {
              std::scoped_lock status_lock(mutex);
              if (token.stop_requested()) return;
              status = {seconds, playing, true, {}};
            }
            if (!playing || !std::exchange(announced_playback, true))
              on_frame({preview->Texture(), request.project, request.drag, seconds, playing}, token, std::move(commit));
            if (!playing) LogWorkbench(WorkbenchLogLevel::Info, "Preview positioned at %.3f seconds", seconds);
          };

          if (request.play && at.Compare(last) == -1) {
            retained.reset();
            timeline->Seek(at);
            const auto stats = PlayClockedTimeline(*timeline, range, at, false, [](MediaTimestamp) { return std::nullopt; },
                token, TimelinePreviewMode::Once, draw,
                [&](const ColorSurface& surface, MediaTimestamp time) { publish(surface, time, true); },
                RunWorkbenchRenderIteration);
            LogWorkbench(WorkbenchLogLevel::Info,
                "Playback rendered %zu clocked frames, %zu late, %.1f ms maximum video lag, %llu PCM underruns",
                stats.clocked_frames, stats.late_video_frames, stats.maximum_video_lag_ms,
                static_cast<unsigned long long>(stats.audio_underruns));
            if (!token.stop_requested()) publish(draw(last, token), last, false);
          } else {
            if (!retained || retained->pts != at) {
              timeline->Seek(at);
              retained = sample(at, token);
            }
            auto frame = *retained;
            if (request.drag) frame.plan = timeline->RecomposeVideo(*retained, request.drag->layout);
            if (!token.stop_requested()) publish(renderer->Draw(frame), at, false);
          }
        } catch (const std::exception& error) {
          if (!renderer) lease.reset();
          std::scoped_lock status_lock(mutex);
          if (!token.stop_requested()) {
            status = {request.seconds, false, false, error.what()};
            LogWorkbench(WorkbenchLogLevel::Error, "%s", error.what());
          }
        }
        return true;
      })) {}
      preview->Finish();
      return false;
    });
  }

  PixelSize canvas;
  OverlayAssets assets;
  std::unique_ptr<BgfxPreviewTexture> preview;
  Present on_frame;
  mutable std::mutex mutex;
  std::condition_variable_any ready;
  std::optional<Request> pending;
  std::stop_source cancel;
  WorkbenchPlayback status;
  bool manipulating{};
  bool closed{};
  std::jthread worker;
};

WorkbenchPlayer::WorkbenchPlayer(PixelSize canvas, OverlayAssets assets, Present present)
    : impl_(std::make_unique<Impl>(canvas, std::move(assets), std::move(present))) {}
WorkbenchPlayer::~WorkbenchPlayer() = default;

WorkbenchPlayback WorkbenchPlayer::Status() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->status;
}

bool WorkbenchPlayer::Closed() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->closed;
}

void WorkbenchPlayer::Close() { impl_->Close(); }

void WorkbenchPlayer::Show(std::shared_ptr<const ProjectSnapshot> revision, double seconds, bool play,
                           std::optional<WorkbenchDrag> drag) {
  std::scoped_lock lock(impl_->mutex);
  if (impl_->closed) return;
  if (!drag || !impl_->manipulating) {
    impl_->cancel.request_stop();
    impl_->cancel = std::stop_source{};
    impl_->status = {seconds, play, false, {}};
  }
  impl_->manipulating = drag.has_value();
  impl_->pending = Impl::Request{std::move(revision), seconds, play, std::move(drag), impl_->cancel};
  impl_->ready.notify_all();
}

void ExportRenderedTimeline(std::shared_ptr<const ProjectSnapshot> project, const File& file,
    const MovieCodecProviders& providers, TimelineRenderer& renderer, const FileExportSettings& settings,
    OnCodecFallback on_fallback, std::shared_ptr<analysis::AnalysisProvider> analysis,
    const WorkbenchExportFrame& on_frame) {
  VideoTransferLog transfer;
  ExportTimeline(std::move(project), file, providers, settings,
      [&](const TimelineFrame& frame, const media::VideoEncoderConfig& bound, MediaTimestamp duration) {
        std::optional<media::VideoFrame> picture;
        RunWorkbenchRenderIteration([&] {
          if (on_frame) on_frame(frame.pts);
          picture = renderer.EncoderFrame(renderer.Draw(frame), bound, frame.pts, duration);
          transfer.Encoded(*picture);
          return false;
        });
        return std::move(*picture);
      }, std::move(on_fallback), std::move(analysis),
      [&](const auto& video) { return transfer.Import(renderer, video); }, renderer.Format());
}

void ExportWorkbenchMovie(std::shared_ptr<const ProjectSnapshot> revision, const File& file, const OverlayAssets& assets,
                          const WorkbenchExportFrame& on_frame) {
  RunWorkbenchRenderIteration([&] {
    const WorkbenchRenderLease lease;
    auto renderer = OpenWorkbenchRenderer(revision->canvas, assets);
    auto settings = FileExportSettings::Sdr(revision->canvas);
    settings.video.frame_rate_numerator = kTimelinePreviewRate;
    ExportRenderedTimeline(std::move(revision), file, TimelineProviders(), *renderer, settings, ReportFallback, {}, on_frame);
    LogWorkbench(WorkbenchLogLevel::Info, "Finalized timeline MP4");
    return false;
  });
}

} // namespace avkit_preview
