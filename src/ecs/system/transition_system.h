#pragma once

#include <entityx/entityx.h>

namespace huxerui::avkit::detail {

class TransitionSystem final : public entityx::System<TransitionSystem> {
public:
  void update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) override;
};

} // namespace huxerui::avkit::detail
