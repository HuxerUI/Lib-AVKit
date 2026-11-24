#pragma once

#include <huxerui/huxerui.h>

#include "workbench_project.h"

namespace avkit_preview {

using WorkbenchMediaSelection = std::function<huxerui::Task<void>(std::vector<huxerui::File>)>;

struct WorkbenchMediaRequest {
  std::string title;
  std::uint32_t limit;
  bool include_photos;
  WorkbenchMediaSelection confirmed;
};

struct WorkbenchContext {
  huxerui::TaskScope tasks;
  std::shared_ptr<huxerui::FilePicker> picker;
  huxerui::File data;
  huxerui::File cache;
  huxerui::State<std::optional<WorkbenchDocument>> document;
  huxerui::State<std::string> message;
  huxerui::State<bool> busy;
  huxerui::State<bool> exporting;
  huxerui::State<std::shared_ptr<const WorkbenchMediaRequest>> media_request;
  huxerui::State<bool> browse_library;
};

huxerui::View WorkbenchRoot();
[[huxerui::composable]] huxerui::View WorkbenchEditor(WorkbenchContext context, std::function<void()> back);
huxerui::View WorkbenchPanel(huxerui::View content, const huxerui::ThemeSpec& theme);
enum class WorkbenchActionTone { Quiet, Surface, Primary };
huxerui::View WorkbenchControl(huxerui::View content, const huxerui::ThemeSpec& theme,
                               WorkbenchActionTone tone = WorkbenchActionTone::Quiet, bool compact = false);
huxerui::View WorkbenchTab(std::string label, const huxerui::ThemeSpec& theme, std::function<void()> action,
                           bool active, bool enabled = true);
huxerui::View WorkbenchTool(huxerui::ImageResource icon, std::string label, const huxerui::ThemeSpec& theme,
                            std::function<void()> action, bool enabled, bool show_label = true,
                            std::optional<bool> selected = {});
huxerui::View WorkbenchCompactAction(huxerui::View content, std::string label, const huxerui::ThemeSpec& theme,
                                    std::function<void()> action, bool enabled);
huxerui::View WorkbenchBack(std::string label, const huxerui::ThemeSpec& theme,
                            std::function<void()> action, bool enabled);
huxerui::View WorkbenchShutter(const huxerui::ThemeSpec& theme, bool recording, bool enabled,
                               std::string label, std::function<void()> action);
huxerui::View WorkbenchRecordLayout(const huxerui::ThemeSpec& theme, huxerui::View picture, huxerui::View overlay,
                                    huxerui::View modes, huxerui::View controls, huxerui::View status);
void RunWorkbenchAction(const WorkbenchContext& context, std::function<huxerui::Task<void>()> action);
huxerui::Task<void> SaveWorkbenchDocument(WorkbenchContext context);
huxerui::Task<std::optional<huxerui::File>> PickWorkbenchFile(
    WorkbenchContext context, huxerui::FilePickerFilter filter, std::string directory, std::string fallback);
huxerui::FilePickerFilter WorkbenchMovieFilter();
huxerui::FilePickerFilter WorkbenchAudioFilter();
bool CanSelectWorkbenchVideo(const WorkbenchContext& context);
void SelectWorkbenchVideo(const WorkbenchContext& context, std::string title, std::uint32_t limit,
                          WorkbenchMediaSelection confirmed);
void StartWorkbenchProject(const WorkbenchContext& context, std::function<void()> on_created);
huxerui::FilePickerFilter WorkbenchProjectFilter();
std::string WorkbenchSeconds(double seconds, int precision = 2);
std::string WorkbenchTimecode(double seconds);
std::string WorkbenchTimelineTime(double seconds, bool fraction);

} // namespace avkit_preview
