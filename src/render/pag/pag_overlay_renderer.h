#pragma once

#include "render/render_passes.h"
#include "render/target_layout.h"

namespace huxerui::avkit::detail {

/// @brief Resolves and batches PAG draws while preparing a plan for bgfx submission.
/// Keep the backend alive on the same execution sequence until this renderer is destroyed.
class PagOverlayRenderer {
public:
  /// @brief Creates a retained overlay cache on the backend's owning sequence.
  /// @param backend Device used to render or import the overlay output.
  /// @param resources Catalog retained by the caller through destruction.
  PagOverlayRenderer(render::RenderBackend& backend, const ResourceCatalog* resources);

  ~PagOverlayRenderer();

  /// @brief Resolves and caches the textures referenced by the compiled passes.
  /// @param passes Execution order lowered from the render tree.
  /// @param images Borrowed drawable image or PAG nodes in pass input order.
  /// @param inputs Prepared color textures. Slots for PAG are empty until sampling.
  /// @param data Already resolved numeric textures in pass input order.
  /// @param layouts Optional target layouts to update with measured PAG artwork.
  /// @return Render request holding the images needed through bgfx submission.
  render::detail::PassRequest Prepare(render::detail::RenderPasses passes,
      std::span<const render::RenderNode* const> images,
      std::vector<render::Texture> inputs, std::vector<render::Texture> data,
      std::span<TargetLayoutEntry> layouts);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::avkit::detail
