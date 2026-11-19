#pragma once

#include <huxerui/avkit/video_texture.h>

namespace avkit_preview {

/// @brief Composites and verifies a small preview through the portable bgfx path.
huxerui::avkit::render::VideoPreviewImage CreateBgfxPreview();

} // namespace avkit_preview
