#include "capture_preview.h"

#include "capture_device.h"
#include "workbench_media.h"

#include <stdexcept>
#include <utility>

namespace avkit_preview {

using namespace huxerui;

Task<void> RequestCapturePermission(ApplicationHandle application, Permission permission) {
  if (co_await application.CheckPermissionAsync(permission) != PermissionStatus::Granted &&
      co_await application.RequestPermissionAsync(permission) != PermissionStatus::Granted) {
    throw std::runtime_error(permission == Permission::Camera ? "需要相机权限" : "需要麦克风权限");
  }
}

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

void InstallPreviewCapture(ApplicationContext& context) {
  camera::Install(context);
  audio::Install(context);
}

[[huxerui::composable]]
View WorkbenchCapture(File data, State<bool> occupied, std::function<void(File)> add_movie,
                      std::function<void()> import_movie) {
  const auto& theme = UseTheme();
  const auto tasks = UseTaskScope();
  const auto picker = UseService<FilePicker>();
  const auto photo_mode = UseState(false);
  const auto robot = UseState(false);
  const auto sticker = UseState(true);
  const auto stopping = UseState(false);
  const auto photo = UseState(std::optional<File>{});
  const auto movie = UseState(std::optional<File>{});
  const auto message = UseState(std::string{});
  auto device = UseCaptureDevice(robot, sticker);
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
        View(Spacer()).With(Grow()),
        device.sticker_available ? WorkbenchControl(Button(sticker.Get() ? "贴纸已开启" : "开启贴纸")
            .OnClick([sticker] { sticker = !sticker.Get(); }).With(Enabled(!taking_photo)),
            theme, WorkbenchActionTone::Surface, true) : View{},
    }.With(Padding(10.0F)),
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
