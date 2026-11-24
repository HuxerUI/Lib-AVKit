#include "workbench_thumbnail.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdexcept>
#include <utility>

#include <huxerui/android/external_texture.h>
#include <huxerui/android/platform_registry.h>

#include "workbench_media.h"

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;

constexpr char kThumbnailVm[] = "avkit/thumbnail-vm";

class AttachedThread {
public:
  explicit AttachedThread(JavaVM* vm) : vm_(vm) {
    const auto status = vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) detach_ = vm_->AttachCurrentThread(&env, nullptr) == JNI_OK;
    if (!env) throw std::runtime_error("Cannot access Android bitmap services on the thumbnail worker");
  }

  ~AttachedThread() { if (detach_) vm_->DetachCurrentThread(); }

  JNIEnv* env{};

private:
  JavaVM* vm_;
  bool detach_{};
};

class ThumbnailContext {
public:
  ~ThumbnailContext() {
    if (display == EGL_NO_DISPLAY) return;
    if (eglGetCurrentContext() == context) eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
  }

  void Open() {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    constexpr EGLint config_options[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config{};
    EGLint count{};
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(display, config_options, &config, 1, &count) || !count)
      throw std::runtime_error("Cannot choose an EGL configuration for thumbnails");
    constexpr EGLint context_options[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    constexpr EGLint surface_options[]{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_options);
    surface = eglCreatePbufferSurface(display, config, surface_options);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || !eglMakeCurrent(display, surface, surface, context))
      throw std::runtime_error("Cannot create the Android thumbnail EGL context");
  }

  EGLDisplay display{EGL_NO_DISPLAY};

private:
  EGLContext context{EGL_NO_CONTEXT};
  EGLSurface surface{EGL_NO_SURFACE};
};

std::shared_ptr<ExternalTexture> MakeCanvasThumbnail(JavaVM* vm, const android::GlFrame& frame, PixelSize size) {
  // A hardware Bitmap is drawn in the existing Canvas, so scrolling cannot recycle a TextureView's surface.
  thread_local AttachedThread thread(vm);
  auto* env = thread.env;
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

  thread_local const auto context = [] {
    auto result = std::make_unique<ThumbnailContext>();
    result->Open();
    return result;
  }();
  const AHardwareBuffer_Desc desc{.width = size.width, .height = size.height, .layers = 1,
      .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
      .usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE};
  AHardwareBuffer* buffer{};
  if (allocate(&desc, &buffer) != 0 || !buffer) throw std::runtime_error("Cannot allocate an Android thumbnail buffer");
  const std::unique_ptr<AHardwareBuffer, decltype(release)> backing(buffer, release);
  struct Images {
    EGLDisplay display;
    EGLImageKHR image{EGL_NO_IMAGE_KHR};
    GLuint textures[2]{}, framebuffers[2]{};
    ~Images() {
      glDeleteFramebuffers(2, framebuffers);
      glDeleteTextures(2, textures);
      if (image != EGL_NO_IMAGE_KHR) destroy_image(display, image);
    }
  } gpu{context->display};
  gpu.textures[0] = frame.ImportCurrent();
  gpu.image = create_image(gpu.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client_buffer(buffer), nullptr);
  if (gpu.image == EGL_NO_IMAGE_KHR) throw std::runtime_error("Cannot import the Android thumbnail buffer into EGL");
  glGenTextures(1, &gpu.textures[1]);
  glBindTexture(GL_TEXTURE_2D, gpu.textures[1]);
  image_texture(GL_TEXTURE_2D, gpu.image);
  glGenFramebuffers(2, gpu.framebuffers);
  for (unsigned i = 0; i < 2; ++i) {
    const GLenum target = i == 0 ? GL_READ_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER;
    glBindFramebuffer(target, gpu.framebuffers[i]);
    glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gpu.textures[i], 0);
    if (glCheckFramebufferStatus(target) != GL_FRAMEBUFFER_COMPLETE)
      throw std::runtime_error("Cannot attach the Android thumbnail buffer");
  }
  const auto width = static_cast<GLint>(size.width), height = static_cast<GLint>(size.height);
  glBlitFramebuffer(0, height, width, 0, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glFinish();
  if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Cannot blit the Android thumbnail buffer");
  const android::LocalRef<jobject> hardware_buffer(env, to_java(env, buffer));
  const android::LocalRef<jobject> bitmap(env, hardware_buffer ? env->CallStaticObjectMethod(bitmap_class.Get(), wrap,
      hardware_buffer.Get(), static_cast<jobject>(nullptr)) : nullptr);
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (hardware_buffer) {
    const android::LocalRef<jclass> buffer_class(env, env->GetObjectClass(hardware_buffer.Get()));
    const auto close = buffer_class ? env->GetMethodID(buffer_class.Get(), "close", "()V") : nullptr;
    if (close) env->CallVoidMethod(hardware_buffer.Get(), close);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  if (!bitmap) throw std::runtime_error("Cannot wrap the Android thumbnail buffer in a hardware bitmap");
  auto texture = std::make_shared<android::BitmapTexture>(Size{float(size.width), float(size.height)});
  texture->Publish(env, bitmap.Get());
  texture->Finish();
  thread_local bool reported{};
  if (!std::exchange(reported, true))
    __android_log_print(ANDROID_LOG_INFO, "AVKitPreview", "Timeline thumbnails present GPU buffers in the Canvas");
  return texture;
}

} // namespace

void InstallWorkbenchMedia(ApplicationContext& context) {
  context.RegisterPlatformModule<JavaVM*>(kThumbnailVm, [](Runtime& runtime) {
    JavaVM* vm{};
    if (android::GetPlatformEnv(runtime).jni->GetJavaVM(&vm) != JNI_OK || !vm)
      throw std::runtime_error("Cannot access the Android Java VM");
    return vm;
  });
}

MakeThumbnail OpenWorkbenchThumbnailSource() {
  auto* vm = OpenPlatformModule<JavaVM*>(kThumbnailVm);
  return [vm](const media::VideoFrame& frame, PixelSize size) -> ThumbnailImage {
    auto image = MakeStripThumbnail(frame, size);
    if (const auto* texture = std::get_if<std::shared_ptr<ExternalTexture>>(&image)) {
      const auto gpu = std::dynamic_pointer_cast<android::GlTexture>(*texture);
      if (const auto picture = gpu ? gpu->AcquireFrame() : nullptr) {
        try { return MakeCanvasThumbnail(vm, *picture, size); }
        catch (const std::exception& failure) {
          thread_local bool reported{};
          if (!std::exchange(reported, true))
            __android_log_print(ANDROID_LOG_WARN, "AVKitPreview", "GPU thumbnail presentation unavailable: %s", failure.what());
          throw PlatformVideoImportError(failure.what());
        }
      }
      throw PlatformVideoImportError("The Android thumbnail texture has no GPU frame");
    }
    return image;
  };
}

} // namespace avkit_preview
