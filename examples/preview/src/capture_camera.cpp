#include "capture_preview.h"
#include "workbench_media.h"

#include <atomic>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

#include <huxerui/camera.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace std::chrono_literals;

Task<void> RequestCapturePermission(ApplicationHandle application, Permission permission) {
  if (co_await application.CheckPermissionAsync(permission) != PermissionStatus::Granted &&
      co_await application.RequestPermissionAsync(permission) != PermissionStatus::Granted) {
    throw std::runtime_error(permission == Permission::Camera ? "需要相机权限" : "需要麦克风权限");
  }
}

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

struct CaptureDevice {
  huxerui::View preview;
  bool ready{};
  bool recording{};
  bool paused{};
  double seconds{};
  std::string error;
  std::function<huxerui::Task<void>(huxerui::File)> photo;
  std::function<huxerui::Task<void>(huxerui::File)> record;
  std::function<void(bool)> pause;
  std::function<void()> stop;
};

void WriteCaptureFile(const File& file, std::span<const std::byte> bytes) {
  if (bytes.empty()) throw std::runtime_error("Capture returned an empty file");
  auto opened = file.OpenRandomAccess(FileAccessMode::ReadWrite, FileOpenMode::CreateNew);
  if (!opened.Succeeded()) throw std::runtime_error(opened.Error().message);
  auto stream = std::move(opened).Value();
  const auto written = stream.WriteAt(0, bytes);
  const auto closed = stream.Close();
  if (!written.Succeeded() || !closed.Succeeded()) {
    (void)file.Delete();
    throw std::runtime_error("Could not save the complete capture");
  }
}

CaptureDevice UseCaptureDevice(State<bool> robot_voice) {
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

} // namespace

void InstallPreviewCapture(ApplicationContext& context) {
  camera::Install(context);
  audio::Install(context);
}

std::optional<View> RequestedCapturePreview() { return std::nullopt; }
std::optional<int> RunCaptureCommand(int, const char**) { return std::nullopt; }

[[huxerui::composable]]
View WorkbenchCapture(File data, State<bool> occupied, std::function<void(File)> add_movie,
                      std::function<void()> import_movie) {
  const auto& theme = UseTheme();
  const auto tasks = UseTaskScope();
  const auto picker = UseService<FilePicker>();
  const auto photo_mode = UseState(false);
  const auto robot = UseState(false);
  const auto stopping = UseState(false);
  const auto photo = UseState(std::optional<File>{});
  const auto movie = UseState(std::optional<File>{});
  const auto message = UseState(std::string{});
  auto device = UseCaptureDevice(robot);
  const bool taking_photo = photo_mode.Get();
  const bool busy = occupied.Get();
  const auto latest = taking_photo ? photo.Get() : movie.Get();
  const bool can_save = latest && picker->CanSaveFiles();
  const bool can_stop = device.recording && !stopping.Get();
  const auto capture = [=, photo_action = std::move(device.photo), record_action = std::move(device.record),
                        stop = std::move(device.stop)] {
    if (can_stop) {
      stopping = true;
      stop();
      return;
    }
    if (occupied.Get()) return;
    occupied = true;
    message = std::string{};
    tasks.Launch([=]() -> Task<void> {
      try {
        auto output = WorkbenchOutput(data, "Captures", taking_photo ? "照片.jpg" : "录像.mp4");
        if (taking_photo) {
          co_await photo_action(output);
          photo = std::move(output);
          message = std::string("照片已保存");
        } else {
          co_await record_action(output);
          movie = std::move(output);
          message = std::string("录像已保存");
        }
      } catch (const std::exception& error) {
        message = std::string(error.what());
      }
      stopping = false;
      occupied = false;
    });
  };
  auto preview = device.ready ? std::move(device.preview) : View(Column{
      Text(device.error.empty() ? "正在连接相机…" : "相机不可用").With(Foreground(theme.colors.on_surface_variant)),
  }.With(MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center)));
  const auto status = stopping.Get() ? "正在写入" : device.paused ? "已暂停" : device.recording ? "录制中" : "正在准备";
  View overlay = Column{
    Row{WorkbenchControl(Button(robot.Get() ? "机器人变声" : "原声").OnClick([robot] { robot = !robot.Get(); })
        .With(Enabled(!taking_photo)), theme, WorkbenchActionTone::Surface, true),
        View(Spacer()).With(Grow())}.With(Padding(10.0F)),
    View(Spacer()).With(Grow()),
    busy && !taking_photo ? View(Row{
        Text(status + std::string("  ") + WorkbenchTimelineTime(device.seconds, true)).Style(
            TextStyle{.font = Font::Monospace(13.0F), .foreground = Color::White()})
            .With(Padding(8.0F), Background(Color::Rgb(20, 21, 25, 0.9F)), CornerRadius(5.0F)),
    }.With(Padding(10.0F), MainAlign(MainAxisAlignment::Center))) : View{},
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
  View modes = Row{
    WorkbenchTab("照片", theme, [photo_mode] { photo_mode = true; }, taking_photo, !busy),
    WorkbenchTab("视频", theme, [photo_mode] { photo_mode = false; }, !taking_photo, !busy),
  }.With(MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center), Spacing(12.0F));
  const auto secondary = [=, paused = device.paused, pause = std::move(device.pause)] {
    if (can_stop) pause(!paused);
    else if (can_save) {
      tasks.Launch([=]() -> Task<void> {
        try {
          if (co_await picker->SaveFileAsync(*latest)) message = std::string("已保存副本");
        } catch (const std::exception& error) { message = std::string(error.what()); }
      });
    } else if (import_movie) import_movie();
  };
  View controls = Row{
    WorkbenchControl(Button(can_stop ? (device.paused ? "继续" : "暂停") : can_save ? "保存副本" : "导入视频")
        .OnClick(secondary).With(Enabled(can_stop || (!busy && (can_save || bool(import_movie))))),
        theme, WorkbenchActionTone::Quiet, true).With(Frame{.width = 76.0F}),
    WorkbenchShutter(theme, can_stop, can_stop || (device.ready && !busy),
        taking_photo ? "拍照" : can_stop ? "停止录像" : "开始录像", capture),
    WorkbenchControl(Button("加入剪辑").OnClick([movie, add_movie] {
      if (const auto saved = movie.Get()) add_movie(*saved);
    }).With(Enabled(!taking_photo && !busy && movie.Get() && HasWorkbenchMedia())),
        theme, WorkbenchActionTone::Primary, true).With(Frame{.width = 76.0F}),
  }.With(Spacing(22.0F), Padding(EdgeInsets::Symmetric(4.0F, 5.0F)),
      MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center));
  const auto label = device.error.empty() ? message.Get() : device.error;
  View notice = label.empty() ? View{} : View(Text(label).With(Padding(EdgeInsets::Symmetric(10.0F, 4.0F)),
      Foreground(device.error.empty() ? theme.colors.on_surface_variant : theme.colors.error),
      Align(HorizontalAlignment::Center, VerticalAlignment::Center)));
  return WorkbenchRecordLayout(theme, std::move(preview), std::move(overlay), std::move(modes),
                              std::move(controls), std::move(notice));
}

} // namespace avkit_preview
