#include "resource_system.h"

#include <algorithm>
#include <stdexcept>

#include "ecs/scene_evaluation.h"
#include "media/image_asset.h"
#include "support/cancellation.h"

namespace huxerui::avkit::detail {
namespace {

template<class Key, class Load>
render::ImagePixels Resolve(std::list<std::pair<Key, render::ImagePixels>>& cache,
    const Key& key, std::size_t capacity, const Load& load) {
  const auto found = std::ranges::find(cache, key, &std::pair<Key, render::ImagePixels>::first);
  if (found != cache.end()) {
    cache.splice(cache.begin(), cache, found);
    return cache.front().second;
  }
  auto pixels = load();
  if (cache.size() >= capacity) cache.pop_back();
  return cache.emplace_front(key, std::move(pixels)).second;
}

} // namespace

render::ImagePixels ImageResources::LoadImage(const ResourceCatalog& catalog, const ResourceRef& resource) {
  return Resolve(images_, resource, 32, [&] { return render::detail::DecodeResourceImage(catalog, resource); });
}

render::ImagePixels ImageResources::LoadMask(const ResourceCatalog& catalog, const MaskImage& mask) {
  return Resolve(masks_, std::pair{mask.resource, mask.channel}, 32, [&] {
    return render::detail::DecodeMaskImage(catalog, mask);
  });
}

render::ImagePixels ImageResources::LoadMaskFrame(const ResourceCatalog& catalog, const ResourceRef& resource,
    std::uint32_t index) {
  const auto& sequence = catalog.Get<MaskSequenceData>(resource);
  auto image = LoadMask(catalog, sequence.frames.at(index));
  const auto [format, inserted] = sequences_.try_emplace(resource, image.info);
  if (!inserted && format->second != image.info) throw std::invalid_argument("Mask sequence frame dimensions differ");
  return image;
}

void ResourceSystem::update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) {
  auto root = SceneRoot(entities);
  const auto& catalog = SceneCatalog(root);
  auto& resources = *root.component<ImageResources>();
  const auto evaluation = root.component<const TimelineEvaluation>();
  const auto stop = evaluation ? evaluation->stop : std::stop_token{};
  const auto load = [&](entityx::Entity entity, const ResourceRef& resource) {
    CheckCancelled(stop);
    entity.assign<SampledStillImage>(resources.LoadImage(catalog, resource));
  };
  const auto has_input = [](entityx::Entity entity) {
    return entity.has_component<HiddenLayer>() || entity.has_component<render::Texture>() ||
           entity.has_component<render::ImagePixels>();
  };
  entities.each<ClipSampleTime, ProjectImageSource>([&](entityx::Entity entity, const ClipSampleTime&,
      const ProjectImageSource& image) {
    if (!has_input(entity)) load(entity, image.resource);
  });
  entities.each<LayerSampleTime, StillImageSource>([&](entityx::Entity entity, const LayerSampleTime&,
      const StillImageSource& image) {
    if (has_input(entity) || entity.has_component<SampledStillImage>()) return;
    if (const auto tracking = entity.component<TrackedSticker>();
        tracking && (!tracking->anchor || !tracking->anchor.has_component<Affine2D>())) return;
    if (entity.has_component<StickerDepth>() && !entity.has_component<SampledStickerDepth>()) return;
    load(entity, image.resource);
  });
}

} // namespace huxerui::avkit::detail
