#pragma once

/// @file
/// @brief Portable bgfx compositor and pixel transfer helpers.

#include <cstddef>

#include <huxerui/avkit/render.h>

namespace huxerui::avkit::render {

/// @brief Platform window and display handles for context creation.
/// The compositor renders to offscreen textures. Web requires a retained canvas selector as `window`,
/// and the context must be used on the browser main thread.
struct BgfxPlatformHandles {
  void* window{};
  void* display{};
};

/// @brief Creates an offscreen compositor using the bgfx renderer selected for the build target.
/// Use and destroy it on the calling sequence. bgfx permits one active context per process.
/// Completed output leases can be submitted as inputs to this compositor.
std::unique_ptr<RenderBackend> CreateBgfxBackend(ImageDesc target, BgfxPlatformHandles handles = {});

/// @brief Retains immutable RGBA8, BGRA8, or R8 pixel rows for upload during submission.
/// @param row_bytes Distance between successive rows in bytes, including any padding.
ImageSurface RetainBgfxPixels(ImageDesc image, BufferReference bytes, std::size_t row_bytes,
                             RowOrigin origin = RowOrigin::TopLeft);

/// @brief Reads a completed output lease from its owning bgfx backend.
/// The tightly packed bytes are in the surface's format and row origin. Call on the backend's
/// owning sequence. This operation waits for the GPU.
Result<BufferReference, RenderError> ReadBgfxPixels(RenderBackend& backend, const ColorSurface& surface);

} // namespace huxerui::avkit::render
