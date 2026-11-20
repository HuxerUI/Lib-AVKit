#pragma once

#include "workbench.h"

namespace avkit_preview {

bool HasWorkbenchMedia();
huxerui::avkit::AssetReference ReadWorkbenchMovie(const huxerui::File& file);
[[huxerui::composable]] huxerui::View EditorStrip(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project);
[[huxerui::composable]] huxerui::View EditorMonitor(WorkbenchContext context,
    std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project, bool wide);

std::optional<huxerui::View> RequestedFilePreview();
std::optional<int> RunFileMediaCommand(int argc, const char** argv);

} // namespace avkit_preview
