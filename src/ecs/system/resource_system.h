#pragma once

#include <list>
#include <map>

#include <huxerui/avkit/resource_catalog.h>
#include <huxerui/avkit/scene.h>

namespace huxerui::avkit::detail {

/// @brief Decoded image cache owned by one scene root.
class ImageResources {
public:
  /// @param catalog Scene resource snapshot that supplies the encoded bytes.
  /// @param resource Key of the still image or image overlay.
  render::ImagePixels LoadImage(const ResourceCatalog& catalog, const ResourceRef& resource);

  /// @param catalog Scene resource snapshot that supplies the mask image.
  /// @param mask Image reference and the channel to use as coverage.
  render::ImagePixels LoadMask(const ResourceCatalog& catalog, const MaskImage& mask);

  /// @param catalog Scene resource snapshot retaining the sequence and its frame images.
  /// @param sequence Key of the sequence whose frame dimensions must agree.
  /// @param index Frame selected by the coverage or transition system.
  render::ImagePixels LoadMaskFrame(const ResourceCatalog& catalog, const ResourceRef& sequence, std::uint32_t index);

private:
  std::list<std::pair<ResourceRef, render::ImagePixels>> images_;
  std::list<std::pair<std::pair<ResourceRef, MaskChannel>, render::ImagePixels>> masks_;
  std::map<ResourceRef, render::ImageInfo> sequences_;
};

/// @brief Resolves active still images and image overlays before spatial compilation.
class ResourceSystem final : public entityx::System<ResourceSystem> {
public:
  void update(entityx::EntityManager& entities, entityx::EventManager& events, entityx::TimeDelta step) override;
};

} // namespace huxerui::avkit::detail
