#pragma once

#include <huxerui/avkit/pag.h>
#include <pag/pag.h>

namespace huxerui::avkit::detail {

class PagGpuTarget {
public:
  virtual ~PagGpuTarget() = default;
  virtual render::ColorSurface Draw(pag::PAGPlayer& player, PagOverlayStats* stats = nullptr) = 0;
};

std::unique_ptr<PagGpuTarget> CreatePagGpuTarget(render::RenderBackend& backend, PixelSize size);
std::shared_ptr<pag::PAGFile> OpenPagAnimation(const BufferReference& encoded,
    std::span<const PagTextReplacement> texts, std::span<const PagImageReplacement> images);

int PagFrameIndex(media::MediaTimestamp elapsed, AnimationPlayback playback, double rate, int count);

class PagFrameRenderer {
public:
  PagFrameRenderer(std::shared_ptr<pag::PAGComposition> composition, render::RenderBackend* backend);
  render::ColorSurface Sample(media::MediaTimestamp elapsed, AnimationPlayback playback, PagOverlayStats* stats = nullptr);

private:
  std::shared_ptr<pag::PAGComposition> composition_;
  std::unique_ptr<PagGpuTarget> target_;
  std::unique_ptr<pag::PAGPlayer> player_;
  std::shared_ptr<pag::PAGDecoder> decoder_;
  render::ImageDesc image_;
  double rate_;
  int count_, current_{-1};
  render::ColorSurface frame_;
  std::string gpu_error_;
};

} // namespace huxerui::avkit::detail
