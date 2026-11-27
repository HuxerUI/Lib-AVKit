#pragma once

#include <entityx/entityx.h>

namespace huxerui::avkit::detail {

class LayerTimingSystem final : public entityx::System<LayerTimingSystem> {
public:
  void update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) override;
};

} // namespace huxerui::avkit::detail
