#include "render/render_passes.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

#include "render/texture_backing.h"

namespace huxerui::avkit::render::detail {
namespace {

std::uint32_t NextIndex(std::size_t size) {
  if (size >= std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument("Too many render inputs or passes");
  return static_cast<std::uint32_t>(size);
}

bool SameTexture(const Texture& a, const Texture& b) {
  return avkit::detail::TextureAccess::Backing(a) == avkit::detail::TextureAccess::Backing(b) &&
         a.Info() == b.Info() && a.Origin() == b.Origin() && a.Alpha() == b.Alpha();
}

bool SameInput(const PixelInput& a, const PixelInput& b) {
  if (a.index() != b.index()) return false;
  if (const auto* pixels = std::get_if<ImagePixels>(&a)) return *pixels == std::get<ImagePixels>(b);
  return SameTexture(std::get<Texture>(a), std::get<Texture>(b));
}

bool Identity(const NodePlacement& placement) {
  return placement.local_to_target.IsIdentity() && placement.opacity == 1 &&
         placement.blend == Blend::PremultipliedOver;
}

struct PassCompiler {
  CompiledRenderPlan result;

  std::uint32_t AddPass(std::vector<Draw> draws) {
    auto& passes = result.passes.offscreen_passes;
    const auto index = NextIndex(passes.size());
    passes.push_back({std::move(draws)});
    return index;
  }

  std::array<std::uint32_t, 2> AddSides(const std::array<CompositeNode, 2>& sides) {
    if (!Identity(sides[0].placement) || !Identity(sides[1].placement))
      throw std::invalid_argument("A transition side must use canvas coordinates");
    return {AddPass(Compile(sides[0])), AddPass(Compile(sides[1]))};
  }

  std::uint32_t AddData(const PixelInput& source) {
    auto& data = result.data;
    const auto found = std::ranges::find_if(data, [&](const auto& input) { return SameInput(source, input); });
    if (found != data.end()) return static_cast<std::uint32_t>(found - data.begin());
    const auto index = NextIndex(data.size());
    data.push_back(source);
    return index;
  }

  template<class Node>
  PassImageSlot AddImage(const Node& node) {
    auto& images = result.images;
    const auto found = std::ranges::find_if(images, [&](const auto* input) {
      const auto* image = dynamic_cast<const Node*>(input);
      if (!image) return false;
      if constexpr (std::is_same_v<Node, ImageNode>) return node.pixels == image->pixels;
      else return SameTexture(node.texture, image->texture);
    });
    if (found != images.end()) return PassImageSlot::Input(static_cast<std::uint32_t>(found - images.begin()));
    const auto index = NextIndex(images.size());
    images.push_back(&node);
    return PassImageSlot::Input(index);
  }

  template<class Node>
  Draw ImageDraw(const Node& node) {
    const auto& layout = node.placement;
    return {AddImage(node), layout.blend, layout.local_to_target, {}, node.sampling, {}, layout.opacity};
  }

  void Append(std::vector<Draw>& draws, const RenderNode& node) {
    switch (node.Kind()) {
      case RenderNodeKind::Image:
        draws.push_back(ImageDraw(dynamic_cast<const ImageNode&>(node)));
        return;
      case RenderNodeKind::Texture:
        draws.push_back(ImageDraw(dynamic_cast<const TextureNode&>(node)));
        return;
      case RenderNodeKind::Pag: {
        const auto& pag = dynamic_cast<const PagNode&>(node);
        if (!pag.sample) throw std::invalid_argument("A PAG node needs an evaluated sample");
        const auto slot = PassImageSlot::Input(NextIndex(result.images.size()));
        result.images.push_back(&pag);
        draws.push_back({slot, pag.placement.blend, pag.placement.local_to_target, {}, {}, {}, pag.placement.opacity});
        return;
      }
      case RenderNodeKind::Composite: {
        const auto& composite = dynamic_cast<const CompositeNode&>(node);
        auto children = Compile(composite);
        const auto& layout = composite.placement;
        if (Identity(layout) && std::ranges::all_of(children, [](const auto& child) {
              return child.blend == Blend::PremultipliedOver;
            })) {
          draws.insert(draws.end(), std::make_move_iterator(children.begin()), std::make_move_iterator(children.end()));
        } else {
          draws.push_back({PassImageSlot::PassOutput(AddPass(std::move(children))), layout.blend,
                           layout.local_to_target, {}, {}, {}, layout.opacity});
        }
        return;
      }
      case RenderNodeKind::BackgroundReplacement: {
        const auto& background = dynamic_cast<const BackgroundReplacementNode&>(node);
        const auto& coverage = background.coverage;
        if (!background.foreground || (background.foreground->Kind() != RenderNodeKind::Image &&
                                       background.foreground->Kind() != RenderNodeKind::Texture))
          throw std::invalid_argument("Background replacement needs an image or texture");
        Append(draws, *background.foreground);
        auto& draw = draws.back();
        draw.blend = coverage.blend.value_or(draw.blend);
        draw.background = BackgroundDraw{AddData(coverage.mask), coverage.draw_to_mask_uv, coverage.valid_uv,
                                         coverage.color, coverage.inverted};
        return;
      }
      case RenderNodeKind::DepthOcclusion: {
        const auto& occlusion = dynamic_cast<const DepthOcclusionNode&>(node);
        if (!occlusion.child) throw std::invalid_argument("Depth occlusion needs a drawable child");
        const auto kind = occlusion.child->Kind();
        if (kind != RenderNodeKind::Image && kind != RenderNodeKind::Texture && kind != RenderNodeKind::Pag &&
            kind != RenderNodeKind::BackgroundReplacement)
          throw std::invalid_argument("Depth occlusion needs an image, PAG, or background replacement child");
        Append(draws, *occlusion.child);
        auto& draw = draws.back();
        draw.depth_occlusion = DepthDraw{AddData(occlusion.depth), occlusion.target_to_depth_uv,
                                         occlusion.valid_uv, occlusion.target_region, occlusion.layer_depth_meters};
        return;
      }
      case RenderNodeKind::MaskTransition: {
        const auto& transition = dynamic_cast<const MaskTransitionNode&>(node);
        const auto [outgoing, incoming] = AddSides(transition.sides);
        const auto& layout = transition.placement;
        const auto& sample = transition.sample;
        const MaskedDraw mask{AddData(sample.mask), sample.feather, sample.inverted, sample.mask_region,
                              PassImageSlot::PassOutput(incoming)};
        draws.push_back({PassImageSlot::PassOutput(outgoing), layout.blend, layout.local_to_target,
                         mask, {}, {}, layout.opacity});
        return;
      }
      case RenderNodeKind::PagTransition: {
        const auto& transition = dynamic_cast<const PagTransitionNode&>(node);
        const auto [outgoing, incoming] = AddSides(transition.sides);
        auto& passes = result.passes.offscreen_passes;
        const auto index = NextIndex(passes.size());
        passes.push_back({{}, PagPass{transition.sample, outgoing, incoming}});
        const auto& layout = transition.placement;
        draws.push_back({PassImageSlot::PassOutput(index), layout.blend, layout.local_to_target,
                         {}, {}, {}, layout.opacity});
        return;
      }
    }
    throw std::invalid_argument("Unknown render node kind");
  }

  std::vector<Draw> Compile(const CompositeNode& composite) {
    std::vector<Draw> draws;
    draws.reserve(composite.children.size());
    for (const auto& child : composite.children) {
      if (!child) throw std::invalid_argument("A composition needs valid child nodes");
      Append(draws, *child);
    }
    return draws;
  }
};

} // namespace

CompiledRenderPlan CompileRenderPasses(const RenderPlan& plan) {
  if (!Identity(plan.root.placement)) throw std::invalid_argument("The render root must use canvas coordinates");
  PassCompiler compiler;
  compiler.result.passes.target = plan.target;
  compiler.result.passes.final_draws = compiler.Compile(plan.root);
  return std::move(compiler.result);
}

} // namespace huxerui::avkit::render::detail
