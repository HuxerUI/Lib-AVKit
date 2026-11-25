#pragma once

#include <huxerui/avkit/timeline_renderer.h>

#include "workbench_overlay.h"

namespace avkit_preview {

huxerui::avkit::render::TimelineRenderResources PreviewTimelineResources(const OverlayAssets& assets);
huxerui::avkit::MovieCodecProviders TimelineProviders();
huxerui::media::VideoEncoderConfig PreviewVideoConfig(huxerui::avkit::PixelSize size);
const huxerui::avkit::ResourceRef& PreviewStickerResource();
const huxerui::avkit::ResourceRef& TrackingStickerResource(huxerui::avkit::analysis::TrackedPart part);
const huxerui::avkit::ResourceRef& PreviewPagStickerResource();
const huxerui::avkit::ResourceRef& PreviewEditablePagResource();
const huxerui::avkit::ResourceRef& PreviewPagImageResource();
const huxerui::avkit::ResourceRef& PreviewPagTransitionResource();
const huxerui::avkit::ResourceRef& PreviewWipeResource();
const std::array<huxerui::avkit::ResourceRef, 2>& PreviewStaticMaskResources();
const huxerui::avkit::ResourceRef& PreviewAnimatedMaskResource();
huxerui::avkit::PagOverlayAsset ResolvePreviewPagSticker(const OverlayAssets& assets,
    const huxerui::avkit::ResourceRef& resource, const huxerui::avkit::StickerReplacements& replacements);

} // namespace avkit_preview
