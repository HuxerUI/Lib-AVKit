#include "workbench_overlay.h"

namespace avkit_preview {

using namespace huxerui;
using namespace huxerui::avkit;

OverlayInstance CaptureSticker(PixelSize canvas) {
  constexpr float width = 0.20F;
  const float height = width * canvas.width / canvas.height;
  return {.id = {1}, .kind = CanvasOverlayKind::Sticker, .destination = {0.69F, 0.12F, width, height},
          .content = PagOverlay{{"pag-spark"}}};
}

} // namespace avkit_preview
