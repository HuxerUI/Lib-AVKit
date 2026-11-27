#pragma once

/// @file
/// @brief Samples PAG compositions and renders the selected frame.

#include <memory>
#include <string>

#include <huxerui/avkit/render.h>
#include <pag/pag.h>

namespace huxerui::avkit {

enum class AnimationPlayback;

namespace detail {

class PagGpuTarget {
public:
  virtual ~PagGpuTarget() = default;

  virtual render::Texture Draw(pag::PAGPlayer& player) = 0;
};

std::unique_ptr<PagGpuTarget> CreatePagGpuTarget(render::RenderBackend& backend, PixelSize size);

int PagFrameIndex(media::MediaTimestamp elapsed, AnimationPlayback playback, double rate, int count);

class PagFrameRenderer {
public:
  PagFrameRenderer(std::shared_ptr<pag::PAGComposition> composition, render::RenderBackend& backend);

  render::Texture Sample(media::MediaTimestamp elapsed, AnimationPlayback playback);

private:
  render::RenderBackend& backend_;
  std::shared_ptr<pag::PAGComposition> composition_;
  std::unique_ptr<PagGpuTarget> target_;
  std::unique_ptr<pag::PAGPlayer> player_;
  std::shared_ptr<pag::PAGDecoder> decoder_;
  render::ImageInfo image_;
  double rate_;
  int count_, current_{-1};
  render::Texture frame_;
  std::string gpu_error_;
};

} // namespace detail
} // namespace huxerui::avkit
