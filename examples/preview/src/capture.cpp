#include "capture_preview.h"

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <huxerui/camera.h>
#include <huxerui/huxerui.h>

#include "live_capture.h"
#include "workbench_media.h"

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace std::chrono_literals;

enum class CapturePreviewMode { Interactive, VerifyCamera, VerifyLive };

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

Task<void> VerifyCameraComposite(const std::shared_ptr<LiveCapture>& live) {
  const auto texture = live->Texture();
  for (const auto deadline = std::chrono::steady_clock::now() + 6s;
       texture->Revision() < 3 && std::chrono::steady_clock::now() < deadline;) {
    const auto status = live->Status();
    if (!status.error.empty()) throw std::runtime_error(status.error);
    co_await Delay(30ms);
  }
  if (texture->Revision() < 3 || !(co_await texture->SnapshotAsync())) {
    throw std::runtime_error("AVKit did not publish the processed camera frames");
  }
  std::cout << "Camera frames reached the shared bgfx compositor and the retained HuxerUI preview texture." << std::endl;
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

void ConfigureCapturePreview(std::string_view directory, CapturePreviewMode mode) {
  File output(directory);
  if (!output.IsDirectory() && !output.CreateDirectories()) throw std::runtime_error("Cannot create the capture directory");
  options.emplace(CaptureOptions{std::move(output), mode});
  options->live = std::make_shared<LiveCapture>();
}

int CapturePreviewExitCode(int app_result) {
  const int result = options->mode != CapturePreviewMode::Interactive && app_result == 0 ? verification_exit : app_result;
  options.reset();
  return result;
}

struct WorkbenchRecording {
  State<bool> occupied;
  std::function<void(File)> add_movie;
  std::function<void()> import_movie;
};

[[huxerui::composable]]
View CapturePanel(File directory, CapturePreviewMode mode, std::shared_ptr<LiveCapture> live,
                  std::optional<WorkbenchRecording> workbench = {}) {
  const auto application = UseApplication();
  const auto tasks = UseTaskScope();
  auto active = UseState(false);
  auto message = UseState(std::string{});
  auto microphone_active = UseState(false);
  auto microphone_starting = UseState(false);
  auto robot_voice = UseState(!workbench.has_value());
  auto observed_live = UseState(LiveCaptureStatus{});
  const auto microphone = audio::UseAudioCapture({.active = microphone_active, .channel_layout = media::AudioChannelLayout::Mono});
  const auto camera = camera::UseCamera({.active = active, .preferred_capture_pixel_width = 640,
      .preferred_capture_pixel_height = 480, .preferred_capture_fps = 30,
      .frame_observer = live ? live->Observer() : nullptr,
  });
  const auto status = camera.Status();
  const bool verify = mode != CapturePreviewMode::Interactive;

  const auto start = [=]() -> Task<void> {
    try {
      co_await Authorize(application, Permission::Camera);
      active = true;
      if (verify) {
        co_await WaitForCamera(camera);
        if (mode == CapturePreviewMode::VerifyCamera) {
          co_await VerifyCameraComposite(live);
          co_await VerifyCameraCapture(camera, tasks, directory);
        } else {
          co_await VerifyLive(application, live, microphone, microphone_active, File(directory, "avkit.mp4"));
        }
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

  Lifecycle([tasks, live, observed_live, microphone_active, microphone_starting, workbench] {
    if (!live) return;
    tasks.Launch([live, observed_live, microphone_active, microphone_starting, workbench]() -> Task<void> {
      for (;;) {
        const auto next = live->Status();
        if (next != static_cast<LiveCaptureStatus>(observed_live)) observed_live = next;
        if ((next.state == RecordingState::Complete || next.state == RecordingState::Failed) && microphone_active && !microphone_starting)
          microphone_active = false;
        if (workbench && workbench->occupied && !microphone_starting && !RecordingInProgress(next.state))
          workbench->occupied = false;
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
  else if (live_status.state == RecordingState::Complete && label.empty()) label = workbench ? "录像已保存" : "Saved avkit.mp4";
  const auto latest_photo = UseState(std::optional<File>{});
  const auto latest_movie = UseState(std::optional<File>{});
  const auto photo_mode = UseState(false);
  const auto picker = UseService<FilePicker>();
  const auto& theme = UseTheme();
  if (workbench && live) {
    const bool paused = live_status.state == RecordingState::Paused;
    const bool recording = live_status.state == RecordingState::Recording;
    const bool finalizing = live_status.state == RecordingState::Finishing;
    const bool can_stop = paused || recording || live_status.state == RecordingState::Starting;
    const bool completed = live_status.state == RecordingState::Complete && latest_movie.Get();
    const auto save_copy = [picker, tasks, message](File file) {
      tasks.Launch([picker, file = std::move(file), message]() -> Task<void> {
        try {
          if (co_await picker->SaveFileAsync(file)) message = std::string("已保存副本");
        } catch (const std::exception& error) { message = std::string(error.what()); }
      });
    };
    const auto elapsed = live_status.sample_rate ? double(live_status.samples) / live_status.sample_rate : 0.0;
    const auto status_text = paused ? "已暂停" : recording ? "录制中" : finalizing ? "正在写入" :
        completed ? "已保存" : ready ? "待拍摄" : "准备相机";
    const auto photo = [=] {
      tasks.Launch([=]() -> Task<void> {
        try {
          auto file = WorkbenchOutput(directory, "Captures", "照片.jpg");
          co_await SavePhoto(camera, file);
          latest_photo = std::move(file);
          message = std::string("照片已保存");
        } catch (const std::exception& error) { message = std::string(error.what()); }
      });
    };
    const auto record = [=] {
      if (RecordingInProgress(live->Status().state)) { live->Stop(); return; }
      if (microphone_starting) return;
      microphone_starting = true;
      workbench->occupied = true;
      message = std::string{};
      tasks.Launch([=]() -> Task<void> {
        try {
          auto file = WorkbenchOutput(directory, "Captures", "录像.mp4");
          co_await StartLive(application, live, microphone, microphone_active, file);
          latest_movie = std::move(file);
        } catch (const std::exception& error) {
          message = std::string(error.what());
          microphone_active = false;
        }
        microphone_starting = false;
      });
    };
    const bool taking_photo = photo_mode.Get();
    View picture = !ready ? View(Column{
        Text(status.error || !label.empty() ? "相机不可用" : "正在连接相机…")
            .With(Foreground(theme.colors.on_surface_variant)),
      }.With(MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center))) :
        taking_photo ? View(camera::CameraPreview(camera, {.fit = ImageFit::Contain, .mirror = camera::MirrorMode::Off})) :
        WorkbenchVideoPicture(live->Texture());
    View top = Row{
      View(Spacer()).With(Grow()),
      WorkbenchControl(Button(robot_voice ? "机器人变声" : "原声").OnClick([robot_voice, live] {
        robot_voice = !robot_voice.Get();
        live->SetRobotVoice(robot_voice.Get());
      }).With(Enabled(!taking_photo)), theme, WorkbenchActionTone::Surface, true),
      View(Spacer()).With(Grow()),
      Text(taking_photo ? "4:3" : "16:9").Style(
          TextStyle{.font = Font::Monospace(11.0F), .foreground = theme.colors.on_surface_variant}),
    }.With(Padding(10.0F), CrossAlign(CrossAxisAlignment::Center));
    View overlay = Column{
      std::move(top),
      View(Spacer()).With(Grow()),
      live_busy || completed ? View(Row{
        Text(status_text + std::string("  ") + WorkbenchTimelineTime(elapsed, true)).Style(
            TextStyle{.font = Font::Monospace(13.0F), .foreground = Color::White()})
            .With(Padding(8.0F), Background(Color::Rgb(20, 21, 25, 0.9F)), CornerRadius(5.0F)),
      }.With(Padding(10.0F), MainAlign(MainAxisAlignment::Center))) : View{},
    }.With(CrossAlign(CrossAxisAlignment::Stretch));
    View modes = Row{
      WorkbenchTab("照片", theme, [photo_mode] { photo_mode = true; }, taking_photo, !live_busy),
      WorkbenchTab("视频", theme, [photo_mode] { photo_mode = false; }, !taking_photo, !live_busy),
    }.With(MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center), Spacing(12.0F));
    const bool photo_saved = taking_photo && latest_photo.Get() && picker->CanSaveFiles();
    const bool can_save = taking_photo ? photo_saved : completed && picker->CanSaveFiles();
    const auto left = paused || recording ? std::function<void()>([live, paused] { live->Pause(!paused); }) :
        std::function<void()>([=] {
          if (can_save) save_copy(taking_photo ? *latest_photo.Get() : *latest_movie.Get());
          else if (workbench->import_movie) workbench->import_movie();
        });
    View controls = Row{
      WorkbenchControl(Button(paused ? "继续" : recording ? "暂停" : can_save ? "保存副本" : "导入视频").OnClick(left)
          .With(Enabled(paused || recording || can_save || (!live_busy && bool(workbench->import_movie)))),
          theme, WorkbenchActionTone::Quiet, true).With(Frame{.width = 76.0F}),
      WorkbenchShutter(theme, can_stop, taking_photo ? ready && !status.capturing_photo : can_stop || (ready && !live_busy),
          taking_photo ? "拍照" : can_stop ? "停止录像" : "开始录像",
          taking_photo ? std::function<void()>(photo) : std::function<void()>(record)),
      WorkbenchControl(Button("加入剪辑").OnClick([workbench, latest_movie] {
        if (latest_movie.Get()) workbench->add_movie(*latest_movie.Get());
      }).With(Enabled(completed)), theme, WorkbenchActionTone::Primary, true).With(Frame{.width = 76.0F}),
    }.With(Spacing(22.0F), Padding(EdgeInsets::Symmetric(4.0F, 5.0F)),
        MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center));
    View notice = label.empty() ? View{} : View(Text(label).With(
        Padding(EdgeInsets::Symmetric(10.0F, 4.0F)),
        Foreground(status.error || !live_status.error.empty() ? theme.colors.error : theme.colors.on_surface_variant),
        Align(HorizontalAlignment::Center, VerticalAlignment::Center)));
    return WorkbenchRecordLayout(theme, std::move(picture), std::move(overlay), std::move(modes), std::move(controls), std::move(notice));
  }

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

[[huxerui::composable]] View CapturePreview() {
  return CapturePanel(options->directory, options->mode, options->live);
}

} // namespace

void InstallPreviewCapture(ApplicationContext& context) {
  camera::Install(context);
  audio::Install(context);
}

std::optional<View> RequestedCapturePreview() {
  if (!options) return std::nullopt;
  return CapturePreview();
}

std::optional<int> RunCaptureCommand(int argc, const char** argv) {
  if (argc != 3) return std::nullopt;
  using enum CapturePreviewMode;
  const std::string_view command(argv[1]);
  if (command != "--camera-preview" && command != "--verify-camera" && command != "--verify-live-capture")
    return std::nullopt;
  try {
    const auto mode = command == "--verify-camera" ? VerifyCamera :
                      command == "--verify-live-capture" ? VerifyLive : Interactive;
    ConfigureCapturePreview(argv[2], mode);
    return CapturePreviewExitCode(RunApplication());
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

[[huxerui::composable]]
View WorkbenchCapture(File data, State<bool> occupied, std::function<void(File)> add_movie,
                      std::function<void()> import_movie) {
  const auto live = UseState(std::shared_ptr<LiveCapture>{});
  Lifecycle([live] {
    auto active = std::make_shared<LiveCapture>();
    active->SetRobotVoice(false);
    live = active;
    return [active] { active->Close(); };
  });
  if (!live.Get()) return Text("正在准备相机…").With(Padding(16.0F));
  return CapturePanel(std::move(data), CapturePreviewMode::Interactive, live.Get(),
                      WorkbenchRecording{occupied, std::move(add_movie), std::move(import_movie)});
}

} // namespace avkit_preview

