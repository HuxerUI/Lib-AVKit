#include "texture_bridge.h"

namespace huxerui::avkit::render::detail {

std::unique_ptr<RenderBackend> CreatePlatformBgfxBackend(ImageDesc target, bgfx::Init init) {
  init.type = bgfx::RendererType::Vulkan;
  return CreateBgfxBackend(target, init);
}

} // namespace huxerui::avkit::render::detail
