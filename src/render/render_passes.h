#pragma once

#include "render/backend.h"

#include <span>

#include <huxerui/avkit/render.h>

namespace huxerui::avkit::render::detail {

/// @brief Slot in the image input table or in the compiled pass outputs.
struct PassImageSlot {
  bool input{};
  std::uint32_t index{};

  static constexpr PassImageSlot Input(std::uint32_t index) noexcept { return {true, index}; }

  static constexpr PassImageSlot PassOutput(std::uint32_t index) noexcept { return {false, index}; }

  constexpr bool IsInput() const noexcept { return input; }

  bool operator==(const PassImageSlot&) const = default;
};

struct MaskedDraw {
  std::uint32_t mask_slot;
  float feather;
  bool inverted;
  Rect mask_region;
  PassImageSlot incoming_image_slot;

  bool operator==(const MaskedDraw&) const = default;
};

struct BackgroundDraw {
  std::uint32_t mask_slot;
  Affine2D draw_to_mask_uv;
  Rect valid_uv;
  Color color;
  bool inverted;

  bool operator==(const BackgroundDraw&) const = default;
};

struct DepthDraw {
  std::uint32_t depth_slot;
  Affine2D target_to_depth_uv;
  Rect valid_uv, target_region;
  float layer_depth_meters;

  bool operator==(const DepthDraw&) const = default;
};

/// @brief One compiled draw. All referenced passes precede its target.
struct Draw {
  PassImageSlot image_slot;
  Blend blend;
  Affine2D local_to_target;
  std::optional<MaskedDraw> masked_blend;
  std::optional<SourceSampling> source_sampling;
  std::optional<DepthDraw> depth_occlusion;
  float opacity{1.0F};
  std::optional<BackgroundDraw> background;

  bool operator==(const Draw&) const = default;
};

struct PagPass : PagTransitionSample {
  std::uint32_t outgoing_pass, incoming_pass;
};

/// @brief Draw commands or a PAG composition of two earlier passes.
struct OffscreenPass {
  std::vector<Draw> draws;
  std::optional<PagPass> pag;
};

/// @brief Transient execution order lowered from the scene's render tree.
struct RenderPasses {
  ImageInfo target;
  std::vector<Draw> final_draws;
  std::vector<OffscreenPass> offscreen_passes;
};

struct PassRequest {
  std::shared_ptr<const RenderPasses> plan;
  std::vector<Texture> image_inputs;
  std::vector<Texture> data_inputs;
};

/// @brief Passes and the sources they reference before GPU preparation.
struct CompiledRenderPlan {
  RenderPasses passes;
  /// Borrowed ImageNode, TextureNode, or PagNode in GPU input order. The caller retains the tree during preparation.
  std::vector<const RenderNode*> images;
  /// Numeric mask and depth sources in GPU data input order.
  std::vector<PixelInput> data;
};

/// @brief Lowers a render tree in dependency order. Identity composites can expand into their parent pass.
/// @param plan Evaluated tree retained by the caller through preparation.
/// @return Ordered passes and their input sources.
CompiledRenderPlan CompileRenderPasses(const RenderPlan& plan);

/// @brief Submits a request already lowered and prepared on the active bgfx render sequence.
/// @param backend Active bgfx compositor.
/// @param request Prepared passes and their retained inputs.
SubmitResult SubmitBgfxPasses(RenderBackend& backend, const PassRequest& request);

template<class Plan, class Visitor>
void VisitDrawLists(Plan& plan, const Visitor& visit) {
  for (auto& pass : plan.offscreen_passes) visit(std::span(pass.draws));
  visit(std::span(plan.final_draws));
}

template<class Draws, class Visitor>
void VisitImageSlots(Draws draws, const Visitor& visit) {
  for (auto& draw : draws) {
    visit(draw.image_slot);
    if (draw.masked_blend) visit(draw.masked_blend->incoming_image_slot);
  }
}

} // namespace huxerui::avkit::render::detail
