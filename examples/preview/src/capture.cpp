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
#include "workbench.h"

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
  auto robot_voice = UseState(true);
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
  else if (live_status.state == RecordingState::Complete && label.empty()) label = "Saved avkit.mp4";
  const auto latest_photo = UseState(std::optional<File>{});
  const auto latest_movie = UseState(std::optional<File>{});
  const auto settings_open = UseState(false);
  const auto picker = UseService<FilePicker>();
  const bool wide = UseViewportClass() == ViewportClass::Expanded;
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
    View monitor = WorkbenchPanel(Column{
      Column{std::move(image)}.With(Frame{.height = wide ? 378.0F : 280.0F}, CornerRadius(10.0F), Background(Color::Rgb(8, 11, 17)),
          CrossAlign(CrossAxisAlignment::Stretch)),
      Text::Format("{} · {} 秒", paused ? "已暂停" : recording ? "录制中" : finalizing ? "正在写入" :
          completed ? "录像已保存" : ready ? "相机已就绪" : "正在准备相机",
          WorkbenchSeconds(live_status.sample_rate ? double(live_status.samples) / live_status.sample_rate : 0.0, 1)),
      Flow{
        Button(can_stop ? "停止录像" : "开始录像").OnClick([=] {
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
        }).With(Enabled(can_stop || (ready && !live_busy))),
        Button(paused ? "继续" : "暂停").OnClick([live, paused] { live->Pause(!paused); }).With(Enabled(paused || recording)),
        Button("拍照").OnClick([=] {
          tasks.Launch([=]() -> Task<void> {
            try {
              auto file = WorkbenchOutput(directory, "Captures", "照片.jpg");
              co_await SavePhoto(camera, file);
              latest_photo = std::move(file);
              message = std::string("照片已保存");
            } catch (const std::exception& error) { message = std::string(error.what()); }
          });
        }).With(Enabled(ready && !live_busy && !status.capturing_photo)),
      }.With(Spacing(8.0F)),
      live_status.error.empty() && !status.error ? View(Text(message.Get())) :
          View(Text(status.error ? status.error->message : live_status.error)),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme);
    std::vector<View> controls{
        Text("录制设置", TextRole::Title),
        Text("画面 · AVKit 贴纸"),
        Button(robot_voice ? "声音 · 机器人" : "声音 · 原声").OnClick([robot_voice, live] {
          const bool enable = !robot_voice;
          robot_voice = enable;
          live->SetRobotVoice(enable);
        }),
    };
    View settings = WorkbenchPanel(Column(std::move(controls)).With(Spacing(8.0F),
        CrossAlign(CrossAxisAlignment::Stretch)), theme);
    std::vector<View> output;
    if (completed) {
      output.emplace_back(Button("加入剪辑").OnClick([file = *latest_movie.Get(), add = workbench->add_movie] {
        add(file);
      }));
      if (picker->CanSaveFiles()) output.emplace_back(Button("保存录像副本").OnClick([=] { save_copy(*latest_movie.Get()); }));
    }
    if (latest_photo.Get() && picker->CanSaveFiles())
      output.emplace_back(Button("保存照片副本").OnClick([=] { save_copy(*latest_photo.Get()); }));
    View body = wide ? View(Row{std::move(monitor).With(Grow()), std::move(settings).With(Frame{.width = 248.0F})}
                         .With(Spacing(12.0F))) :
        View(Column{std::move(monitor), Button(settings_open ? "收起录制设置" : "录制设置")
                        .OnClick([settings_open] { settings_open = !settings_open.Get(); }),
                    settings_open.Get() ? std::move(settings) : View(Text(""))}
             .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)));
    return ScrollView(Column{
      Text("录制", TextRole::Title),
      std::move(body),
      Flow(std::move(output)).With(Spacing(8.0F)),
    }.With(Padding(wide ? 20.0F : 12.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)));
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
View WorkbenchCapture(File data, State<bool> occupied, std::function<void(File)> add_movie) {
  const auto live = UseState(std::shared_ptr<LiveCapture>{});
  Lifecycle([live] {
    auto active = std::make_shared<LiveCapture>();
    live = active;
    return [active] { active->Close(); };
  });
  if (!live.Get()) return Text("正在准备相机…").With(Padding(16.0F));
  return CapturePanel(std::move(data), CapturePreviewMode::Interactive, live.Get(),
                      WorkbenchRecording{occupied, std::move(add_movie)});
}

} // namespace avkit_preview

