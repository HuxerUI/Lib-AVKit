#pragma once

#include <chrono>

#include <huxerui/avkit/render.h>

namespace huxerui::avkit::render::detail {

/// Pumps a pending bgfx submission and measures CPU pixel packing and upload enqueue time.
std::chrono::nanoseconds PumpBgfxWithUploadTiming(RenderBackend& backend);

} // namespace huxerui::avkit::render::detail
