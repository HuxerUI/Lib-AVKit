#pragma once

/// @file
/// @brief Private macOS Metal texture bridge for the shared bgfx compositor.

#include <stop_token>

#include <huxerui/avkit/render.h>

#if defined(__OBJC__)
#import <Metal/Metal.h>
#endif

namespace huxerui::avkit::macos {

#if defined(__OBJC__)
/// @brief Creates a headless bgfx backend on the given Metal device.
/// @param device Metal device shared with the platform texture producers and consumers.
/// @param target Format and physical dimensions of each output texture.
/// @param output_slots Number of independently leased output textures.
/// @param stop Cancels waiting for the active bgfx renderer to release its context.
/// @return A backend used on the calling sequence.
std::unique_ptr<render::RenderBackend> CreateBgfxBackend(id<MTLDevice> device, render::ImageInfo target,
                                                       std::size_t output_slots, std::stop_token stop);

/// @brief Retains a producer-complete RGBA8 or BGRA8 texture with premultiplied color.
/// The producer leaves the submitted texture immutable for the duration of the lease.
/// @param texture Completed Metal texture to retain.
/// @param origin Logical origin of the texture rows.
render::Texture RetainMetalTexture(id<MTLTexture> texture,
                                        render::RowOrigin origin = render::RowOrigin::TopLeft);

/// @brief Retrieves the Metal texture behind a surface, or nil if the storage uses another backend.
/// Retain the surface lease through the last Metal read even when retaining the texture separately.
/// @param surface Retained image to inspect.
id<MTLTexture> GetMetalTexture(const render::Texture& surface);

#endif

} // namespace huxerui::avkit::macos
