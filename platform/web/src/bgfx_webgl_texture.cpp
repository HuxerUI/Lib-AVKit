#include <string>

#include <GLES3/gl3.h>
#include <emscripten.h>
#include <huxerui/web/external_texture.h>

#include "gl_texture_bridge.h"

namespace huxerui::avkit::render::detail {

namespace {

using emscripten::EM_VAL;

EM_JS(int, UploadVideoFrame, (GLuint name, EM_VAL frame_handle), {
  const gl = GLctx;
  const frame = Emval.toValue(frame_handle);
  const oldTexture = gl.getParameter(gl.TEXTURE_BINDING_2D);
  const oldPremultiply = gl.getParameter(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL);
  const oldFlip = gl.getParameter(gl.UNPACK_FLIP_Y_WEBGL);
  try {
    gl.bindTexture(gl.TEXTURE_2D, GL.textures[name]);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, frame);
    return gl.getError() === gl.NO_ERROR;
  } catch (err) {
    return 0;
  } finally {
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, oldPremultiply);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, oldFlip);
    gl.bindTexture(gl.TEXTURE_2D, oldTexture);
  }
});

EM_JS(EM_VAL, FrameFromCanvas, (const char* selector, double timestamp), {
  try {
    const canvas = document.querySelector(UTF8ToString(selector));
    return Emval.toHandle(new VideoFrame(canvas, {timestamp, alpha: "keep"}));
  } catch (err) {
    return Emval.toHandle(null);
  }
});

class WebGlTextureBridge final : public GlTextureBridge {
public:
  explicit WebGlTextureBridge(const char* canvas_selector) : canvas_selector_(canvas_selector) {}

  ImportedTexture Acquire(const ExternalTexture& texture) override {
    using R = ImportedTexture;
    CollectReleasedNames();
    try {
      const auto* mailbox = dynamic_cast<const web::VideoFrameTexture*>(&texture);
      if (!mailbox) return R::Failure({RenderErrorCode::Unsupported, "Expected a browser VideoFrame texture"});
      auto frame = mailbox->AcquireFrame();
      if (frame.isNull() || frame.isUndefined()) {
        return R::Failure({RenderErrorCode::Unsupported, "No browser VideoFrame is published"});
      }
      const auto width = frame["displayWidth"].as<std::uint32_t>();
      const auto height = frame["displayHeight"].as<std::uint32_t>();
      GLuint name = 0;
      glGenTextures(1, &name);
      bool uploaded = name && UploadVideoFrame(name, frame.as_handle());
      frame.call<void>("close");
      if (!uploaded) {
        glDeleteTextures(1, &name);
        return R::Failure({RenderErrorCode::Unsupported, "WebGL could not upload the browser VideoFrame"});
      }
      auto record = std::make_shared<GlTextureName>();
      record->texture = name;
      return R::Success(Retain({{width, height}, PixelFormat::Rgba8Unorm}, RowOrigin::TopLeft, std::move(record)));
    } catch (const std::exception& error) {
      return R::Failure({RenderErrorCode::Unsupported, error.what()});
    }
  }

  PublishedTexture Publish(const Texture& surface, media::MediaTimestamp pts) override {
    using R = PublishedTexture;
    const auto name = static_cast<GLuint>(GetTexture(surface));
    if (!name) return R::Failure({RenderErrorCode::Unsupported, "Expected a completed GL render target"});
    const auto [width, height] = surface.Info().size;
    try {
      const auto timestamp = pts.Rescale(1'000'000);
      if (!timestamp) {
        return R::Failure({RenderErrorCode::Unsupported, "The browser cannot publish a VideoFrame from this canvas"});
      }
      BlitToCanvas(name, width, height);
      auto frame = emscripten::val::take_ownership(FrameFromCanvas(canvas_selector_.c_str(), timestamp->ticks));
      if (frame.isNull()) {
        return R::Failure({RenderErrorCode::Unsupported, "The browser cannot publish a VideoFrame from this canvas"});
      }
      auto mailbox = std::make_shared<web::VideoFrameTexture>(Size{float(width), float(height)});
      mailbox->Publish(frame);
      frame.call<void>("close");
      mailbox->Finish();
      return R::Success(std::move(mailbox));
    } catch (const std::exception& error) {
      return R::Failure({RenderErrorCode::Unsupported, error.what()});
    }
  }

private:
  static void BlitToCanvas(GLuint name, GLsizei width, GLsizei height) {
    GLint previous_read = 0, previous_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, name, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous_draw);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glDeleteFramebuffers(1, &fbo);
    if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Cannot copy the render target to the browser canvas");
  }

  std::string canvas_selector_;
};

} // namespace

std::unique_ptr<RenderBackend> CreatePlatformBgfxBackend(ImageInfo target, bgfx::Init init, std::stop_token stop) {
  init.type = bgfx::RendererType::OpenGLES;
  const auto* canvas_selector = static_cast<const char*>(init.swapChain.nwh);
  if (!canvas_selector) throw std::invalid_argument("WebGL requires a canvas selector");
  if (target.format == PixelFormat::Rgba8Unorm) {
    return CreateBgfxBackend(target, init, stop, std::make_unique<WebGlTextureBridge>(canvas_selector));
  }
  return CreateBgfxBackend(target, init, stop);
}

} // namespace huxerui::avkit::render::detail
