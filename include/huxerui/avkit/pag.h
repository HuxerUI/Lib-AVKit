#pragma once

/// @file
/// @brief Optional libpag adapter for retained animation frames and two-input compositions.

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <huxerui/avkit/animation.h>
#include <huxerui/avkit/render.h>
#include <huxerui/media_format.h>

namespace huxerui::avkit {

/// @brief Registers the first face in a packaged font for use by PAG text layers. Link AVKit::PAG.
/// Share a font between animations using the same resource. The last owner releases the registration.
class PagFont {
public:
  /// @throws std::invalid_argument If the bytes do not contain a supported font.
  explicit PagFont(BufferReference encoded);
  ~PagFont();
  PagFont(const PagFont&) = delete;
  PagFont& operator=(const PagFont&) = delete;

private:
  friend class PagAnimation;
  BufferReference encoded_;
  std::string family_;
};

/// @brief Resolved text slot in one version of a PAG resource.
struct PagTextReplacement {
  int editable_index;
  /// Omit to keep the authored text while applying the packaged font.
  std::optional<std::string> text;
  std::shared_ptr<const PagFont> font;
};

/// @brief Resolved image slot in one version of a PAG resource.
struct PagImageReplacement {
  int editable_index;
  BufferReference encoded;
};

/// @brief Samples one immutable PAG composition with instance-specific replacements. Link AVKit::PAG.
/// Create, sample, and destroy the instance on its owning sequence.
class PagAnimation {
public:
  /// @brief Opens the composition and applies replacements before the first sample.
  /// @throws std::invalid_argument For invalid content, timing, or editable layer indices.
  explicit PagAnimation(BufferReference encoded, std::span<const PagTextReplacement> texts = {},
                        std::span<const PagImageReplacement> images = {});
  ~PagAnimation();
  PagAnimation(const PagAnimation&) = delete;
  PagAnimation& operator=(const PagAnimation&) = delete;

  /// @brief Rasterizes a clip-local elapsed time to a retained premultiplied RGBA8 surface.
  /// Previous surfaces stay readable when another time is sampled.
  /// @throws std::invalid_argument For an invalid elapsed time or playback mode.
  /// @throws std::runtime_error If libpag cannot render the requested animation frame.
  render::ColorSurface Sample(media::MediaTimestamp elapsed, AnimationPlayback playback);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// @brief CPU elapsed time for the stages of one PAG transition sample.
struct PagTransitionTiming {
  std::chrono::nanoseconds stage_inputs{};
  std::chrono::nanoseconds bind_images{};
  std::chrono::nanoseconds flush{};
  std::chrono::nanoseconds wait_for_output{};
};

/// @brief Renders an authored PAG composition with two current GPU pictures. Link AVKit::PAG.
/// Create, sample, and destroy the instance on its owning sequence.
/// The GPU transition adapter is available on macOS.
class PagTransition {
public:
  /// @brief Opens a composition with two distinct editable image indices and a fixed output size.
  /// @throws std::runtime_error When the target platform has no GPU transition adapter.
  explicit PagTransition(BufferReference encoded, PixelSize size, int outgoing_index, int incoming_index);
  ~PagTransition();
  PagTransition(const PagTransition&) = delete;
  PagTransition& operator=(const PagTransition&) = delete;

  /// @brief Samples the half-open overlap with two completed, premultiplied BGRA8 textures.
  /// Inputs use the declared size and top-left origin on one GPU device. The returned premultiplied
  /// GPU surface remains immutable while leased. Both images are rebound on every sample.
  render::ColorSurface Sample(media::MediaTimestamp offset, media::MediaTimestamp duration,
                              const render::ColorSurface& outgoing, const render::ColorSurface& incoming,
                              PagTransitionTiming* timing = nullptr);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::avkit
