#include "workbench_media.h"

#include <algorithm>
#include <utility>

#include <huxerui/avkit/timeline.h>

#include "file_preview.h"
#include "timeline_preview.h"
#include "timeline_strip.h"
#include "workbench_player.h"

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;

double Seconds(media::MediaTimestamp time) { return double(time.ticks) / time.timescale; }

} // namespace

void InstallWorkbenchMedia(ApplicationContext&) {}

bool HasWorkbenchMedia() { return true; }

AssetReference ReadWorkbenchMovie(const File& file) { return InspectTimelineMovie(file); }

View WorkbenchVideoPicture(std::shared_ptr<ExternalTexture> texture) {
  return Canvas([texture = std::move(texture)](PaintContext& paint, Size size) {
    const auto source = texture->IntrinsicSize();
    const auto scale = std::min(size.width / source.width, size.height / source.height);
    const auto w = source.width * scale, h = source.height * scale;
    // Core Image presents the Metal mailbox with inverted rows in the AppKit renderer.
    paint.PushTransform({.m22 = -1.0F, .translate_y = size.height});
    paint.DrawImage(texture, {(size.width - w) * 0.5F, (size.height - h) * 0.5F, w, h});
    paint.PopTransform();
  });
}

std::optional<View> RequestedFilePreview() {
  const auto timeline = HasTimelinePreview();
  if (!timeline && !HasFilePreview()) return std::nullopt;
  static const auto texture = timeline ? CreateTimelinePreview() : CreateFilePreview();
  if (TimelineStripOptions()) return TimelineStripPreview(texture);
  const auto seek = timeline ? SeekTimelinePreview : SeekFilePreview;
  return Column {
    Image(texture).Fit(ImageFit::Contain).With(Grow()),
    Row {
      Button("Start").OnClick([seek] { seek(false); }),
      Button("Middle").OnClick([seek] { seek(true); }),
    }.With(Spacing(8.0F), MainAlign(MainAxisAlignment::Center)),
  }.With(Padding(8.0F), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

std::shared_ptr<const StripResult> UseEditorStrip(std::shared_ptr<const ProjectSnapshot> project, StripViewport visible) {
  return UseWorkbenchStrip(std::move(project), visible, [] {
    return std::make_shared<StripWorker>(TimelineProviders(), MakeStripThumbnail);
  });
}

[[huxerui::composable]]
View EditorMonitor(WorkbenchContext context, std::shared_ptr<const ProjectSnapshot> project, EditorTransport transport, bool wide, std::function<void()> back, std::function<void()> project_menu, EditorMonitorViews views,
                   std::optional<VisualTransformOverride> layout) {
  return WorkbenchMonitor(std::move(context), std::move(project), transport, wide, std::move(back), std::move(project_menu),
                          std::move(views), std::move(layout), ExportWorkbenchMovie);
}

} // namespace avkit_preview
