#include <huxerui/avkit.h>

#if defined(__ANDROID__)
#include "android_canvas_texture.h"
#endif

namespace huxerui::avkit {

void Install(ApplicationContext& context) {
#if defined(__ANDROID__)
  render::detail::InstallAndroidCanvasTexture(context);
#else
  (void)context;
#endif
}

} // namespace huxerui::avkit
