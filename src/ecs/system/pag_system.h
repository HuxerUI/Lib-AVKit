#pragma once

#include <entityx/entityx.h>

namespace huxerui::avkit::detail {

/// @brief Resolves visible PAG bindings from the scene catalog before the layer binding stage.
class PagSystem final : public entityx::System<PagSystem> {
public:
  /// @brief Determines editable slot and layout metadata for the current frame.
  /// @param entities Scene entities and their root context.
  void update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) override;
};

} // namespace huxerui::avkit::detail
