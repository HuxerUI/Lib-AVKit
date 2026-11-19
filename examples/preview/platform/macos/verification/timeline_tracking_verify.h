#pragma once

#include <memory>
#include <string_view>

#include <huxerui/avkit/analysis.h>

namespace avkit_preview {

std::shared_ptr<huxerui::avkit::analysis::AnalysisProvider> CreateMockTrackingProvider();
void VerifyTimelineTracking(std::string_view directory);

#if defined(AVKIT_NCNN_PREVIEW)
std::shared_ptr<huxerui::avkit::analysis::AnalysisProvider> CreateTimelineNcnnProvider(std::string_view model_directory);
void VerifyTimelineNcnn(std::string_view source, std::string_view model_directory, std::string_view directory);
void VerifyTimelineNcnnBody(std::string_view source, std::string_view model_directory, std::string_view landmarks,
                            std::string_view directory);
#endif

} // namespace avkit_preview
