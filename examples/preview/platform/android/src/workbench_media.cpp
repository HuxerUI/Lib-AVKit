#include "workbench_media.h"
#include "workbench_player.h"

#include <utility>

#include <huxerui/avkit/timeline.h>

namespace avkit_preview {

using namespace huxerui;
using namespace huxerui::avkit;

bool HasWorkbenchMedia() { return true; }

std::function<void()> ObserveWorkbenchMagnification(WindowHandle, std::function<bool(Point, double)>) {
  return {};
}

AssetReference ReadWorkbenchMovie(const File& file) { return InspectTimelineMovie(file); }

View WorkbenchVideoPicture(std::shared_ptr<ExternalTexture> texture) {
  return Image(std::move(texture)).Fit(ImageFit::Contain);
}

std::optional<View> RequestedFilePreview() { return std::nullopt; }
std::optional<int> RunFileMediaCommand(int, const char**) { return std::nullopt; }

std::shared_ptr<const StripResult> UseEditorStrip(std::shared_ptr<const ProjectSnapshot> project, StripViewport visible) {
  return UseWorkbenchStrip(std::move(project), visible, [] {
    return std::make_shared<StripWorker>(MovieCodecProviders{CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()}, MakeStripThumbnail);
  });
}

[[huxerui::composable]]
View EditorMonitor(WorkbenchContext context, std::shared_ptr<const ProjectSnapshot> project, EditorTransport transport, bool wide, std::function<void()> back, std::function<void()> project_menu, EditorMonitorViews views,
                   std::optional<VisualTransformOverride> layout) {
  return WorkbenchMonitor(std::move(context), std::move(project), transport, wide, std::move(back), std::move(project_menu),
                          std::move(views), std::move(layout), ExportWorkbenchMovie);
}

} // namespace avkit_preview
