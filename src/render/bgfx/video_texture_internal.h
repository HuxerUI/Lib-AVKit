#pragma once

#include <huxerui/avkit/video_texture.h>

namespace huxerui::avkit::render::detail {

using ImportedTexture = Result<ColorSurface, RenderError>;
using ImportedFrame = Result<ImportedVideoFrame, RenderError>;
using PublishedTexture = Result<std::shared_ptr<ExternalTexture>, RenderError>;
using EncoderPicture = Result<media::VideoFrame, RenderError>;

ImportedTexture AcquireBgfxTexture(RenderBackend& backend, const ExternalTexture& texture);
ImportedFrame ImportDecodedBgfxFrame(RenderBackend& backend, const media::VideoFrame& frame,
                                     media::VideoTextureOptions options, TextureTransferPolicy policy);
PublishedTexture PublishBgfxTexture(RenderBackend& backend, const ColorSurface& surface, media::MediaTimestamp pts);
EncoderPicture EncodeBgfxTexture(RenderBackend& backend, const ColorSurface& surface, const media::VideoEncoderConfig& config,
                                 media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration);

} // namespace huxerui::avkit::render::detail
