#pragma once

#include "workbench.h"
#include "workbench_player.h"
#include "timeline_strip.h"
#include <huxerui/avkit/timeline.h>

namespace avkit_preview {

bool HasWorkbenchMedia();

huxerui::avkit::AssetSource ReadWorkbenchVisual(const huxerui::File& file);


huxerui::avkit::AssetSource ReadWorkbenchMusic(const huxerui::File& file);

huxerui::View WorkbenchVideoPicture(std::shared_ptr<huxerui::ExternalTexture> texture);

std::function<void()> ObserveWorkbenchMagnification(
    huxerui::WindowHandle window, std::function<bool(huxerui::Point, double)> magnify);
struct EditorTransport {
  huxerui::State<double> position;
  huxerui::State<std::optional<double>> seek;
  huxerui::State<bool> scrubbing;
};

struct EditorMonitorViews {
  std::function<huxerui::View(const WorkbenchFrame&, bool current)> preview_controls;
  huxerui::View media;
  huxerui::View inspector;
  std::shared_ptr<const huxerui::avkit::ResourceCatalog> resources;
};

std::shared_ptr<const StripResult> UseEditorStrip(
    std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project, StripViewport visible);

[[huxerui::composable]] huxerui::View EditorMonitor(WorkbenchContext context,
    std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project, EditorTransport transport, bool wide,
    std::function<void()> back, std::function<void()> project_menu, EditorMonitorViews views,
    huxerui::State<std::optional<WorkbenchFrame>> presented,
    std::function<std::optional<WorkbenchDrag>()> preview_drag);

} // namespace avkit_preview
