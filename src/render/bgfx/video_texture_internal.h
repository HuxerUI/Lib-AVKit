#pragma once

#include "render/bgfx/backend.h"

namespace huxerui::avkit::render::detail {

using ImportedTexture = Result<Texture, RenderError>;
using PublishedTexture = Result<std::shared_ptr<ExternalTexture>, RenderError>;
using EncoderPicture = Result<media::VideoFrame, RenderError>;

/// Retains a compatible texture that an external producer can render into on the owning sequence.
/// @param backend Compositor sharing the same graphics device as the producer.
/// @param image Required output extent and pixel format.
Texture CreateBgfxInteropTexture(RenderBackend& backend, ImageInfo image);

std::uint64_t GetBgfxInteropTexture(RenderBackend& backend, const Texture& image);

/// Converts a straight-alpha sRGB bitmap to an opaque SDR encoder picture over black.
/// @param bitmap Straight-alpha sRGB pixels to composite over black.
/// @param format Destination video pixel layout and color metadata.
/// @param pts Picture presentation timestamp for the encoder.
/// @param duration Optional presentation duration.
EncoderPicture EncodeCpuBitmap(const Bitmap& bitmap, const media::VideoFormat& format, media::MediaTimestamp pts,
                               std::optional<media::MediaTimestamp> duration);

/// @brief Transfers a completed canvas image using the encoder's negotiated storage.
/// @param backend Compositor that produced the image.
/// @param surface Completed canvas image on that compositor.
/// @param config Negotiated video encoder input format and storage.
/// @param pts Picture presentation timestamp for the encoder.
/// @param duration Optional presentation duration.
EncoderPicture EncodeBgfxTexture(RenderBackend& backend, const Texture& surface, const media::VideoEncoderConfig& config,
                                 media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration);

} // namespace huxerui::avkit::render::detail
