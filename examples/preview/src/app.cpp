#include <huxerui/avkit.h>
#include <huxerui/huxerui.h>
#include <huxerui/media_library.h>

#include <exception>

#include "capture_preview.h"
#include "workbench_media.h"

using namespace huxerui;

View App() {
  try {
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
            .initial_size = {1200.0F, 800.0F},
            .keyboard_avoidance = KeyboardAvoidance::Overlay,
        },
        .show_debug_overlay = false,
        .application_hooks = {huxerui::avkit::Install, huxerui::media::Install, avkit_preview::InstallPreviewCapture},
    }
};
