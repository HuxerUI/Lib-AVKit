#pragma once

namespace avkit_preview {

enum class WorkbenchLogLevel { Info, Warning, Error };

void LogWorkbench(WorkbenchLogLevel level, const char* format, ...);

} // namespace avkit_preview
