#include "texture_bridge.h"

namespace huxerui::avkit::render::detail {

std::unique_ptr<RenderBackend> CreatePlatformBgfxBackend(ImageInfo target, bgfx::Init init, std::stop_token stop) {
  init.type = bgfx::RendererType::Vulkan;
  return CreateBgfxBackend(target, init, stop);
}

} // namespace huxerui::avkit::render::detail
