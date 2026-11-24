#pragma once

#include "workbench.h"
#include "workbench_player.h"
#include "timeline_strip.h"
#include <huxerui/avkit/timeline.h>

namespace avkit_preview {

bool HasWorkbenchMedia();
huxerui::avkit::AssetReference ReadWorkbenchMovie(const huxerui::File& file);
huxerui::avkit::AssetReference ReadWorkbenchMusic(const huxerui::File& file, const huxerui::File& prepared,
                                                 const huxerui::avkit::ProjectSnapshot& project);
huxerui::View WorkbenchVideoPicture(std::shared_ptr<huxerui::ExternalTexture> texture);
std::function<void()> ObserveWorkbenchMagnification(
    huxerui::WindowHandle window, std::function<bool(huxerui::Point, double)> magnify);
struct EditorTransport {
  huxerui::State<double> position;
  huxerui::State<std::optional<double>> seek;
  huxerui::State<bool> scrubbing;
};

struct OverlayAssets;
struct EditorMonitorViews {
  huxerui::View canvas;
  huxerui::View media;
  huxerui::View inspector;
  std::shared_ptr<const OverlayAssets> assets;
};

std::shared_ptr<const StripResult> UseEditorStrip(
    std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project, StripViewport visible);
[[huxerui::composable]] huxerui::View EditorMonitor(WorkbenchContext context,
    std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project, EditorTransport transport, bool wide,
    std::function<void()> back, std::function<void()> project_menu, EditorMonitorViews views,
    std::optional<huxerui::avkit::VisualTransformOverride> layout);

using WorkbenchExport = decltype(&ExportWorkbenchMovie);
[[huxerui::composable]] huxerui::View WorkbenchMonitor(WorkbenchContext context,
    std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project, EditorTransport transport, bool wide,
    std::function<void()> back, std::function<void()> project_menu, EditorMonitorViews views,
    std::optional<huxerui::avkit::VisualTransformOverride> layout, WorkbenchExport export_movie);

using CreateStripWorker = std::shared_ptr<StripWorker> (*)();
std::shared_ptr<const StripResult> UseWorkbenchStrip(
    std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project, StripViewport visible, CreateStripWorker create_worker);

std::optional<huxerui::View> RequestedFilePreview();
std::optional<int> RunFileMediaCommand(int argc, const char** argv);

} // namespace avkit_preview
