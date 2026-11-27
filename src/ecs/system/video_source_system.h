#pragma once

#include <entityx/entityx.h>

namespace huxerui::avkit::detail {

class VideoSourceSystem final : public entityx::System<VideoSourceSystem> {
public:
  void update(entityx::EntityManager& entities, entityx::EventManager& events, entityx::TimeDelta) override;
};

class VideoPrerollSystem final : public entityx::System<VideoPrerollSystem> {
public:
  void update(entityx::EntityManager& entities, entityx::EventManager& events, entityx::TimeDelta) override;
};

} // namespace huxerui::avkit::detail
