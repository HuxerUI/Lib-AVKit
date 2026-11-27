#pragma once

#include <jni.h>
#include <huxerui/avkit/render.h>

namespace huxerui { class ApplicationContext; }
namespace huxerui::android { class BitmapTexture; }

namespace huxerui::avkit::render::detail {

/// @brief Registers the Java VM used by the Android Canvas texture bridge.
/// @param context Application context that owns the registration.
void InstallAndroidCanvasTexture(ApplicationContext& context);

/// @brief Opens the Java VM from the registered platform module.
JavaVM* OpenAndroidCanvasTextureVm();

/// @brief Checks whether the system can wrap a hardware buffer for Canvas drawing.
/// @param vm Java VM used to query the Android graphics API.
bool SupportsAndroidCanvasTexture(JavaVM* vm);

/// @brief Copies a retained SDK GLES image for direct Canvas painting on an independent EGL context.
/// @param source Retained SDK texture backed by a shareable GLES image.
/// @param vm Java VM used to wrap the copied image for Canvas.
std::shared_ptr<ExternalTexture> MakeAndroidCanvasImage(const ExternalTexture& source, JavaVM* vm);

/// @brief Immediately publishes an immutable GPU-backed bitmap.
/// @param texture SDK bitmap texture that receives the frame.
/// @param vm Java VM used for hardware buffer interop.
/// @param source Completed GLES texture name in the current context.
/// @param size Physical dimensions of the source texture.
/// @param origin Logical origin of the source rows.
void PublishAndroidCanvasTexture(android::BitmapTexture& texture, JavaVM* vm, std::uint32_t source,
                                 PixelSize size, RowOrigin origin);

/// @brief Prepares an immutable GPU-backed bitmap and defers publication to a commit callback.
/// @param texture SDK bitmap texture retained through the commit.
/// @param vm Java VM used to retain the prepared bitmap and attach during publication.
/// @param source Completed GLES texture name in the current context.
/// @param size Physical dimensions of the source texture.
/// @param origin Logical origin of the source rows.
/// @return Callback that publishes the prepared frame when the application applies its matching editor state.
PreviewFrameCommit PrepareAndroidCanvasTexture(std::shared_ptr<android::BitmapTexture> texture, JavaVM* vm,
                                               std::uint32_t source, PixelSize size, RowOrigin origin);

} // namespace huxerui::avkit::render::detail
