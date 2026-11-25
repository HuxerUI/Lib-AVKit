#include "workbench_overlay.h"

#include <algorithm>
#include <stdexcept>

namespace avkit_preview {

using namespace huxerui;
using namespace huxerui::avkit;

OverlayAssets LoadOverlayAssets(const Resources& resources) {
  const auto load = [&](RawResource asset) {
    auto bytes = std::make_shared<Bytes>(resources.GetRawResource(asset).ReadBytes());
    return BufferReference(*bytes, bytes);
  };
  OverlayAssets assets;
  for (std::size_t index = 0; index < kWorkbenchStickers.size(); ++index) assets.stickers[index] = load(kWorkbenchStickers[index].pag);
  assets.editable = load(app::raw::stickers_editable_pag);
  assets.point_text = load(app::raw::stickers_point_text_pag);
  assets.font = load(app::raw::stickers_preview_sans_ttf);
  assets.replacement_image = load(app::raw::stickers_replacement_png);
  assets.transition = load(app::raw::transitions_crossfade_pag);
  const std::array wipe{app::raw::masks_wipe_0_png, app::raw::masks_wipe_1_png, app::raw::masks_wipe_2_png,
                        app::raw::masks_wipe_3_png, app::raw::masks_wipe_4_png, app::raw::masks_wipe_5_png,
                        app::raw::masks_wipe_6_png, app::raw::masks_wipe_7_png};
  static_assert(wipe.size() == kPreviewWipeFrameCount);
  for (std::size_t index = 0; index < wipe.size(); ++index) assets.wipe_masks[index] = load(wipe[index]);
  assets.clip_masks = {load(app::raw::masks_static_window_png), load(app::raw::masks_static_offset_png),
                       load(app::raw::masks_sequence_corner_png)};
  return assets;
}

CanvasOverlay CaptureSticker(PixelSize canvas) {
  constexpr float width = 0.20F;
  const float height = width * canvas.width / canvas.height;
  return {.id = {1}, .kind = CanvasOverlayKind::Sticker, .destination = {0.69F, 0.12F, width, height},
          .resource = {"avkit.preview", "1", "pag-spark"}};
}

PagOverlayResources WorkbenchPagResources(OverlayAssets assets) {
  return {
    .point_text = assets.point_text,
    .font = [font = assets.font](const ResourceRef& resource) -> BufferReference {
      return resource == ResourceRef{"avkit.preview", "1", "preview-sans"} ? font : BufferReference{};
    },
    .canvas_animation = [assets = std::move(assets)](const CanvasOverlay& overlay) -> PagOverlayAsset {
      if (overlay.kind == CanvasOverlayKind::PagText) {
        if (overlay.resource != ResourceRef{"avkit.preview", "1", "pag-editable"})
          throw std::runtime_error("Unknown workbench PAG resource: " + overlay.resource.key);
        return {assets.editable, {{0, overlay.text}}};
      }
      const auto asset = std::ranges::find_if(kWorkbenchStickers, [&](const WorkbenchSticker& sticker) {
        return overlay.resource.key == sticker.key;
      });
      if (overlay.resource.pack != "avkit.preview" || overlay.resource.revision != "1" || asset == kWorkbenchStickers.end())
        throw std::runtime_error("Unknown workbench sticker resource: " + overlay.resource.key);
      return {assets.stickers[asset - kWorkbenchStickers.begin()]};
    },
  };
}

Rect MeasureWorkbenchOverlay(const CanvasOverlay& overlay, PixelSize canvas, const OverlayAssets& assets) {
  return MeasurePagOverlay(overlay, canvas, WorkbenchPagResources(assets));
}

} // namespace avkit_preview
