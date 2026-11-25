#include "capture_device.h"

#include <atomic>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

#include <huxerui/huxerui.h>

namespace avkit_preview {
namespace {
using namespace huxerui;
using namespace std::chrono_literals;

class VoiceProcessor final : public audio::PcmProcessor {
public:
  VoiceProcessor(media::AudioFormat format, std::shared_ptr<std::atomic_bool> enabled)
      : format_(format), enabled_(std::move(enabled)) {}

  audio::PcmTiming Timing() const noexcept override { return audio::PcmTiming::CaptureTime; }

  audio::CaptureResult<void> Process(std::span<const std::int16_t> pcm) override {
    output_.emplace(pcm.begin(), pcm.end());
    const auto channels = format_.ChannelCount();
    const auto rate = format_.sample_rate;
    const auto frames = pcm.size() / channels;
    if (enabled_->load(std::memory_order_relaxed)) {
      for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto position = (sample_ + frame) % rate;
        const auto gain = 0.15 + 0.85 * std::sin(2 * std::numbers::pi * 68 * position / rate);
        for (std::size_t channel = 0; channel < channels; ++channel) {
          auto& sample = (*output_)[frame * channels + channel];
          sample = static_cast<std::int16_t>(std::lround(sample * gain));
        }
      }
    }
    sample_ = (sample_ + frames) % rate;
    return audio::CaptureResult<void>::Success();
  }

  audio::CaptureResult<std::optional<audio::PcmBlock>> TryRead() override {
    return audio::CaptureResult<std::optional<audio::PcmBlock>>::Success(std::exchange(output_, std::nullopt));
  }

  audio::CaptureResult<void> Finish() override { return audio::CaptureResult<void>::Success(); }

private:
  media::AudioFormat format_;
  std::shared_ptr<std::atomic_bool> enabled_;
  std::optional<audio::PcmBlock> output_;
  std::size_t sample_{};
};

} // namespace

CaptureDevice UseCaptureDevice(State<bool> robot_voice, State<bool>) {
  const auto application = UseApplication();
  const auto tasks = UseTaskScope();
  const auto camera_active = UseState(false);
  const auto microphone_active = UseState(false);
  const auto permission_error = UseState(std::string{});
  const auto seconds = UseState(0.0);
  const auto enabled = UseState(std::make_shared<std::atomic_bool>(false)).Get();
  const auto voice = UseState(std::make_shared<const audio::PcmProcessorFactory>([enabled](const auto& format) {
    return std::make_unique<VoiceProcessor>(format, enabled);
  })).Get();
  const bool robot = robot_voice.Get();
  Lifecycle([enabled, robot] { enabled->store(robot, std::memory_order_relaxed); }, robot);
  const auto microphone = audio::UseAudioCapture({.active = microphone_active,
      .sample_rate = 48'000, .channel_layout = media::AudioChannelLayout::Mono, .pcm_processor = voice});
  const auto camera = camera::UseCamera({.active = camera_active, .preferred_capture_pixel_width = 1280,
      .preferred_capture_pixel_height = 720, .preferred_capture_fps = 30});
  const auto status = camera.Status();
  Lifecycle([application, tasks, camera_active, permission_error] {
    tasks.Launch([=]() -> Task<void> {
      try {
        co_await RequestCapturePermission(application, Permission::Camera);
        camera_active = true;
      } catch (const std::exception& error) { permission_error = std::string(error.what()); }
    });
  });
  Lifecycle([camera, tasks, seconds, recording = status.recording_video] {
    if (!recording) return;
    tasks.Launch([=]() -> Task<void> {
      while (camera.Status().recording_video) {
        seconds = std::chrono::duration<double>(camera.RecordedDuration()).count();
        co_await Delay(80ms);
      }
    });
  }, status.recording_video);

  return {
    .preview = camera::CameraPreview(camera, {.fit = ImageFit::Contain, .mirror = camera::MirrorMode::Off}).With(Grow()),
    .ready = status.state == camera::SessionState::Running && status.preview.has_value(),
    .recording = status.recording_video,
    .paused = status.recording_paused,
    .seconds = seconds.Get(),
    .error = status.error ? status.error->message : permission_error.Get(),
    .photo = [camera](File destination) -> Task<void> {
      const auto photo = co_await camera.CapturePhotoAsync();
      if (!photo.Succeeded()) throw std::runtime_error(photo.Error().message);
      WriteCaptureFile(destination, photo.Value().EncodedBytes());
    },
    .record = [=](File destination) -> Task<void> {
      seconds = 0.0;
      co_await RequestCapturePermission(application, Permission::Microphone);
      microphone_active = true;
      try {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
          const auto capture = microphone.Status();
          if (capture.error) throw std::runtime_error(capture.error->message);
          if (capture.state == audio::SessionState::Running && capture.format) break;
          if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("麦克风未返回音频");
          co_await Delay(40ms);
        }
        const auto result = co_await camera.RecordVideoAsync(destination, microphone);
        if (!result.Succeeded()) {
          (void)destination.Delete();
          throw std::runtime_error(result.Error().message);
        }
      } catch (...) {
        microphone_active = false;
        throw;
      }
      microphone_active = false;
    },
    .pause = [camera](bool paused) { if (paused) camera.PauseRecording(); else camera.ResumeRecording(); },
    .stop = [camera] { camera.StopRecording(); },
  };
}

} // namespace avkit_preview
