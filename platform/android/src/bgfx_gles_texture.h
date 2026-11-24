#pragma once

#include <huxerui/avkit/render.h>

namespace huxerui::android { class GlTexture; }

namespace huxerui::avkit::render::detail {

void PublishBgfxGlTexture(huxerui::android::GlTexture& mailbox, const ColorSurface& surface);

} // namespace huxerui::avkit::render::detail
