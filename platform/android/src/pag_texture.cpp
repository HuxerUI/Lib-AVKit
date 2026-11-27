#include "render/pag/pag_frame_renderer.h"
#include "render/bgfx/video_texture_internal.h"
#include "render/texture_backing.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <stdexcept>

namespace huxerui::avkit::detail {
namespace {

class PagEglContext {
public:
  PagEglContext() : display(eglGetCurrentDisplay()), producer(eglGetCurrentContext()) {
    EGLint id = 0, count = 0;
    if (display == EGL_NO_DISPLAY || producer == EGL_NO_CONTEXT || !eglQueryContext(display, producer, EGL_CONFIG_ID, &id))
      throw std::runtime_error("PAG needs an active Android render context");
    const EGLint match[]{EGL_CONFIG_ID, id, EGL_NONE};
    EGLConfig config{};
    if (!eglChooseConfig(display, match, &config, 1, &count) || !count)
      throw std::runtime_error("No Android EGL config for PAG texture sharing");
    const EGLint version[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context = eglCreateContext(display, config, producer, version);
    if (context == EGL_NO_CONTEXT) throw std::runtime_error("Cannot create the shared PAG context");
    const EGLint dimensions[]{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surface = eglCreatePbufferSurface(display, config, dimensions);
    if (surface == EGL_NO_SURFACE) {
      eglDestroyContext(display, context);
      throw std::runtime_error("Cannot create the PAG offscreen EGL surface");
    }
  }

  ~PagEglContext() {
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
  }

  EGLDisplay display;
  EGLContext producer, context;
  EGLSurface surface;
};

class CurrentEgl {
public:
  explicit CurrentEgl(const PagEglContext& target)
      : display_(target.display), restore_(eglGetCurrentDisplay()), context_(eglGetCurrentContext()),
        draw_(eglGetCurrentSurface(EGL_DRAW)), read_(eglGetCurrentSurface(EGL_READ)) {
    if (!eglMakeCurrent(display_, target.surface, target.surface, target.context))
      throw std::runtime_error("Cannot bind the shared PAG EGL context");
  }

  ~CurrentEgl() {
    eglMakeCurrent(restore_ == EGL_NO_DISPLAY ? display_ : restore_, draw_, read_, context_);
  }

private:
  EGLDisplay display_, restore_;
  EGLContext context_;
  EGLSurface draw_, read_;
};

std::shared_ptr<PagEglContext> SharedContext() {
  static thread_local std::weak_ptr<PagEglContext> last;
  auto context = last.lock();
  if (!context || context->producer != eglGetCurrentContext() || context->display != eglGetCurrentDisplay()) {
    context = std::make_shared<PagEglContext>();
    last = context;
  }
  return context;
}

class PagGlesTarget final : public PagGpuTarget {
public:
  PagGlesTarget(render::RenderBackend& backend, PixelSize size) : backend_(backend), egl_(SharedContext()), size_(size) {}

  ~PagGlesTarget() override {
    try {
      CurrentEgl current(*egl_);
      slots_.clear();
    } catch (const std::runtime_error&) {}
  }

  render::Texture Draw(pag::PAGPlayer& player) override {
    auto& slot = AcquireOutput();
    GLsync completed = nullptr;
    {
      CurrentEgl current(*egl_);
      player.setSurface(slot.drawable);
      player.flush();
      completed = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
      if (completed) glFlush();
      else glFinish();
    }
    if (completed) {
      glWaitSync(completed, 0, GL_TIMEOUT_IGNORED);
      glDeleteSync(completed);
    }
    return slot.texture;
  }

private:
  struct OutputSlot {
    render::Texture texture;
    std::shared_ptr<pag::PAGSurface> drawable;
  };

  OutputSlot& AcquireOutput() {
    const auto existing = std::ranges::find_if(slots_, [](const auto& slot) {
      return TextureAccess::Backing(slot.texture).use_count() == 1;
    });
    if (existing != slots_.end()) return *existing;
    auto surface = render::detail::CreateBgfxInteropTexture(backend_, {size_, render::PixelFormat::Rgba8Unorm});
    const auto name = static_cast<GLuint>(render::detail::GetBgfxInteropTexture(backend_, surface));
    if (!name) throw std::runtime_error("Cannot allocate the Android PAG render texture");
    CurrentEgl current(*egl_);
    const pag::BackendTexture texture({name, GL_TEXTURE_2D, GL_RGBA8}, int(size_.width), int(size_.height));
    auto drawable = pag::PAGSurface::MakeFrom(texture, pag::ImageOrigin::BottomLeft);
    if (!drawable) throw std::runtime_error("Cannot wrap the Android PAG render texture");
    return slots_.emplace_back(OutputSlot{std::move(surface), std::move(drawable)});
  }

  render::RenderBackend& backend_;
  std::shared_ptr<PagEglContext> egl_;
  PixelSize size_;
  std::vector<OutputSlot> slots_;
};

} // namespace

std::unique_ptr<PagGpuTarget> CreatePagGpuTarget(render::RenderBackend& backend, PixelSize size) {
  return std::make_unique<PagGlesTarget>(backend, size);
}

} // namespace huxerui::avkit::detail
