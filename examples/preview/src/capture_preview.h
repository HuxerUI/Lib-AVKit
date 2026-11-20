#pragma once

#include <optional>

#include <huxerui/file.h>
#include <huxerui/view.h>

namespace huxerui { class ApplicationContext; }

namespace avkit_preview {

void InstallPreviewCapture(huxerui::ApplicationContext& context);
std::optional<huxerui::View> RequestedCapturePreview();
std::optional<int> RunCaptureCommand(int argc, const char** argv);

[[huxerui::composable]] huxerui::View WorkbenchCapture(huxerui::File data, huxerui::State<bool> occupied,
                                                     std::function<void(huxerui::File)> add_movie);

} // namespace avkit_preview
