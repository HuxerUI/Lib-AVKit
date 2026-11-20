#pragma once

#include <string_view>

namespace avkit_preview {

void VerifyTimelineTemplateMasks(std::string_view document, std::string_view directory);
void VerifyTimelineAnimatedTemplateMasks(std::string_view document, std::string_view directory);

} // namespace avkit_preview
