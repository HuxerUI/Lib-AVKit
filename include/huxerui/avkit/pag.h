#pragma once

/// @file
/// @brief libpag adapter for retained animation frames and two-input compositions.

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <huxerui/avkit/animation.h>
#include <huxerui/avkit/render.h>
#include <huxerui/media_format.h>

namespace huxerui::avkit {

struct CanvasOverlay;
struct ResourceRef;
struct StickerReplacements;
struct TimelineFrame;

/// @brief Registers the first face in a packaged font for use by PAG text layers. Link AVKit::PAG.
/// Share a font between animations using the same resource. The last owner releases the registration.
class PagFont {
public:
  /// @throws std::invalid_argument If the bytes do not contain a supported font.
  explicit PagFont(BufferReference encoded);
  ~PagFont();
  /// @brief Family registered for this packaged font on its owning sequence.
  const std::string& Family() const noexcept { return family_; }
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
  /// @param backend Optional graphics backend kept alive on the owning sequence through destruction.
  /// @throws std::invalid_argument For invalid content, timing, or editable layer indices.
  explicit PagAnimation(BufferReference encoded, std::span<const PagTextReplacement> texts = {},
                        std::span<const PagImageReplacement> images = {}, render::RenderBackend* backend = nullptr);
  ~PagAnimation();
  PagAnimation(const PagAnimation&) = delete;
  PagAnimation& operator=(const PagAnimation&) = delete;

  /// @brief Samples a clip-local elapsed time to a retained premultiplied color surface.
  /// Previous surfaces stay readable when another time is sampled.
  /// @throws std::invalid_argument For an invalid elapsed time or playback mode.
  /// @throws std::runtime_error If libpag cannot render the requested animation frame.
  render::ColorSurface Sample(media::MediaTimestamp elapsed, AnimationPlayback playback);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// @brief Authored animation and resolved editable slots for one overlay instance.
struct PagOverlayAsset {
  BufferReference encoded;
  std::vector<PagTextReplacement> texts;
  std::vector<PagImageReplacement> images;
};

/// @brief Supplies packaged assets without giving the application ownership of the render cache.
struct PagOverlayResources {
  BufferReference point_text;
  std::function<BufferReference(const ResourceRef&)> font;
  std::function<PagOverlayAsset(const CanvasOverlay&)> canvas_animation;
  std::function<PagOverlayAsset(const ResourceRef&, const StickerReplacements&)> clip_animation;
};

/// @brief Optional cumulative counters for one overlay renderer on its owning sequence.
struct PagOverlayStats {
  std::size_t flushes{}, batched_flushes{};
  std::size_t text_layers{}, text_layer_cache_hits{}; ///< Prepared layers and reuses, independent of GPU flushes.
  std::chrono::nanoseconds gpu_sync{}; ///< Time in platform fence and completion calls.
};

/// @brief Measures the normalized artwork bounds used when selecting a canvas overlay.
Rect MeasurePagOverlay(const CanvasOverlay& overlay, PixelSize canvas, const PagOverlayResources& resources);

/// @brief Binds PAG and other color sources for a complete timeline plan, before its bgfx submission.
/// Keep the backend alive on the same execution sequence until this renderer is destroyed.
class PagOverlayRenderer {
public:
  PagOverlayRenderer(render::RenderBackend& backend, PagOverlayResources resources);
  ~PagOverlayRenderer();
  using ResolveColor = std::function<render::ColorSurface(std::size_t slot)>;
  /// @brief Retains and caches resolved textures. Data inputs retain their original slot order.
  /// The reference path keeps each overlay in a dedicated texture for diagnostics.
  render::RenderRequest Prepare(const TimelineFrame& frame, const ResolveColor& resolve_color,
                                std::vector<render::ImageSurface> data = {}, PagOverlayStats* stats = nullptr,
                                bool batch_overlays = true);

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
