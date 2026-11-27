#include "render/pag/pag_overlay_renderer.h"
#include "render/pag/pag_transition.h"
#include "render/render_passes.h"
#include "media/timeline_internal.h"

#include <huxerui/avkit/resource_catalog.h>
#include <huxerui/avkit/timeline.h>
#include "render/bgfx/backend.h"

#include "render/bgfx/render_lease.h"
#include "render/bgfx/video_texture_internal.h"
#include "support/cancellation.h"

#include <algorithm>
#include <list>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace huxerui::avkit::render {
namespace {

using namespace media;
using detail::PassImageSlot;
using detail::PassRequest;
using detail::RenderPasses;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return std::move(result).Value();
}

} // namespace

struct TimelineRenderer::Impl {
  Impl(ImageInfo target, std::unique_ptr<RenderBackend> backend)
      : target_(target), backend_(std::move(backend)) {}

  Texture Draw(const TimelineFrame& frame, std::span<avkit::detail::TargetLayoutEntry> layouts) {
    if (!frame.plan) throw std::invalid_argument("A timeline render plan is required");
    if (frame.plan->target != target_) throw std::invalid_argument("The render plan target does not match the compositor");
    auto compiled = detail::CompileRenderPasses(*frame.plan);
    const bool pag_transition = std::ranges::any_of(compiled.passes.offscreen_passes, [](const auto& pass) {
      return pass.pag.has_value();
    });
    UseResources(frame.plan->resources);
    if (!overlays_) overlays_ = std::make_unique<avkit::detail::PagOverlayRenderer>(*backend_, resources_.get());
    std::vector<Texture> images(compiled.images.size());
    for (std::size_t index = 0; index < compiled.images.size(); ++index) {
      if (const auto* image = dynamic_cast<const ImageNode*>(compiled.images[index]))
        images[index] = PrepareTexture(image->pixels);
      else if (const auto* texture = dynamic_cast<const TextureNode*>(compiled.images[index]))
        images[index] = texture->texture;
    }
    std::vector<Texture> data;
    data.reserve(compiled.data.size());
    for (const auto& input : compiled.data) {
      if (const auto* pixels = std::get_if<ImagePixels>(&input)) data.push_back(PrepareTexture(*pixels));
      else data.push_back(std::get<Texture>(input));
    }
    auto request = overlays_->Prepare(std::move(compiled.passes), compiled.images,
                                      std::move(images), std::move(data), layouts);
    if (pag_transition) return DrawPagTransitions(std::move(request));
    return RenderPassRequest(CompactImageInputs(std::move(request)));
  }

private:
  Texture PrepareTexture(const ImagePixels& pixels) {
    const auto found = std::ranges::find(uploaded_, pixels, &std::pair<ImagePixels, Texture>::first);
    if (found != uploaded_.end()) {
      uploaded_.splice(uploaded_.begin(), uploaded_, found);
      return uploaded_.front().second;
    }
    // Keep recent upload leases by retained pixel identity, including a single oversized input.
    constexpr std::size_t kUploadCacheBytes = 64 * 1024 * 1024;
    const auto bytes = pixels.bytes.AsBytes().size();
    const auto remaining = kUploadCacheBytes - std::min(bytes, kUploadCacheBytes);
    while (!uploaded_.empty() && (uploaded_.size() >= 32 || uploaded_bytes_ > remaining)) {
      uploaded_bytes_ -= uploaded_.back().first.bytes.AsBytes().size();
      uploaded_.pop_back();
    }
    auto texture = Require(backend_->UploadTexture(pixels));
    uploaded_.emplace_front(pixels, texture);
    uploaded_bytes_ += bytes;
    return texture;
  }

  static PassRequest CompactImageInputs(PassRequest request) {
    std::vector<bool> used(request.image_inputs.size());
    detail::VisitDrawLists(*request.plan, [&](auto draws) {
      detail::VisitImageSlots(draws, [&](PassImageSlot slot) {
        if (slot.IsInput()) used.at(slot.index) = true;
      });
    });
    if (std::ranges::all_of(used, [](bool present) { return present; })) return request;
    std::vector<std::uint32_t> slots(used.size());
    std::vector<Texture> retained;
    for (std::size_t index = 0; index < used.size(); ++index) {
      if (!used[index]) continue;
      slots[index] = static_cast<std::uint32_t>(retained.size());
      retained.push_back(std::move(request.image_inputs[index]));
    }
    auto plan = std::make_shared<RenderPasses>(*request.plan);
    detail::VisitDrawLists(*plan, [&](auto draws) {
      detail::VisitImageSlots(draws, [&](PassImageSlot& slot) {
        if (slot.IsInput()) slot.index = slots[slot.index];
      });
    });
    return {std::move(plan), std::move(retained), std::move(request.data_inputs)};
  }

  static PassRequest SelectPasses(const PassRequest& request, std::span<const detail::Draw> draws,
                                  std::span<const std::optional<PassImageSlot>> replacements) {
    const auto& source = *request.plan;
    auto plan = std::make_shared<RenderPasses>();
    plan->target = source.target;
    std::vector<std::optional<PassImageSlot>> selected(source.offscreen_passes.size());
    const auto remap = [&](const auto& self, PassImageSlot& slot) -> void {
      if (slot.IsInput()) return;
      if (const auto& replacement = replacements[slot.index]) {
        slot = *replacement;
        return;
      }
      auto& moved = selected[slot.index];
      if (!moved) {
        auto pass = source.offscreen_passes.at(slot.index);
        Check(!pass.pag, "A referenced PAG transition has not been evaluated");
        detail::VisitImageSlots(std::span(pass.draws), [&](PassImageSlot& child) { self(self, child); });
        moved = PassImageSlot::PassOutput(static_cast<std::uint32_t>(plan->offscreen_passes.size()));
        plan->offscreen_passes.push_back(std::move(pass));
      }
      slot = *moved;
    };
    plan->final_draws.assign(draws.begin(), draws.end());
    detail::VisitImageSlots(std::span(plan->final_draws), [&](PassImageSlot& slot) { remap(remap, slot); });
    return CompactImageInputs({std::move(plan), request.image_inputs, request.data_inputs});
  }

  Texture DrawPagTransitions(PassRequest request) {
    const auto& source = *request.plan;
    std::vector<std::optional<PassImageSlot>> slots(source.offscreen_passes.size());
    for (std::size_t index = 0; index < source.offscreen_passes.size(); ++index) {
      const auto& pass = source.offscreen_passes[index];
      if (!pass.pag) continue;
      const auto& sample = *pass.pag;
      auto& transition = pag_transitions_[sample.instance.value];
      if (!transition.animation || transition.resource != sample.resource) {
        transition = {sample.resource, std::make_unique<PagTransition>(sample.resource, Catalog(), target_.size)};
      }
      const auto render_side = [&](std::uint32_t side) {
        return RenderPassRequest(SelectPasses(request, source.offscreen_passes.at(side).draws, slots));
      };
      const auto outgoing = render_side(sample.outgoing_pass);
      const auto incoming = render_side(sample.incoming_pass);
      slots[index] = PassImageSlot::Input(static_cast<std::uint32_t>(request.image_inputs.size()));
      request.image_inputs.push_back(transition.animation->Sample(sample.offset, sample.duration, outgoing, incoming));
    }
    std::erase_if(pag_transitions_, [&](const auto& entry) {
      return std::ranges::none_of(source.offscreen_passes, [&](const auto& pass) {
        return pass.pag && pass.pag->instance.value == entry.first;
      });
    });
    return RenderPassRequest(SelectPasses(request, source.final_draws, slots));
  }

  const ResourceCatalog& Catalog() const {
    if (!resources_) throw std::invalid_argument("A packaged input needs the frame resource catalog");
    return *resources_;
  }

  void UseResources(const std::shared_ptr<const ResourceCatalog>& resources) {
    if (resources_ == resources) return;
    overlays_.reset();
    pag_transitions_.clear();
    resources_ = resources;
  }

  Texture RenderPassRequest(const PassRequest& request) {
    auto ticket = Require(detail::SubmitBgfxPasses(*backend_, request));
    Check(ticket.has_value(), "The composition target is busy");
    backend_->Pump();
    auto output = Require(backend_->TryTake(*ticket));
    Check(output.has_value(), "The composition target did not finish");
    return std::move(*output);
  }

public:
  VideoFrame EncoderFrame(const Texture& output, const VideoEncoderConfig& config, MediaTimestamp pts,
                          std::optional<MediaTimestamp> duration) {
    auto picture = MakeBgfxEncoderFrame(*backend_, output, config, pts, duration);
    if (!picture.Succeeded() && config.input_storage == VideoStorage::Platform &&
        picture.Error().code == RenderErrorCode::Unsupported) {
      throw PlatformEncoderInputError(picture.Error().message);
    }
    return Require(std::move(picture));
  }

  ImageInfo target_;
  std::shared_ptr<RenderBackend> backend_;

private:
  struct Transition {
    ResourceRef resource;
    std::unique_ptr<PagTransition> animation;
  };

  std::shared_ptr<const ResourceCatalog> resources_;
  std::unique_ptr<avkit::detail::PagOverlayRenderer> overlays_;
  std::list<std::pair<ImagePixels, Texture>> uploaded_;
  std::size_t uploaded_bytes_{};
  std::unordered_map<std::uint64_t, Transition> pag_transitions_;
};

std::unique_ptr<TimelineRenderer> TimelineRenderer::Open(ImageInfo target, std::stop_token stop, RenderPlatformHandles platform) {
  auto backend = detail::OpenBgfxBackend(target, platform, stop);
  if (!backend) return {};
  auto impl = std::make_unique<Impl>(target, std::move(backend));
  if (stop.stop_requested()) return {};
  return std::unique_ptr<TimelineRenderer>(new TimelineRenderer(std::move(impl)));
}

TimelineRenderer::TimelineRenderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

TimelineRenderer::~TimelineRenderer() = default;

std::unique_ptr<TimelineSession> TimelineRenderer::OpenTimeline(std::shared_ptr<const ProjectSnapshot> project,
    CodecProviders providers, CodecPolicy policy, SceneConfiguration scene, OnCodecFallback fallback) const {
  return avkit::detail::TimelineSessionAccess::Open(std::move(project), std::move(providers), policy,
      scene, std::move(fallback), impl_->backend_, impl_->target_);
}

PixelFormat TimelineRenderer::Format() const { return impl_->target_.format; }

Texture TimelineRenderer::ImportTexture(const ExternalTexture& texture) {
  return Require(impl_->backend_->ImportTexture(texture));
}

Texture TimelineRenderer::UploadTexture(const ImagePixels& pixels) {
  return Require(impl_->backend_->UploadTexture(pixels));
}

Texture TimelineRenderer::Draw(const TimelineFrame& frame) {
  return impl_->Draw(frame, {});
}

Texture TimelineRenderer::Draw(const TimelineFrame& frame, std::vector<avkit::detail::TargetLayoutEntry>& layouts) {
  return impl_->Draw(frame, layouts);
}

Result<PreviewFrameCommit, RenderError> TimelineRenderer::PreparePreview(PreviewTexture& preview,
                                                                        const Texture& output) {
  return preview.Prepare(*impl_->backend_, output);
}

Result<void, RenderError> TimelineRenderer::PublishPreview(PreviewTexture& preview, const Texture& output) {
  return preview.Publish(*impl_->backend_, output);
}

VideoFrame TimelineRenderer::EncoderFrame(const Texture& output, const VideoEncoderConfig& config,
                                          MediaTimestamp pts, std::optional<MediaTimestamp> duration) {
  return impl_->EncoderFrame(output, config, pts, duration);
}

void ExportRenderedTimeline(TimelineRenderer& renderer, std::shared_ptr<const ProjectSnapshot> project,
    const File& destination, const CodecProviders& providers, const FileExportSettings& settings,
    const TimelineExportOptions& options) {
  avkit::detail::ExportTimeline(std::move(project), destination, providers, settings,
      renderer.impl_->backend_, renderer.impl_->target_,
      [&](const TimelineFrame& frame, const VideoEncoderConfig& config, MediaTimestamp duration) {
        avkit::detail::CheckCancelled(options.stop);
        std::optional<VideoFrame> output;
        const auto iteration = [&] {
          output = renderer.EncoderFrame(renderer.Draw(frame), config, frame.pts, duration);
          if (options.on_video_transfer) options.on_video_transfer(CodecOperation::VideoEncode, *output);
          if (options.on_frame) options.on_frame(frame.pts);
          return false;
        };
        if (options.scope) options.scope(iteration);
        else iteration();
        avkit::detail::CheckCancelled(options.stop);
        if (!output) throw std::runtime_error("The export iteration did not render a frame");
        return std::move(*output);
      }, options);
}

} // namespace huxerui::avkit::render
