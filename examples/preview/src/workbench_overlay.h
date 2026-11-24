#pragma once

#include <huxerui/avkit/pag.h>
#include <huxerui/avkit/timeline.h>
#include <huxerui/resource.h>

#include "app_resources.h"

#include <array>

namespace avkit_preview {

struct OverlayAssets {
  std::array<huxerui::BufferReference, 4> stickers;
  huxerui::BufferReference editable, point_text, font;
};

struct WorkbenchSticker {
  const char* key;
  const char* title;
  huxerui::RawResource pag;
  huxerui::ImageResource preview;
  float aspect;
};

inline const std::array kWorkbenchStickers{
    WorkbenchSticker{"pag-like", "点赞", app::raw::stickers_like_pag, app::images::stickers_like, 9.0F / 16.0F},
    WorkbenchSticker{"pag-spark", "星光", app::raw::stickers_spark_pag, app::images::stickers_spark, 1.0F},
    WorkbenchSticker{"pag-arrow", "箭头", app::raw::stickers_arrow_pag, app::images::stickers_arrow, 1.0F},
    WorkbenchSticker{"pag-circle", "手绘圈", app::raw::stickers_circle_pag, app::images::stickers_circle, 1.0F},
};
OverlayAssets LoadOverlayAssets(const huxerui::Resources& resources);
huxerui::Rect MeasureWorkbenchOverlay(const huxerui::avkit::CanvasOverlay& overlay,
                                     huxerui::avkit::PixelSize canvas, const OverlayAssets& assets);

huxerui::avkit::PagOverlayResources WorkbenchPagResources(OverlayAssets assets);

} // namespace avkit_preview
