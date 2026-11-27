#include "workbench_media.h"
#include "workbench_overlay.h"
#include "app_resources.h"

#include <stdexcept>

namespace avkit_preview {

using namespace huxerui;
using namespace huxerui::avkit;


bool HasWorkbenchMedia() { return false; }

std::shared_ptr<const ResourceCatalog> LoadWorkbenchResources(const Resources&) { return {}; }

AssetSource ReadWorkbenchMusic(const File&) {
  throw std::runtime_error("This build does not support timeline audio import");
}

std::function<void()> ObserveWorkbenchMagnification(WindowHandle, std::function<bool(Point, double)>) {
  return {};
}

AssetSource ReadWorkbenchVisual(const File&) {
  throw std::logic_error("Video import is unavailable in this build");
}


std::shared_ptr<const StripResult> UseEditorStrip(std::shared_ptr<const ProjectSnapshot>, StripViewport) {
  return {};
}

[[huxerui::composable]]
View EditorMonitor(WorkbenchContext, std::shared_ptr<const ProjectSnapshot>, EditorTransport, bool wide, std::function<void()> back, std::function<void()>,
                   EditorMonitorViews, State<std::optional<WorkbenchFrame>>, std::function<std::optional<WorkbenchDrag>()>) {
  const auto& theme = UseTheme();
  return WorkbenchPanel(Column{
      wide ? View{} : WorkbenchCompactAction(Row{
        Image(app::images::icons_chevron_left).With(InheritForeground{}),
        Text("工作台").Style(TextStyle{.font = Font::System(12.0F).WithWeight(FontWeight::SemiBold)}).With(InheritForeground{}),
      }.With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Center)), "返回工作台", theme, std::move(back), true),
      Text("此构建未接入视频预览").With(Foreground(theme.colors.on_surface_variant))}
      .With(Frame{.height = wide ? 280.0F : 170.0F}, MainAlign(MainAxisAlignment::Center),
            CrossAlign(CrossAxisAlignment::Center), Background(Color::Rgb(8, 11, 17)), CornerRadius(8.0F)), theme);
}

} // namespace avkit_preview
