#pragma once

/// @file
/// @brief EntityX scene evaluation and compilation into immutable render plans.
///
/// Link AVKit::Core. Components and systems belong to the scene's owning sequence.

#include <concepts>
#include <memory>
#include <optional>
#include <stop_token>
#include <typeinfo>
#include <utility>
#include <vector>

#include <entityx/entityx.h>

#include <huxerui/avkit/component.h>
#include <huxerui/media_format.h>

namespace huxerui::avkit {

class TimelineSession;

/// @brief Reads the resource catalog installed on a scene root.
/// @param root Live root entity returned by RenderScene::Root.
/// @return Catalog borrowed for lookup on the scene's owning sequence.
/// @throws std::invalid_argument If the entity does not hold a scene catalog.
const ResourceCatalog& SceneCatalog(entityx::Entity root);

/// @brief Ordered stages within one evaluation. Systems in a stage run in registration order.
enum class SceneSystemPhase {
  BeforeSampling, ResolveTiming, SampleSources, AfterSampling, ResolveLayers, BeforeCompile,
  ResolveResources, Spatial, Preroll, BeforeAudio, Audio,
};

/// @brief Owns one EntityX scene and evaluates it into render plans.
class RenderScene {
public:
  /// @brief Creates an ECS world with a read-only ResourceCatalog component on the root entity.
  /// @param resources Snapshot used to initialize the root catalog and retain the same resources in render plans.
  /// Pass an empty pointer for a scene that uses only supplied frame data.
  explicit RenderScene(std::shared_ptr<const ResourceCatalog> resources = {});

  ~RenderScene();

  /// @brief Returns the native EntityX entity manager on the scene's owning sequence.
  entityx::EntityManager& Entities();

  /// @brief Returns the native EntityX event manager on the scene's owning sequence.
  entityx::EventManager& Events();

  /// @brief The single scene-owned entity for evaluation-wide components.
  /// Keep the root and its VideoFrameContext alive for the lifetime of the scene.
  entityx::Entity Root() const noexcept;

  /// @brief Returns the exact evaluation context stored on the root entity.
  const VideoFrameContext& Frame() const noexcept;

  /// @brief Adds one EntityX system after source sampling and before layer resolution.
  /// Query VideoFrameContext for exact time. The optional continuous update supplies a step in seconds.
  /// @tparam S EntityX system type to register.
  /// @tparam Args Types of the forwarded constructor arguments.
  /// @param args Arguments used to construct the system.
  template<class S, class... Args> requires std::derived_from<S, entityx::System<S>>
  std::shared_ptr<S> RegisterSystem(Args&&... args) {
    return RegisterSystemAt<S>(SceneSystemPhase::AfterSampling, std::forward<Args>(args)...);
  }

  /// @brief Adds one EntityX system at the specified stage of frame evaluation.
  /// @tparam S EntityX system type to register.
  /// @tparam Args Types of the forwarded constructor arguments.
  /// @param phase Stage where the system runs, in registration order within that stage.
  /// @param args Arguments used to construct the system.
  /// @throws std::invalid_argument If this system type is already registered in the scene.
  /// @throws std::logic_error If called during scene evaluation.
  template<class S, class... Args> requires std::derived_from<S, entityx::System<S>>
  std::shared_ptr<S> RegisterSystemAt(SceneSystemPhase phase, Args&&... args) {
    CheckSystemRegistration(typeid(S));
    auto system = std::make_shared<S>(std::forward<Args>(args)...);
    ConfigureAddedSystem(*system);
    Systems().add(system);
    TrackSystem(typeid(S), phase, [](entityx::SystemManager& manager, entityx::TimeDelta step) { manager.update<S>(step); });
    return system;
  }

  /// @brief Reparents two live entities in this scene, or detaches the child with an empty handle.
  /// @param child Scene entity whose spatial parent changes.
  /// @param parent New scene parent, or empty to detach.
  /// @throws std::invalid_argument If either entity is invalid or the link creates a cycle.
  void SetSpatialParent(entityx::Entity child, entityx::Entity parent = {});

  /// @brief Destroys a scene entity and detaches its direct spatial children.
  /// @param entity Live scene entity other than the root.
  /// @throws std::invalid_argument If the entity is invalid, belongs to another scene, or is the root.
  void Destroy(entityx::Entity entity);

  /// @brief Runs systems and compiles the drawable components they prepare for this frame.
  /// @param frame Exact evaluation time and output target installed on the root.
  /// @param stop Cooperative cancellation checked before and between evaluation systems.
  /// @return Immutable render plan for the supplied frame.
  std::shared_ptr<const render::RenderPlan> Evaluate(VideoFrameContext frame, std::stop_token stop = {});

  /// @brief Runs one stage against the data already published on this scene's root.
  /// @param phase Stage to execute without running any other stages.
  /// @param stop Cooperative cancellation checked between registered systems.
  void RunPhase(SceneSystemPhase phase, std::stop_token stop = {});

  /// @brief Runs spatial evaluation on the last evaluated frame without resampling media.
  /// @param stop Cooperative cancellation checked through spatial evaluation.
  /// @return Immutable plan with resolved placement and draw order.
  std::shared_ptr<const render::RenderPlan> Compile(std::stop_token stop = {});

private:
  friend class TimelineSession;

  std::shared_ptr<const render::RenderPlan> Evaluate(VideoFrameContext frame, std::stop_token stop,
      std::vector<detail::TargetLayoutEntry>* layouts);

  std::shared_ptr<const render::RenderPlan> Compile(entityx::Entity target, Affine2D delta,
      std::vector<detail::TargetLayoutEntry>* layouts, std::stop_token stop = {});

  const std::shared_ptr<const ResourceCatalog>& CatalogSnapshot() const noexcept;

  void ReuseImageResources(const RenderScene& previous);

  using UpdateSystem = void (*)(entityx::SystemManager&, entityx::TimeDelta);
  entityx::SystemManager& Systems();

  void CheckSystemRegistration(const std::type_info& type);

  void ConfigureAddedSystem(entityx::BaseSystem& system);

  void TrackSystem(const std::type_info& type, SceneSystemPhase phase, UpdateSystem update);

  void UpdateSystems(VideoFrameContext frame, std::stop_token stop = {});

  void RunSystems(SceneSystemPhase first, SceneSystemPhase last, std::stop_token stop, entityx::TimeDelta step = 0);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::avkit
