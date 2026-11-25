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

CanvasOverlay CaptureSticker(PixelSize canvas) {
  constexpr float width = 0.20F;
  const float height = width * canvas.width / canvas.height;
  return {.id = {1}, .kind = CanvasOverlayKind::Sticker, .destination = {0.69F, 0.12F, width, height},
          .resource = {"avkit.preview", "1", "pag-spark"}};
}

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
    auto active = std::make_shared<CameraRecording>(canvas, PreviewTimelineResources(LoadOverlayAssets(*resources)),
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
      recording->SetRobotVoice(robot_enabled);
      recording->SetSticker(sticker_enabled ? std::optional{CaptureSticker(canvas)} : std::nullopt);
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
    .photo = [camera](File destination) -> Task<void> {
      const auto photo = co_await camera.CapturePhotoAsync();
      if (!photo.Succeeded()) throw std::runtime_error(photo.Error().message);
      WriteCaptureFile(destination, photo.Value().EncodedBytes());
    },
    .record = [=](File destination) -> Task<void> {
      if (!recording) throw std::runtime_error("录制未就绪");
      co_await RequestCapturePermission(application, Permission::Microphone);
      microphone_active = true;
      try {
        const auto start_deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
          const auto status = microphone.Status();
          if (status.error) throw std::runtime_error(status.error->message);
          if (status.state == audio::SessionState::Running && status.format) {
            auto reader = microphone.OpenReader();
            if (!reader.Succeeded()) throw std::runtime_error(reader.Error().message);
            recording->Start(destination, std::move(reader).Value(), *status.format);
            break;
          }
          if (std::chrono::steady_clock::now() >= start_deadline) throw std::runtime_error("麦克风未返回音频");
          co_await Delay(40ms);
        }
        const auto capture_deadline = std::chrono::steady_clock::now() + 10s;
        auto finish_deadline = std::chrono::steady_clock::time_point::max();
        for (;;) {
          const auto status = recording->Status();
          if (status.state == RecordingState::Complete) {
            LogWorkbench(WorkbenchLogLevel::Info, "Recorded %zu frames, %lld samples at %u Hz, paused %lld samples",
                status.video_frames, static_cast<long long>(status.samples), status.sample_rate,
                static_cast<long long>(status.paused_samples));
            break;
          }
          if (status.state == RecordingState::Failed) throw std::runtime_error(status.error);
          const auto camera_state = camera.Status();
          if (camera_state.error || camera_state.state == camera::SessionState::Suspended)
            throw std::runtime_error(camera_state.error ? camera_state.error->message : "相机录制已中断");
          const auto now = std::chrono::steady_clock::now();
          if (status.state == RecordingState::Starting && now >= capture_deadline)
            throw std::runtime_error("相机和麦克风未就绪");
          if (status.state == RecordingState::Finishing && finish_deadline == std::chrono::steady_clock::time_point::max())
            finish_deadline = now + 20s;
          if (now >= finish_deadline) throw std::runtime_error("录像未能完成写入");
          co_await Delay(40ms);
        }
      } catch (const std::exception& error) {
        recording->Cancel(error.what());
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
