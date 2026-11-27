#include "pag_asset.h"
#include "project/resource_internal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace huxerui::avkit {
namespace {

constexpr auto kFontStyle = "Regular";

bool SupportsDimensions(int width, int height) {
  return width > 0 && height > 0 && width <= 8192 && height <= 8192;
}

std::shared_ptr<pag::PAGFile> OpenComposition(const ResourceCatalog& catalog, const ResourceRef& resource) {
  auto composition = detail::LoadPagFile(catalog, resource);
  if (!composition || !std::isfinite(composition->frameRate()) || composition->frameRate() <= 0 ||
      !SupportsDimensions(composition->width(), composition->height()))
    throw std::invalid_argument("Invalid PAG composition or texture dimensions");
  return composition;
}

template<class Replacement>
void ValidateEditableIndices(pag::PAGFile& file, pag::LayerType type, std::span<const Replacement> replacements) {
  if (replacements.empty()) return;
  const auto editable = file.getEditableIndices(type);
  std::unordered_set<int> seen;
  for (const auto& value : replacements) {
    if (std::ranges::find(editable, value.editable_index) == editable.end() || !seen.insert(value.editable_index).second)
      throw std::invalid_argument("PAG replacement needs a unique editable layer index");
  }
}

} // namespace

std::shared_ptr<pag::PAGFile> detail::LoadPagFile(const ResourceCatalog& catalog, const ResourceRef& resource) {
  if (const auto file = catalog.FindLocalFile(resource)) return pag::PAGFile::Load(file->Path());
  const auto bytes = ReadResourceBytes(catalog, resource);
  return pag::PAGFile::Load(bytes.data(), bytes.size());
}

detail::PagFont::PagFont(const ResourceCatalog& catalog, const ResourceRef& source) {
  static std::atomic_uint64_t next_alias{1};
  family_ = "AVKit PAG " + std::to_string(next_alias.fetch_add(1, std::memory_order_relaxed));
  const auto register_face = [&] {
    if (const auto file = catalog.FindLocalFile(source))
      return pag::PAGFont::RegisterFont(file->Path(), 0, family_, kFontStyle);
    packaged_data_ = ReadResourceBytes(catalog, source);
    if (packaged_data_.empty()) throw std::invalid_argument("PAG font is empty");
    return pag::PAGFont::RegisterFont(packaged_data_.data(), packaged_data_.size(), 0, family_, kFontStyle);
  };
  if (register_face().fontFamily.empty())
    throw std::invalid_argument("Cannot register the PAG font");
}

detail::PagFont::~PagFont() { pag::PAGFont::UnregisterFont({family_, kFontStyle}); }

namespace {

std::shared_ptr<const detail::PagFont> ResolveFont(const ResourceCatalog& resources, detail::PagFontCache& fonts,
                                                  const ResourceRef& reference) {
  auto& cached = fonts[reference];
  if (auto font = cached.lock()) return font;
  auto font = std::make_shared<const detail::PagFont>(resources, reference);
  cached = font;
  return font;
}

} // namespace

detail::ResolvedPagAsset detail::ResolvePagAsset(const PagOverlay& source, const ResourceCatalog& resources,
                                                  PagFontCache& fonts, const PagEditableIndices* slots) {
  const auto& asset = resources.Get<PagAsset>(source.resource);
  ResolvedPagAsset result{source.resource};
  const auto font = asset.font ? ResolveFont(resources, fonts, *asset.font) : nullptr;
  if (font) {
    for (const auto& slot : asset.text_slots) result.texts.push_back({slot.editable_index, std::nullopt, font});
  }
  const auto bound = slots ? std::nullopt : std::optional{ResolvePagIndices(asset, source.replacements)};
  const auto& editable = slots ? *slots : *bound;
  for (std::size_t i = 0; i < source.replacements.texts.size(); ++i) {
    const auto& text = source.replacements.texts[i];
    const auto index = editable.text.at(i);
    IndexedPagTextReplacement value{index, text.text, text.font ? ResolveFont(resources, fonts, *text.font) : font,
                                    text.size_scale, text.foreground, text.background};
    if (const auto replacement = std::ranges::find(result.texts, index, &IndexedPagTextReplacement::editable_index);
        replacement != result.texts.end()) *replacement = std::move(value);
    else result.texts.push_back(std::move(value));
  }
  if (asset.fit_text_to_canvas && result.texts.empty()) result.texts.push_back({asset.text_slots.front().editable_index});
  for (std::size_t i = 0; i < source.replacements.images.size(); ++i) {
    const auto& image = source.replacements.images[i];
    const auto index = editable.image.at(i);
    result.images.push_back({index, image.image});
  }
  return result;
}

void detail::ApplyPagTextReplacement(pag::TextDocument& document, const IndexedPagTextReplacement& value) {
  const auto byte = [](float component) { return static_cast<std::uint8_t>(std::lround(std::clamp(component, 0.F, 1.F) * 255)); };
  const auto color = [&](Color paint) { return pag::Color{byte(paint.red), byte(paint.green), byte(paint.blue)}; };
  if (value.text) document.text = *value.text;
  if (value.font) {
    document.fontFamily = value.font->Family();
    document.fontStyle = kFontStyle;
  }
  if (value.size_scale) document.fontSize *= *value.size_scale;
  if (value.foreground) {
    document.applyFill = true;
    document.fillColor = color(*value.foreground);
  }
  if (value.background) {
    document.backgroundColor = color(*value.background);
    document.backgroundAlpha = byte(value.background->alpha);
  }
}

std::shared_ptr<pag::PAGFile> detail::OpenPagAnimation(const ResourceCatalog& catalog, const ResolvedPagAsset& asset) {
  const auto composition = OpenComposition(catalog, asset.resource);
  ValidateEditableIndices(*composition, pag::LayerType::Text, std::span(asset.texts));
  ValidateEditableIndices(*composition, pag::LayerType::Image, std::span(asset.images));
  for (const auto& value : asset.texts) {
    if (value.text || value.font || value.size_scale || value.foreground || value.background) {
      auto document = composition->getTextData(value.editable_index);
      if (!document) throw std::invalid_argument("Cannot open the editable PAG text document");
      detail::ApplyPagTextReplacement(*document, value);
      composition->replaceText(value.editable_index, std::move(document));
    }
    if (value.foreground && value.foreground->alpha != 1) {
      for (const auto& layer : composition->getLayersByEditableIndex(value.editable_index, pag::LayerType::Text))
        layer->setAlpha(layer->alpha() * value.foreground->alpha);
    }
  }
  for (const auto& value : asset.images) {
    auto image = [&] {
      if (const auto path = catalog.FindLocalFile(value.resource)) return pag::PAGImage::FromPath(path->Path());
      const auto bytes = ReadResourceBytes(catalog, value.resource);
      return pag::PAGImage::FromBytes(bytes.data(), bytes.size());
    }();
    if (!image) throw std::invalid_argument("Cannot decode the PAG image replacement");
    composition->replaceImage(value.editable_index, std::move(image));
  }
  return composition;
}

} // namespace huxerui::avkit
