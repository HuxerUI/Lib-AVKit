#pragma once

/// @file
/// @brief Objective-C++ Metal texture bridge for the macOS bgfx adapter.
///
/// Include this header from macOS Objective-C++ and link AVKit::Bgfx.

#include <huxerui/avkit/render.h>

#if defined(__APPLE__) && defined(__OBJC__)
#include <TargetConditionals.h>

#if TARGET_OS_OSX
#import <Metal/Metal.h>

namespace huxerui::avkit::macos {

/// @brief Creates a headless bgfx backend on the given Metal device.
/// @param device Metal device shared with the platform texture producers and consumers.
/// @param target Format and physical dimensions of each output texture.
/// @return A backend with two leased output slots. Use it on the calling sequence.
std::unique_ptr<render::RenderBackend> CreateBgfxBackend(id<MTLDevice> device, render::ImageDesc target);

/// @brief Retains a producer-complete RGBA8 or BGRA8 texture with premultiplied color.
/// The producer leaves the submitted texture immutable for the duration of the lease.
render::RenderSurface RetainMetalTexture(id<MTLTexture> texture,
                                        render::RowOrigin origin = render::RowOrigin::TopLeft);

/// @brief Retains a producer-complete R8Unorm texture containing numeric coverage.
render::DataSurface RetainMetalDataTexture(id<MTLTexture> texture,
                                          render::RowOrigin origin = render::RowOrigin::TopLeft);

/// @brief Retrieves the Metal texture behind a surface, or nil if the storage uses another backend.
/// Retain the surface lease through the last Metal read even when retaining the texture separately.
id<MTLTexture> GetMetalTexture(const render::RenderSurface& surface);

} // namespace huxerui::avkit::macos
#endif
#endif
