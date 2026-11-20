#include "workbench_media.h"

#include <stdexcept>

namespace avkit_preview {

using namespace huxerui;
using namespace huxerui::avkit;

bool HasWorkbenchMedia() { return false; }

AssetReference ReadWorkbenchMovie(const File&) {
  throw std::logic_error("Video import is unavailable in this build");
}

std::optional<View> RequestedFilePreview() { return std::nullopt; }
std::optional<int> RunFileMediaCommand(int, const char**) { return std::nullopt; }

[[huxerui::composable]]
View EditorStrip(std::shared_ptr<const ProjectSnapshot>) { return Text("预览取样不可用"); }

[[huxerui::composable]]
View EditorMonitor(WorkbenchContext, std::shared_ptr<const ProjectSnapshot>, bool wide) {
  const auto& theme = UseTheme();
  return WorkbenchPanel(Column{Text("此构建未接入视频预览").With(Foreground(theme.colors.on_surface_variant))}
      .With(Frame{.height = wide ? 280.0F : 170.0F}, MainAlign(MainAxisAlignment::Center),
            CrossAlign(CrossAxisAlignment::Center), Background(Color::Rgb(8, 11, 17)), CornerRadius(8.0F)), theme);
}

} // namespace avkit_preview
