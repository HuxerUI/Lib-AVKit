#include "capture_preview.h"
#include "workbench.h"

namespace avkit_preview {

using namespace huxerui;

void InstallPreviewCapture(ApplicationContext&) {}

[[huxerui::composable]]
View WorkbenchCapture(File, State<bool>, std::function<void(File)>, std::function<void()> import_movie) {
  const auto& theme = UseTheme();
  return Column{
    Column{
      Text("相机录制不可用", TextRole::Title),
      import_movie ? WorkbenchControl(Button("导入视频去剪辑").OnClick(std::move(import_movie)),
          theme, WorkbenchActionTone::Primary) : View{},
    }.With(Spacing(14.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Padding(20.0F), Background(theme.colors.background), MainAlign(MainAxisAlignment::Center),
        CrossAlign(CrossAxisAlignment::Center));
}

} // namespace avkit_preview
