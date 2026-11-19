#pragma once

#include <memory>

#include <CoreVideo/CVPixelBuffer.h>

#include <huxerui/avkit/render.h>
#include <huxerui/avkit/timeline.h>
#include <huxerui/media_codec.h>

namespace avkit_preview {

class MovieRenderer {
public:
  explicit MovieRenderer(huxerui::avkit::PixelSize size);
  ~MovieRenderer();

  huxerui::avkit::render::ImportedVideoFrame ImportFrame(const huxerui::media::VideoFrame& input);
  huxerui::avkit::render::ColorSurface Draw(const huxerui::avkit::render::ColorSurface& input,
      huxerui::media::MediaTimestamp time, bool cue = false);
  huxerui::avkit::render::ColorSurface Draw(const huxerui::media::VideoFrame& input, bool cue = false);
  huxerui::avkit::render::ColorSurface Draw(CVPixelBufferRef input, huxerui::Rect crop,
      huxerui::media::VideoRotation rotation, huxerui::media::MediaTimestamp time, bool cue = false);
  huxerui::avkit::render::ColorSurface Draw(const huxerui::avkit::TimelineFrame& frame);
  huxerui::media::VideoFrame EncoderFrame(const huxerui::avkit::render::ColorSurface& output,
      const huxerui::media::VideoEncoderConfig& config, huxerui::media::MediaTimestamp pts,
      std::optional<huxerui::media::MediaTimestamp> duration = {});
  huxerui::media::VideoFrame EncodeFrame(const huxerui::media::VideoFrame& input,
      const huxerui::media::VideoEncoderConfig& config, bool cue = false);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

huxerui::media::VideoEncoderConfig PreviewVideoConfig(huxerui::avkit::PixelSize size);
const huxerui::avkit::ResourceRef& PreviewStickerResource();
const huxerui::avkit::ResourceRef& TrackingStickerResource(huxerui::avkit::analysis::TrackedPart part);
const huxerui::avkit::ResourceRef& PreviewWipeResource();
inline constexpr std::uint32_t kPreviewWipeFrameCount = 8;

} // namespace avkit_preview
