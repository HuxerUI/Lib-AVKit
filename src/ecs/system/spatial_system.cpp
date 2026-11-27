#include "spatial_system.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

#include "ecs/scene_evaluation.h"
#include "render/target_layout.h"
#include "support/geometry.h"

namespace huxerui::avkit::detail {
namespace {

struct OrderedNode {
  std::int32_t order;
  entityx::Entity group;
  std::unique_ptr<const render::RenderNode> node;
  std::vector<TargetLayoutEntry> layouts;
};

struct TransitionSide {
  render::CompositeNode composition;
  std::vector<TargetLayoutEntry> layouts;
};

struct ResolvedLayer {
  Affine2D local_to_target;
  entityx::Entity group;
};

constexpr Rect kUnitRect{0, 0, 1, 1};

struct SpatialResolver {
  entityx::EntityManager& entities;
  std::vector<entityx::Entity> ancestry;

  bool Owns(entityx::Entity entity) {
    return entity != entityx::Entity{} && entities.valid(entity.id()) && entities.get(entity.id()) == entity;
  }

  entityx::Entity Parent(entityx::Entity entity) {
    const auto link = entity.component<SpatialParent>();
    return link && Owns(link->entity) ? link->entity : entityx::Entity{};
  }

  std::optional<ResolvedLayer> Resolve(entityx::Entity entity, PixelSize canvas,
      const RenderPlanCompilation& request) {
    ResolvedLayer result;
    auto& transform = result.local_to_target;
    bool apply_delta = false;
    ancestry.clear();
    for (; entity; entity = Parent(entity)) {
      if (std::ranges::find(ancestry, entity) != ancestry.end()) return std::nullopt;
      ancestry.push_back(entity);
      apply_delta |= entity == request.target;
      if (entity.has_component<TransitionGroupRoot>()) {
        result.group = entity;
        break;
      }
      if (const auto matrix = entity.component<Affine2D>()) {
        transform = Compose(*matrix, transform);
      } else if (const auto fitted = entity.component<FittedVideoTransform>()) {
        if (!(fitted->value.scale.width > 0 && fitted->value.scale.height > 0)) return std::nullopt;
        transform = Compose(LocalMatrix(fitted->value, canvas), transform);
      } else if (const auto local = entity.component<Transform2D>()) {
        if (!(local->scale.width > 0 && local->scale.height > 0)) return std::nullopt;
        transform = Compose(LocalMatrix(*local, canvas), transform);
      }
    }
    if (apply_delta) transform = Compose(request.delta, transform);
    const auto determinant = double(transform.m11) * transform.m22 - double(transform.m21) * transform.m12;
    if (!IsFinite(transform) || !std::isfinite(determinant) || determinant == 0) return std::nullopt;
    return result;
  }
};

std::unique_ptr<const render::RenderNode> Drawable(entityx::Entity entity, const LayerStyle& style,
    SpatialResolver& resolver, PixelSize canvas, const RenderPlanCompilation& request, ResolvedLayer& resolved) {
  const auto placement = [&] {
    const auto layout = resolver.Resolve(entity, canvas, request);
    if (!layout) throw std::invalid_argument("Visible instance has a cyclic or invalid spatial transform");
    resolved = *layout;
    return render::NodePlacement{style.blend, resolved.local_to_target, style.opacity};
  };
  const auto make_drawable = [&](const auto& input) -> std::unique_ptr<const render::RenderNode> {
    const auto sampling = entity.component<const render::SourceSampling>();
    const auto uv = sampling ? std::optional{*sampling} : std::nullopt;
    const auto coverage = entity.component<const render::BackgroundMask>();
    const auto layout = placement();
    std::unique_ptr<const render::RenderNode> node;
    if constexpr (std::is_same_v<std::decay_t<decltype(input)>, render::Texture>)
      node = std::make_unique<render::TextureNode>(input, layout, uv);
    else
      node = std::make_unique<render::ImageNode>(input, layout, uv);
    if (coverage) return std::make_unique<render::BackgroundReplacementNode>(std::move(node), *coverage);
    return node;
  };
  if (entity.has_component<LayerSampleTime>() || entity.has_component<ClipSampleTime>()) {
    const auto texture = entity.component<const render::Texture>();
    const auto image = entity.component<const render::ImagePixels>();
    const auto video = entity.component<const SampledVideo>();
    if (texture && (!image || !video || DrawableStorage(*texture) != video->owned_drawable))
      return make_drawable(*texture);
    if (image) return make_drawable(*image);
  }
  if (const auto still = entity.component<const SampledStillImage>()) return make_drawable(still->pixels);
  if (const auto pag = entity.component<PagLayerSample>())
    return std::make_unique<render::PagNode>(std::make_shared<const PagLayerSample>(*pag), placement());
  return {};
}

std::optional<EditTarget> FindEditTarget(entityx::Entity entity) {
  if (const auto id = entity.component<ClipId>()) return EditTarget::ForClip(*id);
  if (const auto id = entity.component<InstanceId>(); id && !entity.has_component<ClipAttachment>() &&
      (entity.has_component<PagOverlay>() || entity.has_component<StillImageSource>())) return EditTarget::ForOverlay(*id);
  return std::nullopt;
}

} // namespace

void SpatialSystem::update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) {
  auto root = SceneRoot(entities);
  auto request = root.component<RenderPlanCompilation>();
  if (!request) throw std::logic_error("Missing scene layout request");
  const auto target = root.component<const VideoFrameContext>()->target;
  auto plan = std::make_shared<render::RenderPlan>();
  plan->target = target;
  SpatialResolver state{entities};
  std::vector<OrderedNode> ordered;
  for (auto entity : entities.entities_with_components<LayerStyle>()) {
    if (entity.has_component<HiddenLayer>()) continue;
    const auto tracking = entity.component<TrackedSticker>();
    if (tracking && (!state.Owns(tracking->anchor) || !tracking->anchor.has_component<Affine2D>())) continue;
    const auto depth = entity.component<SampledStickerDepth>();
    if (!depth && entity.has_component<StickerDepth>()) continue;
    const auto& style = *entity.component<LayerStyle>();
    ResolvedLayer resolved;
    auto node = Drawable(entity, style, state, target.size, *request, resolved);
    if (!node) continue;
    const auto& matrix = resolved.local_to_target;
    if (depth) {
      if (!state.Owns(depth->source)) continue;
      const auto source = state.Resolve(depth->source, target.size, *request);
      if (!source || source->group != resolved.group) continue;
      const auto& clip_to_target = source->local_to_target;
      node = std::make_unique<render::DepthOcclusionNode>(std::move(node), depth->depth,
          Compose(depth->clip_to_depth_uv, InverseAffine(clip_to_target)), depth->valid_uv,
          TransformBounds(kUnitRect, clip_to_target), depth->layer_depth_meters);
    }
    std::vector<TargetLayoutEntry> layouts;
    if (request->layouts) {
      if (const auto target = FindEditTarget(entity)) layouts.push_back({*target, {matrix}});
    }
    ordered.push_back({style.order, resolved.group, std::move(node), std::move(layouts)});
  }

  const auto order = [](const OrderedNode& node) { return std::pair{node.group.id().id(), node.order}; };
  std::ranges::sort(ordered, {}, order);
  std::map<std::uint64_t, TransitionSide> groups;
  std::vector<OrderedNode> roots;
  roots.reserve(ordered.size());
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    auto& entry = ordered[index];
    if (index && order(ordered[index - 1]) == order(entry))
      throw std::invalid_argument("Visible composite children must have distinct orders");
    if (entry.group) {
      auto& side = groups[entry.group.id().id()];
      side.composition.children.push_back(std::move(entry.node));
      for (auto& item : entry.layouts) side.layouts.push_back(std::move(item));
    } else {
      roots.push_back(std::move(entry));
    }
  }

  for (auto entity : entities.entities_with_components<ActiveTransition, TransitionLink, LayerStyle>()) {
    const auto mask = entity.component<render::MaskTransitionSample>();
    const auto pag = entity.component<render::PagTransitionSample>();
    if (!mask && !pag) continue;
    const auto& link = *entity.component<TransitionLink>();
    if (!state.Owns(link.from_root) || !state.Owns(link.to_root) || link.from_root == link.to_root)
      throw std::invalid_argument("A transition needs two distinct live scene roots");
    const auto resolved = state.Resolve(entity, target.size, *request);
    if (!resolved || resolved->group)
      throw std::invalid_argument("A transition requires valid top-level placement");
    const auto& style = *entity.component<LayerStyle>();
    const render::NodePlacement placement{style.blend, resolved->local_to_target, style.opacity};
    std::array<render::CompositeNode, 2> sides;
    std::vector<TargetLayoutEntry> layouts;
    const std::array side_roots{link.from_root, link.to_root};
    for (std::size_t index = 0; index < sides.size(); ++index) {
      auto group = groups.extract(side_roots[index].id().id());
      if (group.empty()) continue;
      sides[index] = std::move(group.mapped().composition);
      for (auto& item : group.mapped().layouts) {
        item.layout.transform = Compose(placement.local_to_target, item.layout.transform);
        layouts.push_back(std::move(item));
      }
    }
    if (entity.has_component<HiddenLayer>()) continue;
    std::unique_ptr<const render::RenderNode> node;
    if (mask) node = std::make_unique<render::MaskTransitionNode>(std::move(sides), *mask, placement);
    else node = std::make_unique<render::PagTransitionNode>(std::move(sides), *pag, placement);
    roots.push_back({style.order, {}, std::move(node), std::move(layouts)});
  }
  if (!groups.empty()) throw std::invalid_argument("Visible instance references an unevaluated transition side");
  std::ranges::sort(roots, {}, &OrderedNode::order);
  plan->root.children.reserve(roots.size());
  if (request->layouts) request->layouts->clear();
  for (std::size_t index = 0; index < roots.size(); ++index) {
    auto& entry = roots[index];
    if (index && roots[index - 1].order == entry.order)
      throw std::invalid_argument("Visible composite children must have distinct orders");
    plan->root.children.push_back(std::move(entry.node));
    if (request->layouts) {
      for (auto& item : entry.layouts) request->layouts->push_back(std::move(item));
    }
  }
  request->plan = std::move(plan);
}

} // namespace huxerui::avkit::detail
