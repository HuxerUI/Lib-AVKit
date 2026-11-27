#include <huxerui/avkit/timeline.h>

#include "render/render_iteration.h"
#include "render/target_layout.h"
#include "support/geometry.h"
#include "support/media_time.h"
#include "support/cancellation.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace huxerui::avkit::render {

#if !defined(__APPLE__) || !TARGET_OS_OSX
bool detail::RunRenderIteration(const std::function<bool()>& work) { return work(); }
#endif

namespace {

using media::MediaTimestamp;
using namespace std::chrono_literals;

MediaTimestamp PreviewTime(MediaTimestamp at, MediaTimestamp last) {
  const auto ticks = at.Rescale(kTimelinePreviewRate);
  if (!ticks) throw std::invalid_argument("Invalid preview time");
  return {std::clamp(ticks->ticks, std::int64_t{0}, last.ticks), kTimelinePreviewRate};
}

std::unique_ptr<PreviewTexture> OpenTexture(PixelSize size) {
  auto result = OpenPreviewTexture(size);
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return std::move(result).Value();
}

} // namespace

std::optional<EditTarget> PreviewFrame::HitTest(Point point, const std::function<bool(EditTarget)>& accepts) const {
  if (!layouts_ || !Rect{0, 0, 1, 1}.Contains(point)) return std::nullopt;
  for (auto entry = layouts_->rbegin(); entry != layouts_->rend(); ++entry) {
    const auto& layout = entry->layout;
    if (layout.local_bounds.IsEmpty()) continue;
    const auto local = layout.transform.Inverse(point);
    if (local && layout.local_bounds.Contains(*local) && (!accepts || accepts(entry->target))) return entry->target;
  }
  return std::nullopt;
}

std::optional<TargetLayout> PreviewFrame::FindLayout(EditTarget target) const {
  if (!layouts_) return std::nullopt;
  const auto entry = std::ranges::find(*layouts_, target, &avkit::detail::TargetLayoutEntry::target);
  if (entry == layouts_->end()) return std::nullopt;
  return entry->layout;
}

struct TimelinePreviewSession::Impl {
  enum class TransformMode { Absolute, Delta };

  struct TransformRequest {
    EditTarget target;
    Affine2D transform;
    TransformMode mode;
    std::uint64_t id;

    std::optional<Affine2D> ResolveDelta(const std::vector<avkit::detail::TargetLayoutEntry>& layouts) const {
      if (mode == TransformMode::Delta) return transform;
      const auto base = std::ranges::find(layouts, target, &avkit::detail::TargetLayoutEntry::target);
      if (base == layouts.end()) return std::nullopt;
      if (transform == base->layout.transform) return Affine2D{};
      return avkit::detail::Compose(transform, avkit::detail::InverseAffine(base->layout.transform));
    }
  };

  struct Request {
    std::shared_ptr<const ProjectSnapshot> project;
    MediaTimestamp at{0, 1};
    bool play{};
    std::optional<TransformRequest> transform;
    std::stop_source presentation;
  };

  Impl(ImageInfo output, CodecProviders codecs, Present on_frame, SceneConfiguration setup, OnCodecFallback report)
      : target(output), providers(std::move(codecs)), scene(std::move(setup)), fallback(std::move(report)),
        present(std::move(on_frame)), preview(OpenTexture(output.size)), worker([this](std::stop_token stop) { Run(stop); }) {}

  ~Impl() { Close(); }

  void Close() {
    {
      std::scoped_lock lock(mutex);
      if (closed) return;
      closed = true;
      Interrupt();
    }
    worker.request_stop();
    changed.notify_all();
    worker.join();
  }

  void Interrupt() {
    operation.request_stop();
    requested.presentation.request_stop();
  }

  void ApplyProject(std::shared_ptr<const ProjectSnapshot> project) {
    std::scoped_lock lock(mutex);
    if (closed || requested.project == project) return;
    requested.project = std::move(project);
    requested.transform.reset();
    Schedule();
  }

  void Seek(MediaTimestamp at) {
    std::scoped_lock lock(mutex);
    if (closed) return;
    requested.at = at;
    requested.transform.reset();
    Schedule();
  }

  void Play() {
    std::scoped_lock lock(mutex);
    if (closed || requested.play) return;
    requested.play = true;
    requested.transform.reset();
    Schedule();
  }

  void Pause() {
    std::scoped_lock lock(mutex);
    if (closed || !requested.play) return;
    requested.play = false;
    Schedule();
  }

  std::uint64_t SetTransform(EditTarget target, Affine2D transform, TransformMode mode) {
    if (!avkit::detail::IsInvertible(transform))
      throw std::invalid_argument("Preview requires an invertible finite transform");
    std::scoped_lock lock(mutex);
    if (closed || !requested.project || requested.play) return 0;
    const bool coalesce = requested.transform.has_value();
    const auto id = ++next_transform_request;
    requested.transform = TransformRequest{target, transform, mode, id};
    Schedule(coalesce);
    return id;
  }

  void ClearTransform() {
    std::scoped_lock lock(mutex);
    if (closed || !requested.transform) return;
    requested.transform.reset();
    Schedule();
  }

  // Transport methods update the same desired state under the mutex before replacing pending work.
  void Schedule(bool coalesces_drag = false) {
    if (!requested.project) return;
    const bool seeks_in_playback = clocked && requested.play && requested.project == active;
    if ((!seeks_in_playback && !coalesces_drag) || operation.stop_requested()) {
      operation.request_stop();
      operation = std::stop_source{};
    }
    requested.presentation.request_stop();
    requested.presentation = std::stop_source{};
    pending = requested;
    status = {requested.at, requested.play, false, {}};
    changed.notify_all();
  }

  void Suspend() {
    std::unique_lock lock(mutex);
    if (closed) return;
    ++suspensions;
    Interrupt();
    changed.notify_all();
    changed.wait(lock, [&] { return released || closed; });
  }

  void Resume() {
    std::scoped_lock lock(mutex);
    if (closed || !suspensions || --suspensions) return;
    if (requested.project) Schedule();
    else changed.notify_all();
  }

  void Run(std::stop_token stop) {
    detail::RunRenderIteration([&] {
      std::unique_ptr<TimelineRenderer> renderer;
      std::unique_ptr<TimelineSession> timeline;
      std::shared_ptr<const ProjectSnapshot> installed;
      std::optional<TimelineFrame> retained;
      std::vector<avkit::detail::TargetLayoutEntry> retained_layouts;
      while (detail::RunRenderIteration([&] {
        std::unique_lock lock(mutex);
        if (!changed.wait(lock, stop, [&] { return suspensions || pending; }) || stop.stop_requested()) return false;
        if (suspensions) {
          lock.unlock();
          retained.reset();
          timeline.reset();
          installed.reset();
          renderer.reset();
          lock.lock();
          released = true;
          changed.notify_all();
          return changed.wait(lock, stop, [&] { return !suspensions; });
        }
        auto request = std::move(*pending);
        pending.reset();
        const auto token = operation.get_token();
        active = request.project;
        clocked = request.play;
        released = false;
        lock.unlock();

        try {
          const auto range = request.project->Range();
          auto last = range.end.Rescale(kTimelinePreviewRate).value();
          if (last.Compare(range.end) >= 0) --last.ticks;
          if (last.ticks < 0) throw std::invalid_argument("Preview needs a nonempty project interval");
          avkit::detail::CheckCancelled(token);
          const auto at = PreviewTime(request.at, last);
          if (!renderer) renderer = TimelineRenderer::Open(target, token);
          avkit::detail::CheckCancelled(token);
          if (!renderer) throw std::runtime_error("Cannot open the preview renderer");
          if (!timeline) {
            timeline = renderer->OpenTimeline(request.project, providers, CodecPolicy::Auto, scene, fallback);
            installed = request.project;
          } else if (installed != request.project) {
            timeline->ApplyProject(request.project);
            installed = request.project;
            retained.reset();
          }

          const auto sample = [&](MediaTimestamp time, std::vector<avkit::detail::TargetLayoutEntry>& layouts,
                                  std::optional<MediaTimestamp> step = {}) {
            return timeline->SampleVideo(time, {}, token, 12ms, step, {}, &layouts);
          };
          std::vector<avkit::detail::TargetLayoutEntry> layouts;
          std::optional<MediaTimestamp> previous;
          const auto draw = [&](MediaTimestamp time, std::stop_token) {
            const auto step = previous && previous->Compare(time) == -1 ?
                std::optional{avkit::detail::Difference(time, *previous)} : std::nullopt;
            previous = time;
            const auto frame = sample(time, layouts, step);
            return renderer->Draw(frame, layouts);
          };
          const auto publish = [&](const Texture& image, MediaTimestamp time, bool playing) {
            const auto present_stop = request.presentation.get_token();
            if (token.stop_requested() || present_stop.stop_requested()) return;
            auto prepared = renderer->PreparePreview(*preview, image);
            if (!prepared.Succeeded()) throw std::runtime_error(prepared.Error().message);
            auto commit = std::move(prepared).Value();
            if (!commit) {
              const auto result = renderer->PublishPreview(*preview, image);
              if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
            }
            {
              std::scoped_lock guard(mutex);
              if (token.stop_requested() || present_stop.stop_requested()) return;
              status = {time, playing, true, {}};
              requested.at = time;
              requested.play = playing;
            }
            PreviewFrame frame;
            frame.texture = preview->Texture();
            frame.project = request.project;
            frame.pts = time;
            frame.sequence = ++sequence;
            frame.playing = playing;
            frame.texture_size = target.size;
            if (request.transform) frame.transform_request = request.transform->id;
            frame.stop = present_stop;
            frame.commit = std::move(commit);
            if (!layouts.empty())
              frame.layouts_ = std::make_shared<const std::vector<avkit::detail::TargetLayoutEntry>>(std::move(layouts));
            present(std::move(frame));
          };

          if (request.play && at.Compare(last) == -1) {
            retained.reset();
            timeline->Seek(at);
            const auto seek = [&](MediaTimestamp) -> std::optional<MediaTimestamp> {
              std::scoped_lock guard(mutex);
              if (token.stop_requested() || !pending || !pending->play ||
                  pending->project != request.project) return std::nullopt;
              request = std::move(*pending);
              pending.reset();
              previous.reset();
              return PreviewTime(request.at, last);
            };
            PlayClockedTimeline(*timeline, range, at, false, seek, token, draw,
                [&](const Texture& image, MediaTimestamp time) { publish(image, time, true); },
                detail::RunRenderIteration, kTimelinePreviewRate, false);
            if (!token.stop_requested()) publish(draw(last, token), last, false);
          } else {
            if (!retained || retained->pts != at) {
              timeline->Seek(at);
              retained = sample(at, retained_layouts);
            }
            auto frame = *retained;
            const auto delta = request.transform ? request.transform->ResolveDelta(retained_layouts) : std::nullopt;
            if (delta) {
              frame = timeline->RecomposeVideo(frame, request.transform->target, *delta, &layouts);
            } else {
              request.transform.reset();
              layouts = retained_layouts;
            }
            if (!token.stop_requested()) publish(renderer->Draw(frame, layouts), at, false);
          }
        } catch (const std::exception& error) {
          retained.reset();
          std::scoped_lock guard(mutex);
          if (!token.stop_requested() && !request.presentation.stop_requested()) {
            requested.play = false;
            status = {request.at, false, false, error.what()};
          }
        }
        {
          std::scoped_lock guard(mutex);
          clocked = false;
        }
        return true;
      })) {}
      preview->Finish();
      return false;
    });
  }

  ImageInfo target;
  CodecProviders providers;
  SceneConfiguration scene;
  OnCodecFallback fallback;
  Present present;
  std::unique_ptr<PreviewTexture> preview;
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  Request requested;
  std::optional<Request> pending;
  std::shared_ptr<const ProjectSnapshot> active;
  std::stop_source operation;
  TimelinePreviewStatus status;
  std::uint64_t sequence{};
  std::uint64_t next_transform_request{};
  unsigned suspensions{};
  bool closed{}, clocked{}, released{};
  std::jthread worker;
};

TimelinePreviewSession::TimelinePreviewSession(ImageInfo target, CodecProviders providers, Present present,
                                               SceneConfiguration scene, OnCodecFallback fallback) {
  if (!present) throw std::invalid_argument("Preview needs a frame observer");
  impl_ = std::make_unique<Impl>(target, std::move(providers), std::move(present), std::move(scene), std::move(fallback));
}

TimelinePreviewSession::~TimelinePreviewSession() = default;

void TimelinePreviewSession::ApplyProject(std::shared_ptr<const ProjectSnapshot> project) {
  if (!project) throw std::invalid_argument("Preview needs a project");
  impl_->ApplyProject(std::move(project));
}

void TimelinePreviewSession::Seek(MediaTimestamp at) {
  if (!at.timescale) throw std::invalid_argument("Invalid preview time");
  impl_->Seek(at);
}

void TimelinePreviewSession::Play() { impl_->Play(); }

void TimelinePreviewSession::Pause() { impl_->Pause(); }

std::uint64_t TimelinePreviewSession::SetTransform(EditTarget target, Affine2D transform) {
  return impl_->SetTransform(target, transform, Impl::TransformMode::Absolute);
}

std::uint64_t TimelinePreviewSession::SetTransformDelta(EditTarget target, Affine2D delta) {
  return impl_->SetTransform(target, delta, Impl::TransformMode::Delta);
}

void TimelinePreviewSession::ClearTransform() { impl_->ClearTransform(); }

TimelinePreviewStatus TimelinePreviewSession::Status() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->status;
}

bool TimelinePreviewSession::Closed() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->closed;
}

void TimelinePreviewSession::Close() { impl_->Close(); }

void ExportProject(std::shared_ptr<const ProjectSnapshot> project, const File& destination,
                   const CodecProviders& providers, const FileExportSettings& settings,
                   const TimelineExportOptions& options) {
  if (!project) throw std::invalid_argument("Export needs a project snapshot");
  auto* player = options.preview ? options.preview->impl_.get() : nullptr;
  if (player) player->Suspend();
  struct ResumePreview {
    TimelinePreviewSession::Impl* player;

    ~ResumePreview() { if (player) player->Resume(); }
  } resume{player};
  detail::RunRenderIteration([&] {
    const auto& visible = settings.video.input.visible;
    auto renderer = TimelineRenderer::Open({{visible.width, visible.height}, PixelFormat::Rgba8Unorm}, options.stop);
    if (!renderer) throw std::runtime_error("Export cancelled");
    auto configured = options;
    if (!configured.scope) configured.scope = detail::RunRenderIteration;
    ExportRenderedTimeline(*renderer, std::move(project), destination, providers, settings, configured);
    return false;
  });
}

} // namespace huxerui::avkit::render
