#pragma once

#include <map>

#include <entityx/entityx.h>

#include <huxerui/avkit/types.h>

#include "analysis/source_texture.h"

namespace huxerui::avkit::detail {

struct VideoSourceInvalidatedEvent;

class CoverageSystem final : public entityx::System<CoverageSystem>, public entityx::Receiver<CoverageSystem> {
public:
  void configure(entityx::EventManager& events) override;

  void receive(const VideoSourceInvalidatedEvent& event);

  void update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) override;

private:
  struct PortraitMaskSession {
    ClipId id;
    std::string model_id;
    SourceMask source;
  };

  std::map<entityx::Entity, PortraitMaskSession> masks_;
  std::map<entityx::Entity, SourceDepth> depths_;
};

} // namespace huxerui::avkit::detail
