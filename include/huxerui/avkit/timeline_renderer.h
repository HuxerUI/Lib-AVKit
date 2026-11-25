#pragma once

/// @file
/// @brief Resolves and composites sampled timeline frames on a retained bgfx backend.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <huxerui/avkit/pag.h>
#include <huxerui/avkit/timeline.h>
#include <huxerui/avkit/video_texture.h>

namespace huxerui::avkit::render {

/// @brief Immutable coverage frames in authored order. Use R8Unorm with the same dimensions throughout.
/// Clip masks use the frame interval. Transition masks distribute the frames over the transition duration.
struct MaskSequence {
  std::uint32_t frame_count{};
  media::MediaTimestamp frame_interval;
  std::function<ImageSurface(std::uint32_t index)> frame; ///< Loads one authored frame on demand.
};

/// @brief Authored PAG transition and its two editable image indices.
struct PagTransitionAsset {
  BufferReference encoded;
  int outgoing_index;
  int incoming_index;
};

/// @brief Resolves application resources on the render sequence.
/// Versioned references identify immutable resources for the lifetime of the renderer.
/// A resolver is required only when a frame references that kind of resource.
struct TimelineRenderResources {
  PagOverlayResources overlays;
  std::function<ColorSurface(const ResourceRef&)> sticker;
  std::function<ImageSurface(const ResourceRef&)> static_mask;
  std::function<MaskSequence(const ResourceRef&)> mask_sequence;
  std::function<PagTransitionAsset(const ResourceRef&)> pag_transition;
};

/// @brief Elapsed time in transition rendering. Upload time measures CPU packing and enqueue of the final pass.
struct PagTransitionRenderTiming {
  PagTransitionTiming pag;
  std::chrono::nanoseconds prepare_sides{};
  std::chrono::nanoseconds enqueue_upload{};
  std::chrono::nanoseconds complete_output{};
};

/// @brief Optional diagnostics for one draw. The reference overlay path flushes each overlay separately.
struct TimelineRenderOptions {
  PagOverlayStats* overlays{};
  PagTransitionRenderTiming* transition{};
  bool batch_overlays{true};
};

/// @brief Composites sampled timeline frames and bound render plans. Link AVKit::Render.
/// The bgfx backend, resource resolvers, and returned surfaces belong to the owning render sequence.
/// Keep the renderer alive while its output leases are used.
class TimelineRenderer {
public:
  /// @param target Output description used to create the backend and sample the timeline.
  /// @param backend An initialized bgfx compositor with the given target.
  TimelineRenderer(ImageDesc target, std::unique_ptr<RenderBackend> backend, TimelineRenderResources resources = {});
  ~TimelineRenderer();

  RenderBackend& Backend();
  PixelFormat Format() const;

  /// @brief Imports a decoded frame and its source-to-texture UV mapping.
  /// Throws PlatformVideoImportError on incompatible platform textures so TimelineSession can retry the same source PTS.
  ImportedVideoFrame ImportFrame(const media::VideoFrame& frame);

  /// @brief Resolves timeline color and data slots in plan order and returns a completed composite.
  ColorSurface Draw(const TimelineFrame& frame, const TimelineRenderOptions& options = {});

  /// @brief Renders a bound plan, including a live scene with caller-supplied textures.
  ColorSurface Render(const RenderRequest& request);

  /// @brief Packs an output lease for the negotiated encoder while preserving its timing.
  /// Throws PlatformVideoTransferError when the encoder cannot import the platform output.
  media::VideoFrame EncoderFrame(const ColorSurface& output, const media::VideoEncoderConfig& config,
      media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration = {});

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::avkit::render
