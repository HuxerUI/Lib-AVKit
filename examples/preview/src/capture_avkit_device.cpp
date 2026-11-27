#include "capture_device.h"

#include "timeline_resources.h"
#include "workbench_media.h"
#include "workbench_platform.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#include <huxerui/avkit/camera_recording.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace std::chrono_literals;
using RecordingState = CameraRecordingStatus::State;

} // namespace

CaptureDevice UseCaptureDevice(State<bool> robot, State<bool> sticker) {
  const auto application = UseApplication();
  const auto tasks = UseTaskScope();
  const auto resources = UseService<Resources>();
  // Keep the preview and encoder geometry stable for the lifetime of the capture page.
  const auto canvas = UseState(UseViewportClass() == ViewportClass::Compact ? PixelSize{360, 640} : PixelSize{640, 360}).Get();
  const auto recorder = UseState(std::shared_ptr<CameraRecording>{});
  const auto observed = UseState(CameraRecordingStatus{});
  const auto camera_active = UseState(false);
  const auto microphone_active = UseState(false);
  const auto permission_error = UseState(std::string{});
  const auto recording = recorder.Get();
  const auto microphone = audio::UseAudioCapture({.active = microphone_active, .sample_rate = 48'000,
      .channel_layout = media::AudioChannelLayout::Mono});

  camera::CameraOptions options{.active = bool(recording) && camera_active.Get(), .preferred_capture_pixel_width = 1280,
      .preferred_capture_pixel_height = 720, .preferred_capture_fps = 30};
  if (recording) recording->Connect(options);
  const auto camera = camera::UseCamera(std::move(options));
  const auto camera_status = camera.Status();

  Lifecycle([=] {
    auto active = std::make_shared<CameraRecording>(canvas,
        SceneConfiguration{.resources = LoadWorkbenchResources(*resources)},
        [](const CodecFallback& fallback) {
          LogWorkbench(WorkbenchLogLevel::Warning, "Recording codec fallback %s to %s: %s",
              fallback.from.c_str(), fallback.to.c_str(), fallback.cause.message.c_str());
        });
    recorder = active;
    tasks.Launch([=]() -> Task<void> {
      try {
        co_await RequestCapturePermission(application, Permission::Camera);
        camera_active = true;
      } catch (const std::exception& error) {
        permission_error = std::string(error.what());
      }
    });
    return [active] { active->Close(); };
  });

  const bool robot_enabled = robot.Get(), sticker_enabled = sticker.Get();
  Lifecycle([=] {
    if (recording) {
      recording->SetVoiceEffect(robot_enabled ? std::optional{VoiceEffect::Robot} : std::nullopt);
      recording->SetOverlays(sticker_enabled ? std::vector{CaptureSticker(canvas)} : std::vector<OverlayInstance>{});
    }
  }, recording, robot_enabled, sticker_enabled, canvas);

  Lifecycle([recording, observed, tasks] {
    if (!recording) return;
    tasks.Launch([=]() -> Task<void> {
      for (;;) {
        const auto status = recording->Status();
        if (status != observed.Get()) observed = status;
        co_await Delay(60ms);
      }
    });
  }, recording);

  const bool paused = observed->state == RecordingState::Paused;
  const bool ready = recording && camera_status.state == camera::SessionState::Running && camera_status.preview;
  return {
    .preview = ready ? WorkbenchVideoPicture(recording->Texture()).With(Grow()) : View{},
    .ready = ready,
    .recording = paused || observed->state == RecordingState::Recording,
    .paused = paused,
    .sticker_available = true,
    .seconds = observed->sample_rate ? double(observed->samples) / observed->sample_rate : 0.0,
    .error = camera_status.error ? camera_status.error->message : permission_error.Get(),
    .photo = [recording](File destination) -> Task<void> {
      if (!recording) throw std::runtime_error("相机未就绪");
      co_await recording->CapturePhoto(std::move(destination));
    },
    .record = [=](File destination) -> Task<void> {
      if (!recording) throw std::runtime_error("录制未就绪");
      co_await RequestCapturePermission(application, Permission::Microphone);
      microphone_active = true;
      try {
        const auto status = co_await recording->Record(std::move(destination), camera, microphone);
        LogWorkbench(WorkbenchLogLevel::Info, "Recorded %zu frames, %lld samples at %u Hz, paused %lld samples",
            status.video_frames, static_cast<long long>(status.samples), status.sample_rate,
            static_cast<long long>(status.paused_samples));
      } catch (...) {
        microphone_active = false;
        throw;
      }
      microphone_active = false;
    },
    .pause = [recording](bool paused) { if (recording) recording->Pause(paused); },
    .stop = [recording] { if (recording) recording->Stop(); },
  };
}

} // namespace avkit_preview
