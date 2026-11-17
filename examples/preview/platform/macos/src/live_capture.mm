#include "live_capture.h"
#include "metal_preview.h"
#include "movie_renderer.h"
#include "sticker_scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <numbers>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#import <CoreMedia/CoreMedia.h>

#include <huxerui/avkit/file_movie.h>
#include <huxerui/macos/external_texture.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::media;
using namespace std::chrono_literals;
using State = LiveCaptureStatus::State;
using Tick = std::int64_t;
using huxerui::macos::MetalTexture;
using huxerui::macos::PixelBufferTexture;

constexpr PixelSize kCaptureSize{640, 360};

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

MediaTimestamp HostNow() {
  const auto now = CMClockGetTime(CMClockGetHostTimeClock());
  Check(CMTIME_IS_NUMERIC(now) && now.epoch == 0 && now.timescale > 0, "CoreMedia host clock is unavailable");
  return {now.value, static_cast<std::uint32_t>(now.timescale)};
}

struct CameraImage {
  std::shared_ptr<std::remove_pointer_t<CVPixelBufferRef>> pixels;
  huxerui::Rect crop;
  VideoRotation rotation;
  std::uint64_t generation;
  std::chrono::nanoseconds source_time;
  std::optional<MediaTimestamp> host_time;
};

VideoRotation Rotation(camera::PreviewRotation rotation) {
  switch (rotation) {
    case camera::PreviewRotation::R0: return VideoRotation::None;
    case camera::PreviewRotation::R90: return VideoRotation::Clockwise90;
    case camera::PreviewRotation::R180: return VideoRotation::Clockwise180;
    case camera::PreviewRotation::R270: return VideoRotation::Clockwise270;
  }
  throw std::runtime_error("Unknown camera orientation");
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
  Recording(File destination, audio::AudioReader reader, AudioFormat format, const std::atomic_bool& robot)
      : destination_(std::move(destination)), reader_(std::move(reader)), format_(format), robot_(robot) {
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

  LiveCaptureStatus Status() const {
    std::lock_guard lock(mutex_);
    return {.state = state_, .sample_rate = format_.sample_rate, .samples = end_sample_.value_or(0),
            .paused_samples = paused_samples_, .cue_sample = cue_time_ ? Map(*cue_time_) : std::nullopt,
            .video_frames = submitted_video_, .error = error_};
  }

  void Pause(bool pause) {
    std::lock_guard lock(mutex_);
    const auto now = Rescale(HostNow(), format_.sample_rate);
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
      const auto cutoff = HostNow();
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
    cue_time_ = Rescale(HostNow(), format_.sample_rate) + format_.sample_rate / 4;
  }

  std::optional<VideoWork> PlanVideo(const CameraImage& image) {
    std::lock_guard lock(mutex_);
    if (!config_ || Settled() || state_ == State::Finishing) return {};
    Check(image.host_time.has_value(), "Camera frames have no CoreMedia host time");
    const auto host = Rescale(*image.host_time, format_.sample_rate);
    if (state_ == State::Starting) {
      if (!audio_first_ || host < std::max(first_requested_video_, *audio_first_)) return {};
      intervals_.push_back({host, 0, {}});
      generation_ = image.generation;
      state_ = State::Recording;
      changed_.notify_all();
    }
    Check(image.generation == generation_, "Camera source changed during recording");
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
    @autoreleasepool {
      try {
        MovieCodecProviders providers{CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()};
        FileExportSettings settings{.video = PreviewVideoConfig(kCaptureSize)};
        WriteLiveMovie(destination_, providers, settings, format_,
            [this](const VideoEncoderConfig& config) {
              std::unique_lock lock(mutex_);
              if (!config_) {
                first_requested_video_ = Rescale(HostNow(), format_.sample_rate);
                config_ = config;
              }
              return ReadQueue(lock, video_, video_done_);
            },
            [this] {
              std::unique_lock lock(mutex_);
              return ReadQueue(lock, audio_, audio_done_);
            },
            [](const CodecFallback& event) {
              std::cout << "Recording codec fallback " << event.from << " -> " << event.to
                        << " (" << event.cause.message << ")" << std::endl;
            });
        std::lock_guard lock(mutex_);
        if (!Settled()) state_ = State::Complete;
      } catch (const std::exception& error) {
        Fail(error.what());
      }
    }
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
        Check(block->clock == audio::CaptureClock::CoreMediaHostTime, "Microphone is outside the CoreMedia host clock");
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

struct Inbox {
  std::mutex mutex;
  std::condition_variable_any changed;
  std::optional<CameraImage> latest;
  std::shared_ptr<Recording> recording;
  std::string error;
};

std::optional<Tick> FindVoiceCue(std::span<const std::int16_t> pcm, std::uint32_t rate) {
  const auto size = rate / 50;
  const auto amplitude = [rate, size](std::span<const std::int16_t> window, double frequency) {
    double real = 0, imaginary = 0;
    const auto step = 2 * std::numbers::pi * frequency / rate;
    for (std::size_t index = 0; index < window.size(); ++index) {
      real += window[index] * std::cos(step * index);
      imaginary += window[index] * std::sin(step * index);
    }
    return 2 * std::hypot(real, imaginary) / size;
  };
  for (std::size_t start = 0; start + size <= pcm.size(); start += size / 2) {
    const auto window = pcm.subspan(start, size);
    if (amplitude(window, 372) > 2300 && amplitude(window, 508) > 2300) return static_cast<Tick>(start);
  }
  return std::nullopt;
}

} // namespace

struct LiveCapture::Impl {
  Impl() : observer(std::make_shared<const camera::FrameObserver>([weak = std::weak_ptr(inbox)](const auto& frame) {
    auto state = weak.lock();
    auto texture = std::dynamic_pointer_cast<PixelBufferTexture>(frame.output.texture);
    if (!state || !texture) return;
    auto buffer = texture->AcquireFrame();
    if (!buffer) return;
    CameraImage image{{buffer, CVPixelBufferRelease}, frame.output.crop, Rotation(frame.output.rotation),
                      frame.generation, frame.source_time, std::nullopt};
    if (frame.synchronized_time && frame.synchronized_time->clock == audio::CaptureClock::CoreMediaHostTime)
      image.host_time = frame.synchronized_time->timestamp;
    {
      std::lock_guard lock(state->mutex);
      state->latest = std::move(image);
    }
    state->changed.notify_one();
  })), worker([this](std::stop_token stop) { Render(stop); }) {}

  ~Impl() {
    worker.request_stop();
    inbox->changed.notify_all();
    worker.join();
    std::shared_ptr<Recording> recording;
    {
      std::lock_guard lock(inbox->mutex);
      recording = std::move(inbox->recording);
    }
    if (recording) recording->Fail("Capture preview closed");
  }

  std::shared_ptr<Recording> Current() const {
    std::lock_guard lock(inbox->mutex);
    return inbox->recording;
  }

  void Render(std::stop_token stop) {
    @autoreleasepool {
      try {
        MovieRenderer renderer(kCaptureSize);
        std::optional<std::chrono::nanoseconds> preview_origin;
        std::uint64_t preview_generation{};
        while (!stop.stop_requested()) {
          @autoreleasepool {
            std::unique_lock lock(inbox->mutex);
            if (!inbox->changed.wait(lock, stop, [this] { return inbox->latest.has_value(); })) break;
            auto image = std::move(*inbox->latest);
            inbox->latest.reset();
            const auto recording = inbox->recording;
            lock.unlock();
            if (!preview_origin || image.generation != preview_generation || image.source_time < *preview_origin) {
              preview_generation = image.generation;
              preview_origin = image.source_time;
            }
            std::optional<VideoWork> work;
            if (recording) {
              try { work = recording->PlanVideo(image); }
              catch (const std::exception& error) { recording->Fail(error.what()); }
            }
            const MediaTimestamp preview_time{(image.source_time - *preview_origin).count(), 1'000'000'000};
            const auto surface = renderer.Draw(image.pixels.get(), image.crop, image.rotation,
                                               work ? work->pts : preview_time, work && work->cue);
            mailbox->Publish({.texture = huxerui::avkit::macos::GetMetalTexture(surface), .alpha = MetalTexture::Alpha::Opaque});
            if (work && work->config) {
              try {
                if (work->cue) {
                  MetalTexture::Origin origin;
                  auto snapshot = mailbox->AcquireFrame(origin);
                  Check(snapshot != nil && origin == MetalTexture::Origin::TopLeft && HasPreviewStickerCue(snapshot, work->pts),
                        "Live preview misplaced its ECS sticker at recording PTS");
                }
                recording->OfferVideo(renderer.EncoderFrame(surface, *work->config, work->pts));
              }
              catch (const std::exception& error) { recording->Fail(error.what()); }
            }
          }
        }
      } catch (const std::exception& error) {
        {
          std::lock_guard lock(inbox->mutex);
          inbox->error = error.what();
        }
        if (auto recording = Current()) recording->Fail(error.what());
      }
      mailbox->Finish();
    }
  }

  std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
  std::shared_ptr<MetalTexture> mailbox = std::make_shared<MetalTexture>(huxerui::Size{640, 360});
  std::atomic_bool robot{true};
  std::shared_ptr<const camera::FrameObserver> observer;
  std::jthread worker;
};

LiveCapture::LiveCapture() : impl_(std::make_unique<Impl>()) {}
LiveCapture::~LiveCapture() = default;

std::shared_ptr<const camera::FrameObserver> LiveCapture::Observer() const { return impl_->observer; }
std::shared_ptr<ExternalTexture> LiveCapture::Texture() const { return impl_->mailbox; }

LiveCaptureStatus LiveCapture::Status() const {
  std::shared_ptr<Recording> recording;
  {
    std::lock_guard lock(impl_->inbox->mutex);
    if (!impl_->inbox->error.empty()) return {.state = State::Failed, .error = impl_->inbox->error};
    recording = impl_->inbox->recording;
  }
  return recording ? recording->Status() : LiveCaptureStatus{};
}

void LiveCapture::Start(File file, audio::AudioReader reader, AudioFormat format) {
  const auto status = Status();
  Check(status.state == State::Idle || status.state == State::Complete || status.state == State::Failed,
        "A recording is already in progress");
  auto recording = std::make_shared<Recording>(std::move(file), std::move(reader), format, impl_->robot);
  {
    std::lock_guard lock(impl_->inbox->mutex);
    Check(impl_->inbox->error.empty(), "The capture renderer is unavailable");
    impl_->inbox->recording = recording;
  }
  recording->Launch();
}

void LiveCapture::SetRobotVoice(bool enabled) { impl_->robot.store(enabled, std::memory_order_relaxed); }
void LiveCapture::Pause(bool paused) { if (auto recording = impl_->Current()) recording->Pause(paused); }
void LiveCapture::Stop() { if (auto recording = impl_->Current()) recording->Stop(); }
void LiveCapture::Interrupt(std::string reason) {
  if (auto recording = impl_->Current()) recording->Fail(std::move(reason));
}
void LiveCapture::ScheduleVerificationCue() { if (auto recording = impl_->Current()) recording->ScheduleCue(); }

void VerifyLiveMovie(const File& output, const LiveCaptureStatus& expected) {
  const auto asset = ImportMovie(output);
  const MediaTimestamp duration{expected.samples, expected.sample_rate};
  Check(asset.video_range.start == MediaTimestamp{} && asset.audio_range.start == MediaTimestamp{} &&
        asset.video_range.end == duration && asset.audio_range.end == duration && expected.cue_sample.has_value(),
        "MP4 tracks do not exactly match the capture cutoff");
  Check(asset.pcm.channel_layout == media::AudioChannelLayout::Mono, "The verification cue expects mono PCM");
  MovieReader reader(asset, {CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()}, CodecPolicy::SoftwareOnly);
  Tick video_end{}, audio_end{};
  std::size_t pictures{};
  std::optional<Tick> video_cue;
  std::vector<std::int16_t> pcm;
  pcm.reserve(expected.samples);
  while (auto frame = reader.ReadVideo()) {
    Check(frame->Validate() && frame->format.pixel_format == VideoPixelFormat::I420 && frame->duration &&
              frame->pts == MediaTimestamp{video_end, expected.sample_rate},
          "Recorded video PTS contains a gap or duplicate");
    if (!video_cue && HasStickerCue(*frame)) video_cue = video_end;
    video_end += Rescale(*frame->duration, expected.sample_rate);
    ++pictures;
  }
  while (auto frame = reader.ReadAudio()) {
    Check(frame->Validate() && frame->pts == MediaTimestamp{audio_end, expected.sample_rate},
          "Recorded PCM PTS contains a gap or duplicate");
    pcm.resize(audio_end + frame->sample_count);
    std::memcpy(pcm.data() + audio_end, frame->planes.front().AsBytes().data(), frame->sample_count * sizeof(std::int16_t));
    audio_end += frame->sample_count;
  }
  Check(video_end == expected.samples && audio_end == expected.samples && pictures == expected.video_frames,
        "Decoded recording does not end at the common cutoff");
  const auto audio_cue = FindVoiceCue(pcm, expected.sample_rate);
  Check(video_cue && audio_cue && *video_cue >= *expected.cue_sample &&
            std::abs(*audio_cue - *expected.cue_sample) <= expected.sample_rate / 50 &&
            std::abs(*video_cue - *audio_cue) <= expected.sample_rate * 8 / 100,
        "The decoded picture cue and robot voice sidebands do not align");
  std::cout << "AVKit MP4 " << pictures << " frames, " << audio_end << '/' << expected.sample_rate
            << " s, paused " << expected.paused_samples << " samples, cue " << *expected.cue_sample
            << '/' << expected.sample_rate << " s, decoded A/V cue delta "
            << (1000.0 * (*video_cue - *audio_cue) / expected.sample_rate) << " ms " << output.Path() << std::endl;
  std::cout << "ECS sticker position verified at recording PTS in the live preview and reimported MP4." << std::endl;
}

} // namespace avkit_preview
