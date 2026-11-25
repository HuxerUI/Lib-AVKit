#include "workbench_media.h"

namespace avkit_preview {

std::optional<huxerui::View> RequestedFilePreview() { return std::nullopt; }
std::optional<int> RunFileMediaCommand(int, const char**) { return std::nullopt; }

} // namespace avkit_preview
