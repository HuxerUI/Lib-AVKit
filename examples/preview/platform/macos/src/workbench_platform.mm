#include "workbench_platform.h"

#include <huxerui/macos/external_texture.h>
#include "workbench_media.h"

#include <algorithm>
#include <utility>

namespace avkit_preview {

using namespace huxerui;
using namespace huxerui::avkit;

View WorkbenchVideoPicture(std::shared_ptr<ExternalTexture> texture) {
  return Canvas([texture = std::move(texture)](PaintContext& paint, huxerui::Size size) {
    const auto source = texture->IntrinsicSize();
    const auto scale = std::min(size.width / source.width, size.height / source.height);
    const auto w = source.width * scale, h = source.height * scale;
    // Core Image presents the Metal mailbox with inverted rows in the AppKit renderer.
    paint.PushTransform({.m22 = -1.0F, .translate_y = size.height});
    paint.DrawImage(texture, {(size.width - w) * 0.5F, (size.height - h) * 0.5F, w, h});
    paint.PopTransform();
  });
}

bool RunWorkbenchRenderIteration(const std::function<bool()>& iteration) {
  @autoreleasepool { return iteration(); }
}

void LogWorkbench(WorkbenchLogLevel, const char*, ...) {}

} // namespace avkit_preview
