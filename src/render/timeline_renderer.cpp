#include <huxerui/avkit/timeline_renderer.h>

#include "render/bgfx/timing.h"

#include <algorithm>
#include <map>
#include <span>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace huxerui::avkit::render {
namespace {

using namespace media;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return std::move(result).Value();
}

void CheckMask(const ImageSurface& surface) {
  Check(surface.storage && surface.image.format == PixelFormat::R8Unorm &&
        surface.image.size.width && surface.image.size.height, "Expected a nonempty R8Unorm mask");
}

struct ResourceLess {
  bool operator()(const ResourceRef& a, const ResourceRef& b) const {
    return std::tie(a.pack, a.revision, a.key) < std::tie(b.pack, b.revision, b.key);
  }
};

template<class T> using ResourceCache = std::map<ResourceRef, T, ResourceLess>;

template<class T, class Resolver>
T& Resolve(ResourceCache<T>& cache, const Resolver& resolve, const ResourceRef& resource) {
  const auto item = cache.lower_bound(resource);
  if (item != cache.end() && item->first == resource) return item->second;
  return cache.emplace_hint(item, resource, resolve(resource))->second;
}

} // namespace

struct TimelineRenderer::Impl {
  Impl(ImageDesc target, std::unique_ptr<RenderBackend> backend, TimelineRenderResources resources)
      : target_(target), backend_(std::move(backend)), resources_(std::move(resources)),
        overlays_(*backend_, std::move(resources_.overlays)) {}

  ImportedVideoFrame ImportFrame(const VideoFrame& frame) {
    const bool platform = frame.Storage() == VideoStorage::Platform;
    auto result = ImportDecodedBgfxFrame(*backend_, frame, {target_.size.width, target_.size.height},
        platform ? TextureTransferPolicy::GpuOnly : TextureTransferPolicy::AllowCpuTransfer);
    if (!result.Succeeded() && platform && result.Error().code == RenderErrorCode::Unsupported) {
      throw PlatformVideoImportError(result.Error().message);
    }
    return Require(std::move(result));
  }

  ColorSurface Draw(const TimelineFrame& frame, const TimelineRenderOptions& options) {
    if (options.transition) *options.transition = {};
    auto data = frame.analysis_masks;
    data.reserve(data.size() + frame.depth_maps.size() + frame.static_masks.size() + frame.animated_masks.size() + frame.masks.size());
    data.insert(data.end(), frame.depth_maps.begin(), frame.depth_maps.end());
    for (const auto& resource : frame.static_masks) data.push_back(StaticMaskTexture(resource));
    for (const auto& sample : frame.animated_masks) data.push_back(AnimatedMaskTexture(sample));
    for (const auto& sample : frame.masks) data.push_back(TransitionMaskTexture(sample));
    auto request = overlays_.Prepare(frame, [&](std::size_t slot) {
      const auto& input = frame.inputs.at(slot);
      if (const auto* picture = std::get_if<VideoFrame>(&input)) return ImportFrame(*picture).surface;
      if (const auto* surface = std::get_if<ColorSurface>(&input)) return *surface;
      if (const auto* resource = std::get_if<ResourceRef>(&input)) {
        if (!resources_.sticker) throw std::runtime_error("No resolver for the sticker " + resource->key);
        return Resolve(stickers_, resources_.sticker, *resource);
      }
      throw std::runtime_error("The timeline color input is unavailable");
    }, std::move(data), options.overlays, options.batch_overlays);
    if (!frame.transition_animations.empty()) {
      return DrawPagTransitions(frame, std::move(request), options.transition);
    }
    return Render(request);
  }

private:
  using Clock = std::chrono::steady_clock;

  static std::vector<ColorSurface> CompactColorInputs(RenderPlan& plan, std::span<const ColorSurface> inputs) {
    const auto visit_slots = [&](const auto& visit) {
      const auto visit_draw = [&](render::Draw& draw) {
        visit(draw.input_slot);
        if (draw.masked_blend) visit(draw.masked_blend->incoming_slot);
      };
      for (auto& pass : plan.subcompositions)
        for (auto& draw : pass.draws) visit_draw(draw);
      for (auto& draw : plan.draws) visit_draw(draw);
    };
    std::vector<bool> used(inputs.size());
    visit_slots([&](std::uint32_t slot) {
      if (slot < inputs.size()) used[slot] = true;
    });
    std::vector<std::uint32_t> slots(inputs.size());
    std::vector<ColorSurface> retained;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      if (!used[i]) continue;
      slots[i] = static_cast<std::uint32_t>(retained.size());
      retained.push_back(inputs[i]);
    }
    visit_slots([&](std::uint32_t& slot) {
      slot = slot < inputs.size() ? slots[slot] : static_cast<std::uint32_t>(retained.size() + slot - inputs.size());
    });
    return retained;
  }

  ColorSurface RenderTransitionSide(const RenderPlan& plan, std::uint32_t pass, const std::vector<ColorSurface>& inputs,
                                    const std::vector<ImageSurface>& data) {
    auto side = std::make_shared<RenderPlan>(RenderPlan{plan.target, plan.subcompositions.at(pass).draws});
    auto used = CompactColorInputs(*side, inputs);
    return Render({std::move(side), std::move(used), data});
  }

  ColorSurface DrawPagTransitions(const TimelineFrame& frame, RenderRequest request, PagTransitionRenderTiming* timing) {
    const auto& source = *request.plan;
    auto& inputs = request.inputs;
    const auto& data = request.data_inputs;
    auto final = std::make_shared<RenderPlan>(RenderPlan{source.target, source.draws});
    const auto external_slots = inputs.size();
    std::vector<std::optional<std::uint32_t>> slots(source.subcompositions.size());
    for (const auto& sample : frame.transition_animations) {
      const auto [from, to] = std::pair{sample.outgoing_pass, sample.incoming_pass};
      Check(from != to && !slots.at(from) && !slots.at(to), "Expected two independent transition passes");
      auto& transition = pag_transitions_[sample.instance.value];
      if (!transition.animation || transition.resource != sample.resource) {
        if (!resources_.pag_transition) throw std::runtime_error("No resolver for the PAG transition " + sample.resource.key);
        auto asset = resources_.pag_transition(sample.resource);
        transition = {sample.resource, std::make_unique<PagTransition>(std::move(asset.encoded), target_.size,
                                                                      asset.outgoing_index, asset.incoming_index)};
      }
      const auto started = timing ? Clock::now() : Clock::time_point{};
      const auto outgoing = RenderTransitionSide(source, from, inputs, data);
      const auto incoming = RenderTransitionSide(source, to, inputs, data);
      if (timing) timing->prepare_sides += Clock::now() - started;
      slots[from] = slots[to] = static_cast<std::uint32_t>(inputs.size());
      PagTransitionTiming stage;
      inputs.push_back(transition.animation->Sample(sample.offset, sample.duration, outgoing, incoming, timing ? &stage : nullptr));
      if (timing) {
        timing->pag.stage_inputs += stage.stage_inputs;
        timing->pag.bind_images += stage.bind_images;
        timing->pag.flush += stage.flush;
        timing->pag.wait_for_output += stage.wait_for_output;
      }
    }
    for (std::size_t pass = 0; pass < slots.size(); ++pass) {
      if (slots[pass]) continue;
      slots[pass] = static_cast<std::uint32_t>(inputs.size() + final->subcompositions.size());
      final->subcompositions.push_back(source.subcompositions[pass]);
    }
    const auto remap = [&](std::uint32_t& slot) {
      if (slot >= external_slots) slot = *slots.at(slot - external_slots);
    };
    for (auto& draw : final->draws) {
      remap(draw.input_slot);
      if (draw.masked_blend) remap(draw.masked_blend->incoming_slot);
    }
    std::erase_if(pag_transitions_, [&](const auto& entry) {
      return std::ranges::none_of(frame.transition_animations, [&](const auto& sample) {
        return sample.instance.value == entry.first;
      });
    });
    auto used = CompactColorInputs(*final, inputs);
    return Render({std::move(final), std::move(used), data}, timing);
  }

  const ImageSurface& StaticMaskTexture(const ResourceRef& resource) {
    return Resolve(static_masks_, [&](const auto& ref) {
      if (!resources_.static_mask) throw std::runtime_error("No resolver for the static mask " + ref.key);
      auto surface = resources_.static_mask(ref);
      CheckMask(surface);
      return surface;
    }, resource);
  }

  struct ResolvedMaskSequence {
    MaskSequence source;
    std::optional<ImageDesc> image;
    std::unordered_map<std::uint32_t, ImageSurface> frames;

    const ImageSurface& Frame(std::uint32_t index) {
      if (const auto item = frames.find(index); item != frames.end()) return item->second;
      auto frame = source.frame(index);
      CheckMask(frame);
      Check(!image || frame.image == *image, "Mask sequence frame dimensions differ");
      image = frame.image;
      return frames.emplace(index, std::move(frame)).first->second;
    }
  };

  ResolvedMaskSequence& Sequence(const ResourceRef& resource) {
    return Resolve(mask_sequences_, [&](const auto& ref) {
      if (!resources_.mask_sequence) throw std::runtime_error("No resolver for the mask sequence " + ref.key);
      auto sequence = resources_.mask_sequence(ref);
      Check(sequence.frame_count && sequence.frame, "A mask sequence needs at least one resolvable frame");
      return ResolvedMaskSequence{std::move(sequence)};
    }, resource);
  }

  const ImageSurface& TransitionMaskTexture(const TransitionMaskSample& sample) {
    auto& sequence = Sequence(sample.resource);
    return sequence.Frame(sample.FrameIndex(sequence.source.frame_count));
  }

  const ImageSurface& AnimatedMaskTexture(const AnimatedMaskSample& sample) {
    auto& sequence = Sequence(sample.resource);
    return sequence.Frame(sample.FrameIndex(sequence.source.frame_interval, sequence.source.frame_count));
  }

public:
  ColorSurface Render(const RenderRequest& request, PagTransitionRenderTiming* timing = nullptr) {
    const auto start = timing ? Clock::now() : Clock::time_point{};
    auto ticket = Require(backend_->TrySubmit(request));
    Check(ticket.has_value(), "The composition target is busy");
    if (timing) timing->enqueue_upload = render::detail::PumpBgfxWithUploadTiming(*backend_);
    else backend_->Pump();
    auto output = Require(backend_->TryTake(*ticket));
    Check(output.has_value(), "The composition target did not finish");
    if (timing) timing->complete_output = Clock::now() - start;
    return std::move(*output);
  }

  VideoFrame EncoderFrame(const ColorSurface& output, const VideoEncoderConfig& config, MediaTimestamp pts,
                          std::optional<MediaTimestamp> duration) {
    auto picture = MakeBgfxEncoderFrame(*backend_, output, config, pts, duration);
    if (!picture.Succeeded() && config.input_storage == VideoStorage::Platform &&
        picture.Error().code == RenderErrorCode::Unsupported) {
      throw PlatformVideoTransferError(picture.Error().message);
    }
    return Require(std::move(picture));
  }

  ImageDesc target_;
  std::unique_ptr<RenderBackend> backend_;

private:
  struct Transition {
    ResourceRef resource;
    std::unique_ptr<PagTransition> animation;
  };

  TimelineRenderResources resources_;
  PagOverlayRenderer overlays_;
  ResourceCache<ColorSurface> stickers_;
  ResourceCache<ImageSurface> static_masks_;
  ResourceCache<ResolvedMaskSequence> mask_sequences_;
  std::unordered_map<std::uint64_t, Transition> pag_transitions_;
};

TimelineRenderer::TimelineRenderer(ImageDesc target, std::unique_ptr<RenderBackend> backend,
                                   TimelineRenderResources resources) {
  if (!backend) throw std::invalid_argument("A timeline renderer needs a bgfx backend");
  impl_ = std::make_unique<Impl>(target, std::move(backend), std::move(resources));
}
TimelineRenderer::~TimelineRenderer() = default;

RenderBackend& TimelineRenderer::Backend() { return *impl_->backend_; }
PixelFormat TimelineRenderer::Format() const { return impl_->target_.format; }

ImportedVideoFrame TimelineRenderer::ImportFrame(const VideoFrame& input) { return impl_->ImportFrame(input); }

ColorSurface TimelineRenderer::Draw(const TimelineFrame& frame, const TimelineRenderOptions& options) {
  return impl_->Draw(frame, options);
}

ColorSurface TimelineRenderer::Render(const RenderRequest& request) { return impl_->Render(request); }

VideoFrame TimelineRenderer::EncoderFrame(const ColorSurface& output, const VideoEncoderConfig& config,
                                          MediaTimestamp pts, std::optional<MediaTimestamp> duration) {
  return impl_->EncoderFrame(output, config, pts, duration);
}

} // namespace huxerui::avkit::render
