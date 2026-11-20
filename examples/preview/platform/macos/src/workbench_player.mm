#include "workbench_player.h"

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
      std::unique_ptr<MovieRenderer> renderer;
      std::unique_ptr<TimelineSession> timeline;
      std::shared_ptr<const ProjectSnapshot> active;
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
            if (!renderer) renderer = std::make_unique<MovieRenderer>(canvas);
            if (!timeline) timeline = std::make_unique<TimelineSession>(request.project, TimelineProviders());
            else if (active != request.project) timeline->ApplyProject(request.project);
            active = request.project;
            const auto last = LastFrame(range.end);
            const auto at = MediaTimestamp{std::clamp<std::int64_t>(
                std::llround(request.seconds * kTimelinePreviewRate), 0, last.ticks), kTimelinePreviewRate};
            const auto publish = [&](MediaTimestamp displayed, bool playing) {
              std::scoped_lock status_lock(mutex);
              if (!request.cancel.stop_requested()) status = {Seconds(displayed), playing, true, {}};
            };
            timeline->Seek(at);
            if (request.play && at.Compare(last) == -1) {
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
              RenderTimelineSample(*timeline, *renderer, at, *texture, request.cancel.get_token());
              publish(at, false);
            }
          } catch (const std::exception& error) {
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
  std::jthread worker;
};

WorkbenchPlayer::WorkbenchPlayer(PixelSize canvas) : impl_(std::make_unique<Impl>(canvas)) {}
WorkbenchPlayer::~WorkbenchPlayer() = default;
std::shared_ptr<ExternalTexture> WorkbenchPlayer::Texture() const { return impl_->texture; }

WorkbenchPlayback WorkbenchPlayer::Status() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->status;
}

bool WorkbenchPlayer::Closed() const { return !impl_->worker.joinable(); }
void WorkbenchPlayer::Close() { impl_->Close(); }

void WorkbenchPlayer::Show(std::shared_ptr<const ProjectSnapshot> revision, double seconds, bool play) {
  if (Closed()) return;
  std::scoped_lock lock(impl_->mutex);
  impl_->cancel.request_stop();
  impl_->cancel = std::stop_source{};
  impl_->status = {seconds, play, false, {}};
  impl_->pending = Impl::Request{std::move(revision), seconds, play, impl_->cancel};
  impl_->ready.notify_all();
}

void ExportWorkbenchMovie(std::shared_ptr<const ProjectSnapshot> revision, const File& file) {
  @autoreleasepool {
    MovieRenderer renderer(revision->canvas);
    ExportComposedTimeline(std::move(revision), file, TimelineProviders(), renderer, CodecPolicy::Auto);
  }
}

} // namespace avkit_preview
