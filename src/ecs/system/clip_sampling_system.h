#pragma once

#include <entityx/entityx.h>

namespace huxerui::avkit::detail {

class ClipSamplingSystem final : public entityx::System<ClipSamplingSystem> {
public:
  void update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) override;
};

} // namespace huxerui::avkit::detail
