#pragma once

#include <jni.h>
#include <huxerui/avkit/video_texture.h>

namespace huxerui { class ApplicationContext; }
namespace huxerui::android { class BitmapTexture; }

namespace huxerui::avkit::render::detail {

void InstallAndroidCanvasTexture(ApplicationContext& context);
JavaVM* OpenAndroidCanvasTextureVm();
bool SupportsAndroidCanvasTexture(JavaVM* vm);

// Copies a retained SDK GLES image for direct Canvas painting on an independent EGL context.
std::shared_ptr<ExternalTexture> MakeAndroidCanvasImage(const ExternalTexture& source, JavaVM* vm);

// Publishes an immutable GPU-backed bitmap from a texture in the current GLES context.
void PublishAndroidCanvasTexture(android::BitmapTexture& texture, JavaVM* vm, std::uint32_t source,
                                 PixelSize size, RowOrigin origin);
PreviewFrameCommit PrepareAndroidCanvasTexture(std::shared_ptr<android::BitmapTexture> texture, JavaVM* vm,
                                               std::uint32_t source, PixelSize size, RowOrigin origin);

} // namespace huxerui::avkit::render::detail
