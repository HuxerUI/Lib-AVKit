#pragma once

/// @file
/// @brief Flecs scene evaluation and compilation into immutable render plans.
///
/// Link AVKit::Core and register application components and systems on the evaluator's world.
/// Flecs systems run on the scene's owning sequence at the requested media time.

#include <cstdint>
#include <memory>

#include <flecs.h>

#include <huxerui/avkit/render.h>
#include <huxerui/media_format.h>

namespace huxerui::avkit {

/// @brief Flecs phase for effects evaluated during each sampled frame.
/// Register its systems with .kind<EffectEvaluation>().
struct EffectEvaluation {};

/// @brief Singleton exposing the exact requested media time to scene systems.
struct FrameContext {
  media::MediaTimestamp time;
};

/// @brief A spatial relationship with independent entity lifetimes.
struct SpatialParent {};

/// @brief Local translation and scale in normalized top-left coordinates.
/// An entity's optional HuxerUI Transform2D is applied after this component.
struct LocalTransform2D {
  Point translation;
  Size scale{1, 1};
};

/// @brief Starts a clip-local pass for this layer's spatial descendants.
struct SubcompositionRoot {
  std::uint32_t index;
};

/// @brief One input to the composite, with a distinct order within its destination pass.
struct CompositeLayer {
  std::uint32_t input_slot;
  std::int32_t order;
  render::Blend blend{render::Blend::PremultipliedOver};
};

/// @brief Owns the Flecs world for one scene evaluation sequence.
class SceneEvaluator {
public:
  SceneEvaluator();
  ~SceneEvaluator();

  /// @brief Returns the world for registration and scene edits on the owning sequence.
  flecs::world& World();

  /// @brief Replaces a spatial parent after validating world identity, lifetimes, and ancestry.
  /// @throws std::invalid_argument If either entity is invalid or the link creates a cycle.
  void SetSpatialParent(flecs::entity child, flecs::entity parent);

  /// @brief Sets FrameContext, runs the world pipeline, and compiles its visible layers.
  /// @param time Exact media time, including arbitrary backward seeks.
  /// @param target Physical size and format of the output surfaces.
  /// @param subcomposition_count Number of offscreen passes addressed by SubcompositionRoot.
  /// @return Immutable render plan independent of subsequent ECS changes.
  std::shared_ptr<const render::RenderPlan> Evaluate(media::MediaTimestamp time, render::ImageDesc target,
                                                     std::uint32_t subcomposition_count = 0);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::avkit
