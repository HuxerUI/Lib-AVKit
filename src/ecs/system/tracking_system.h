#pragma once

#include <map>

#include <entityx/entityx.h>

#include <huxerui/avkit/types.h>

#include "analysis/tracking.h"

namespace huxerui::avkit::detail {

struct VideoSourceInvalidatedEvent;

class TrackingSystem final : public entityx::System<TrackingSystem>, public entityx::Receiver<TrackingSystem> {
public:
  void configure(entityx::EventManager& events) override;

  void receive(const VideoSourceInvalidatedEvent& event);

  void update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) override;

private:
  struct Tracker {
    ClipId id{};
    std::vector<analysis::TrackedPart> parts;
    std::unique_ptr<SourceTracking> source;
  };

  std::map<entityx::Entity, Tracker> trackers_;
};

} // namespace huxerui::avkit::detail
