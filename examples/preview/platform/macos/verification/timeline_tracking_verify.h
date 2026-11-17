#pragma once

#include <memory>
#include <string_view>

#include <huxerui/avkit/analysis.h>

namespace avkit_preview {

std::shared_ptr<huxerui::avkit::analysis::AnalysisProvider> CreateMockTrackingProvider();
void VerifyTimelineTracking(std::string_view directory);

} // namespace avkit_preview
