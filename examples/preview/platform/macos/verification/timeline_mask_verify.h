#pragma once

#include <memory>
#include <string_view>

#include <huxerui/avkit/analysis.h>

namespace avkit_preview {

std::shared_ptr<huxerui::avkit::analysis::AnalysisProvider> CreateMockMaskProvider();
void VerifyTimelineMasks(std::string_view directory);
void VerifyTimelineNcnnMasks(std::string_view source, std::string_view model_directory, std::string_view directory,
                             bool mixed_models);

} // namespace avkit_preview
