#pragma once

#include <huxerui/avkit/render.h>
#include <huxerui/media_codec.h>

namespace huxerui::avkit::detail {

struct ImportedVideoTexture {
  render::Texture texture;
  /// Maps normalized oriented source coordinates to logical texture UVs.
  Affine2D source_to_texture_uv;
};

/// @brief Imports a platform decoder picture on the render device's owning sequence.
/// @param device Render device that will compose the imported picture.
/// @param frame Valid hardware decoder picture.
/// @param options Output dimensions and fit used to derive the logical source UVs.
/// @return Retained texture and source mapping, or a diagnostic. Unsupported allows source-PTS retry.
Result<ImportedVideoTexture, render::RenderError> ImportVideoTexture(
    render::RenderBackend& device, const media::VideoFrame& frame, media::VideoTextureOptions options);

/// @brief Converts and fits a software decoder picture for deferred GPU upload.
/// @param frame Source picture with CPU-accessible planes and color metadata.
/// @param options Output dimensions and fit shared with the platform import path.
Result<render::ImagePixels, render::RenderError> FitCpuVideoFrame(
    const media::VideoFrame& frame, media::VideoTextureOptions options);

} // namespace huxerui::avkit::detail
