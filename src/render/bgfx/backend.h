#pragma once

/// @file
/// @brief Portable bgfx compositor, preview textures, and video frame interchange.

#include "render/backend.h"
#include <huxerui/external_texture.h>
#include <huxerui/media_codec.h>

namespace huxerui::avkit::render {

/// @brief Reads a completed output lease from its owning bgfx backend.
/// The tightly packed bytes are in the surface's format and row origin. Call on the backend's
/// owning sequence. This operation waits for the GPU.
/// @param backend Owning bgfx backend.
/// @param surface Completed image lease from that backend.
/// @return Tightly packed bytes, or a diagnostic if the image cannot be read.
Result<BufferReference, RenderError> ReadBgfxPixels(RenderBackend& backend, const Texture& surface);

/// @brief Converts a completed composite to the negotiated encoder input and preserves PTS and duration.
/// Platform and CPU are distinct encoder contracts. On Unsupported, a session may negotiate a CPU
/// encoder input and retry the same output lease.
/// @param backend Owning bgfx backend.
/// @param surface Completed composite matching the encoder's coded dimensions.
/// @param config Confirmed encoder input format, storage, and color metadata.
/// @param pts Exact output time on the encoder's presentation clock.
/// @param duration Optional positive display duration.
/// @return Retained frame in the negotiated encoder layout, or a render diagnostic.
Result<media::VideoFrame, RenderError> MakeBgfxEncoderFrame(
    RenderBackend& backend, const Texture& surface, const media::VideoEncoderConfig& config,
    media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration = std::nullopt);

} // namespace huxerui::avkit::render
