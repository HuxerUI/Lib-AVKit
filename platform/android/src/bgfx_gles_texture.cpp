#include <GLES3/gl3.h>
#include <huxerui/android/external_texture.h>

#include "gl_texture_bridge.h"

namespace huxerui::avkit::render::detail {

namespace {

struct PublicationTexture {
  explicit PublicationTexture(PixelSize size) : name(AllocateRgba(size)) {}
  ~PublicationTexture() { glDeleteTextures(1, &name); }
  const GLuint name;
};

void CopyIntoPublicationTexture(GLuint source, GLuint destination, PixelSize size) {
  GLint read = 0, texture = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
  glBindTexture(GL_TEXTURE_2D, destination);
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, size.width, size.height);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
  glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read));
  glDeleteFramebuffers(1, &fbo);
  if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Cannot copy the composite for EGL publication");
}

class GlesTextureBridge final : public GlTextureBridge {
public:
  ImportedTexture Acquire(const ExternalTexture& texture) override {
    using R = ImportedTexture;
    CollectReleasedNames();
    try {
      std::shared_ptr<const android::GlFrame> frame;
      if (const auto* mailbox = dynamic_cast<const android::GlTexture*>(&texture)) frame = mailbox->AcquireFrame();
      if (const auto* stream = dynamic_cast<const android::SurfaceStreamTexture*>(&texture)) frame = stream->AcquireFrame();
      if (!frame) return R::Failure({RenderErrorCode::Unsupported, "Expected a published Android GPU frame"});
      const ImageDesc image{{static_cast<std::uint32_t>(frame->PixelWidth()),
                             static_cast<std::uint32_t>(frame->PixelHeight())}, PixelFormat::Rgba8Unorm};
      auto record = std::make_shared<GlTextureName>();
      record->texture = frame->ImportCurrent();
      record->producer = std::move(frame);
      return R::Success({Retain(image, RowOrigin::BottomLeft, std::move(record))});
    } catch (const std::exception& error) {
      return R::Failure({RenderErrorCode::Unsupported, error.what()});
    }
  }

  PublishedTexture Publish(const ColorSurface& surface, media::MediaTimestamp) override {
    using R = PublishedTexture;
    const auto name = static_cast<GLuint>(GetTexture(surface.pixels));
    if (!name) return R::Failure({RenderErrorCode::Unsupported, "Expected a completed GL render target"});
    const auto [width, height] = surface.pixels.image.size;
    try {
      auto mailbox = std::make_shared<android::GlTexture>(Size{float(width), float(height)});
      android::GlTexture::Frame frame{.texture_name = name, .pixel_width = static_cast<int>(width),
                                     .pixel_height = static_cast<int>(height)};
      try {
        mailbox->PublishCurrent(frame);
      } catch (const std::runtime_error&) {
        // A separate GL image covers drivers that reject republishing a reused render target.
        PublicationTexture copy({width, height});
        CopyIntoPublicationTexture(name, copy.name, {width, height});
        frame.texture_name = copy.name;
        mailbox->PublishCurrent(frame);
      }
      mailbox->Finish();
      return R::Success(std::move(mailbox));
    } catch (const std::exception& error) {
      return R::Failure({RenderErrorCode::Unsupported, error.what()});
    }
  }
};

} // namespace

std::unique_ptr<RenderBackend> CreatePlatformBgfxBackend(ImageDesc target, bgfx::Init init) {
  if (target.format == PixelFormat::Rgba8Unorm) {
    return CreateBgfxBackend(target, init, std::make_unique<GlesTextureBridge>());
  }
  return CreateBgfxBackend(target, init);
}

} // namespace huxerui::avkit::render::detail
