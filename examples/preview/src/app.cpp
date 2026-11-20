#include <huxerui/avkit.h>
#include <huxerui/huxerui.h>

#include <exception>
#include <utility>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "capture_preview.h"
#include "workbench_media.h"

using namespace huxerui;

View App() {
  try {
    if (auto capture = avkit_preview::RequestedCapturePreview()) return std::move(*capture);
    if (auto file = avkit_preview::RequestedFilePreview()) return std::move(*file);
    return avkit_preview::WorkbenchRoot();
  } catch (const std::exception& error) {
    return Text(error.what());
  }
}

const Application application{
    App,
    {
        .window = {
            .title = "AVKit Workbench",
#if defined(TARGET_OS_OSX) && TARGET_OS_OSX
            .initial_size = {1200.0F, 800.0F},
#endif
        },
        .application_hooks = {huxerui::avkit::Install, avkit_preview::InstallPreviewCapture},
    }
};
