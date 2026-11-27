#pragma once

/// @file
/// @brief libpag adapter for two-input transitions.

#include <memory>

#include <huxerui/avkit/render.h>

namespace huxerui::avkit {

class ResourceCatalog;

/// @brief Renders an authored PAG composition with two current GPU pictures. Link AVKit::PAG.
/// Create, sample, and destroy the instance on its owning sequence.
/// The GPU transition adapter is available on macOS.
class PagTransition {
public:
  /// @brief Opens a composition with two distinct editable image indices and a fixed output size.
  /// @param source PAG asset whose descriptor identifies the outgoing and incoming image slots.
  /// @param resources Catalog containing the transition asset and its named slots.
  /// @param size Fixed input and output extent in physical pixels.
  /// @throws std::runtime_error When the target platform has no GPU transition adapter.
  PagTransition(const ResourceRef& source, const ResourceCatalog& resources, PixelSize size);

  ~PagTransition();

  PagTransition(const PagTransition&) = delete;

  PagTransition& operator=(const PagTransition&) = delete;

  /// @brief Samples the half-open overlap with two completed, premultiplied RGBA8 or BGRA8 textures.
  /// Inputs use the declared size and top-left origin on one GPU device. The returned premultiplied
  /// GPU surface remains immutable while leased. Both images are rebound on every sample.
  /// @param offset Elapsed time from the start of the overlap, in [0, duration).
  /// @param duration Total length of this overlap. Used to map elapsed time to PAG progress.
  /// @param outgoing Current picture for the outgoing editable slot.
  /// @param incoming Current picture for the incoming editable slot.
  render::Texture Sample(media::MediaTimestamp offset, media::MediaTimestamp duration,
                              const render::Texture& outgoing, const render::Texture& incoming);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::avkit
