#include <huxerui/avkit/camera_recording.h>

#include "capture/camera_recording_verification.h"
#include "capture/platform_capture.h"
#include "render/bgfx/render_lease.h"
#include "render/bgfx/video_texture_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <huxerui/avkit/bgfx.h>
#include <huxerui/avkit_ecs.h>

namespace huxerui::avkit {
namespace {

using namespace media;
using namespace render;
using State = CameraRecordingStatus::State;
using Tick = std::int64_t;

template<class T, class E>
T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

Tick Rescale(MediaTimestamp timestamp, std::uint32_t rate) {
  const auto result = timestamp.Rescale(rate);
  Check(result.has_value(), "Capture timestamp exceeds the sample grid");
  return result->ticks;
}

struct ActiveInterval {
  Tick begin;
  Tick movie_begin;
  std::optional<Tick> end;

  Tick MovieEnd() const { return movie_begin + *end - begin; }
};

struct VideoWork {
  MediaTimestamp pts;
  std::optional<VideoEncoderConfig> config;
  bool cue{};
};

class Recording {
public:
  Recording(File destination, audio::AudioReader reader, AudioFormat format, const std::atomic_bool& robot, PixelSize size, const OnCodecFallback& fallback)
      : destination_(std::move(destination)), reader_(std::move(reader)), format_(format), robot_(robot), size_(size), fallback_(fallback) {
    Check(format_.sample_format == AudioSampleFormat::S16 &&
              format_.packing == AudioPacking::Interleaved && (format_.sample_rate == 44100 || format_.sample_rate == 48000) &&
              (format_.channel_layout == media::AudioChannelLayout::Mono || format_.channel_layout == media::AudioChannelLayout::Stereo),
          "Recording needs 44.1 or 48 kHz mono or stereo S16 PCM");
  }

  ~Recording() {
    audio_stop_.request_stop();
    {
      std::lock_guard lock(mutex_);
      if (!Settled()) FailLocked("Recording closed");
    }
    changed_.notify_all();
    Join();
  }

  void Join() {
    if (writer_.joinable()) writer_.join();
    if (audio_thread_.joinable()) audio_thread_.join();
  }

  void Launch() {
    audio_thread_ = std::jthread([this] {
      ReadMicrophone(audio_stop_.get_token());
      std::lock_guard lock(mutex_);
      reader_.reset();
      audio_done_ = true;
      changed_.notify_all();
    });
    writer_ = std::jthread([this] { Write(); });
  }

  CameraRecordingStatus Status() const {
    std::lock_guard lock(mutex_);
    auto samples = end_sample_.value_or(0);
    if (!intervals_.empty() && !end_sample_) {
      const auto& interval = intervals_.back();
      const auto now = interval.end.value_or(Rescale(avkit::detail::CameraClockNow(), format_.sample_rate));
      samples = interval.movie_begin + std::max<Tick>(0, now - interval.begin);
    }
    return {.state = state_, .sample_rate = format_.sample_rate, .samples = samples,
            .paused_samples = paused_samples_, .video_frames = submitted_video_, .error = error_};
  }

  std::optional<Tick> CueSample() const {
    std::lock_guard lock(mutex_);
    return cue_time_ ? Map(*cue_time_) : std::nullopt;
  }

  void Pause(bool pause) {
    std::lock_guard lock(mutex_);
    const auto now = Rescale(avkit::detail::CameraClockNow(), format_.sample_rate);
    if (pause && state_ == State::Recording) {
      intervals_.back().end = now;
      state_ = State::Paused;
    } else if (!pause && state_ == State::Paused) {
      const auto end = intervals_.back().MovieEnd();
      paused_samples_ += now - *intervals_.back().end;
      intervals_.push_back({now, end, {}});
      state_ = State::Recording;
    }
  }

  void Stop() {
    try {
      std::lock_guard lock(mutex_);
      if (state_ == State::Starting) {
        FailLocked("Recording stopped before both sources were ready");
        return;
      }
      if (state_ != State::Recording && state_ != State::Paused) return;
      const auto cutoff = avkit::detail::CameraClockNow();
      if (!intervals_.back().end) intervals_.back().end = Rescale(cutoff, format_.sample_rate);
      end_sample_ = intervals_.back().MovieEnd();
      state_ = State::Finishing;
      Check(pending_video_.has_value() && *end_sample_ > pending_video_->pts.ticks && reader_, "Recording has no usable input");
      FlushVideo(*end_sample_);
      video_done_ = true;
      changed_.notify_all();
      auto stopped = reader_->FinishInput(cutoff);
      if (!stopped.Succeeded() && stopped.Error().code == audio::CaptureErrorCode::InvalidArgument) {
        // The final raw block can already be in ReadMicrophone. It still passes through the recording cutoff.
        stopped = reader_->FinishInput();
      }
      Require(std::move(stopped));
    } catch (const std::exception& error) {
      Fail(error.what());
    }
  }

  void ScheduleCue() {
    std::lock_guard lock(mutex_);
    Check(state_ == State::Recording, "Start or resume before scheduling the synchronization cue");
    cue_time_ = Rescale(avkit::detail::CameraClockNow(), format_.sample_rate) + format_.sample_rate / 4;
  }

  std::optional<VideoWork> PlanVideo(std::optional<camera::SynchronizedFrameTime> time, std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    if (!config_ || Settled() || state_ == State::Finishing) return {};
    Check(time && time->clock == avkit::detail::CameraCaptureClock(),
          "Camera frames have no timestamp on the microphone clock");
    const auto host = Rescale(time->timestamp, format_.sample_rate);
    if (state_ == State::Starting) {
      if (!audio_first_ || host < std::max(first_requested_video_, *audio_first_)) return {};
      intervals_.push_back({host, 0, {}});
      generation_ = generation;
      state_ = State::Recording;
      changed_.notify_all();
    }
    Check(generation == generation_, "Camera source changed during recording");
    const auto pts = Map(host);
    const auto& interval = intervals_.back();
    const auto scene_time = pts.value_or(interval.end ? interval.MovieEnd() : interval.movie_begin);
    if (!pts || (pending_video_ && *pts <= pending_video_->pts.ticks)) return VideoWork{{scene_time, format_.sample_rate}};
    return VideoWork{{scene_time, format_.sample_rate}, config_, InCue(host)};
  }

  void OfferVideo(VideoFrame frame) {
    std::lock_guard lock(mutex_);
    if (Settled() || state_ == State::Finishing) return;
    if (pending_video_) FlushVideo(frame.pts.ticks);
    pending_video_ = std::move(frame);
    changed_.notify_all();
  }

  void Fail(std::string message) {
    std::lock_guard lock(mutex_);
    if (!Settled()) FailLocked(std::move(message));
  }

private:
  bool Settled() const { return state_ == State::Complete || state_ == State::Failed; }

  void FailLocked(std::string message) {
    error_ = std::move(message);
    state_ = State::Failed;
    audio_stop_.request_stop();
    changed_.notify_all();
  }

  std::optional<Tick> Map(Tick capture_tick) const {
    for (auto it = intervals_.rbegin(); it != intervals_.rend(); ++it) {
      if (capture_tick < it->begin) continue;
      if (it->end && capture_tick >= *it->end) return {};
      return it->movie_begin + capture_tick - it->begin;
    }
    return {};
  }

  bool InCue(Tick host) const {
    return cue_time_ && host >= *cue_time_ && host - *cue_time_ < format_.sample_rate / 5;
  }

  void FlushVideo(Tick next_pts) {
    Check(video_.size() < 24, "Video encoder is falling behind capture");
    Check(pending_video_ && next_pts > pending_video_->pts.ticks, "Recording video PTS did not advance");
    pending_video_->duration = MediaTimestamp{next_pts - pending_video_->pts.ticks, format_.sample_rate};
    video_.push_back(std::move(*pending_video_));
    pending_video_.reset();
    ++submitted_video_;
  }

  template<class Frame>
  std::optional<Frame> ReadQueue(std::unique_lock<std::mutex>& lock, std::deque<Frame>& queue, const bool& ended) {
    changed_.wait(lock, [&] { return !queue.empty() || ended || state_ == State::Failed; });
    if (state_ == State::Failed) throw std::runtime_error(error_);
    if (queue.empty()) return std::nullopt;
    auto frame = std::move(queue.front());
    queue.pop_front();
    return frame;
  }

  void Write() {
    avkit::detail::RunCaptureIteration([&] {
      try {
        MovieCodecProviders providers{CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()};
        const auto settings = FileExportSettings::Sdr(size_);
        WriteLiveMovie(destination_, providers, settings, format_,
            [this](const VideoEncoderConfig& config) {
              std::unique_lock lock(mutex_);
              if (!config_) {
                first_requested_video_ = Rescale(avkit::detail::CameraClockNow(), format_.sample_rate);
                config_ = config;
              }
              return ReadQueue(lock, video_, video_done_);
            },
            [this] {
              std::unique_lock lock(mutex_);
              return ReadQueue(lock, audio_, audio_done_);
            },
            fallback_);
        std::lock_guard lock(mutex_);
        if (!Settled()) state_ = State::Complete;
      } catch (const std::exception& error) {
        Fail(error.what());
      }
    });
  }

  void QueuePcm(Tick begin, Tick host_begin, std::uint32_t count, BufferReference data) {
    Check(audio_.size() < 128, "Audio encoder is falling behind capture");
    const auto channels = format_.ChannelCount();
    const auto stride = channels * sizeof(std::int16_t);
    const bool robot = robot_.load(std::memory_order_relaxed);
    const bool cue = cue_time_ && host_begin + count > *cue_time_ && host_begin < *cue_time_ + format_.sample_rate / 5;
    if (robot || cue) {
      auto samples = std::make_shared<std::vector<std::int16_t>>(std::size_t(count) * channels);
      std::memcpy(samples->data(), data.AsBytes().data(), std::size_t(count) * stride);
      const auto rate = format_.sample_rate;
      constexpr double tau = 2.0 * std::numbers::pi;
      for (std::uint32_t i = 0; i < count; ++i) {
        const double carrier = robot ? 0.15 + 0.85 * std::sin(tau * 68 * ((begin + i) % rate) / rate) : 1.0;
        for (std::size_t channel = 0; channel < channels; ++channel) {
          auto& out = (*samples)[std::size_t(i) * channels + channel];
          const double input = cue && InCue(host_begin + i)
              ? 18000 * std::sin(tau * 440 * (host_begin + i - *cue_time_) / rate) : out;
          out = static_cast<std::int16_t>(std::lround(input * carrier));
        }
      }
      data = BufferReference(std::as_bytes(std::span(*samples)), samples);
    }
    audio_.push_back({format_, {begin, format_.sample_rate}, count, {std::move(data)}});
    audio_next_ = begin + count;
  }

  void PadAudio(Tick until) {
    if (until <= audio_next_) return;
    const auto count = until - audio_next_;
    Check(count <= format_.sample_rate / 200, "Microphone did not cover the recording timeline");
    auto bytes = std::make_shared<Bytes>(count * format_.ChannelCount() * sizeof(std::int16_t));
    QueuePcm(audio_next_, 0, static_cast<std::uint32_t>(count), BufferReference(*bytes, bytes));
  }

  void OfferAudio(const AudioFrame& frame) {
    const auto first = Rescale(frame.pts, format_.sample_rate);
    const auto end = first + frame.sample_count;
    const auto stride = frame.format.ChannelCount() * sizeof(std::int16_t);
    for (const auto& interval : intervals_) {
      auto begin = std::max(first, interval.begin);
      const auto last = std::min(end, interval.end.value_or(end));
      begin = std::max(begin, interval.begin + audio_next_ - interval.movie_begin);
      if (begin >= last) continue;
      const auto pts = interval.movie_begin + begin - interval.begin;
      PadAudio(pts);
      const auto count = static_cast<std::uint32_t>(last - begin);
      auto data = frame.planes.front().Slice(std::size_t(begin - first) * stride, std::size_t(count) * stride);
      QueuePcm(pts, begin, count, std::move(data));
    }
    changed_.notify_all();
  }

  void ReadMicrophone(std::stop_token stop) {
    try {
      std::optional<AudioFrame> pending;
      while (auto block = Require(reader_->Read(stop))) {
        Check(block->clock == avkit::detail::CameraCaptureClock(), "Microphone is outside the camera capture clock");
        Check(block->frame.format == format_ && block->frame.Validate(), "The microphone PCM format changed");
        std::unique_lock lock(mutex_);
        if (!audio_first_) {
          audio_first_ = Rescale(block->frame.pts, format_.sample_rate);
          changed_.wait(lock, [&] { return !intervals_.empty() || state_ == State::Failed || stop.stop_requested(); });
        }
        if (state_ == State::Failed || stop.stop_requested()) return;
        if (pending) OfferAudio(*pending);
        // One complete block stays available to trim if the stop cutoff crosses it.
        pending = std::move(block->frame);
      }
      std::lock_guard lock(mutex_);
      if (stop.stop_requested() || state_ == State::Failed) return;
      Check(end_sample_.has_value(), "Microphone ended before Stop");
      if (pending) OfferAudio(*pending);
      PadAudio(*end_sample_);
      Check(audio_next_ == *end_sample_, "PCM extended past the recording cutoff");
    } catch (const std::exception& error) {
      if (!stop.stop_requested()) Fail(error.what());
    }
  }

  File destination_;
  std::optional<audio::AudioReader> reader_;
  AudioFormat format_;
  const std::atomic_bool& robot_;
  PixelSize size_;
  OnCodecFallback fallback_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  State state_{State::Starting};
  std::string error_;
  std::optional<VideoEncoderConfig> config_;
  std::optional<Tick> audio_first_, end_sample_, cue_time_;
  Tick first_requested_video_{}, audio_next_{}, paused_samples_{};
  std::uint64_t generation_{};
  std::vector<ActiveInterval> intervals_;
  std::deque<AudioFrame> audio_;
  std::deque<VideoFrame> video_;
  std::optional<VideoFrame> pending_video_;
  std::size_t submitted_video_{};
  bool audio_done_{}, video_done_{};
  std::stop_source audio_stop_;
  std::jthread audio_thread_, writer_;
};

struct FrameStamp {
  std::uint64_t generation{};
  std::optional<camera::SynchronizedFrameTime> time;
};

struct FrameRequest {
  camera::PreviewOutput input;
  std::chrono::nanoseconds source_time;
  FrameStamp stamp;
  std::promise<camera::CameraResult<camera::PreviewOutput>> result;
};

using FrameResult = camera::CameraResult<camera::PreviewOutput>;

FrameResult FailedFrame(std::string message) {
  return FrameResult::Failure({camera::CameraErrorCode::CaptureFailed, std::move(message)});
}

Draw CameraDraw(const camera::PreviewOutput& input, PixelSize source, PixelSize target) {
  Rect crop = input.crop.Intersection({0, 0, 1, 1});
  Check(!crop.IsEmpty(), "Camera frame has no visible area");
  float width = crop.width * source.width, height = crop.height * source.height;
  using enum camera::PreviewRotation;
  if (input.rotation == R90 || input.rotation == R270) std::swap(width, height);
  const auto scale = std::min(float(target.width) / width, float(target.height) / height);
  const float fitted_width = width * scale / target.width, fitted_height = height * scale / target.height;
  const auto source_point = [&](float x, float y) {
    if (input.mirrored) x = 1 - x;
    Point point{x, y};
    switch (input.rotation) {
      case R0: break;
      case R90: point = {y, 1 - x}; break;
      case R180: point = {1 - x, 1 - y}; break;
      case R270: point = {1 - y, x}; break;
    }
    return Point{crop.x + point.x * crop.width, crop.y + point.y * crop.height};
  };
  const auto origin = source_point(0, 0);
  const auto horizontal = source_point(1, 0) - origin;
  const auto vertical = source_point(0, 1) - origin;
  return {.input_slot = 0, .blend = Blend::Replace,
      .destination = {(1 - fitted_width) * 0.5F, (1 - fitted_height) * 0.5F, fitted_width, fitted_height},
      .source_sampling = SourceSampling{{horizontal.x, horizontal.y, vertical.x, vertical.y, origin.x, origin.y}}};
}

ColorSurface CueSurface() {
  static const auto color = std::make_shared<const std::array<std::byte, 4>>(
      std::array{std::byte{16}, std::byte{250}, std::byte{32}, std::byte{255}});
  return {RetainBgfxPixels({{1, 1}, PixelFormat::Rgba8Unorm}, BufferReference(*color, color), 4)};
}

class CaptureScene {
public:
  CaptureScene()
      : camera_(scene_.World().entity("Camera").set<CompositeLayer>({0, 0, Blend::Replace})),
        sticker_(scene_.World().entity("Sticker")),
        cue_(scene_.World().entity("VerificationCue")) {}

  TimelineFrame Compose(const camera::PreviewOutput& input, ColorSurface source, const ImageDesc& target,
                        MediaTimestamp time, const std::optional<CanvasOverlay>& sticker, bool cue) {
    const auto geometry = CameraDraw(input, source.pixels.image.size, target.size);
    Place(camera_, geometry.destination);
    TimelineFrame frame{.pts = time, .inputs = {std::move(source)}};
    if (sticker) {
      sticker_.enable().set<CompositeLayer>({1, 1, Blend::PremultipliedOver, sticker->opacity});
      Place(sticker_, sticker->destination);
      if (sticker->rotation_degrees)
        sticker_.set<Transform2D>(CanvasRotation(sticker->destination, target.size, sticker->rotation_degrees));
      else sticker_.remove<Transform2D>();
      frame.inputs.push_back(CanvasOverlaySample{*sticker, time});
    } else {
      sticker_.disable();
    }
    if (cue) {
      cue_.enable().set<CompositeLayer>({static_cast<std::uint32_t>(frame.inputs.size()), 2, Blend::Replace});
      Place(cue_, {0.56F, 0.42F, 0.08F, 0.14F});
      frame.inputs.push_back(CueSurface());
    } else {
      cue_.disable();
    }

    auto plan = std::make_shared<RenderPlan>(*scene_.Evaluate(time, target));
    plan->draws.front().source_sampling = geometry.source_sampling;
    frame.plan = std::move(plan);
    return frame;
  }

private:
  static void Place(flecs::entity layer, Rect rectangle) {
    layer.set<LocalTransform2D>({{rectangle.x, rectangle.y}, {rectangle.width, rectangle.height}});
  }

  SceneEvaluator scene_;
  flecs::entity camera_, sticker_, cue_;
};

class CaptureWorker {
public:
  CaptureWorker(PixelSize size, TimelineRenderResources resources, OnCodecFallback fallback)
      : target_{size, PixelFormat::Rgba8Unorm}, resources_(std::move(resources)), fallback_(std::move(fallback)),
        output_(Require(OpenBgfxPreviewTexture(size))), worker_([this](std::stop_token stop) { Run(stop); }) {}

  ~CaptureWorker() { Close(); }

  void Observe(const camera::FrameObservation& observation) {
    const std::lock_guard lock(mutex_);
    observed_ = {observation.output.texture, observation.source_time,
                 {observation.generation, observation.synchronized_time}};
  }

  FrameResult Process(const camera::PreviewOutput& input, std::chrono::nanoseconds source_time) {
    FrameStamp stamp;
    std::future<FrameResult> result;
    {
      const std::lock_guard lock(mutex_);
      if (closed_) return FrameResult::Success(input);
      if (pending_) return FailedFrame("A camera frame is already waiting for the compositor");
      if (observed_ && observed_->texture.lock() == input.texture && observed_->source_time == source_time)
        stamp = observed_->stamp;
      pending_.emplace(FrameRequest{input, source_time, stamp});
      result = pending_->result.get_future();
    }
    changed_.notify_one();
    // Camera owns the input until this callback returns. Render on the bgfx sequence before releasing it.
    return result.get();
  }

  void Close() {
    std::optional<FrameRequest> abandoned;
    std::shared_ptr<Recording> recording;
    {
      const std::lock_guard lock(mutex_);
      if (closed_) return;
      closed_ = true;
      abandoned = std::exchange(pending_, std::nullopt);
      recording = std::exchange(recording_, {});
    }
    if (abandoned) abandoned->result.set_value(FrameResult::Success(abandoned->input));
    if (recording) recording->Fail("Camera recording closed");
    worker_.request_stop();
    changed_.notify_all();
    worker_.join();
    if (recording) recording->Join();
  }

  std::shared_ptr<ExternalTexture> Texture() const { return output_->Texture(); }

  CameraRecordingStatus Status() const {
    const std::lock_guard lock(mutex_);
    if (!error_.empty()) return {.state = State::Failed, .error = error_};
    return recording_ ? recording_->Status() : CameraRecordingStatus{};
  }

  void Start(File file, audio::AudioReader reader, AudioFormat format) {
    auto take = std::make_shared<Recording>(std::move(file), std::move(reader), format, robot_, target_.size, fallback_);
    std::shared_ptr<Recording> previous;
    {
      const std::lock_guard lock(mutex_);
      Check(!closed_ && error_.empty(), "The camera compositor is unavailable");
      if (recording_) {
        const auto state = recording_->Status().state;
        Check(state == State::Complete || state == State::Failed, "A recording is already in progress");
      }
      previous = std::exchange(recording_, take);
      take->Launch();
    }
  }

  std::shared_ptr<Recording> Current() const {
    const std::lock_guard lock(mutex_);
    return recording_;
  }

  void SetSticker(std::optional<CanvasOverlay> sticker) {
    const std::lock_guard lock(mutex_);
    sticker_ = std::move(sticker);
  }

  void SetRobotVoice(bool enabled) { robot_.store(enabled, std::memory_order_relaxed); }

private:
  struct Observation {
    std::weak_ptr<ExternalTexture> texture;
    std::chrono::nanoseconds source_time;
    FrameStamp stamp;
  };

  void Run(std::stop_token stop) {
    avkit::detail::RunCaptureIteration([&] {
      std::optional<render::detail::BgfxRenderLease> lease;
      std::unique_ptr<TimelineRenderer> renderer;
      CaptureScene scene;
      std::optional<std::chrono::nanoseconds> preview_origin;
      std::uint64_t preview_generation{};
      while (!stop.stop_requested()) {
        std::optional<FrameRequest> request;
        std::shared_ptr<Recording> recording;
        std::optional<CanvasOverlay> sticker;
        {
          std::unique_lock lock(mutex_);
          if (!changed_.wait(lock, stop, [&] { return pending_.has_value(); })) break;
          request = std::exchange(pending_, std::nullopt);
          recording = recording_;
          sticker = sticker_;
        }
        avkit::detail::RunCaptureIteration([&] {
          try {
            if (stop.stop_requested()) {
              request->result.set_value(FrameResult::Success(request->input));
              return;
            }
            if (!renderer) {
              if (!lease.emplace(stop)) {
                request->result.set_value(FrameResult::Success(request->input));
                return;
              }
              renderer = std::make_unique<TimelineRenderer>(target_, CreateBgfxBackend(target_), std::move(resources_));
            }
            if (!preview_origin || request->stamp.generation != preview_generation || request->source_time < *preview_origin) {
              preview_generation = request->stamp.generation;
              preview_origin = request->source_time;
            }
            std::optional<VideoWork> work;
            if (recording) {
              try { work = recording->PlanVideo(request->stamp.time, request->stamp.generation); }
              catch (const std::exception& error) { recording->Fail(error.what()); }
            }
            const MediaTimestamp time = work ? work->pts :
                MediaTimestamp{(request->source_time - *preview_origin).count(), 1'000'000'000};
            auto source = Require(render::detail::AcquireBgfxTexture(renderer->Backend(), *request->input.texture));
            auto frame = scene.Compose(request->input, std::move(source), target_, time, sticker, work && work->cue);
            const auto surface = renderer->Draw(frame);
            Require(output_->Publish(renderer->Backend(), surface));
            if (work && work->config) {
              try { recording->OfferVideo(renderer->EncoderFrame(surface, *work->config, work->pts)); }
              catch (const std::exception& error) { recording->Fail(error.what()); }
            }
            request->result.set_value(FrameResult::Success({.texture = output_->Texture()}));
          } catch (const std::exception& error) {
            {
              const std::lock_guard lock(mutex_);
              error_ = error.what();
            }
            if (recording) recording->Fail(error.what());
            request->result.set_value(FailedFrame(error.what()));
          }
        });
      }
      output_->Finish();
    });
  }

  ImageDesc target_;
  TimelineRenderResources resources_;
  OnCodecFallback fallback_;
  std::unique_ptr<BgfxPreviewTexture> output_;
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::optional<Observation> observed_;
  std::optional<FrameRequest> pending_;
  std::optional<CanvasOverlay> sticker_;
  std::shared_ptr<Recording> recording_;
  std::atomic_bool robot_{};
  std::string error_;
  bool closed_{};
  std::jthread worker_;
};

} // namespace

struct CameraRecording::Impl {
  Impl(PixelSize size, TimelineRenderResources resources, OnCodecFallback fallback)
      : worker(std::make_shared<CaptureWorker>(size, std::move(resources), std::move(fallback))) {
    const auto weak = std::weak_ptr(worker);
    observer = std::make_shared<const camera::FrameObserver>([weak](const auto& frame) {
      if (const auto active = weak.lock()) active->Observe(frame);
    });
    processor = std::make_shared<const camera::FrameProcessorFactory>([weak](std::uint32_t, std::uint32_t) {
      return [weak](const auto& input, auto time) {
        if (const auto active = weak.lock()) return active->Process(input, time);
        return FrameResult::Success(input);
      };
    });
  }

  std::shared_ptr<CaptureWorker> worker;
  std::shared_ptr<const camera::FrameObserver> observer;
  std::shared_ptr<const camera::FrameProcessorFactory> processor;
};

CameraRecording::CameraRecording(PixelSize size, TimelineRenderResources resources, OnCodecFallback fallback) {
  if (!size.width || !size.height || size.width % 2 || size.height % 2 ||
      size.width > 8192 || size.height > 8192)
    throw std::invalid_argument("Camera recording needs positive even dimensions up to 8192");
  impl_ = std::make_unique<Impl>(size, std::move(resources), std::move(fallback));
}

CameraRecording::~CameraRecording() { Close(); }

void CameraRecording::Connect(camera::CameraOptions& options) const {
  options.frame_processor_factory = impl_->processor;
  options.frame_observer = impl_->observer;
  options.output_pixel_width = options.output_pixel_height = 0;
}

std::shared_ptr<ExternalTexture> CameraRecording::Texture() const { return impl_->worker->Texture(); }
CameraRecordingStatus CameraRecording::Status() const { return impl_->worker->Status(); }
void CameraRecording::SetRobotVoice(bool enabled) { impl_->worker->SetRobotVoice(enabled); }
void CameraRecording::SetSticker(std::optional<CanvasOverlay> sticker) { impl_->worker->SetSticker(std::move(sticker)); }

void CameraRecording::Start(File file, audio::AudioReader reader, AudioFormat format) {
  impl_->worker->Start(std::move(file), std::move(reader), format);
}

void CameraRecording::Pause(bool paused) { if (auto take = impl_->worker->Current()) take->Pause(paused); }
void CameraRecording::Stop() { if (auto take = impl_->worker->Current()) take->Stop(); }
void CameraRecording::Cancel(std::string error) { if (auto take = impl_->worker->Current()) take->Fail(std::move(error)); }
void CameraRecording::Close() { impl_->worker->Close(); }

void avkit::detail::CameraRecordingVerification::ScheduleCue(CameraRecording& recording) {
  const auto take = recording.impl_->worker->Current();
  Check(bool(take), "No recording for the synchronization cue");
  take->ScheduleCue();
}

std::optional<Tick> avkit::detail::CameraRecordingVerification::CueSample(const CameraRecording& recording) {
  const auto take = recording.impl_->worker->Current();
  return take ? take->CueSample() : std::nullopt;
}

} // namespace huxerui::avkit
