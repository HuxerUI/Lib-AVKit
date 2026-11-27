#include <huxerui/avkit/camera_recording.h>

#include "capture/platform_capture.h"
#include "ecs/project_scene.h"
#include "ecs/system/audio_system.h"
#include "ecs/system/scene_systems.h"
#include "support/media_time.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <set>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <huxerui/avkit/scene.h>
#include <huxerui/avkit/component.h>
#include <huxerui/bitmap.h>
#include <huxerui/task.h>

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
};

class CaptureAudio {
public:
  explicit CaptureAudio(const SceneConfiguration& setup) : scene_(setup.resources) {
    avkit::detail::InstallAudioSystem(scene_);
    if (setup.analysis) scene_.Root().assign<std::shared_ptr<analysis::AnalysisProvider>>(setup.analysis);
    if (setup.module) setup.module(scene_);
  }

  AudioFrame Process(AudioFrame pcm, std::optional<VoiceEffect> effect) {
    if (effect) input_.replace<VoiceEffect>(*effect);
    else if (input_.has_component<VoiceEffect>()) input_.remove<VoiceEffect>();
    const auto pts = pcm.pts;
    const auto samples = pcm.sample_count;
    const auto format = pcm.format;
    input_.replace<LivePcmInput>(std::move(pcm));
    return avkit::detail::EvaluateAudioBlock(scene_, pts, samples, format);
  }

private:
  RenderScene scene_;
  entityx::Entity input_ = scene_.Entities().create();
};

class Recording {
public:
  Recording(File destination, audio::AudioReader reader, AudioFormat format, const std::atomic<std::optional<VoiceEffect>>& voice, PixelSize size,
            const OnCodecFallback& fallback, const SceneConfiguration& scene)
      : destination_(std::move(destination)), reader_(std::move(reader)), format_(format), voice_(voice), size_(size), fallback_(fallback), scene_(scene) {
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
    return VideoWork{{scene_time, format_.sample_rate}, config_};
  }

  std::optional<VideoEncoderConfig> RetryFirstVideoInput(std::string reason) {
    std::unique_lock lock(mutex_);
    if (Settled() || !config_ || config_->input_storage != VideoStorage::Platform || pending_video_ || submitted_video_ || consumed_audio_)
      return std::nullopt;
    first_input_error_ = std::move(reason);
    changed_.notify_all();
    changed_.wait(lock, [&] { return Settled() || (config_ && config_->input_storage == VideoStorage::Cpu); });
    return Settled() ? std::nullopt : config_;
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
        CodecProviders providers{CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()};
        const auto encode = [&](FileExportSettings settings) {
          WriteLiveMovie(destination_, providers, settings, format_,
              [this](const VideoEncoderConfig& config) {
                std::unique_lock lock(mutex_);
                if (!config_) {
                  first_requested_video_ = Rescale(avkit::detail::CameraClockNow(), format_.sample_rate);
                  config_ = config;
                  changed_.notify_all();
                }
                changed_.wait(lock, [&] { return first_input_error_ || !video_.empty() || video_done_ || Settled(); });
                if (first_input_error_) throw PlatformEncoderInputError(*first_input_error_);
                return ReadQueue(lock, video_, video_done_);
              },
              [this] {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return first_input_error_ || !audio_.empty() || audio_done_ || Settled(); });
                if (first_input_error_) throw PlatformEncoderInputError(*first_input_error_);
                auto frame = ReadQueue(lock, audio_, audio_done_);
                if (frame) consumed_audio_ = true;
                return frame;
              },
              fallback_);
        };
        auto settings = FileExportSettings::Sdr(size_);
        try {
          encode(settings);
        } catch (const std::exception&) {
          std::optional<std::string> transfer;
          {
            std::lock_guard lock(mutex_);
            if (Settled() || !first_input_error_) throw;
            transfer = std::exchange(first_input_error_, std::nullopt);
            config_.reset();
          }
          if (fallback_) fallback_({CodecOperation::VideoEncode, std::string(providers.platform->Name()),
              std::string(providers.software->Name()), {CodecErrorCode::Unsupported, *transfer, {}}, MediaTimestamp{}});
          settings.policy = CodecPolicy::SoftwareOnly;
          encode(settings);
        }
        std::lock_guard lock(mutex_);
        if (!Settled()) state_ = State::Complete;
      } catch (const std::exception& error) {
        Fail(error.what());
      }
    });
  }

  void QueuePcm(CaptureAudio& mixer, Tick begin, std::uint32_t count, BufferReference data) {
    Check(audio_.size() < 128, "Audio encoder is falling behind capture");
    AudioFrame pcm{format_, {begin, format_.sample_rate}, count, {std::move(data)}};
    audio_.push_back(mixer.Process(std::move(pcm), voice_.load(std::memory_order_relaxed)));
    audio_next_ = begin + count;
  }

  void PadAudio(CaptureAudio& mixer, Tick until) {
    if (until <= audio_next_) return;
    const auto count = until - audio_next_;
    Check(count <= format_.sample_rate / 200, "Microphone did not cover the recording timeline");
    auto bytes = std::make_shared<Bytes>(count * format_.ChannelCount() * sizeof(std::int16_t));
    QueuePcm(mixer, audio_next_, static_cast<std::uint32_t>(count), BufferReference(*bytes, bytes));
  }

  void OfferAudio(CaptureAudio& mixer, const AudioFrame& frame) {
    const auto first = Rescale(frame.pts, format_.sample_rate);
    const auto end = first + frame.sample_count;
    const auto stride = frame.format.ChannelCount() * sizeof(std::int16_t);
    for (const auto& interval : intervals_) {
      auto begin = std::max(first, interval.begin);
      const auto last = std::min(end, interval.end.value_or(end));
      begin = std::max(begin, interval.begin + audio_next_ - interval.movie_begin);
      if (begin >= last) continue;
      const auto pts = interval.movie_begin + begin - interval.begin;
      PadAudio(mixer, pts);
      const auto count = static_cast<std::uint32_t>(last - begin);
      auto data = frame.planes.front().Slice(std::size_t(begin - first) * stride, std::size_t(count) * stride);
      QueuePcm(mixer, pts, count, std::move(data));
    }
    changed_.notify_all();
  }

  void ReadMicrophone(std::stop_token stop) {
    try {
      CaptureAudio mixer(scene_);
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
        if (pending) OfferAudio(mixer, *pending);
        // One complete block stays available to trim if the stop cutoff crosses it.
        pending = std::move(block->frame);
      }
      std::lock_guard lock(mutex_);
      if (stop.stop_requested() || state_ == State::Failed) return;
      Check(end_sample_.has_value(), "Microphone ended before Stop");
      if (pending) OfferAudio(mixer, *pending);
      PadAudio(mixer, *end_sample_);
      Check(audio_next_ == *end_sample_, "PCM extended past the recording cutoff");
    } catch (const std::exception& error) {
      if (!stop.stop_requested()) Fail(error.what());
    }
  }

  File destination_;
  std::optional<audio::AudioReader> reader_;
  AudioFormat format_;
  const std::atomic<std::optional<VoiceEffect>>& voice_;
  PixelSize size_;
  OnCodecFallback fallback_;
  SceneConfiguration scene_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  State state_{State::Starting};
  std::string error_;
  std::optional<VideoEncoderConfig> config_;
  std::optional<std::string> first_input_error_;
  std::optional<Tick> audio_first_, end_sample_;
  Tick first_requested_video_{}, audio_next_{}, paused_samples_{};
  std::uint64_t generation_{};
  std::vector<ActiveInterval> intervals_;
  std::deque<AudioFrame> audio_;
  std::deque<VideoFrame> video_;
  std::optional<VideoFrame> pending_video_;
  std::size_t submitted_video_{};
  bool audio_done_{}, video_done_{}, consumed_audio_{};
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

VideoRotation CameraRotation(camera::PreviewRotation rotation) {
  switch (rotation) {
    case camera::PreviewRotation::R0: return VideoRotation::None;
    case camera::PreviewRotation::R90: return VideoRotation::Clockwise90;
    case camera::PreviewRotation::R180: return VideoRotation::Clockwise180;
    case camera::PreviewRotation::R270: return VideoRotation::Clockwise270;
  }
  throw std::invalid_argument("Unknown camera rotation");
}

class CaptureScene {
public:
  explicit CaptureScene(const SceneConfiguration& setup) : scene_(setup.resources) {
    avkit::detail::InstallCaptureSystems(scene_);
    if (setup.analysis) scene_.Root().assign<std::shared_ptr<analysis::AnalysisProvider>>(setup.analysis);
    if (setup.module) setup.module(scene_);
  }

  TimelineFrame Compose(const camera::PreviewOutput& input, Texture source, const ImageInfo& target,
                        MediaTimestamp time, std::shared_ptr<const std::vector<OverlayInstance>> overlays,
                        std::optional<MediaTimestamp> step) {
    UpdateOverlays(std::move(overlays));
    TimelineFrame frame{.pts = time};
    struct ReleaseInputs {
      entityx::Entity camera;

      ~ReleaseInputs() {
        if (camera.has_component<Texture>()) camera.remove<Texture>();
      }
    } release{camera_};
    camera_.replace<CameraFrameGeometry>(input.crop, CameraRotation(input.rotation), input.mirrored);
    camera_.assign<Texture>(std::move(source));
    frame.plan = scene_.Evaluate({time, target, step});
    return frame;
  }

private:
  entityx::Entity CreateLayer(std::int32_t order, Blend blend) {
    auto layer = scene_.Entities().create();
    layer.assign<LayerStyle>(order, blend);
    return layer;
  }

  void UpdateOverlays(std::shared_ptr<const std::vector<OverlayInstance>> overlays) {
    if (overlays == overlays_) return;
    auto previous = std::move(layers_);
    layers_.clear();
    if (overlays) {
      layers_.reserve(overlays->size());
      std::int32_t order = 1;
      for (const auto& overlay : *overlays) {
        const auto found = std::ranges::find_if(previous, [&](const auto& entry) { return entry.first == overlay.id; });
        const auto layer = found == previous.end() ? scene_.Entities().create() : found->second;
        if (found != previous.end()) previous.erase(found);
        const auto range = avkit::detail::Before(overlay.range.start, overlay.range.end) ?
            std::optional{overlay.range} : std::nullopt;
        avkit::detail::ProjectOverlay(layer, overlay, range, order++);
        layers_.emplace_back(overlay.id, layer);
      }
    }
    for (const auto& entry : previous) scene_.Destroy(entry.second);
    overlays_ = std::move(overlays);
  }

  RenderScene scene_;
  entityx::Entity camera_ = CreateLayer(0, Blend::Replace);
  std::vector<std::pair<InstanceId, entityx::Entity>> layers_;
  std::shared_ptr<const std::vector<OverlayInstance>> overlays_;
};

class CaptureWorker {
public:
  CaptureWorker(PixelSize size, SceneConfiguration scene, OnCodecFallback fallback)
      : target_{size, PixelFormat::Rgba8Unorm}, scene_(std::move(scene)), fallback_(std::move(fallback)),
        output_(Require(OpenPreviewTexture(size))), worker_([this](std::stop_token stop) { Run(stop); }) {}

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
    auto take = std::make_shared<Recording>(std::move(file), std::move(reader), format, voice_, target_.size, fallback_, scene_);
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

  void SetOverlays(std::vector<OverlayInstance> overlays) {
    const std::lock_guard lock(mutex_);
    if (overlays_ && *overlays_ == overlays) return;
    overlays_ = std::make_shared<const std::vector<OverlayInstance>>(std::move(overlays));
  }

  void SetVoiceEffect(std::optional<VoiceEffect> effect) { voice_.store(effect, std::memory_order_relaxed); }

private:
  struct Observation {
    std::weak_ptr<ExternalTexture> texture;
    std::chrono::nanoseconds source_time;
    FrameStamp stamp;
  };

  void Run(std::stop_token stop) {
    avkit::detail::RunCaptureIteration([&] {
      std::unique_ptr<TimelineRenderer> renderer;
      std::optional<CaptureScene> scene;
      std::optional<MediaTimestamp> previous_time;
      std::optional<std::chrono::nanoseconds> preview_origin;
      std::uint64_t preview_generation{};
      while (!stop.stop_requested()) {
        std::optional<FrameRequest> request;
        std::shared_ptr<Recording> recording;
        std::shared_ptr<const std::vector<OverlayInstance>> overlays;
        {
          std::unique_lock lock(mutex_);
          if (!changed_.wait(lock, stop, [&] { return pending_.has_value(); })) break;
          request = std::exchange(pending_, std::nullopt);
          recording = recording_;
          overlays = overlays_;
        }
        avkit::detail::RunCaptureIteration([&] {
          try {
            if (stop.stop_requested()) {
              request->result.set_value(FrameResult::Success(request->input));
              return;
            }
            if (!renderer) {
              if (!scene) scene.emplace(scene_);
              renderer = TimelineRenderer::Open(target_, stop);
              if (!renderer) {
                request->result.set_value(FrameResult::Success(request->input));
                return;
              }
            }
            if (!preview_origin || request->stamp.generation != preview_generation || request->source_time < *preview_origin) {
              previous_time.reset();
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
            auto source = renderer->ImportTexture(*request->input.texture);
            const auto step = previous_time && previous_time->timescale == time.timescale &&
                previous_time->Compare(time) == -1 ? std::optional{avkit::detail::Difference(time, *previous_time)} : std::nullopt;
            previous_time = time;
            auto frame = scene->Compose(request->input, std::move(source), target_, time, std::move(overlays), step);
            const auto surface = renderer->Draw(frame);
            Require(renderer->PublishPreview(*output_, surface));
            if (work && work->config) {
              try {
                try {
                  recording->OfferVideo(renderer->EncoderFrame(surface, *work->config, work->pts));
                } catch (const PlatformEncoderInputError& error) {
                  const auto software = recording->RetryFirstVideoInput(error.what());
                  if (!software) throw;
                  recording->OfferVideo(renderer->EncoderFrame(surface, *software, work->pts));
                }
              } catch (const std::exception& error) {
                recording->Fail(error.what());
              }
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

  ImageInfo target_;
  SceneConfiguration scene_;
  OnCodecFallback fallback_;
  std::unique_ptr<PreviewTexture> output_;
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::optional<Observation> observed_;
  std::optional<FrameRequest> pending_;
  std::shared_ptr<const std::vector<OverlayInstance>> overlays_;
  std::shared_ptr<Recording> recording_;
  std::atomic<std::optional<VoiceEffect>> voice_{};
  std::string error_;
  bool closed_{};
  std::jthread worker_;
};

} // namespace

struct CameraRecording::Impl {
  Impl(PixelSize size, SceneConfiguration scene, OnCodecFallback fallback)
      : worker(std::make_shared<CaptureWorker>(size, std::move(scene), std::move(fallback))) {
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

CameraRecording::CameraRecording(PixelSize size, SceneConfiguration scene, OnCodecFallback fallback) {
  if (!size.width || !size.height || size.width % 2 || size.height % 2 ||
      size.width > 8192 || size.height > 8192)
    throw std::invalid_argument("Camera recording needs positive even dimensions up to 8192");
  impl_ = std::make_unique<Impl>(size, std::move(scene), std::move(fallback));
}

CameraRecording::~CameraRecording() { Close(); }

void CameraRecording::Connect(camera::CameraOptions& options) const {
  options.frame_processor_factory = impl_->processor;
  options.frame_observer = impl_->observer;
  options.output_pixel_width = options.output_pixel_height = 0;
}

std::shared_ptr<ExternalTexture> CameraRecording::Texture() const { return impl_->worker->Texture(); }

CameraRecordingStatus CameraRecording::Status() const { return impl_->worker->Status(); }

void CameraRecording::SetVoiceEffect(std::optional<VoiceEffect> effect) { impl_->worker->SetVoiceEffect(effect); }

Task<void> CameraRecording::CapturePhoto(File destination) const {
  auto image = co_await Texture()->SnapshotAsync();
  if (!image) throw std::runtime_error("The camera has not published a composed frame");
  if (!co_await image->SaveAsync(std::move(destination), {.format = ImageFormat::Jpeg}))
    throw std::runtime_error("Cannot save the composed photo");
}

Task<CameraRecordingStatus> CameraRecording::Record(File destination, camera::CameraSession camera,
                                                     audio::AudioCaptureSession microphone) {
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  const auto microphone_deadline = Clock::now() + 10s;
  for (;;) {
    const auto status = microphone.Status();
    if (status.error) throw std::runtime_error(status.error->message);
    if (status.state == audio::SessionState::Running && status.format) {
      Start(std::move(destination), Require(microphone.OpenReader()), *status.format);
      break;
    }
    if (Clock::now() >= microphone_deadline) throw std::runtime_error("Microphone did not publish its PCM format");
    co_await Delay(40ms);
  }
  struct CancelUnfinished {
    CameraRecording& recording;
    bool finished{};

    ~CancelUnfinished() {
      if (!finished) recording.Cancel("Recording interrupted");
    }
  } active{*this};
  const auto capture_deadline = Clock::now() + 10s;
  auto finish_deadline = Clock::time_point::max();
  for (;;) {
    const auto status = Status();
    if (status.state == State::Complete) {
      active.finished = true;
      co_return status;
    }
    if (status.state == State::Failed) throw std::runtime_error(status.error);
    const auto camera_state = camera.Status();
    if (camera_state.error || camera_state.state == camera::SessionState::Suspended)
      throw std::runtime_error(camera_state.error ? camera_state.error->message : "Camera recording interrupted");
    const auto now = Clock::now();
    if (status.state == State::Starting && now >= capture_deadline)
      throw std::runtime_error("Camera and microphone did not synchronize");
    if (status.state == State::Finishing && finish_deadline == Clock::time_point::max())
      finish_deadline = now + 20s;
    if (now >= finish_deadline) throw std::runtime_error("Recording did not finalize");
    co_await Delay(40ms);
  }
}

void CameraRecording::SetOverlays(std::vector<OverlayInstance> overlays) {
  if (overlays.size() >= std::numeric_limits<std::int32_t>::max())
    throw std::length_error("Too many capture overlays");
  std::set<std::uint64_t> instances;
  for (const auto& overlay : overlays)
    if (!instances.insert(overlay.id.value).second) throw std::invalid_argument("Capture overlay IDs must be unique");
  impl_->worker->SetOverlays(std::move(overlays));
}

void CameraRecording::Start(File file, audio::AudioReader reader, AudioFormat format) {
  impl_->worker->Start(std::move(file), std::move(reader), format);
}

void CameraRecording::Pause(bool paused) { if (auto take = impl_->worker->Current()) take->Pause(paused); }

void CameraRecording::Stop() { if (auto take = impl_->worker->Current()) take->Stop(); }

void CameraRecording::Cancel(std::string error) { if (auto take = impl_->worker->Current()) take->Fail(std::move(error)); }

void CameraRecording::Close() { impl_->worker->Close(); }

} // namespace huxerui::avkit
