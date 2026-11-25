#pragma once

#include <array>
#include <chrono>
#include <memory>

#include <huxerui/avkit/pag.h>
#include <huxerui/avkit/render.h>
#include <huxerui/avkit/timeline.h>
#include <huxerui/media_codec.h>

#include "timeline_resources.h"

namespace avkit_preview {

class MovieRenderer {
public:
  explicit MovieRenderer(huxerui::avkit::PixelSize size);
  ~MovieRenderer();
  huxerui::avkit::render::TimelineRenderer& Timeline();

  huxerui::avkit::render::ImportedVideoFrame ImportFrame(const huxerui::media::VideoFrame& input);
  huxerui::avkit::render::ColorSurface Draw(const huxerui::avkit::render::ColorSurface& input,
      huxerui::media::MediaTimestamp time, bool cue = false);
  huxerui::avkit::render::ColorSurface Draw(const huxerui::media::VideoFrame& input, bool cue = false);
  huxerui::avkit::render::ColorSurface Draw(const huxerui::avkit::TimelineFrame& frame,
                                            huxerui::avkit::render::PagTransitionRenderTiming* timing = nullptr);
  huxerui::avkit::render::ColorSurface Draw(const huxerui::avkit::TimelineFrame& frame,
                                            huxerui::avkit::PagOverlayStats& stats, bool batch_overlays);
  huxerui::media::VideoFrame EncoderFrame(const huxerui::avkit::render::ColorSurface& output,
      const huxerui::media::VideoEncoderConfig& config, huxerui::media::MediaTimestamp pts,
      std::optional<huxerui::media::MediaTimestamp> duration = {});
  huxerui::media::VideoFrame EncodeFrame(const huxerui::media::VideoFrame& input,
      const huxerui::media::VideoEncoderConfig& config, bool cue = false);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::unique_ptr<huxerui::avkit::PagTransition> OpenPreviewPagTransition(huxerui::avkit::PixelSize size);
std::unique_ptr<huxerui::avkit::PagAnimation> OpenPreviewPagSticker(
    const huxerui::avkit::ResourceRef& resource = PreviewPagStickerResource(),
    const huxerui::avkit::StickerReplacements& replacements = {},
    huxerui::avkit::render::RenderBackend* backend = nullptr);
} // namespace avkit_preview
