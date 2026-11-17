#include "capture_preview.h"

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <huxerui/camera.h>
#include <huxerui/huxerui.h>

#include "live_capture.h"

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace std::chrono_literals;

struct CaptureOptions {
  File directory;
  CapturePreviewMode mode;
  std::shared_ptr<LiveCapture> live;
};
std::optional<CaptureOptions> options;
int verification_exit = 1;

template<class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

Task<void> Authorize(ApplicationHandle application, Permission permission) {
  if (co_await application.CheckPermissionAsync(permission) != PermissionStatus::Granted &&
      co_await application.RequestPermissionAsync(permission) != PermissionStatus::Granted) {
    throw std::runtime_error(permission == Permission::Camera ? "Camera access is required" : "Microphone access is required");
  }
}

Task<void> WaitForCamera(camera::CameraSession camera) {
  for (auto deadline = std::chrono::steady_clock::now() + 12s; std::chrono::steady_clock::now() < deadline;) {
    const auto status = camera.Status();
    if (status.error) throw std::runtime_error(status.error->message);
    if (status.state == camera::SessionState::Running && status.preview) co_return;
    co_await Delay(40ms);
  }
  throw std::runtime_error("Camera delivered no preview");
}

Task<void> SavePhoto(camera::CameraSession camera, File destination) {
  const auto photo = Require(co_await camera.CapturePhotoAsync());
  auto stream = Require(destination.OpenRandomAccess(FileAccessMode::ReadWrite, FileOpenMode::CreateNew));
  Require(stream.WriteAt(0, photo.EncodedBytes()));
  Require(stream.Close());
  std::cout << "Camera JPEG " << photo.PixelWidth() << 'x' << photo.PixelHeight() << ' '
            << photo.EncodedBytes().size() << " bytes " << destination.Path() << std::endl;
}

Task<void> VerifyCameraCapture(camera::CameraSession camera, TaskScope tasks, const File& directory) {
  co_await SavePhoto(camera, File(directory, "photo.jpg"));
  tasks.Launch([camera]() -> Task<void> {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!camera.Status().recording_video && std::chrono::steady_clock::now() < deadline) co_await Delay(20ms);
    co_await Delay(1200ms);
    camera.StopRecording();
  });
  const File movie(directory, "camera.mp4");
  Require(co_await camera.RecordVideoAsync(movie));
  const auto duration = camera.RecordedDuration();
  if (duration < 500ms) throw std::runtime_error("Camera recorded too little video");
  std::cout << "Camera MP4 " << duration.count() << " us " << movie.Path() << std::endl;
}

using RecordingState = LiveCaptureStatus::State;

bool RecordingInProgress(RecordingState state) {
  return state == RecordingState::Starting || state == RecordingState::Recording ||
         state == RecordingState::Paused || state == RecordingState::Finishing;
}

Task<media::AudioFormat> WaitForMicrophone(audio::AudioCaptureSession microphone) {
  for (auto deadline = std::chrono::steady_clock::now() + 10s; std::chrono::steady_clock::now() < deadline;) {
    const auto status = microphone.Status();
    if (status.error) throw std::runtime_error(status.error->message);
    if (status.state == audio::SessionState::Running && status.format) co_return *status.format;
    co_await Delay(40ms);
  }
  throw std::runtime_error("Microphone delivered no PCM");
}

Task<void> StartLive(ApplicationHandle application, std::shared_ptr<LiveCapture> live,
                     audio::AudioCaptureSession microphone, State<bool> microphone_active, File file) {
  if (file.Exists()) throw std::runtime_error("Use a fresh output directory for avkit.mp4");
  co_await Authorize(application, Permission::Microphone);
  microphone_active = true;
  auto format = co_await WaitForMicrophone(microphone);
  live->Start(std::move(file), Require(microphone.OpenReader()), format);
}

Task<LiveCaptureStatus> WaitForLive(std::shared_ptr<LiveCapture> live, RecordingState state) {
  for (auto deadline = std::chrono::steady_clock::now() + 12s; std::chrono::steady_clock::now() < deadline;) {
    auto status = live->Status();
    if (status.state == RecordingState::Failed) throw std::runtime_error(status.error);
    if (status.state == state) co_return status;
    co_await Delay(30ms);
  }
  throw std::runtime_error("AVKit recording did not reach the requested state");
}

Task<void> VerifyLive(ApplicationHandle application, std::shared_ptr<LiveCapture> live,
                      audio::AudioCaptureSession microphone, State<bool> microphone_active, const File& output) {
  live->SetRobotVoice(false);
  co_await StartLive(application, live, microphone, microphone_active, output);
  (void)co_await WaitForLive(live, RecordingState::Recording);
  co_await Delay(800ms);
  live->Pause(true);
  co_await Delay(600ms);
  live->Pause(false);
  live->SetRobotVoice(true);
  live->ScheduleVerificationCue();
  co_await Delay(820ms);
  live->Stop();
  const auto status = co_await WaitForLive(live, RecordingState::Complete);
  microphone_active = false;
  if (status.paused_samples < status.sample_rate / 2 || status.samples < status.sample_rate ||
      status.samples > status.sample_rate * 2) {
    throw std::runtime_error("The recording did not exclude the paused interval");
  }
  VerifyLiveMovie(output, status);
}

} // namespace

void ConfigureCapturePreview(std::string_view directory, CapturePreviewMode mode) {
  File output(directory);
  if (!output.IsDirectory() && !output.CreateDirectories()) throw std::runtime_error("Cannot create the capture directory");
  options.emplace(CaptureOptions{std::move(output), mode});
  if (mode != CapturePreviewMode::VerifyCamera) options->live = std::make_shared<LiveCapture>();
}

bool HasCapturePreview() { return options.has_value(); }

int CapturePreviewExitCode(int app_result) {
  const int result = options->mode != CapturePreviewMode::Interactive && app_result == 0 ? verification_exit : app_result;
  options.reset();
  return result;
}

[[huxerui::composable]]
View CapturePreview() {
  const auto application = UseApplication();
  const auto tasks = UseTaskScope();
  auto active = UseState(false);
  auto message = UseState(std::string{});
  const auto live = options->live;
  auto microphone_active = UseState(false);
  auto microphone_starting = UseState(false);
  auto robot_voice = UseState(true);
  auto observed_live = UseState(LiveCaptureStatus{});
  const auto microphone = audio::UseAudioCapture({.active = microphone_active, .channel_layout = media::AudioChannelLayout::Mono});
  const auto camera = camera::UseCamera({.active = active, .preferred_capture_pixel_width = 640,
      .preferred_capture_pixel_height = 480, .preferred_capture_fps = 30,
      .frame_observer = live ? live->Observer() : nullptr,
  });
  const auto status = camera.Status();
  const auto directory = options->directory;
  const auto mode = options->mode;
  const bool verify = mode != CapturePreviewMode::Interactive;

  const auto start = [=]() -> Task<void> {
    try {
      co_await Authorize(application, Permission::Camera);
      active = true;
      if (verify) {
        co_await WaitForCamera(camera);
        if (mode == CapturePreviewMode::VerifyCamera) co_await VerifyCameraCapture(camera, tasks, directory);
        else co_await VerifyLive(application, live, microphone, microphone_active, File(directory, "avkit.mp4"));
        verification_exit = 0;
      }
    } catch (const std::exception& error) {
      message = std::string(error.what());
      std::cerr << error.what() << std::endl;
    }
    if (verify) application.Quit();
  };
  Lifecycle([tasks, start] { tasks.Launch(start); });

  const bool camera_interrupted = status.error.has_value() || status.state == camera::SessionState::Suspended;
  Lifecycle([live, camera_interrupted, error = status.error] {
    if (live && camera_interrupted) live->Interrupt(error ? error->message : "Camera capture was suspended");
  }, camera_interrupted, status.error);

  Lifecycle([tasks, live, observed_live, microphone_active, microphone_starting] {
    if (!live) return;
    tasks.Launch([live, observed_live, microphone_active, microphone_starting]() -> Task<void> {
      for (;;) {
        const auto next = live->Status();
        if (next != static_cast<LiveCaptureStatus>(observed_live)) observed_live = next;
        if ((next.state == RecordingState::Complete || next.state == RecordingState::Failed) && microphone_active && !microphone_starting)
          microphone_active = false;
        co_await Delay(80ms);
      }
    });
  });

  const bool ready = status.state == camera::SessionState::Running && status.preview.has_value();
  auto label = status.error ? status.error->message : std::string(message);
  View image = camera::CameraPreview(camera, {.fit = ImageFit::Contain, .mirror = camera::MirrorMode::Off}).With(Grow());
  bool live_busy = false;
  const LiveCaptureStatus live_status = observed_live;
  live_busy = RecordingInProgress(live_status.state) || microphone_starting;
  if (live && !status.recording_video) image = Image(live->Texture()).Fit(ImageFit::Contain).With(Grow());
  if (!live_status.error.empty()) label = live_status.error;
  else if (live_status.state == RecordingState::Complete && label.empty()) label = "Saved avkit.mp4";
  View camera_buttons = Row {
      Button("Photo").OnClick([tasks, camera, directory, message] {
        tasks.Launch([camera, directory, message]() -> Task<void> {
          try {
            co_await SavePhoto(camera, File(directory, "photo.jpg"));
            message = std::string("Saved photo.jpg");
          } catch (const std::exception& error) { message = std::string(error.what()); }
        });
      }).With(Enabled(!verify && ready && !status.capturing_photo)),
      Button(status.recording_video ? "Stop Camera recording" : "Record with Camera").OnClick([=] {
        if (camera.Status().recording_video) {
          camera.StopRecording();
          return;
        }
        if (microphone_starting || (live && RecordingInProgress(live->Status().state))) return;
        tasks.Launch([camera, directory, message]() -> Task<void> {
          try {
            Require(co_await camera.RecordVideoAsync(File(directory, "camera.mp4")));
            message = std::string("Saved camera.mp4");
          } catch (const std::exception& error) { message = std::string(error.what()); }
        });
      }).With(Enabled(!verify && ready && !live_busy)),
  }.With(Spacing(8.0F), MainAlign(MainAxisAlignment::Center));
  if (live) {
    const bool paused = live_status.state == RecordingState::Paused;
    const bool can_pause = live_status.state == RecordingState::Recording || paused;
    const bool can_stop = can_pause || live_status.state == RecordingState::Starting;
    return Column {
      image,
      camera_buttons,
      Row {
        Button(can_stop ? "Stop AVKit recording" : "Record with AVKit").OnClick([=] {
          if (RecordingInProgress(live->Status().state)) {
            live->Stop();
            return;
          }
          if (camera.Status().recording_video || microphone_starting) return;
          microphone_starting = true;
          message = std::string{};
          tasks.Launch([=]() -> Task<void> {
            try { co_await StartLive(application, live, microphone, microphone_active, File(directory, "avkit.mp4")); }
            catch (const std::exception& error) {
              message = std::string(error.what());
              microphone_active = false;
            }
            microphone_starting = false;
          });
        }).With(Enabled(!verify && !status.recording_video && (can_stop || (ready && !live_busy)))),
        Button(paused ? "Resume" : "Pause").OnClick([live, paused] { live->Pause(!paused); })
            .With(Enabled(!verify && can_pause)),
        Button(robot_voice ? "Robot voice" : "Original voice").OnClick([robot_voice, live] {
          const bool enable = !robot_voice;
          live->SetRobotVoice(enable);
          robot_voice = enable;
        }).With(Enabled(!verify)),
      }.With(Spacing(8.0F), MainAlign(MainAxisAlignment::Center)),
      Text(label),
    }.With(Padding(8.0F), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
  }
  return Column {
    image,
    camera_buttons,
    Text(label),
  }.With(Padding(8.0F), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

} // namespace avkit_preview

