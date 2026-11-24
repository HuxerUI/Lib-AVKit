#include <huxerui/avkit/pag.h>
#include <huxerui/avkit/timeline.h>

#include "animation/pag_frame_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace huxerui::avkit {
namespace {

using namespace render;

auto Byte(float value) { return static_cast<std::uint8_t>(std::lround(value * 255)); }

pag::Color PagColor(Color color) { return {Byte(color.red), Byte(color.green), Byte(color.blue)}; }

struct PointText {
  std::shared_ptr<pag::PAGComposition> composition;
  Rect artwork{0, 0, 1, 1};
};

PointText PreparePointText(const CanvasOverlay& overlay, PixelSize canvas, const PagOverlayResources& assets, const PagFont* font) {
  const auto output_width = overlay.destination.width * canvas.width;
  const auto output_height = overlay.destination.height * canvas.height;
  // Rasterize the overlay at its destination size in the current render target.
  const auto scale = std::min({1.0F, 8192.0F / output_width, 8192.0F / output_height});
  const auto width = static_cast<int>(std::max(8.0F, std::ceil(output_width * scale)));
  const auto height = static_cast<int>(std::max(8.0F, std::ceil(output_height * scale)));
  auto comp = pag::PAGComposition::Make(width, height);
  if (!comp) throw std::runtime_error("Cannot create the text composition");
  const auto encoded = assets.point_text.AsBytes();
  const auto master = pag::PAGFile::Load(encoded.data(), encoded.size());
  auto style = master ? master->getTextData(0) : nullptr;
  if (!style) throw std::runtime_error("Cannot open the point text master");
  style->text = overlay.text;
  style->fontSize = height * 0.46F * overlay.style.size;
  style->fillColor = PagColor(overlay.style.foreground);
  if (font) {
    style->fontFamily = font->Family();
    style->fontStyle = "Regular";
  }
  style->applyStroke = true;
  style->strokeWidth = std::max(0.6F * scale, height * 0.014F);
  style->strokeOverFill = false;
  if (overlay.style.background.alpha > 0) {
    auto background = pag::PAGSolidLayer::Make(1'000'000, width, height, PagColor(overlay.style.background),
                                              Byte(overlay.style.background.alpha));
    if (!background || !comp->addLayer(background)) throw std::runtime_error("Cannot create the text background");
  }
  auto layer = pag::PAGTextLayer::Make(1'000'000, std::move(style));
  if (!layer || !comp->addLayer(layer)) throw std::runtime_error("Cannot create the text overlay");
  layer->setAlpha(overlay.style.foreground.alpha);
  const auto bounds = layer->getTotalMatrix().mapRect(layer->getBounds());
  PointText result{comp};
  if (bounds.width() > 0 && bounds.height() > 0) {
    const auto fit = std::min({1.0F, width * 0.94F / bounds.width(), height * 0.9F / bounds.height()});
    auto transform = pag::Matrix::MakeScale(fit, fit);
    transform.postTranslate(width * 0.5F - fit * (bounds.left + bounds.right) * 0.5F,
                            height * 0.5F - fit * (bounds.top + bounds.bottom) * 0.5F);
    layer->setMatrix(transform);
    if (overlay.style.background.alpha == 0) {
      const auto w = std::min(1.0F, (bounds.width() * fit + 2.0F * scale) / width);
      const auto h = std::min(1.0F, (bounds.height() * fit + 2.0F * scale) / height);
      result.artwork = {(1.0F - w) * 0.5F, (1.0F - h) * 0.5F, w, h};
    }
  }
  return result;
}

bool SameArtwork(const CanvasOverlay& a, const CanvasOverlay& b) {
  if (a.kind != b.kind || a.resource != b.resource || a.text != b.text || a.style != b.style) return false;
  return a.kind == CanvasOverlayKind::Sticker || a.kind == CanvasOverlayKind::PagText ||
         (a.destination.width == b.destination.width && a.destination.height == b.destination.height);
}

struct OverlayEntry {
  PagOverlayAsset asset;
  std::unique_ptr<detail::PagFrameRenderer> renderer;
  std::shared_ptr<pag::PAGComposition> point_text;
  std::optional<CanvasOverlay> canvas;
  std::optional<AnimatedStickerSample> clip;
  std::uint64_t last_used{};
};

struct OverlayInput {
  std::shared_ptr<OverlayEntry> entry;
  media::MediaTimestamp elapsed;
  AnimationPlayback playback{AnimationPlayback::Loop};
};

class OverlayBatch {
public:
  OverlayBatch(std::span<const OverlayInput> sources, PixelSize size) : size_(size) {
    composition_ = pag::PAGComposition::Make(size.width, size.height);
    bool compatible = bool(composition_);
    for (const auto& input : sources) {
      auto source = input.entry->point_text;
      double rate = 0;
      int count = 0;
      if (!source) {
        const auto& asset = input.entry->asset;
        source = detail::OpenPagAnimation(asset.encoded, asset.texts, asset.images);
        rate = source->frameRate();
        const auto frames = std::round(source->duration() * rate / 1'000'000.0);
        if (rate > 60 || frames < 1 || frames > std::numeric_limits<int>::max()) compatible = false;
        else count = static_cast<int>(frames);
      }
      auto layer = pag::PAGComposition::Make(source->width(), source->height());
      if (!layer || !compatible || !layer->addLayer(source) || !composition_->addLayer(layer)) compatible = false;
      else source->setExcludedFromTimeline(true);
      layers_.push_back({input.entry, std::move(source), std::move(layer), rate, count});
    }
    if (!compatible) composition_.reset();
  }

  bool Matches(std::span<const OverlayInput> inputs) const {
    return layers_.size() == inputs.size() && std::equal(layers_.begin(), layers_.end(), inputs.begin(),
        [](const Layer& layer, const OverlayInput& input) { return layer.entry == input.entry; });
  }

  ColorSurface Sample(std::span<const OverlayInput> inputs, std::span<const Draw> draws,
                      RenderBackend& backend, PagOverlayStats* stats) {
    if (!composition_) return {};
    try {
      if (!target_) {
        target_ = detail::CreatePagGpuTarget(backend, size_);
        if (!target_) {
          composition_.reset();
          return {};
        }
        player_ = std::make_unique<pag::PAGPlayer>();
        player_->setComposition(composition_);
      }
      bool changed = false;
      for (std::size_t i = 0; i < layers_.size(); ++i) {
        auto& layer = layers_[i];
        const auto& draw = draws[i];
        if (!layer.draw || draw.destination != layer.draw->destination || draw.transform != layer.draw->transform ||
            draw.opacity != layer.draw->opacity) {
          const auto& rect = draw.destination;
          const auto map = draw.transform.value_or(Transform2D{});
          const auto origin = map.Apply({rect.x, rect.y});
          const auto sx = rect.width / layer.source->width(), sy = rect.height / layer.source->height();
          layer.placed->setMatrix(pag::Matrix::MakeAll(
              map.m11 * sx * size_.width, map.m21 * sy * size_.width, origin.x * size_.width,
              map.m12 * sx * size_.height, map.m22 * sy * size_.height, origin.y * size_.height, 0, 0, 1));
          layer.placed->setAlpha(draw.opacity);
          layer.draw = draw;
          changed = true;
        }
        if (layer.count == 0) continue;
        const auto index = detail::PagFrameIndex(inputs[i].elapsed, inputs[i].playback, layer.rate, layer.count);
        if (index == layer.index) continue;
        layer.source->setProgress((index + 0.5) / layer.count);
        layer.index = index;
        changed = true;
      }
      if (changed || !surface_.pixels.storage) {
        surface_ = {};
        surface_ = target_->Draw(*player_, stats);
        if (stats) ++stats->batched_flushes;
      }
      return surface_;
    } catch (const std::runtime_error&) {
      surface_ = {};
      player_.reset();
      target_.reset();
      composition_.reset();
      return {};
    }
  }

  std::uint64_t last_used{};

private:
  struct Layer {
    std::shared_ptr<OverlayEntry> entry;
    std::shared_ptr<pag::PAGComposition> source, placed;
    double rate;
    int count, index{-1};
    std::optional<Draw> draw;
  };
  std::vector<Layer> layers_;
  PixelSize size_;
  std::shared_ptr<pag::PAGComposition> composition_;
  std::unique_ptr<detail::PagGpuTarget> target_;
  std::unique_ptr<pag::PAGPlayer> player_;
  ColorSurface surface_;
};

} // namespace

Rect MeasurePagOverlay(const CanvasOverlay& overlay, PixelSize canvas, const PagOverlayResources& resources) {
  if (overlay.kind == CanvasOverlayKind::Sticker || overlay.kind == CanvasOverlayKind::PagText ||
      overlay.style.background.alpha > 0) return {0, 0, 1, 1};
  std::optional<PagFont> font;
  if (!overlay.style.font.key.empty() && resources.font) {
    const auto bytes = resources.font(overlay.style.font);
    if (!bytes.AsBytes().empty()) font.emplace(bytes);
  }
  return PreparePointText(overlay, canvas, resources, font ? &*font : nullptr).artwork;
}

struct PagOverlayRenderer::Impl {
  Impl(RenderBackend& target, PagOverlayResources assets) : backend(target), resources(std::move(assets)) {}

  OverlayInput Resolve(const TimelineInput& input, PagOverlayStats* stats) {
    if (const auto* sample = std::get_if<CanvasOverlaySample>(&input)) {
      const auto& overlay = sample->overlay;
      auto& entry = canvas_entries[overlay.id.value];
      if (!entry || !SameArtwork(*entry->canvas, overlay)) {
        auto next = std::make_shared<OverlayEntry>();
        next->canvas = overlay;
        if (overlay.kind == CanvasOverlayKind::Sticker || overlay.kind == CanvasOverlayKind::PagText) {
          if (!resources.canvas_animation) throw std::invalid_argument("A canvas PAG resource resolver is required");
          next->asset = resources.canvas_animation(overlay);
        } else {
          const auto& resource = overlay.style.font;
          if (!resource.key.empty() && resources.font && resource != font_resource) {
            const auto bytes = resources.font(resource);
            font = bytes.AsBytes().empty() ? nullptr : std::make_unique<PagFont>(bytes);
            font_resource = resource;
          }
          next->point_text = PreparePointText(overlay, canvas, resources, resource == font_resource ? font.get() : nullptr).composition;
          if (stats) ++stats->text_layers;
        }
        entry = std::move(next);
      } else if (entry->point_text && stats) {
        ++stats->text_layer_cache_hits;
      }
      entry->last_used = frame_number;
      return {entry, sample->elapsed, AnimationPlayback::Loop};
    }
    if (const auto* sample = std::get_if<AnimatedStickerSample>(&input); sample && resources.clip_animation) {
      auto& entry = clip_entries[sample->instance.value];
      if (!entry || entry->clip->resource != sample->resource || entry->clip->replacements != sample->replacements) {
        auto next = std::make_shared<OverlayEntry>();
        next->clip = *sample;
        next->asset = resources.clip_animation(sample->resource, sample->replacements);
        entry = std::move(next);
      }
      entry->last_used = frame_number;
      return {entry, sample->elapsed, sample->playback};
    }
    return {};
  }

  ColorSurface Sample(const OverlayInput& input, PagOverlayStats* stats) {
    auto& entry = *input.entry;
    if (!entry.renderer) {
      auto composition = entry.point_text;
      if (!composition) composition = detail::OpenPagAnimation(entry.asset.encoded, entry.asset.texts, entry.asset.images);
      entry.renderer = std::make_unique<detail::PagFrameRenderer>(std::move(composition), &backend);
    }
    return entry.renderer->Sample(entry.point_text ? media::MediaTimestamp{} : input.elapsed,
        entry.point_text ? AnimationPlayback::HoldLast : input.playback, stats);
  }

  render::RenderRequest Prepare(const TimelineFrame& frame, const ResolveColor& resolve_color,
                                std::vector<ImageSurface> data, PagOverlayStats* stats, bool batch_overlays) {
    ++frame_number;
    if (canvas != frame.plan->target.size) {
      canvas = frame.plan->target.size;
      canvas_entries.clear();
      clip_entries.clear();
      batches.clear();
    }
    std::vector<OverlayInput> sources;
    sources.reserve(frame.inputs.size());
    for (const auto& input : frame.inputs) sources.push_back(Resolve(input, stats));
    const auto count = sources.size();
    auto plan = std::make_shared<RenderPlan>(*frame.plan);
    const auto visit = [&](const auto& fn) {
      for (auto& pass : plan->subcompositions) fn(pass.draws);
      fn(plan->draws);
    };
    std::vector<unsigned> uses(count);
    visit([&](std::span<Draw> draws) {
      const auto mark = [&](std::uint32_t slot) { if (slot < count) ++uses[slot]; };
      for (const auto& draw : draws) {
        mark(draw.input_slot);
        if (draw.masked_blend) mark(draw.masked_blend->incoming_slot);
      }
    });

    constexpr auto unbound = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> slots(count, unbound);
    std::vector<ColorSurface> inputs;
    inputs.reserve(count);
    if (batch_overlays) visit([&](std::vector<Draw>& draws) {
      const auto eligible = [&](const Draw& draw) {
        const auto slot = draw.input_slot;
        return slot < count && uses[slot] == 1 && sources[slot].entry && !draw.masked_blend &&
            !draw.source_sampling && !draw.depth_occlusion && draw.blend == Blend::PremultipliedOver;
      };
      std::vector<OverlayInput> group;
      for (std::size_t start = 0; start < draws.size();) {
        if (!eligible(draws[start])) { ++start; continue; }
        auto end = start;
        group.clear();
        while (end < draws.size() && eligible(draws[end])) group.push_back(sources[draws[end++].input_slot]);
        if (group.size() > 1) {
          auto found = std::ranges::find_if(batches, [&](const auto& batch) { return batch->Matches(group); });
          auto& batch = *(found == batches.end() ? batches.emplace_back(std::make_unique<OverlayBatch>(group, canvas)) : *found);
          batch.last_used = frame_number;
          auto surface = batch.Sample(group, std::span(draws).subspan(start, end - start), backend, stats);
          if (surface.pixels.storage) {
            const auto slot = draws[start].input_slot;
            slots[slot] = static_cast<std::uint32_t>(inputs.size());
            inputs.push_back(std::move(surface));
            for (auto i = start + 1; i < end; ++i) uses[draws[i].input_slot] = 0;
            draws[start] = {slot, Blend::PremultipliedOver};
            draws.erase(draws.begin() + start + 1, draws.begin() + end);
            end = start + 1;
          }
        }
        start = end;
      }
    });
    for (std::size_t slot = 0; slot < count; ++slot) {
      if (!uses[slot] || slots[slot] != unbound) continue;
      slots[slot] = static_cast<std::uint32_t>(inputs.size());
      inputs.push_back(sources[slot].entry ? Sample(sources[slot], stats) : resolve_color(slot));
    }
    visit([&](std::span<Draw> draws) {
      const auto remap = [&](std::uint32_t& slot) {
        slot = slot < count ? slots[slot] : static_cast<std::uint32_t>(slot - count + inputs.size());
      };
      for (auto& draw : draws) {
        remap(draw.input_slot);
        if (draw.masked_blend) remap(draw.masked_blend->incoming_slot);
      }
    });
    const auto unused = [&](const auto& item) { return item.second->last_used != frame_number; };
    std::erase_if(canvas_entries, unused);
    std::erase_if(clip_entries, unused);
    std::erase_if(batches, [&](const auto& batch) { return batch->last_used != frame_number; });
    return {std::move(plan), std::move(inputs), std::move(data)};
  }

  RenderBackend& backend;
  PagOverlayResources resources;
  PixelSize canvas{};
  std::uint64_t frame_number{};
  ResourceRef font_resource;
  std::unique_ptr<PagFont> font;
  std::unordered_map<std::uint64_t, std::shared_ptr<OverlayEntry>> canvas_entries, clip_entries;
  std::vector<std::unique_ptr<OverlayBatch>> batches;
};

PagOverlayRenderer::PagOverlayRenderer(RenderBackend& backend, PagOverlayResources resources)
    : impl_(std::make_unique<Impl>(backend, std::move(resources))) {}
PagOverlayRenderer::~PagOverlayRenderer() = default;

RenderRequest PagOverlayRenderer::Prepare(const TimelineFrame& frame, const ResolveColor& resolve_color,
    std::vector<ImageSurface> data, PagOverlayStats* stats, bool batch_overlays) {
  if (!frame.plan) throw std::invalid_argument("A timeline render plan is required");
  return impl_->Prepare(frame, resolve_color, std::move(data), stats, batch_overlays);
}

} // namespace huxerui::avkit
