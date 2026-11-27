#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gtk/gtk.h>
#include <huxerui/linux/external_texture.h>

#include "gl_texture_bridge.h"
#include "avkit_fs_composite_gl.h"
#include "avkit_fs_masked_blend_gl.h"
#include "avkit_vs_composite_gl.h"

namespace huxerui::avkit::render::detail {

namespace {

class GdkTextureBridge final : public GlTextureBridge {
public:
  ImportedTexture Acquire(const ExternalTexture& texture) override {
    using R = ImportedTexture;
    CollectReleasedNames();
    try {
      const auto* mailbox = dynamic_cast<const linux::GlTexture*>(&texture);
      auto frame = mailbox ? mailbox->AcquireFrame() : nullptr;
      if (!frame) return R::Failure({RenderErrorCode::Unsupported, "Expected a published GDK GL texture"});
      auto* gdk = frame->Texture();
      const ImageInfo image{{static_cast<std::uint32_t>(gdk_texture_get_width(gdk)),
                             static_cast<std::uint32_t>(gdk_texture_get_height(gdk))}, PixelFormat::Rgba8Unorm};
      const auto format = gdk_texture_get_format(gdk);
      const auto alpha = format == GDK_MEMORY_R8G8B8A8 ? AlphaMode::Straight : AlphaMode::Premultiplied;
      const auto origin = frame->TextureOrigin() == linux::GlTexture::Origin::TopLeft
          ? RowOrigin::TopLeft : RowOrigin::BottomLeft;
      auto record = std::make_shared<GlTextureName>();
      record->texture = frame->GlTextureNameCurrent();
      record->owned = false;
      record->producer = std::move(frame);
      return R::Success(Retain(image, origin, std::move(record), alpha));
    } catch (const std::exception& error) {
      return R::Failure({RenderErrorCode::Unsupported, error.what()});
    }
  }

  PublishedTexture Publish(const Texture& surface, media::MediaTimestamp) override {
    using R = PublishedTexture;
    const auto name = static_cast<GLuint>(GetTexture(surface));
    if (!name) return R::Failure({RenderErrorCode::Unsupported, "Expected a completed GL render target"});
    const auto [width, height] = surface.Info().size;
    try {
      auto mailbox = std::make_shared<linux::GlTexture>(Size{float(width), float(height)});
      mailbox->PublishCurrent({.texture_name = name, .pixel_width = static_cast<int>(width),
                              .pixel_height = static_cast<int>(height)});
      mailbox->Finish();
      return R::Success(std::move(mailbox));
    } catch (const std::exception& error) {
      return R::Failure({RenderErrorCode::Unsupported, error.what()});
    }
  }
};

} // namespace

std::unique_ptr<RenderBackend> CreatePlatformBgfxBackend(ImageInfo target, bgfx::Init init, std::stop_token stop) {
  if (target.format == PixelFormat::Rgba8Unorm && !init.swapChain.nwh) {
    auto* gdk = gdk_gl_context_get_current();
    const auto context = eglGetCurrentContext();
    if (gdk && context != EGL_NO_CONTEXT && !gdk_gl_context_get_use_es(gdk) && epoxy_gl_version() >= 43) {
      auto gl = init;
      gl.type = bgfx::RendererType::OpenGL;
      gl.platformData.context = context;
      const CompositeShaders shaders{avkit_vs_composite_gl, avkit_fs_composite_gl, avkit_fs_masked_blend_gl};
      try {
        return CreateBgfxBackend(target, gl, stop, std::make_unique<GdkTextureBridge>(), &shaders);
      } catch (const std::runtime_error&) {
        // A Vulkan context can still composite pixels when the GDK context is incompatible.
      }
    }
  }
  init.type = bgfx::RendererType::Vulkan;
  return CreateBgfxBackend(target, init, stop);
}

} // namespace huxerui::avkit::render::detail
