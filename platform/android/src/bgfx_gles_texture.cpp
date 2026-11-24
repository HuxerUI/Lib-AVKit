#include <GLES3/gl3.h>
#include <huxerui/android/external_texture.h>

#include "bgfx_gles_texture.h"
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

void PublishGlFrame(android::GlTexture& mailbox, GLuint name, PixelSize size) {
  android::GlTexture::Frame frame{.texture_name = name, .pixel_width = static_cast<int>(size.width),
                                 .pixel_height = static_cast<int>(size.height)};
  try {
    mailbox.PublishCurrent(frame);
  } catch (const std::runtime_error&) {
    // A separate GL image covers drivers that reject republishing a reused render target.
    PublicationTexture copy(size);
    CopyIntoPublicationTexture(name, copy.name, size);
    frame.texture_name = copy.name;
    mailbox.PublishCurrent(frame);
  }
}

class GlesTextureBridge final : public GlTextureBridge {
public:
  bool SamplesBt709Signal(const media::VideoFormat& format) const override {
    // The EGL video sampler expands the range and applies the matrix, retaining the SDR signal transfer.
    // MediaCodec's SDR transfer code also covers BT.601. Select the BT.709 profile using both fields.
    return format.pixel_format == media::VideoPixelFormat::Opaque && format.matrix == media::ColorMatrix::BT709 &&
           format.transfer == media::ColorTransfer::BT709;
  }

  bool ExpectsBt709Signal(const media::VideoEncoderConfig& config) const override {
    // The MediaCodec input surface converts RGB to YUV. It expects the requested signal transfer in RGB.
    return config.input_storage == media::VideoStorage::Platform && config.input.transfer == media::ColorTransfer::BT709;
  }

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
      PublishGlFrame(*mailbox, name, {width, height});
      mailbox->Finish();
      return R::Success(std::move(mailbox));
    } catch (const std::exception& error) {
      return R::Failure({RenderErrorCode::Unsupported, error.what()});
    }
  }
};

} // namespace

void PublishBgfxGlTexture(android::GlTexture& mailbox, const ColorSurface& surface) {
  const auto* storage = dynamic_cast<const GlStorage*>(surface.pixels.storage.get());
  if (!storage || storage->image != surface.pixels.image || storage->origin != surface.pixels.origin)
    throw std::invalid_argument("Expected a completed GLES render target");
  PublishGlFrame(mailbox, storage->name->texture, storage->image.size);
}

std::unique_ptr<RenderBackend> CreatePlatformBgfxBackend(ImageDesc target, bgfx::Init init) {
  init.type = bgfx::RendererType::OpenGLES;
  if (target.format == PixelFormat::Rgba8Unorm) {
    return CreateBgfxBackend(target, init, std::make_unique<GlesTextureBridge>());
  }
  return CreateBgfxBackend(target, init);
}

} // namespace huxerui::avkit::render::detail
