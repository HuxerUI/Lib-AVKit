#pragma once

#include <string_view>

namespace avkit_preview {

void ExportMediaFile(std::string_view input, std::string_view output, bool software_only);
void VerifyFileMedia(std::string_view input, std::string_view output);

} // namespace avkit_preview
