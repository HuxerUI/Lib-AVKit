#pragma once

#include <huxerui/camera.h>

namespace huxerui::avkit {
struct CanvasOverlay;
struct PixelSize;
}

namespace avkit_preview {

struct CaptureDevice {
  huxerui::View preview;
  bool ready{};
  bool recording{};
  bool paused{};
  bool sticker_available{};
  double seconds{};
  std::string error;
  std::function<huxerui::Task<void>(huxerui::File)> photo;
  std::function<huxerui::Task<void>(huxerui::File)> record;
  std::function<void(bool)> pause;
  std::function<void()> stop;
};

huxerui::Task<void> RequestCapturePermission(huxerui::ApplicationHandle application, huxerui::Permission permission);
void WriteCaptureFile(const huxerui::File& file, std::span<const std::byte> bytes);
huxerui::avkit::CanvasOverlay CaptureSticker(huxerui::avkit::PixelSize canvas);
CaptureDevice UseCaptureDevice(huxerui::State<bool> robot, huxerui::State<bool> sticker);

} // namespace avkit_preview
