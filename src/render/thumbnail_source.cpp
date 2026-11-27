#include <huxerui/avkit/asset_sampling.h>

#include "media/video_texture.h"

#if defined(__ANDROID__)
#include "android_canvas_texture.h"
#endif

namespace huxerui::avkit {
namespace {

ThumbnailImage MakeSoftwareThumbnail(const media::VideoFrame& frame, media::VideoTextureOptions options) {
  const auto fitted = avkit::detail::FitCpuVideoFrame(frame, options);
  if (!fitted.Succeeded()) throw std::runtime_error(fitted.Error().message);
  const auto& pixels = fitted.Value();
  return Bitmap::CopyPixels(options.width, options.height, pixels.bytes.AsBytes(), pixels.row_bytes,
                            Bitmap::PixelFormat::Rgba8888);
}

} // namespace

MakeThumbnail OpenCanvasThumbnailSource() {
#if defined(__ANDROID__)
  const auto to_canvas = [vm = render::detail::OpenAndroidCanvasTextureVm()](std::shared_ptr<ExternalTexture> texture) {
    try {
      return render::detail::MakeAndroidCanvasImage(*texture, vm);
    } catch (const std::exception& error) {
      throw PlatformVideoImportError(error.what());
    }
  };
#else
  const auto to_canvas = [](std::shared_ptr<ExternalTexture> texture) { return texture; };
#endif
  return [to_canvas](const media::VideoFrame& frame, PixelSize size) -> ThumbnailImage {
    const media::VideoTextureOptions options{size.width, size.height, ImageFit::Cover};
    if (frame.Storage() == media::VideoStorage::Cpu) return MakeSoftwareThumbnail(frame, options);
    auto texture = media::MakeExternalTexture(frame, options);
    if (!texture.Succeeded()) throw PlatformVideoImportError(texture.Error().message);
    return to_canvas(std::move(texture).Value());
  };
}

} // namespace huxerui::avkit
