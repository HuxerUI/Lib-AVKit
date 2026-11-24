#include <huxerui/avkit/pag.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pag/pag.h>

#include "animation/pag_frame_renderer.h"

namespace huxerui::avkit {
namespace {

constexpr auto kFontStyle = "Regular";

bool SupportsDimensions(int width, int height) {
  return width > 0 && height > 0 && width <= 8192 && height <= 8192;
}

std::shared_ptr<pag::PAGFile> OpenComposition(const BufferReference& encoded) {
  const auto data = encoded.AsBytes();
  auto composition = pag::PAGFile::Load(data.data(), data.size());
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

PagFont::PagFont(BufferReference encoded) : encoded_(std::move(encoded)) {
  static std::atomic_uint64_t next_alias{1};
  family_ = "AVKit PAG " + std::to_string(next_alias.fetch_add(1, std::memory_order_relaxed));
  const auto bytes = encoded_.AsBytes();
  if (bytes.empty() || pag::PAGFont::RegisterFont(bytes.data(), bytes.size(), 0, family_, kFontStyle).fontFamily.empty())
    throw std::invalid_argument("Cannot register the PAG font");
}

PagFont::~PagFont() { pag::PAGFont::UnregisterFont({family_, kFontStyle}); }

std::shared_ptr<pag::PAGFile> detail::OpenPagAnimation(const BufferReference& encoded,
    std::span<const PagTextReplacement> texts, std::span<const PagImageReplacement> images) {
  const auto composition = OpenComposition(encoded);
  ValidateEditableIndices(*composition, pag::LayerType::Text, texts);
  ValidateEditableIndices(*composition, pag::LayerType::Image, images);
  for (const auto& value : texts) {
    auto document = composition->getTextData(value.editable_index);
    if (!document) throw std::invalid_argument("Cannot open the editable PAG text document");
    if (value.text) document->text = *value.text;
    if (value.font) {
      document->fontFamily = value.font->Family();
      document->fontStyle = kFontStyle;
    }
    if (value.text || value.font) composition->replaceText(value.editable_index, std::move(document));
  }
  for (const auto& value : images) {
    const auto pixels = value.encoded.AsBytes();
    if (pixels.empty()) throw std::invalid_argument("PAG image replacement is empty");
    auto image = pag::PAGImage::FromBytes(pixels.data(), pixels.size());
    if (!image) throw std::invalid_argument("Cannot decode the PAG image replacement");
    composition->replaceImage(value.editable_index, std::move(image));
  }
  return composition;
}

struct PagAnimation::Impl {
  Impl(BufferReference bytes, std::span<const PagTextReplacement> texts, std::span<const PagImageReplacement> images,
       render::RenderBackend* backend)
      : asset{std::move(bytes), {texts.begin(), texts.end()}, {images.begin(), images.end()}},
        renderer(detail::OpenPagAnimation(asset.encoded, asset.texts, asset.images), backend) {}

  PagOverlayAsset asset;
  detail::PagFrameRenderer renderer;
};

PagAnimation::PagAnimation(BufferReference encoded, std::span<const PagTextReplacement> texts,
                           std::span<const PagImageReplacement> images, render::RenderBackend* backend)
    : impl_(std::make_unique<Impl>(std::move(encoded), texts, images, backend)) {}
PagAnimation::~PagAnimation() = default;

render::ColorSurface PagAnimation::Sample(media::MediaTimestamp elapsed, AnimationPlayback playback) {
  return impl_->renderer.Sample(elapsed, playback);
}

} // namespace huxerui::avkit
