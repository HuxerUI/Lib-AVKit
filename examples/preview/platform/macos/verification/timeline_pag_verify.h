#pragma once

#include <string_view>

namespace avkit_preview {

void VerifyTimelinePagStickers(std::string_view document, std::string_view directory);
void VerifyTimelinePagReplacements(std::string_view document, std::string_view directory);
void VerifyTimelinePagTransition(std::string_view document, std::string_view directory);
void VerifyTimelinePagOverlayBatch(std::string_view first, std::string_view second, std::string_view directory);

} // namespace avkit_preview
