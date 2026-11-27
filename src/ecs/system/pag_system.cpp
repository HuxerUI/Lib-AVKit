#include "pag_system.h"

#include "ecs/scene_evaluation.h"
#include "project/resource_internal.h"

#include <algorithm>
#include <stdexcept>

namespace huxerui::avkit::detail {

void PagSystem::update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) {
  const auto root = SceneRoot(entities);
  ClearComponents<PagLayerSample>(entities);
  entities.each<PagOverlay, LayerSampleTime>([&](entityx::Entity layer, const PagOverlay& overlay,
                                                const LayerSampleTime& time) {
    if (layer.has_component<render::Texture>() || layer.has_component<render::ImagePixels>() ||
        layer.has_component<HiddenLayer>()) return;
    if (const auto tracking = layer.component<TrackedSticker>();
        tracking && (!tracking->anchor || !tracking->anchor.has_component<Affine2D>())) return;
    if (layer.has_component<StickerDepth>() && !layer.has_component<SampledStickerDepth>()) return;
    const auto& asset = SceneCatalog(root).Get<PagAsset>(overlay.resource);
    auto slots = ResolvePagIndices(asset, overlay.replacements);
    std::optional<Size> extent;
    if (asset.fit_text_to_canvas) {
      const auto transform = layer.component<Transform2D>();
      if (!transform) throw std::invalid_argument("Point text needs a layer transform");
      extent = transform->scale;
    }
    const auto id = layer.component<InstanceId>();
    layer.assign<PagLayerSample>(id ? std::optional{*id} : std::nullopt, time.elapsed, overlay, extent, std::move(slots));
  });
}

} // namespace huxerui::avkit::detail
