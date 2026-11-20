#include "capture_preview.h"

namespace avkit_preview {

using namespace huxerui;

void InstallPreviewCapture(ApplicationContext&) {}
std::optional<View> RequestedCapturePreview() { return std::nullopt; }
std::optional<int> RunCaptureCommand(int, const char**) { return std::nullopt; }

[[huxerui::composable]]
View WorkbenchCapture(File, State<bool>, std::function<void(File)>) {
  return Column{Text("录制", TextRole::Title), Text("当前平台的示例构建尚未接入相机和麦克风。")}
      .With(Padding(16.0F), Spacing(12.0F));
}

} // namespace avkit_preview
