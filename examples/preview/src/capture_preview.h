#pragma once

#include <string_view>

#include <huxerui/view.h>

namespace avkit_preview {

enum class CapturePreviewMode { Interactive, VerifyCamera, VerifyLive };

void ConfigureCapturePreview(std::string_view directory, CapturePreviewMode mode);
bool HasCapturePreview();
int CapturePreviewExitCode(int app_result);

[[huxerui::composable]] huxerui::View CapturePreview();

} // namespace avkit_preview
