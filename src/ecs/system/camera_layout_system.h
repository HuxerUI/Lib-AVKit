#pragma once

#include <entityx/entityx.h>

namespace huxerui::avkit::detail {

class CameraLayoutSystem final : public entityx::System<CameraLayoutSystem> {
public:
  void update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) override;
};

} // namespace huxerui::avkit::detail
