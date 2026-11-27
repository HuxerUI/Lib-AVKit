#pragma once

#include <huxerui/avkit/scene.h>

namespace huxerui::avkit::detail {

class SpatialSystem final : public entityx::System<SpatialSystem> {
public:
  void update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) override;
};

} // namespace huxerui::avkit::detail
