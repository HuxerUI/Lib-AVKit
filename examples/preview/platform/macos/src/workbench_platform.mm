#include "workbench_platform.h"

#include <huxerui/macos/external_texture.h>
#include "workbench_media.h"

#include <algorithm>
#include <utility>

namespace avkit_preview {

using namespace huxerui;
using namespace huxerui::avkit;

View WorkbenchVideoPicture(std::shared_ptr<ExternalTexture> texture) {
  return Canvas([texture = std::move(texture)](PaintContext& paint, huxerui::Size preview_view_size) {
    const auto texture_size = texture->IntrinsicSize();
    const auto scale = std::min(preview_view_size.width / texture_size.width,
                                preview_view_size.height / texture_size.height);
    const auto width = texture_size.width * scale, height = texture_size.height * scale;
    const huxerui::Rect preview_content_rect{(preview_view_size.width - width) * 0.5F,
                                            (preview_view_size.height - height) * 0.5F, width, height};
    // Core Image presents the Metal mailbox with inverted rows in the AppKit renderer.
    paint.PushTransform({.m22 = -1.0F, .translate_y = preview_view_size.height});
    paint.DrawImage(texture, preview_content_rect);
    paint.PopTransform();
  });
}

void LogWorkbench(WorkbenchLogLevel, const char*, ...) {}

} // namespace avkit_preview
