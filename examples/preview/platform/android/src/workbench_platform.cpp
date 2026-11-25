#include "workbench_platform.h"

#include "workbench_media.h"

#include <array>
#include <cstdarg>
#include <utility>

#include <android/log.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;

constexpr auto kLogTag = "AVKitPreview";

} // namespace

View WorkbenchVideoPicture(std::shared_ptr<ExternalTexture> texture) {
  return Image(std::move(texture)).Fit(ImageFit::Contain);
}

std::function<void()> ObserveWorkbenchMagnification(WindowHandle, std::function<bool(Point, double)>) {
  return {};
}

bool RunWorkbenchRenderIteration(const std::function<bool()>& iteration) { return iteration(); }

void LogWorkbench(WorkbenchLogLevel level, const char* format, ...) {
  constexpr std::array priorities{ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
  va_list args;
  va_start(args, format);
  __android_log_vprint(priorities[static_cast<std::size_t>(level)], kLogTag, format, args);
  va_end(args);
}

} // namespace avkit_preview
