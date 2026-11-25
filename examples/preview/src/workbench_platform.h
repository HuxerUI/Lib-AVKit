#pragma once

#include <huxerui/avkit/timeline_renderer.h>
#include "workbench_player.h"

namespace avkit_preview {

bool RunWorkbenchRenderIteration(const std::function<bool()>& iteration);

enum class WorkbenchLogLevel { Info, Warning, Error };
void LogWorkbench(WorkbenchLogLevel level, const char* format, ...);

void ExportRenderedTimeline(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project, const huxerui::File& file,
    const huxerui::avkit::MovieCodecProviders& providers, huxerui::avkit::render::TimelineRenderer& renderer,
    const huxerui::avkit::FileExportSettings& settings, huxerui::avkit::OnCodecFallback on_fallback = {},
    std::shared_ptr<huxerui::avkit::analysis::AnalysisProvider> analysis = {}, const WorkbenchExportFrame& on_frame = {});

} // namespace avkit_preview
