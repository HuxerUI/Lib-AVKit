#pragma once

#include <huxerui/avkit/resource_catalog.h>
#include <huxerui/avkit/timeline.h>
#include <huxerui/resource.h>

#include "app_resources.h"

#include <array>
#include <utility>

namespace avkit_preview {

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

/// @brief Registers the example's packaged assets and shares the catalog with media workers.
/// @param resources SDK service used to obtain the packaged raw asset handles.
std::shared_ptr<const huxerui::avkit::ResourceCatalog> LoadWorkbenchResources(const huxerui::Resources& resources);

inline const huxerui::avkit::ResourceRef kPreviewPointTextResource{"point-text"};
inline constexpr auto kPreviewPointTextSlot = "text";

/// @brief Authors a title using the workbench's packaged PAG master.
/// @param text Replacement for the master's named text slot.
inline huxerui::avkit::PagOverlay PreviewPointText(std::string text) {
  return {kPreviewPointTextResource, {{{kPreviewPointTextSlot, std::move(text)}}},
          huxerui::avkit::AnimationPlayback::HoldLast};
}

huxerui::avkit::OverlayInstance CaptureSticker(huxerui::avkit::PixelSize canvas);

} // namespace avkit_preview
