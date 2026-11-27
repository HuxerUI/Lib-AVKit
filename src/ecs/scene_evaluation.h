#pragma once

#include <stdexcept>

#include <huxerui/avkit/resource_catalog.h>
#include <huxerui/avkit/scene.h>
#include <huxerui/avkit/timeline.h>

#include "render/texture_backing.h"

namespace huxerui::avkit::detail {

/// @brief Verifies that a clip selects the media track inspected during import.
/// @param source Authored clip binding with its source track identifier.
/// @param movie Imported track and format metadata.
/// @param kind Required source track kind.
void ValidateMediaTrack(const ClipSource& source, const MovieAsset& movie, media::TrackKind kind);

/// @brief Reads the optional provider component independently of resource lookup.
/// @param root Scene root whose analysis provider is configured for this scene.
inline std::shared_ptr<analysis::AnalysisProvider> SceneAnalysisProvider(entityx::Entity root) {
  const auto provider = root.component<const std::shared_ptr<analysis::AnalysisProvider>>();
  return provider ? *provider : nullptr;
}

inline entityx::Entity SceneRoot(entityx::EntityManager& entities) {
  const auto roots = entities.entities_with_components<VideoFrameContext>();
  const auto root = roots.begin();
  if (root == roots.end()) throw std::logic_error("The scene is missing its frame context");
  return *root;
}

template<class... Components>
void ClearComponents(entityx::EntityManager& entities) {
  const auto clear = [&]<class C> {
    for (entityx::Entity entity : entities.entities_with_components<C>()) entity.remove<C>();
  };
  (clear.template operator()<Components>(), ...);
}

inline const void* DrawableStorage(const render::Texture& texture) {
  return TextureAccess::Backing(texture).get();
}

inline const void* DrawableStorage(const render::ImagePixels& image) {
  return image.bytes.AsBytes().data();
}

inline void ClearSampledVideo(entityx::EntityManager& entities) {
  for (auto entity : entities.entities_with_components<SampledVideo>()) {
    if (const auto owned = entity.component<const SampledVideo>()->owned_drawable) {
      if (const auto texture = entity.component<const render::Texture>(); texture && DrawableStorage(*texture) == owned)
        entity.remove<render::Texture>();
      if (const auto pixels = entity.component<const render::ImagePixels>(); pixels && DrawableStorage(*pixels) == owned)
        entity.remove<render::ImagePixels>();
    }
    entity.remove<SampledVideo>();
  }
}

} // namespace huxerui::avkit::detail
