#include "workbench_player.h"
#include "workbench_render.h"

#include "bgfx_gles_texture.h"
#include "render/bgfx/video_texture_internal.h"
#include "timeline_playback.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <android/log.h>
#include <huxerui/android/external_texture.h>
#include <huxerui/avkit/bgfx.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using namespace std::chrono_literals;
using media::MediaTimestamp;
using media::VideoFrame;
using media::VideoStorage;

constexpr auto kLogTag = "AVKitPreview";
constexpr auto kFormat = PixelFormat::Rgba8Unorm;

double Seconds(MediaTimestamp time) { return double(time.ticks) / time.timescale; }

MediaTimestamp LastFrame(MediaTimestamp end) {
  auto last = end.Rescale(kTimelinePreviewRate).value();
  if (last.Compare(end) >= 0) --last.ticks;
  return last;
}

template<class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return std::move(result).Value();
}

class MovieRenderer {
public:
  MovieRenderer(PixelSize canvas, OverlayAssets assets)
      : canvas_(canvas), backend_(CreateBgfxBackend({canvas, kFormat})), overlays_(*backend_, WorkbenchPagResources(std::move(assets))) {}

  ImportedVideoFrame Import(const VideoFrame& video) {
    const bool platform = video.Storage() == VideoStorage::Platform;
    auto imported = render::detail::ImportDecodedBgfxFrame(*backend_, video, {canvas_.width, canvas_.height},
        platform ? TextureTransferPolicy::GpuOnly : TextureTransferPolicy::AllowCpuTransfer);
    if (!imported.Succeeded() && platform && imported.Error().code == RenderErrorCode::Unsupported)
      throw PlatformVideoImportError(imported.Error().message);
    auto prepared = Require(std::move(imported));
    if (platform) {
      if (!std::exchange(imported_platform_, true))
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "Imported a decoder frame on the GPU");
    } else if (!std::exchange(imported_cpu_, true)) {
      __android_log_print(ANDROID_LOG_INFO, kLogTag, "Uploaded an FFmpeg decoder frame");
    }
    return prepared;
  }

  ColorSurface Draw(const TimelineFrame& frame) {
    if (!frame.static_masks.empty() || !frame.animated_masks.empty() || !frame.masks.empty() ||
        !frame.transition_animations.empty())
      throw std::runtime_error("Android cannot resolve this project's effect resources");
    auto data = frame.analysis_masks;
    data.insert(data.end(), frame.depth_maps.begin(), frame.depth_maps.end());
    auto request = overlays_.Prepare(frame, [&](std::size_t slot) {
      if (const auto* surface = std::get_if<ColorSurface>(&frame.inputs[slot])) return *surface;
      throw std::runtime_error("Android cannot resolve this project's color input");
    }, std::move(data));

    const auto ticket = Require(backend_->TrySubmit(request));
    if (!ticket) throw std::runtime_error("The Android render target is busy");
    backend_->Pump();
    auto surface = Require(backend_->TryTake(*ticket));
    if (!surface) throw std::runtime_error("The Android render target did not finish");
    return std::move(*surface);
  }

  VideoFrame Encode(const TimelineFrame& frame, const media::VideoEncoderConfig& config, MediaTimestamp duration) {
    auto encoded = MakeBgfxEncoderFrame(*backend_, Draw(frame), config, frame.pts, duration);
    if (!encoded.Succeeded() && config.input_storage == VideoStorage::Platform &&
        encoded.Error().code == RenderErrorCode::Unsupported) {
      throw PlatformVideoTransferError(encoded.Error().message);
    }
    auto picture = Require(std::move(encoded));
    const bool platform = picture.Storage() == VideoStorage::Platform;
    if (!std::exchange(platform ? encoded_platform_ : encoded_cpu_, true)) {
      __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s at PTS %.6f",
          platform ? "Submitted a GPU texture to the platform encoder" : "Prepared CPU I420 for the FFmpeg encoder",
          Seconds(picture.pts));
    }
    return picture;
  }

private:
  PixelSize canvas_;
  std::unique_ptr<RenderBackend> backend_;
  PagOverlayRenderer overlays_;
  bool imported_platform_{}, imported_cpu_{}, encoded_platform_{}, encoded_cpu_{};
};

void ReportFallback(const CodecFallback& fallback) {
  constexpr std::array operations{"Video decode", "Audio decode", "Video encode", "Audio encode"};
  __android_log_print(ANDROID_LOG_WARN, kLogTag, "%s, %s to %s, PTS %.3f: %s [%s]",
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
    std::optional<VisualTransformOverride> layout;
    std::stop_source cancel;
  };

  Impl(PixelSize size, OverlayAssets resources)
      : canvas(size), assets(std::move(resources)),
        texture(std::make_shared<android::GlTexture>(Size{float(size.width), float(size.height)})),
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
    std::optional<WorkbenchRenderLease> lease;
    std::unique_ptr<MovieRenderer> renderer;
    std::unique_ptr<TimelineSession> timeline;
    std::shared_ptr<const ProjectSnapshot> active;
    std::optional<TimelineFrame> retained;
    while (!stop.stop_requested()) {
      std::unique_lock lock(mutex);
      if (!ready.wait(lock, stop, [&] { return pending.has_value(); })) break;
      auto request = std::move(*pending);
      pending.reset();
      lock.unlock();
      const auto cancel_token = request.cancel.get_token();
      try {
        if (cancel_token.stop_requested()) continue;
        const auto range = request.project->Range();
        if (range.end.ticks <= 0) continue;
        if (!renderer) {
          if (!lease.emplace(stop)) break;
          renderer = std::make_unique<MovieRenderer>(canvas, assets);
        }
        if (!timeline) {
          timeline = std::make_unique<TimelineSession>(request.project,
              MovieCodecProviders{CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()},
              CodecPolicy::Auto, ReportFallback);
        } else if (active != request.project) {
          timeline->ApplyProject(request.project);
        }
        if (active != request.project) retained.reset();
        active = request.project;
        const auto last = LastFrame(range.end);
        const MediaTimestamp at{std::clamp<std::int64_t>(std::llround(request.seconds * kTimelinePreviewRate), 0, last.ticks),
                                kTimelinePreviewRate};
        const auto draw = [&](MediaTimestamp time, std::stop_token token) {
          return renderer->Draw(timeline->SampleVideo(time, kFormat, token,
              [&](const VideoFrame& video) { return renderer->Import(video); }, 12ms));
        };
        const auto present = [&](const ColorSurface& surface, MediaTimestamp time, bool playing) {
          if (cancel_token.stop_requested()) return;
          render::detail::PublishBgfxGlTexture(*texture, surface);
          std::scoped_lock status_lock(mutex);
          if (cancel_token.stop_requested()) return;
          status = {Seconds(time), playing, true, {}};
          if (!playing)
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "Preview positioned at %.3f seconds", Seconds(time));
        };

        if (request.play && at.Compare(last) == -1) {
          retained.reset();
          timeline->Seek(at);
          const auto stats = PlayClockedTimeline(*timeline, range, at, false, [](MediaTimestamp) { return std::nullopt; },
              cancel_token, TimelinePreviewMode::Once,
              draw,
              [&](const ColorSurface& surface, MediaTimestamp time) { present(surface, time, true); });
          __android_log_print(ANDROID_LOG_INFO, kLogTag,
              "Playback rendered %zu clocked frames, %zu late, %.1f ms maximum video lag, %llu PCM underruns",
              stats.clocked_frames, stats.late_video_frames, stats.maximum_video_lag_ms,
              static_cast<unsigned long long>(stats.audio_underruns));
          if (!cancel_token.stop_requested()) present(draw(last, cancel_token), last, false);
        } else {
          if (!retained || retained->pts != at) {
            timeline->Seek(at);
            retained = timeline->SampleVideo(at, kFormat, cancel_token,
                [&](const VideoFrame& video) { return renderer->Import(video); }, 12ms);
          }
          auto frame = *retained;
          if (request.layout) frame.plan = timeline->RecomposeVideo(*retained, *request.layout);
          present(renderer->Draw(frame), at, false);
        }
      } catch (const std::exception& error) {
        if (!renderer) lease.reset();
        std::scoped_lock status_lock(mutex);
        if (!cancel_token.stop_requested()) {
          status = {request.seconds, false, false, error.what()};
          __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", error.what());
        }
      }
    }
    texture->Finish();
  }

  PixelSize canvas;
  OverlayAssets assets;
  std::shared_ptr<android::GlTexture> texture;
  mutable std::mutex mutex;
  std::condition_variable_any ready;
  std::optional<Request> pending;
  std::stop_source cancel;
  WorkbenchPlayback status;
  bool manipulating{};
  std::jthread worker;
};

WorkbenchPlayer::WorkbenchPlayer(PixelSize canvas, OverlayAssets assets)
    : impl_(std::make_unique<Impl>(canvas, std::move(assets))) {}
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

void ExportWorkbenchMovie(std::shared_ptr<const ProjectSnapshot> revision, const File& file, const OverlayAssets& assets,
                          const WorkbenchExportFrame& on_frame) {
  const WorkbenchRenderLease lease;
  const auto [width, height] = revision->canvas;
  MovieRenderer renderer(revision->canvas, assets);
  FileExportSettings settings;
  settings.video.input = {.width = width, .height = height, .visible = {0, 0, width, height},
      .primaries = media::ColorPrimaries::BT709, .transfer = media::ColorTransfer::BT709,
      .matrix = media::ColorMatrix::BT709, .range = media::ColorRange::Limited};
  settings.video.frame_rate_numerator = kTimelinePreviewRate;
  // Android AAC encoders may omit the priming and padding required by the MP4 edit list.
  settings.audio_policy = CodecPolicy::SoftwareOnly;
  const MovieCodecProviders codecs{CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()};
  ExportTimeline(std::move(revision), file, codecs, settings,
      [&](const auto& frame, const auto& bound, auto duration) {
        if (on_frame) on_frame(frame.pts);
        return renderer.Encode(frame, bound, duration);
      },
      ReportFallback, {}, [&](const auto& video) { return renderer.Import(video); }, kFormat);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "Finalized timeline MP4");
}

} // namespace avkit_preview
