#include "capture_preview.h"

namespace avkit_preview {

std::optional<huxerui::View> RequestedCapturePreview() { return std::nullopt; }
std::optional<int> RunCaptureCommand(int, const char**) { return std::nullopt; }

} // namespace avkit_preview
