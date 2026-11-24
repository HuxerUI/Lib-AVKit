#include "workbench_player.h"
#include "workbench_render.h"

#include "metal_preview.h"
#include "movie_renderer.h"
#include "timeline_preview.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using media::MediaTimestamp;
using huxerui::macos::MetalTexture;

double Seconds(MediaTimestamp time) { return double(time.ticks) / time.timescale; }

MediaTimestamp LastFrame(MediaTimestamp end) {
  auto last = end.Rescale(kTimelinePreviewRate).value();
  if (last.Compare(end) >= 0) --last.ticks;
  return last;
}

} // namespace

struct WorkbenchPlayer::Impl {
  struct Request {
    std::shared_ptr<const ProjectSnapshot> project;
    double seconds;
    bool play;
    std::optional<VisualTransformOverride> layout;
    std::stop_source cancel;
  };

  explicit Impl(PixelSize size)
      : canvas(size), texture(std::make_shared<MetalTexture>(huxerui::Size{float(size.width), float(size.height)})),
        worker([this](std::stop_token stop) { Run(stop); }) {}

  ~Impl() { Close(); }

  void Close() {
    if (!worker.joinable()) return;
    worker.request_stop();
    cancel.request_stop();
    ready.notify_all();
    worker.join();
  }

  void Run(std::stop_token stop) {
    @autoreleasepool {
      std::optional<WorkbenchRenderLease> lease;
      std::unique_ptr<MovieRenderer> renderer;
      std::unique_ptr<TimelineSession> timeline;
      std::shared_ptr<const ProjectSnapshot> active;
      std::optional<TimelineFrame> retained;
      for (;;) {
        @autoreleasepool {
          std::unique_lock lock(mutex);
          if (!ready.wait(lock, stop, [&] { return pending.has_value(); })) break;
          auto request = std::move(*pending);
          pending.reset();
          lock.unlock();
          try {
            if (request.cancel.stop_requested()) continue;
            const auto range = request.project->Range();
            if (range.end.ticks <= 0) continue;
            if (!renderer) {
              if (!lease.emplace(stop)) break;
              renderer = std::make_unique<MovieRenderer>(canvas);
            }
            if (!timeline) timeline = std::make_unique<TimelineSession>(request.project, TimelineProviders());
            else if (active != request.project) timeline->ApplyProject(request.project);
            if (active != request.project) retained.reset();
            active = request.project;
            const auto last = LastFrame(range.end);
            const auto at = MediaTimestamp{std::clamp<std::int64_t>(
                std::llround(request.seconds * kTimelinePreviewRate), 0, last.ticks), kTimelinePreviewRate};
            const auto publish = [&](MediaTimestamp displayed, bool playing) {
              std::scoped_lock status_lock(mutex);
              if (!request.cancel.stop_requested()) status = {Seconds(displayed), playing, true, {}};
            };
            if (request.play && at.Compare(last) == -1) {
              retained.reset();
              timeline->Seek(at);
              PlayTimeline(*timeline, range, *renderer, *texture, at, false,
                  [&](MediaTimestamp displayed) -> std::optional<MediaTimestamp> {
                    publish(displayed, true);
                    return {};
                  }, request.cancel.get_token(), TimelinePreviewMode::Once);
              if (!request.cancel.stop_requested()) {
                RenderTimelineSample(*timeline, *renderer, last, *texture, request.cancel.get_token());
                publish(last, false);
              }
            } else {
              if (!retained || retained->pts != at) {
                timeline->Seek(at);
                retained = timeline->SampleVideo(at, render::PixelFormat::Bgra8Unorm, request.cancel.get_token(),
                    [&](const auto& video) { return renderer->ImportFrame(video); }, std::chrono::milliseconds(12));
              }
              auto frame = *retained;
              if (request.layout) frame.plan = timeline->RecomposeVideo(*retained, *request.layout);
              if (!request.cancel.stop_requested()) PublishPreviewSurface(*texture, renderer->Draw(frame));
              publish(at, false);
            }
          } catch (const std::exception& error) {
            if (!renderer) lease.reset();
            std::scoped_lock status_lock(mutex);
            if (!request.cancel.stop_requested()) status = {request.seconds, false, false, error.what()};
          }
        }
      }
      texture->Finish();
    }
  }

  PixelSize canvas;
  std::shared_ptr<MetalTexture> texture;
  mutable std::mutex mutex;
  std::condition_variable_any ready;
  std::optional<Request> pending;
  std::stop_source cancel;
  WorkbenchPlayback status;
  bool manipulating{};
  std::jthread worker;
};

WorkbenchPlayer::WorkbenchPlayer(PixelSize canvas, OverlayAssets) : impl_(std::make_unique<Impl>(canvas)) {}
WorkbenchPlayer::~WorkbenchPlayer() = default;
std::shared_ptr<ExternalTexture> WorkbenchPlayer::Texture() const { return impl_->texture; }

WorkbenchPlayback WorkbenchPlayer::Status() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->status;
}

bool WorkbenchPlayer::Closed() const { return !impl_->worker.joinable(); }
void WorkbenchPlayer::Close() { impl_->Close(); }

void WorkbenchPlayer::Show(std::shared_ptr<const ProjectSnapshot> revision, double seconds, bool play,
                           std::optional<VisualTransformOverride> layout) {
  if (Closed()) return;
  std::scoped_lock lock(impl_->mutex);
  if (!layout || !impl_->manipulating) {
    impl_->cancel.request_stop();
    impl_->cancel = std::stop_source{};
    impl_->status = {seconds, play, false, {}};
  }
  impl_->manipulating = layout.has_value();
  impl_->pending = Impl::Request{std::move(revision), seconds, play, std::move(layout), impl_->cancel};
  impl_->ready.notify_all();
}

void ExportWorkbenchMovie(std::shared_ptr<const ProjectSnapshot> revision, const File& file, const OverlayAssets&,
                          const WorkbenchExportFrame& on_frame) {
  @autoreleasepool {
    const WorkbenchRenderLease lease;
    MovieRenderer renderer(revision->canvas);
    ExportComposedTimeline(std::move(revision), file, TimelineProviders(), renderer, CodecPolicy::Auto, {}, {}, on_frame);
  }
}

} // namespace avkit_preview
