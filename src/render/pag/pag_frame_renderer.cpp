#include "pag_frame_renderer.h"

#include "render/backend.h"

#include <huxerui/avkit/component.h>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace huxerui::avkit::detail {

PagFrameRenderer::PagFrameRenderer(std::shared_ptr<pag::PAGComposition> composition, render::RenderBackend& backend)
    : backend_(backend), composition_(std::move(composition)), rate_(composition_->frameRate()) {
  const auto width = composition_->width(), height = composition_->height();
  const auto count = std::round(composition_->duration() * rate_ / 1'000'000.0);
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || !std::isfinite(rate_) || rate_ <= 0 ||
      !(count > 0 && count <= std::numeric_limits<int>::max()))
    throw std::invalid_argument("Invalid PAG composition size or timing");
  image_ = {{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}, render::PixelFormat::Rgba8Unorm};
  count_ = static_cast<int>(count);
  try { target_ = CreatePagGpuTarget(backend_, image_.size); }
  catch (const std::runtime_error& error) { gpu_error_ = error.what(); }
  if (target_) {
    player_ = std::make_unique<pag::PAGPlayer>();
    player_->setComposition(composition_);
  }
}

int PagFrameIndex(media::MediaTimestamp elapsed, AnimationPlayback playback, double rate, int count) {
  if (elapsed.ticks < 0 || !elapsed.timescale ||
      (playback != AnimationPlayback::HoldLast && playback != AnimationPlayback::Loop))
    throw std::invalid_argument("Invalid animation sample time or playback mode");
  const auto requested = std::floor(static_cast<long double>(elapsed.ticks) * rate / elapsed.timescale);
  return playback == AnimationPlayback::Loop ? static_cast<int>(std::fmod(requested, count)) :
         requested >= count ? count - 1 : static_cast<int>(requested);
}

render::Texture PagFrameRenderer::Sample(media::MediaTimestamp elapsed, AnimationPlayback playback) {
  if (!target_ && !decoder_) {
    decoder_ = pag::PAGDecoder::MakeFrom(composition_, static_cast<float>(rate_));
    if (!decoder_ || decoder_->numFrames() <= 0) throw std::runtime_error("Cannot open the PAG decoder");
    count_ = decoder_->numFrames();
    rate_ = decoder_->frameRate();
  }
  const auto index = PagFrameIndex(elapsed, playback, rate_, count_);
  if (index == current_) return frame_;
  if (target_) {
    try {
      player_->setProgress((index + 0.5) / count_);
      frame_ = target_->Draw(*player_);
      current_ = index;
      return frame_;
    } catch (const std::runtime_error& error) {
      gpu_error_ = error.what();
      player_.reset();
      target_.reset();
      frame_ = {};
      current_ = -1;
      return Sample(elapsed, playback);
    }
  }
  const auto stride = std::size_t(image_.size.width) * 4;
  auto pixels = std::make_shared<Bytes>(stride * image_.size.height);
  if (!decoder_->readFrame(index, pixels->data(), stride, pag::ColorType::RGBA_8888, pag::AlphaType::Premultiplied))
    throw std::runtime_error(gpu_error_.empty() ? "PAG could not rasterize the sampled animation frame" :
        "PAG could not rasterize the sampled animation frame after GPU fallback: " + gpu_error_);
  auto uploaded = backend_.UploadTexture({image_, BufferReference(*pixels, pixels), stride});
  if (!uploaded.Succeeded()) throw std::runtime_error(uploaded.Error().message);
  frame_ = std::move(uploaded).Value();
  current_ = index;
  return frame_;
}

} // namespace huxerui::avkit::detail
