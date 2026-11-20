#pragma once

#include <huxerui/huxerui.h>

#include "workbench_project.h"

namespace avkit_preview {

struct WorkbenchContext {
  huxerui::TaskScope tasks;
  std::shared_ptr<huxerui::FilePicker> picker;
  huxerui::File data;
  huxerui::File cache;
  huxerui::State<std::optional<WorkbenchDocument>> document;
  huxerui::State<std::string> message;
  huxerui::State<bool> busy;
};

huxerui::View WorkbenchRoot();
[[huxerui::composable]] huxerui::View WorkbenchEditor(WorkbenchContext context);
huxerui::View WorkbenchPanel(huxerui::View content, const huxerui::ThemeSpec& theme);
void RunWorkbenchAction(const WorkbenchContext& context, std::function<huxerui::Task<void>()> action);
huxerui::Task<void> SaveWorkbenchDocument(WorkbenchContext context);
huxerui::Task<std::optional<huxerui::File>> PickWorkbenchFile(
    WorkbenchContext context, huxerui::FilePickerFilter filter, std::string directory, std::string fallback);
huxerui::FilePickerFilter WorkbenchMovieFilter();
huxerui::FilePickerFilter WorkbenchProjectFilter();
std::string WorkbenchSeconds(double seconds, int precision = 2);

} // namespace avkit_preview
