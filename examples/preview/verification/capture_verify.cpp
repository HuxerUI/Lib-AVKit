#include "capture_verify.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <huxerui/camera.h>
#include <huxerui/huxerui.h>
#include <huxerui/media_library.h>
#include <huxerui/avkit.h>

#include "timeline_resources.h"
#include "capture/camera_recording_verification.h"
#include "workbench_media.h"

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace std::chrono_literals;
using huxerui::avkit::CameraRecording;
using huxerui::avkit::CameraRecordingStatus;
using huxerui::avkit::detail::CameraRecordingVerification;

struct CaptureOptions {
  File directory;
  bool composited;
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

Task<void> VerifyCameraComposite(const std::shared_ptr<CameraRecording>& live) {
  const auto texture = live->Texture();
  for (const auto deadline = std::chrono::steady_clock::now() + 6s;
       texture->Revision() < 3 && std::chrono::steady_clock::now() < deadline;) {
    const auto status = live->Status();
    if (!status.error.empty()) throw std::runtime_error(status.error);
    co_await Delay(30ms);
  }
  if (texture->Revision() < 3 || texture != live->Texture() || !(co_await texture->SnapshotAsync())) {
    throw std::runtime_error("AVKit did not publish the processed camera frames");
  }
  std::cout << "Camera frames reached the shared bgfx compositor and the retained HuxerUI preview texture." << std::endl;
}

using RecordingState = CameraRecordingStatus::State;

Task<media::AudioFormat> WaitForMicrophone(audio::AudioCaptureSession microphone) {
  for (auto deadline = std::chrono::steady_clock::now() + 10s; std::chrono::steady_clock::now() < deadline;) {
    const auto status = microphone.Status();
    if (status.error) throw std::runtime_error(status.error->message);
    if (status.state == audio::SessionState::Running && status.format) co_return *status.format;
    co_await Delay(40ms);
  }
  throw std::runtime_error("Microphone delivered no PCM");
}

Task<void> StartLive(ApplicationHandle application, std::shared_ptr<CameraRecording> live,
                     audio::AudioCaptureSession microphone, State<bool> microphone_active, File file) {
  if (file.Exists()) throw std::runtime_error("Use a fresh output directory for avkit.mp4");
  co_await Authorize(application, Permission::Microphone);
  microphone_active = true;
  auto format = co_await WaitForMicrophone(microphone);
  live->Start(std::move(file), Require(microphone.OpenReader()), format);
}

Task<CameraRecordingStatus> WaitForLive(std::shared_ptr<CameraRecording> live, RecordingState state) {
  for (auto deadline = std::chrono::steady_clock::now() + 12s; std::chrono::steady_clock::now() < deadline;) {
    auto status = live->Status();
    if (status.state == RecordingState::Failed) throw std::runtime_error(status.error);
    if (status.state == state) co_return status;
    co_await Delay(30ms);
  }
  throw std::runtime_error("AVKit recording did not reach the requested state");
}

Task<void> VerifyLive(ApplicationHandle application, std::shared_ptr<CameraRecording> live,
                      audio::AudioCaptureSession microphone, State<bool> microphone_active, const File& output) {
  live->SetRobotVoice(false);
  co_await StartLive(application, live, microphone, microphone_active, output);
  (void)co_await WaitForLive(live, RecordingState::Recording);
  co_await Delay(800ms);
  live->Pause(true);
  co_await Delay(600ms);
  live->Pause(false);
  live->SetRobotVoice(true);
  CameraRecordingVerification::ScheduleCue(*live);
  co_await Delay(820ms);
  live->Stop();
  const auto status = co_await WaitForLive(live, RecordingState::Complete);
  microphone_active = false;
  if (status.paused_samples < status.sample_rate / 2 || status.samples < status.sample_rate ||
      status.samples > status.sample_rate * 2) {
    throw std::runtime_error("The recording did not exclude the paused interval");
  }
  VerifyLiveMovie(output, status, CameraRecordingVerification::CueSample(*live));
}

[[huxerui::composable]]
View CaptureVerificationPanel(File directory, bool composited) {
  const auto application = UseApplication();
  const auto tasks = UseTaskScope();
  const auto active = UseState(false);
  const auto microphone_active = UseState(false);
  const auto recorder = UseState(std::shared_ptr<CameraRecording>{});
  const auto resources = UseService<Resources>();
  Lifecycle([=]() -> std::function<void()> {
    if (!composited) return [] {};
    constexpr huxerui::avkit::PixelSize canvas{640, 360};
    auto session = std::make_shared<CameraRecording>(canvas,
        PreviewTimelineResources(LoadOverlayAssets(*resources)));
    session->SetSticker(CaptureSticker(canvas));
    recorder = session;
    return [session] { session->Close(); };
  }, composited);
  const auto live = recorder.Get();
  const auto microphone = audio::UseAudioCapture({.active = microphone_active, .channel_layout = media::AudioChannelLayout::Mono});
  camera::CameraOptions camera_options{.active = active, .preferred_capture_pixel_width = 640,
      .preferred_capture_pixel_height = 480, .preferred_capture_fps = 30};
  if (live) live->Connect(camera_options);
  const auto camera = camera::UseCamera(std::move(camera_options));

  Lifecycle([=] {
    if (composited && !live) return;
    tasks.Launch([=]() -> Task<void> {
      try {
        co_await Authorize(application, Permission::Camera);
        active = true;
        co_await WaitForCamera(camera);
        if (!composited) {
          co_await VerifyCameraCapture(camera, tasks, directory);
        } else {
          co_await VerifyCameraComposite(live);
          co_await VerifyLive(application, live, microphone, microphone_active, File(directory, "avkit.mp4"));
        }
        verification_exit = 0;
      } catch (const std::exception& error) {
        if (live) live->Cancel(error.what());
        std::cerr << error.what() << std::endl;
      }
      application.Quit();
    });
  }, live);

  return live ? WorkbenchVideoPicture(live->Texture()).With(Grow()) :
                camera::CameraPreview(camera, {.fit = ImageFit::Contain}).With(Grow());
}

} // namespace

View CaptureVerificationApp() {
  return CaptureVerificationPanel(options->directory, options->composited);
}

int VerifyCapture(std::string_view directory, bool composited) {
  try {
    File output(directory);
    if (!output.IsDirectory() && !output.CreateDirectories()) throw std::runtime_error("Cannot create the capture directory");
    options.emplace(CaptureOptions{std::move(output), composited});
    // AppKit can terminate the process after runtime shutdown without returning to main.
    if (std::atexit([] { if (verification_exit != 0) std::_Exit(verification_exit); }) != 0)
      throw std::runtime_error("Could not register the capture verification exit status");
    const auto result = RunApplication();
    options.reset();
    if (result != 0) verification_exit = result;
    return verification_exit;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

} // namespace avkit_preview

const huxerui::Application application{
    avkit_preview::CaptureVerificationApp,
    {
        .window = {.title = "AVKit Capture Verification", .initial_size = {640.0F, 480.0F}},
        .show_debug_overlay = false,
        .application_hooks = {huxerui::avkit::Install, huxerui::media::Install, huxerui::camera::Install, huxerui::audio::Install},
    }
};
