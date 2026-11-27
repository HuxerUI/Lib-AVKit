#pragma once

#include <entityx/entityx.h>

namespace huxerui::avkit::detail {

class ClipPresentationRangeSystem final : public entityx::System<ClipPresentationRangeSystem> {
public:
  void update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) override;
};

class TimelineSystem final : public entityx::System<TimelineSystem> {
public:
  void update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) override;
};

} // namespace huxerui::avkit::detail
