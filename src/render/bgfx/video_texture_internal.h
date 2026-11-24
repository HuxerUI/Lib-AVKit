#pragma once

#include <huxerui/avkit/video_texture.h>

namespace huxerui::avkit::render::detail {

using ImportedTexture = Result<ColorSurface, RenderError>;
using ImportedFrame = Result<ImportedVideoFrame, RenderError>;
using PublishedTexture = Result<std::shared_ptr<ExternalTexture>, RenderError>;
using EncoderPicture = Result<media::VideoFrame, RenderError>;

ImportedTexture AcquireBgfxTexture(RenderBackend& backend, const ExternalTexture& texture,
                                   const media::VideoFormat* decoded_format = nullptr);
/// Retains a compatible texture that an external producer can render into on the owning sequence.
ColorSurface CreateBgfxInteropTexture(RenderBackend& backend, ImageDesc image);
std::uint64_t GetBgfxInteropTexture(RenderBackend& backend, const ImageSurface& image);
ImportedFrame ImportDecodedBgfxFrame(RenderBackend& backend, const media::VideoFrame& frame,
                                     media::VideoTextureOptions options, TextureTransferPolicy policy);
ImportedTexture FitCpuVideoFrame(const media::VideoFrame& frame, media::VideoTextureOptions options);
/// Converts a straight-alpha sRGB bitmap to an opaque SDR encoder picture over black.
EncoderPicture EncodeCpuBitmap(const Bitmap& bitmap, const media::VideoFormat& format, media::MediaTimestamp pts,
                               std::optional<media::MediaTimestamp> duration);
PublishedTexture PublishBgfxTexture(RenderBackend& backend, const ColorSurface& surface, media::MediaTimestamp pts);
EncoderPicture EncodeBgfxTexture(RenderBackend& backend, const ColorSurface& surface, const media::VideoEncoderConfig& config,
                                 media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration);

} // namespace huxerui::avkit::render::detail
