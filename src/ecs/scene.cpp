#include <huxerui/avkit/scene.h>
#include <huxerui/avkit/resource_catalog.h>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "support/cancellation.h"
#include "ecs/system/layer_timing_system.h"
#include "ecs/system/pag_system.h"
#include "ecs/system/resource_system.h"
#include "ecs/system/spatial_system.h"

namespace huxerui::avkit {

struct RenderScene::Impl {
  bool Owns(entityx::Entity entity) {
    return entity != entityx::Entity{} && runtime.entities.valid(entity.id()) && runtime.entities.get(entity.id()) == entity;
  }

  entityx::Entity Parent(entityx::Entity entity) {
    const auto link = entity.component<SpatialParent>();
    return link && Owns(link->entity) ? link->entity : entityx::Entity{};
  }

  entityx::EntityX runtime;
  entityx::Entity root = runtime.entities.create();
  std::shared_ptr<const ResourceCatalog> render_resources;
  struct RegisteredSystem { const std::type_info* type; SceneSystemPhase phase; UpdateSystem update; };
  std::vector<RegisteredSystem> systems;
  bool configured{};
  bool evaluating{};
  std::vector<entityx::Entity> ancestry;
};

RenderScene::RenderScene(std::shared_ptr<const ResourceCatalog> resources) : impl_(std::make_unique<Impl>()) {
  impl_->root.assign<VideoFrameContext>();
  impl_->root.assign<ResourceCatalog>(resources ? *resources : ResourceCatalog{});
  impl_->root.assign<detail::ImageResources>();
  impl_->render_resources = std::move(resources);
  RegisterSystemAt<detail::LayerTimingSystem>(SceneSystemPhase::BeforeCompile);
  RegisterSystemAt<detail::PagSystem>(SceneSystemPhase::BeforeCompile);
  RegisterSystemAt<detail::ResourceSystem>(SceneSystemPhase::ResolveResources);
  RegisterSystemAt<detail::SpatialSystem>(SceneSystemPhase::Spatial);
}

RenderScene::~RenderScene() = default;

entityx::EntityManager& RenderScene::Entities() { return impl_->runtime.entities; }

entityx::EventManager& RenderScene::Events() { return impl_->runtime.events; }

entityx::SystemManager& RenderScene::Systems() { return impl_->runtime.systems; }

entityx::Entity RenderScene::Root() const noexcept { return impl_->root; }

const ResourceCatalog& SceneCatalog(entityx::Entity root) {
  if (root) {
    const auto catalog = root.component<const ResourceCatalog>();
    if (catalog) return *catalog;
  }
  throw std::invalid_argument("The scene root needs a resource catalog component");
}

const std::shared_ptr<const ResourceCatalog>& RenderScene::CatalogSnapshot() const noexcept {
  return impl_->render_resources;
}

void RenderScene::ReuseImageResources(const RenderScene& previous) {
  if (CatalogSnapshot() == previous.CatalogSnapshot())
    *Root().component<detail::ImageResources>() = *previous.Root().component<const detail::ImageResources>();
}

const VideoFrameContext& RenderScene::Frame() const noexcept { return *impl_->root.component<const VideoFrameContext>(); }

void RenderScene::CheckSystemRegistration(const std::type_info& type) {
  if (impl_->evaluating) throw std::logic_error("Cannot add a system during scene evaluation");
  if (std::ranges::any_of(impl_->systems, [&](const auto& entry) { return *entry.type == type; }))
    throw std::invalid_argument("This scene system type is already registered");
  impl_->systems.reserve(impl_->systems.size() + 1);
}

void RenderScene::ConfigureAddedSystem(entityx::BaseSystem& system) {
  if (impl_->configured) system.configure(Entities(), Events());
}

void RenderScene::TrackSystem(const std::type_info& type, SceneSystemPhase phase, UpdateSystem update) {
  auto& systems = impl_->systems;
  const auto next = std::ranges::upper_bound(systems, phase, {}, &Impl::RegisteredSystem::phase);
  systems.insert(next, {&type, phase, update});
}

void RenderScene::SetSpatialParent(entityx::Entity child, entityx::Entity parent) {
  auto& state = *impl_;
  if (!state.Owns(child) || (parent != entityx::Entity{} && !state.Owns(parent))) {
    throw std::invalid_argument("Spatial parent and child must belong to the scene");
  }
  if (!parent) {
    if (child.has_component<SpatialParent>()) child.remove<SpatialParent>();
    return;
  }
  auto& ancestry = state.ancestry;
  ancestry.clear();
  for (auto ancestor = parent; ancestor; ancestor = state.Parent(ancestor)) {
    if (ancestor == child || std::ranges::find(ancestry, ancestor) != ancestry.end()) {
      throw std::invalid_argument("The spatial parent would create a cycle");
    }
    ancestry.push_back(ancestor);
  }
  child.replace<SpatialParent>(parent);
}

void RenderScene::Destroy(entityx::Entity entity) {
  if (!impl_->Owns(entity)) throw std::invalid_argument("The scene entity is not live");
  if (entity == impl_->root) throw std::invalid_argument("The scene root belongs to the scene");
  for (auto child : Entities().entities_with_components<SpatialParent>()) {
    if (child.component<SpatialParent>()->entity == entity) child.remove<SpatialParent>();
  }
  entity.destroy();
}

std::shared_ptr<const render::RenderPlan> RenderScene::Evaluate(VideoFrameContext frame, std::stop_token stop) {
  return Evaluate(frame, stop, nullptr);
}

std::shared_ptr<const render::RenderPlan> RenderScene::Evaluate(VideoFrameContext frame, std::stop_token stop,
    std::vector<detail::TargetLayoutEntry>* layouts) {
  UpdateSystems(frame, stop);
  return Compile({}, {}, layouts, stop);
}

void RenderScene::UpdateSystems(VideoFrameContext frame, std::stop_token stop) {
  if (!frame.time.timescale || !frame.target.size.width || !frame.target.size.height) {
    throw std::invalid_argument("Scene evaluation needs a valid media time and target size");
  }
  if (impl_->evaluating) throw std::logic_error("Scene evaluation cannot be reentered");
  entityx::TimeDelta step{};
  if (frame.continuous_step) {
    const auto duration = *frame.continuous_step;
    if (!duration.timescale || duration.ticks <= 0)
      throw std::invalid_argument("A continuous scene update needs a positive media duration");
    step = static_cast<entityx::TimeDelta>(duration.ticks) / duration.timescale;
  }
  detail::CheckCancelled(stop);
  for (auto entity : Entities().entities_with_components<SampledStillImage>())
    entity.remove<SampledStillImage>();
  *impl_->root.component<VideoFrameContext>() = frame;
  RunSystems(SceneSystemPhase::BeforeSampling, SceneSystemPhase::ResolveResources, stop, step);
}

void RenderScene::RunPhase(SceneSystemPhase phase, std::stop_token stop) {
  RunSystems(phase, phase, stop);
}

void RenderScene::RunSystems(SceneSystemPhase first, SceneSystemPhase last, std::stop_token stop,
                             entityx::TimeDelta step) {
  auto& state = *impl_;
  if (state.evaluating) throw std::logic_error("Scene evaluation cannot be reentered");
  struct Evaluation {
    bool& active;

    ~Evaluation() { active = false; }
  } evaluation{state.evaluating};
  state.evaluating = true;
  detail::CheckCancelled(stop);
  if (!state.configured) {
    Systems().configure();
    state.configured = true;
  }
  for (const auto& system : state.systems) {
    if (system.phase < first || system.phase > last) continue;
    detail::CheckCancelled(stop);
    system.update(Systems(), step);
  }
  detail::CheckCancelled(stop);
}

std::shared_ptr<const render::RenderPlan> RenderScene::Compile(std::stop_token stop) {
  return Compile({}, {}, nullptr, stop);
}

std::shared_ptr<const render::RenderPlan> RenderScene::Compile(entityx::Entity target, Affine2D delta,
    std::vector<detail::TargetLayoutEntry>* layouts, std::stop_token stop) {
  if (!Frame().target.size.width || !Frame().target.size.height)
    throw std::invalid_argument("Scene compilation requires a target size");
  if (impl_->evaluating) throw std::logic_error("Scene evaluation cannot be reentered");
  auto root = Root();
  auto request = root.assign<detail::RenderPlanCompilation>(target, delta, layouts);
  struct Release {
    entityx::Entity root;

    ~Release() { root.remove<detail::RenderPlanCompilation>(); }
  } release{root};
  RunPhase(SceneSystemPhase::Spatial, stop);
  request->plan->resources = CatalogSnapshot();
  return std::move(request->plan);
}

} // namespace huxerui::avkit
