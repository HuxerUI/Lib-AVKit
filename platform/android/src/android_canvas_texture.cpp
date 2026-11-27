#include "android_canvas_texture.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <huxerui/android/external_texture.h>
#include <huxerui/android/platform_registry.h>
#include <huxerui/app.h>

namespace huxerui::avkit::render::detail {
namespace {

constexpr char kCanvasTextureVm[] = "avkit/canvas-texture-vm";

class AttachedThread {
public:
  explicit AttachedThread(JavaVM* vm) : vm_(vm) {
    const auto status = vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) detach_ = vm_->AttachCurrentThread(&env, nullptr) == JNI_OK;
    if (!env) throw std::runtime_error("Cannot access Android bitmap services on the render worker");
  }

  ~AttachedThread() { if (detach_) vm_->DetachCurrentThread(); }

  JNIEnv* env{};

private:
  JavaVM* vm_;
  bool detach_{};
};

JNIEnv* RenderEnvironment(JavaVM* vm) {
  thread_local AttachedThread thread(vm);
  return thread.env;
}

class CanvasImportContext {
public:
  ~CanvasImportContext() {
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
  }

  void Open() {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    constexpr EGLint config_options[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config{};
    EGLint count{};
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr) ||
        !eglChooseConfig(display, config_options, &config, 1, &count) || !count)
      throw std::runtime_error("Cannot choose an EGL configuration for Canvas image transfer");

    const auto previous_api = eglQueryAPI();
    if (!eglBindAPI(EGL_OPENGL_ES_API)) throw std::runtime_error("Cannot select the GLES API for Canvas image transfer");
    constexpr EGLint context_options[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_options);
    eglBindAPI(previous_api);
    constexpr EGLint surface_options[]{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surface = eglCreatePbufferSurface(display, config, surface_options);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE)
      throw std::runtime_error("Cannot create the EGL context for Canvas image transfer");
  }

  EGLDisplay display{EGL_NO_DISPLAY};
  EGLContext context{EGL_NO_CONTEXT};
  EGLSurface surface{EGL_NO_SURFACE};
};

class ScopedCanvasContext {
public:
  explicit ScopedCanvasContext(const CanvasImportContext& target)
      : target_display_(target.display), display_(eglGetCurrentDisplay()),
        draw_(eglGetCurrentSurface(EGL_DRAW)), read_(eglGetCurrentSurface(EGL_READ)),
        context_(eglGetCurrentContext()), api_(eglQueryAPI()) {
    if (!eglBindAPI(EGL_OPENGL_ES_API) || !eglMakeCurrent(target.display, target.surface, target.surface, target.context)) {
      eglBindAPI(api_);
      throw std::runtime_error("Cannot bind the EGL context for Canvas image transfer");
    }
  }

  ~ScopedCanvasContext() {
    if (display_ == target_display_ && api_ == EGL_OPENGL_ES_API) {
      eglMakeCurrent(display_, draw_, read_, context_);
    } else {
      eglMakeCurrent(target_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglBindAPI(api_);
      if (context_ != EGL_NO_CONTEXT) eglMakeCurrent(display_, draw_, read_, context_);
    }
  }

private:
  EGLDisplay target_display_, display_;
  EGLSurface draw_, read_;
  EGLContext context_;
  EGLenum api_;
};

android::LocalRef<jobject> CopyCanvasBitmap(JNIEnv* env, std::uint32_t source, PixelSize size, RowOrigin origin) {
  // The hardware bitmap is painted in the Canvas without a separate TextureView surface.
  const android::LocalRef<jclass> bitmap_class(env, env->FindClass("android/graphics/Bitmap"));
  const auto wrap = bitmap_class ? env->GetStaticMethodID(bitmap_class.Get(), "wrapHardwareBuffer",
      "(Landroid/hardware/HardwareBuffer;Landroid/graphics/ColorSpace;)Landroid/graphics/Bitmap;") : nullptr;
  if (env->ExceptionCheck()) env->ExceptionClear();

  static const auto allocate = reinterpret_cast<int (*)(const AHardwareBuffer_Desc*, AHardwareBuffer**)>(
      dlsym(RTLD_DEFAULT, "AHardwareBuffer_allocate"));
  static const auto release = reinterpret_cast<void (*)(AHardwareBuffer*)>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_release"));
  static const auto to_java = reinterpret_cast<jobject (*)(JNIEnv*, AHardwareBuffer*)>(
      dlsym(RTLD_DEFAULT, "AHardwareBuffer_toHardwareBuffer"));
  static const auto client_buffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
  static const auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  static const auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  static const auto image_texture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (!wrap || !allocate || !release || !to_java || !client_buffer || !create_image || !destroy_image || !image_texture)
    throw std::runtime_error("Android hardware bitmap presentation is unavailable");

  const auto display = eglGetCurrentDisplay();
  if (!source || display == EGL_NO_DISPLAY) throw std::runtime_error("Expected a texture in a current EGL context");
  const AHardwareBuffer_Desc desc{.width = size.width, .height = size.height, .layers = 1,
      .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
      .usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE};
  AHardwareBuffer* buffer{};
  if (allocate(&desc, &buffer) != 0 || !buffer) throw std::runtime_error("Cannot allocate an Android image buffer");
  const std::unique_ptr<AHardwareBuffer, decltype(release)> backing(buffer, release);
  struct Images {
    explicit Images(EGLDisplay current) : display(current), scissor(glIsEnabled(GL_SCISSOR_TEST)) {
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
      if (scissor) glDisable(GL_SCISSOR_TEST);
    }
    EGLDisplay display;
    GLboolean scissor;
    GLint read{}, draw{}, texture{};
    EGLImageKHR image{EGL_NO_IMAGE_KHR};
    GLuint destination{}, framebuffers[2]{};

    ~Images() {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read));
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw));
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
      if (scissor) glEnable(GL_SCISSOR_TEST);
      glDeleteFramebuffers(2, framebuffers);
      glDeleteTextures(1, &destination);
      if (image != EGL_NO_IMAGE_KHR) destroy_image(display, image);
    }
  } gpu{display};
  gpu.image = create_image(gpu.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client_buffer(buffer), nullptr);
  if (gpu.image == EGL_NO_IMAGE_KHR) throw std::runtime_error("Cannot import the Android image buffer into EGL");
  glGenTextures(1, &gpu.destination);
  glBindTexture(GL_TEXTURE_2D, gpu.destination);
  image_texture(GL_TEXTURE_2D, gpu.image);
  glGenFramebuffers(2, gpu.framebuffers);
  for (unsigned i = 0; i < 2; ++i) {
    const GLenum target = i == 0 ? GL_READ_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER;
    glBindFramebuffer(target, gpu.framebuffers[i]);
    glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, i == 0 ? source : gpu.destination, 0);
    if (glCheckFramebufferStatus(target) != GL_FRAMEBUFFER_COMPLETE)
      throw std::runtime_error("Cannot attach the Android image buffer");
  }
  const auto width = static_cast<GLint>(size.width), height = static_cast<GLint>(size.height);
  const bool flip = origin == RowOrigin::BottomLeft;
  glBlitFramebuffer(0, flip ? height : 0, width, flip ? 0 : height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glFinish();
  if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Cannot blit the Android image buffer");
  const android::LocalRef<jobject> hardware_buffer(env, to_java(env, buffer));
  android::LocalRef<jobject> bitmap(env, hardware_buffer ? env->CallStaticObjectMethod(bitmap_class.Get(), wrap,
      hardware_buffer.Get(), static_cast<jobject>(nullptr)) : nullptr);
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (hardware_buffer) {
    const android::LocalRef<jclass> buffer_class(env, env->GetObjectClass(hardware_buffer.Get()));
    const auto close = buffer_class ? env->GetMethodID(buffer_class.Get(), "close", "()V") : nullptr;
    if (close) env->CallVoidMethod(hardware_buffer.Get(), close);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  if (!bitmap) throw std::runtime_error("Cannot wrap the Android image buffer in a hardware bitmap");
  return bitmap;
}

} // namespace

std::shared_ptr<ExternalTexture> MakeAndroidCanvasImage(const ExternalTexture& source, JavaVM* vm) {
  const auto* gl = dynamic_cast<const android::GlTexture*>(&source);
  const auto frame = gl ? gl->AcquireFrame() : nullptr;
  if (!frame) throw std::runtime_error("The Android Canvas image source has no GLES frame");

  try {
    thread_local const auto context = [] {
      auto result = std::make_unique<CanvasImportContext>();
      result->Open();
      return result;
    }();
    const ScopedCanvasContext current(*context);
    struct ImportedTexture {
      GLuint name;

      ~ImportedTexture() { glDeleteTextures(1, &name); }
    } imported{frame->ImportCurrent()};
    const PixelSize size{static_cast<std::uint32_t>(frame->PixelWidth()), static_cast<std::uint32_t>(frame->PixelHeight())};
    auto output = std::make_shared<android::BitmapTexture>(source.IntrinsicSize());
    PublishAndroidCanvasTexture(*output, vm, imported.name, size, RowOrigin::BottomLeft);
    output->Finish();
    thread_local bool reported{};
    if (!std::exchange(reported, true))
      __android_log_print(ANDROID_LOG_INFO, "AVKit", "Published GPU thumbnails for Canvas painting");
    return output;
  } catch (const std::exception& error) {
    thread_local bool reported{};
    if (!std::exchange(reported, true))
      __android_log_print(ANDROID_LOG_WARN, "AVKit", "GPU thumbnail presentation unavailable: %s", error.what());
    throw;
  }
}

void PublishAndroidCanvasTexture(android::BitmapTexture& texture, JavaVM* vm, std::uint32_t source,
                                 PixelSize size, RowOrigin origin) {
  auto* env = RenderEnvironment(vm);
  const auto bitmap = CopyCanvasBitmap(env, source, size, origin);
  texture.Publish(env, bitmap.Get());
}

PreviewFrameCommit PrepareAndroidCanvasTexture(std::shared_ptr<android::BitmapTexture> texture, JavaVM* vm,
                                               std::uint32_t source, PixelSize size, RowOrigin origin) {
  auto* env = RenderEnvironment(vm);
  const auto bitmap = CopyCanvasBitmap(env, source, size, origin);
  auto reference = env->NewGlobalRef(bitmap.Get());
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (!reference) throw std::runtime_error("Cannot retain the prepared Android preview bitmap");
  auto retained = std::shared_ptr<std::remove_pointer_t<jobject>>(reference, [vm](jobject value) noexcept {
    try {
      const AttachedThread thread(vm);
      thread.env->DeleteGlobalRef(value);
    } catch (const std::runtime_error&) {}
  });
  return [texture = std::move(texture), retained = std::move(retained), vm] {
    const AttachedThread thread(vm);
    texture->Publish(thread.env, retained.get());
  };
}

void InstallAndroidCanvasTexture(ApplicationContext& context) {
  context.RegisterPlatformModule<JavaVM*>(kCanvasTextureVm, [](Runtime& runtime) {
    JavaVM* vm{};
    if (android::GetPlatformEnv(runtime).jni->GetJavaVM(&vm) != JNI_OK || !vm)
      throw std::runtime_error("Cannot access the Android Java VM");
    return vm;
  });
}

JavaVM* OpenAndroidCanvasTextureVm() { return OpenPlatformModule<JavaVM*>(kCanvasTextureVm); }

bool SupportsAndroidCanvasTexture(JavaVM* vm) {
  const AttachedThread thread(vm);
  auto* env = thread.env;
  const android::LocalRef<jclass> bitmap(env, env->FindClass("android/graphics/Bitmap"));
  const auto wrap = bitmap ? env->GetStaticMethodID(bitmap.Get(), "wrapHardwareBuffer",
      "(Landroid/hardware/HardwareBuffer;Landroid/graphics/ColorSpace;)Landroid/graphics/Bitmap;") : nullptr;
  if (env->ExceptionCheck()) env->ExceptionClear();
  return wrap && dlsym(RTLD_DEFAULT, "AHardwareBuffer_allocate") && dlsym(RTLD_DEFAULT, "AHardwareBuffer_toHardwareBuffer");
}

} // namespace huxerui::avkit::render::detail
