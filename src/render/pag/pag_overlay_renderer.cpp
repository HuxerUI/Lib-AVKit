#include "pag_overlay_renderer.h"

#include "pag_asset.h"
#include "pag_frame_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <map>
#include <utility>

namespace huxerui::avkit {
namespace {

using namespace render;
using render::detail::Draw;
using render::detail::PassRequest;
using render::detail::PassImageSlot;
using render::detail::RenderPasses;

auto Byte(float value) { return static_cast<std::uint8_t>(std::lround(value * 255)); }

pag::Color PagColor(Color color) { return {Byte(color.red), Byte(color.green), Byte(color.blue)}; }

struct PointText {
  std::shared_ptr<pag::PAGComposition> composition;
  Rect artwork{0, 0, 1, 1};
};

PointText PreparePointText(const ResourceCatalog& catalog, const avkit::detail::ResolvedPagAsset& asset,
                           Size extent, PixelSize canvas) {
  const auto output_width = extent.width * canvas.width;
  const auto output_height = extent.height * canvas.height;
  if (!(output_width > 0 && output_height > 0 && std::isfinite(output_width) && std::isfinite(output_height)))
    throw std::invalid_argument("Point text needs a finite, positive layout extent");
  // Lay out the glyphs using the overlay's size in the current render target.
  const auto scale = std::min({1.0F, 8192.0F / output_width, 8192.0F / output_height});
  const auto width = static_cast<int>(std::max(8.0F, std::ceil(output_width * scale)));
  const auto height = static_cast<int>(std::max(8.0F, std::ceil(output_height * scale)));
  auto comp = pag::PAGComposition::Make(width, height);
  if (!comp) throw std::runtime_error("Cannot create the text composition");
  const auto& replacement = asset.texts.front();
  const auto master = avkit::detail::LoadPagFile(catalog, asset.resource);
  auto style = master ? master->getTextData(replacement.editable_index) : nullptr;
  if (!style) throw std::runtime_error("Cannot open the point text master");
  style->fontSize = height * 0.46F;
  style->applyFill = true;
  style->applyStroke = false;
  style->fillColor = PagColor(Color::White());
  avkit::detail::ApplyPagTextReplacement(*style, replacement);
  const auto background_alpha = style->backgroundAlpha;
  if (background_alpha > 0) {
    auto background = pag::PAGSolidLayer::Make(1'000'000, width, height, style->backgroundColor, background_alpha);
    if (!background || !comp->addLayer(background)) throw std::runtime_error("Cannot create the text background");
    style->backgroundAlpha = 0;
  }
  auto layer = pag::PAGTextLayer::Make(1'000'000, std::move(style));
  if (!layer || !comp->addLayer(layer)) throw std::runtime_error("Cannot create the text overlay");
  if (replacement.foreground) layer->setAlpha(replacement.foreground->alpha);
  const auto bounds = layer->getTotalMatrix().mapRect(layer->getBounds());
  PointText result{comp};
  if (bounds.width() > 0 && bounds.height() > 0) {
    const auto fit = std::min({1.0F, width * 0.94F / bounds.width(), height * 0.9F / bounds.height()});
    auto transform = pag::Matrix::MakeScale(fit, fit);
    transform.postTranslate(width * 0.5F - fit * (bounds.left + bounds.right) * 0.5F,
                            height * 0.5F - fit * (bounds.top + bounds.bottom) * 0.5F);
    layer->setMatrix(transform);
    if (background_alpha == 0) {
      const auto w = std::min(1.0F, (bounds.width() * fit + 2.0F * scale) / width);
      const auto h = std::min(1.0F, (bounds.height() * fit + 2.0F * scale) / height);
      result.artwork = {(1.0F - w) * 0.5F, (1.0F - h) * 0.5F, w, h};
    }
  }
  return result;
}

struct OverlayEntry {
  avkit::detail::ResolvedPagAsset asset;
  std::unique_ptr<avkit::detail::PagFrameRenderer> renderer;
  std::shared_ptr<pag::PAGComposition> point_text;
  Rect artwork{0, 0, 1, 1};
  PagOverlay source;
  std::optional<Size> layout_extent;
  std::uint64_t last_used{};
};

struct OverlayInput {
  std::shared_ptr<OverlayEntry> entry;
  media::MediaTimestamp elapsed;
};

class OverlayBatch {
public:
  OverlayBatch(const ResourceCatalog& catalog, std::span<const OverlayInput> sources, PixelSize size) : size_(size) {
    composition_ = pag::PAGComposition::Make(size.width, size.height);
    bool compatible = bool(composition_);
    for (const auto& input : sources) {
      auto source = input.entry->point_text;
      double rate = 0;
      int count = 0;
      if (!source) {
        const auto& asset = input.entry->asset;
        source = avkit::detail::OpenPagAnimation(catalog, asset);
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

  Texture Sample(std::span<const OverlayInput> inputs, std::span<const Draw> draws,
                      RenderBackend& backend) {
    if (!composition_) return {};
    try {
      if (!target_) {
        target_ = avkit::detail::CreatePagGpuTarget(backend, size_);
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
        if (!layer.draw || draw.local_to_target != layer.draw->local_to_target || draw.opacity != layer.draw->opacity) {
          const auto& map = draw.local_to_target;
          const auto sx = float(size_.width) / layer.source->width();
          const auto sy = float(size_.height) / layer.source->height();
          layer.placed->setMatrix(pag::Matrix::MakeAll(
              map.m11 * sx, map.m21 * size_.width / layer.source->height(), map.translate_x * size_.width,
              map.m12 * size_.height / layer.source->width(), map.m22 * sy, map.translate_y * size_.height, 0, 0, 1));
          layer.placed->setAlpha(draw.opacity);
          layer.draw = draw;
          changed = true;
        }
        if (layer.count == 0) continue;
        const auto index = avkit::detail::PagFrameIndex(inputs[i].elapsed, inputs[i].entry->source.playback, layer.rate, layer.count);
        if (index == layer.index) continue;
        layer.source->setProgress((index + 0.5) / layer.count);
        layer.index = index;
        changed = true;
      }
      if (changed || !surface_) {
        surface_ = {};
        surface_ = target_->Draw(*player_);
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
  std::unique_ptr<avkit::detail::PagGpuTarget> target_;
  std::unique_ptr<pag::PAGPlayer> player_;
  Texture surface_;
};

} // namespace

struct avkit::detail::PagOverlayRenderer::Impl {
  Impl(RenderBackend& target, const ResourceCatalog* assets) : backend(target), resources(assets) {}

  OverlayInput Resolve(const PagLayerSample& sample, std::size_t slot) {
    if (!resources) throw std::invalid_argument("A PAG node needs the render plan's resource catalog");
    auto& entry = entries[{sample.instance.has_value(), sample.instance ? sample.instance->value : slot}];
    if (!entry || entry->source != sample.source || entry->layout_extent != sample.layout_extent) {
      auto next = std::make_shared<OverlayEntry>();
      next->source = sample.source;
      next->layout_extent = sample.layout_extent;
      next->asset = avkit::detail::ResolvePagAsset(next->source, *resources, fonts,
                                                   sample.slots ? &*sample.slots : nullptr);
      if (next->layout_extent) {
        auto prepared = PreparePointText(*resources, next->asset, *next->layout_extent, canvas);
        next->point_text = std::move(prepared.composition);
        next->artwork = prepared.artwork;
      }
      entry = std::move(next);
    }
    entry->last_used = frame_number;
    return {entry, sample.elapsed};
  }

  Texture Sample(const OverlayInput& input) {
    auto& entry = *input.entry;
    if (!entry.renderer) {
      auto composition = entry.point_text;
      if (!composition) composition = avkit::detail::OpenPagAnimation(*resources, entry.asset);
      entry.renderer = std::make_unique<avkit::detail::PagFrameRenderer>(std::move(composition), backend);
    }
    return entry.renderer->Sample(entry.point_text ? media::MediaTimestamp{} : input.elapsed,
        entry.point_text ? AnimationPlayback::HoldLast : entry.source.playback);
  }

  PassRequest Prepare(RenderPasses passes, std::span<const RenderNode* const> images,
                      std::vector<Texture> inputs, std::vector<Texture> data, std::span<TargetLayoutEntry> layouts) {
    ++frame_number;
    if (canvas != passes.target.size) {
      canvas = passes.target.size;
      entries.clear();
      batches.clear();
    }
    const auto count = images.size();
    auto plan = std::make_shared<RenderPasses>(std::move(passes));
    std::vector<unsigned> uses(count);
    render::detail::VisitDrawLists(*plan, [&](std::span<const Draw> draws) {
      render::detail::VisitImageSlots(draws, [&](PassImageSlot slot) {
        if (slot.IsInput()) ++uses.at(slot.index);
      });
    });
    std::vector<OverlayInput> sources(count);
    for (std::size_t slot = 0; slot < count; ++slot) {
      if (!uses[slot]) continue;
      const auto* node = dynamic_cast<const PagNode*>(images[slot]);
      if (!node) continue;
      const auto& sample = *node->sample;
      sources[slot] = Resolve(sample, slot);
      if (!sample.instance || layouts.empty()) continue;
      const auto item = std::ranges::find(layouts, EditTarget::ForOverlay(*sample.instance), &TargetLayoutEntry::target);
      if (item != layouts.end()) item->layout.local_bounds = sources[slot].entry->artwork;
    }

    {
      const auto eligible = [&](const Draw& draw) {
        const auto slot = draw.image_slot;
        return slot.IsInput() && uses[slot.index] == 1 && sources[slot.index].entry &&
               !draw.masked_blend && !draw.source_sampling && !draw.background && !draw.depth_occlusion &&
               draw.blend == Blend::PremultipliedOver;
      };
      std::vector<OverlayInput> group;
      const auto collapse = [&](std::vector<Draw>& draws) {
        for (auto end = draws.size(); end;) {
          if (!eligible(draws[end - 1])) {
            --end;
            continue;
          }
          auto first = end - 1;
          while (first && eligible(draws[first - 1])) --first;
          const auto layers = std::span(draws).subspan(first, end - first);
          end = first;
          if (layers.size() == 1 && !sources[layers.front().image_slot.index].entry->layout_extent) continue;
          group.clear();
          for (const auto& draw : layers) group.push_back(sources[draw.image_slot.index]);
          const auto found = std::ranges::find_if(batches, [&](const auto& batch) { return batch->Matches(group); });
          auto& batch = *(found == batches.end() ? batches.emplace_back(std::make_unique<OverlayBatch>(*resources, group, canvas)) : *found);
          batch.last_used = frame_number;
          auto surface = batch.Sample(group, layers, backend);
          if (!surface) continue;
          const auto slot = layers.front().image_slot;
          inputs[slot.index] = std::move(surface);
          for (const auto& layer : layers.subspan(1)) uses[layer.image_slot.index] = 0;
          draws[first] = {slot, Blend::PremultipliedOver};
          draws.erase(draws.begin() + first + 1, draws.begin() + first + layers.size());
        }
      };
      for (auto& pass : plan->offscreen_passes) collapse(pass.draws);
      collapse(plan->final_draws);
    }
    for (std::size_t slot = 0; slot < count; ++slot) {
      if (uses[slot] && sources[slot].entry && !inputs[slot])
        inputs[slot] = Sample(sources[slot]);
    }
    const auto unused = [&](const auto& item) { return item.second->last_used != frame_number; };
    std::erase_if(entries, unused);
    std::erase_if(batches, [&](const auto& batch) { return batch->last_used != frame_number; });
    std::erase_if(fonts, [](const auto& entry) { return entry.second.expired(); });
    return {std::move(plan), std::move(inputs), std::move(data)};
  }

  RenderBackend& backend;
  const ResourceCatalog* resources;
  PixelSize canvas{};
  std::uint64_t frame_number{};
  PagFontCache fonts;
  std::map<std::pair<bool, std::uint64_t>, std::shared_ptr<OverlayEntry>> entries;
  std::vector<std::unique_ptr<OverlayBatch>> batches;
};

avkit::detail::PagOverlayRenderer::PagOverlayRenderer(RenderBackend& backend, const ResourceCatalog* resources)
    : impl_(std::make_unique<Impl>(backend, resources)) {}

avkit::detail::PagOverlayRenderer::~PagOverlayRenderer() = default;

PassRequest avkit::detail::PagOverlayRenderer::Prepare(RenderPasses passes,
    std::span<const RenderNode* const> images,
    std::vector<Texture> inputs, std::vector<Texture> data, std::span<TargetLayoutEntry> layouts) {
  return impl_->Prepare(std::move(passes), images, std::move(inputs), std::move(data), layouts);
}

} // namespace huxerui::avkit
