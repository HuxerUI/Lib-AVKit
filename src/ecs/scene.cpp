#include <huxerui/avkit_ecs.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace huxerui::avkit {
namespace {

static_assert(FLECS_VERSION_MAJOR == 4 && FLECS_VERSION_MINOR == 2 && FLECS_VERSION_PATCH == 0,
              "AVKit Core expects Flecs v4.2.0");

bool ValidRectangle(const Rect& rect) {
  return std::isfinite(rect.x) && std::isfinite(rect.y) &&
         std::isfinite(rect.width) && std::isfinite(rect.height) && !rect.IsEmpty();
}

struct OrderedDraw {
  std::int32_t order;
  std::optional<std::uint32_t> subcomposition;
  render::Draw draw;
};

struct ResolvedLayer {
  Rect destination{0, 0, 1, 1};
  std::optional<std::uint32_t> subcomposition;
  std::optional<Transform2D> transform;
};

Transform2D Compose(const Transform2D& outer, const Transform2D& inner) {
  const auto origin = outer.Apply({inner.translate_x, inner.translate_y});
  return {outer.m11 * inner.m11 + outer.m21 * inner.m12, outer.m12 * inner.m11 + outer.m22 * inner.m12,
          outer.m11 * inner.m21 + outer.m21 * inner.m22, outer.m12 * inner.m21 + outer.m22 * inner.m22,
          origin.x, origin.y};
}

} // namespace

struct SceneEvaluator::Impl {
  Impl() {
    world.entity<EffectEvaluation>().add(flecs::Phase).depends_on(flecs::OnUpdate);
    world.component<SpatialParent>().add(flecs::Exclusive).add(flecs::OnDeleteTarget, flecs::Remove);
    world.component<LocalTransform2D>();
    world.component<Transform2D>();
    world.component<SubcompositionRoot>();
    world.component<render::MaskedBlend>();
    world.component<FrameContext>().add(flecs::Singleton);
    world.set<FrameContext>({});
    layers = world.query<const CompositeLayer>();
  }

  std::optional<ResolvedLayer> Resolve(flecs::entity entity) {
    ResolvedLayer result;
    Transform2D transform;
    ancestry.clear();
    for (; entity; entity = entity.target<SpatialParent>()) {
      if (std::ranges::find(ancestry, entity.id()) != ancestry.end()) return std::nullopt;
      ancestry.push_back(entity.id());
      if (const auto* root = entity.try_get<SubcompositionRoot>()) {
        result.subcomposition = root->index;
        break;
      }
      if (const auto* local = entity.try_get<LocalTransform2D>()) {
        if (!(local->scale.width > 0 && local->scale.height > 0)) return std::nullopt;
        transform = Compose({local->scale.width, 0, 0, local->scale.height, local->translation.x, local->translation.y},
                            transform);
      }
      if (const auto* local = entity.try_get<Transform2D>()) transform = Compose(*local, transform);
    }
    if (!transform.m12 && !transform.m21 && transform.m11 > 0 && transform.m22 > 0) {
      result.destination = {transform.translate_x, transform.translate_y, transform.m11, transform.m22};
      if (!ValidRectangle(result.destination)) return std::nullopt;
    } else {
      const auto determinant = double(transform.m11) * transform.m22 - double(transform.m21) * transform.m12;
      if (!std::isfinite(determinant) || determinant == 0 || !std::isfinite(transform.translate_x) ||
          !std::isfinite(transform.translate_y)) return std::nullopt;
      result.transform = transform;
    }
    return result;
  }

  flecs::world world;
  flecs::query<const CompositeLayer> layers;
  std::vector<flecs::entity_t> ancestry;
  std::vector<OrderedDraw> ordered;
};

SceneEvaluator::SceneEvaluator() : impl_(std::make_unique<Impl>()) {}
SceneEvaluator::~SceneEvaluator() = default;

flecs::world& SceneEvaluator::World() { return impl_->world; }

void SceneEvaluator::SetSpatialParent(flecs::entity child, flecs::entity parent) {
  const auto* world = impl_->world.c_ptr();
  if (!child || !parent || child.world().c_ptr() != world || parent.world().c_ptr() != world ||
      !child.is_alive() || !parent.is_alive()) {
    throw std::invalid_argument("Spatial parent and child must be live entities in this world");
  }
  auto& ancestry = impl_->ancestry;
  ancestry.clear();
  for (auto ancestor = parent; ancestor; ancestor = ancestor.target<SpatialParent>()) {
    if (ancestor == child || std::ranges::find(ancestry, ancestor.id()) != ancestry.end()) {
      throw std::invalid_argument("The spatial parent would create a cycle");
    }
    ancestry.push_back(ancestor.id());
  }
  child.add<SpatialParent>(parent);
}

std::shared_ptr<const render::RenderPlan> SceneEvaluator::Evaluate(media::MediaTimestamp time, render::ImageDesc target,
                                                                   std::uint32_t subcomposition_count) {
  if (!time.timescale || !target.size.width || !target.size.height) {
    throw std::invalid_argument("Scene evaluation needs a valid media time and target size");
  }
  auto& state = *impl_;
  state.world.set<FrameContext>({time});
  state.world.run_pipeline(state.world.get_pipeline(), 0.0);

  state.ordered.clear();
  bool invalid_transform = false;
  state.layers.each([&](flecs::entity entity, const CompositeLayer& layer) {
    if (const auto resolved = state.Resolve(entity)) {
      const auto* mask = entity.try_get<render::MaskedBlend>();
      state.ordered.push_back({layer.order, resolved->subcomposition,
                               {layer.input_slot, layer.blend, resolved->destination,
                                mask ? std::optional{*mask} : std::nullopt, resolved->transform}});
    } else {
      invalid_transform = true;
    }
  });
  if (invalid_transform) throw std::invalid_argument("Visible layer has a cyclic or invalid spatial transform");
  const auto order = [](const OrderedDraw& draw) { return std::pair{draw.subcomposition, draw.order}; };
  std::ranges::sort(state.ordered, {}, order);
  auto plan = std::make_shared<render::RenderPlan>();
  plan->target = target;
  plan->subcompositions.resize(subcomposition_count);
  plan->draws.reserve(state.ordered.size());
  for (std::size_t index = 0; index < state.ordered.size(); ++index) {
    const auto& layer = state.ordered[index];
    if (index && order(state.ordered[index - 1]) == order(layer)) {
      throw std::invalid_argument("Visible composite layers must have distinct orders");
    }
    if (layer.subcomposition && *layer.subcomposition >= subcomposition_count)
      throw std::invalid_argument("Visible layer references an unavailable subcomposition");
    auto& draws = layer.subcomposition ? plan->subcompositions[*layer.subcomposition].draws : plan->draws;
    draws.push_back(layer.draw);
  }
  return plan;
}

} // namespace huxerui::avkit
