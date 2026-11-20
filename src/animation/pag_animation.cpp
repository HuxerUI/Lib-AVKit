#include <huxerui/avkit/pag.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pag/pag.h>

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

struct PagAnimation::Impl {
  Impl(BufferReference bytes, std::span<const PagTextReplacement> texts, std::span<const PagImageReplacement> images)
      : encoded(std::move(bytes)) {
    const auto composition = OpenComposition(encoded);
    ValidateEditableIndices(*composition, pag::LayerType::Text, texts);
    ValidateEditableIndices(*composition, pag::LayerType::Image, images);
    for (const auto& value : texts) {
      auto document = composition->getTextData(value.editable_index);
      if (!document) throw std::invalid_argument("Cannot open the editable PAG text document");
      if (value.text) document->text = *value.text;
      if (value.font) {
        document->fontFamily = value.font->family_;
        document->fontStyle = kFontStyle;
        fonts.push_back(value.font);
      }
      if (value.text || value.font) composition->replaceText(value.editable_index, std::move(document));
    }
    for (const auto& value : images) {
      const auto pixels = value.encoded.AsBytes();
      if (pixels.empty()) throw std::invalid_argument("PAG image replacement is empty");
      auto image = pag::PAGImage::FromBytes(pixels.data(), pixels.size());
      if (!image) throw std::invalid_argument("Cannot decode the PAG image replacement");
      composition->replaceImage(value.editable_index, std::move(image));
      image_bytes.push_back(value.encoded);
    }
    decoder = pag::PAGDecoder::MakeFrom(composition, composition->frameRate());
    if (!decoder) throw std::invalid_argument("Cannot open the PAG decoder");
    frames = decoder->numFrames();
    frame_rate = decoder->frameRate();
    if (frames <= 0 || !std::isfinite(frame_rate) || frame_rate <= 0)
      throw std::invalid_argument("Invalid PAG animation timing");
    image = {{static_cast<std::uint32_t>(decoder->width()), static_cast<std::uint32_t>(decoder->height())},
             render::PixelFormat::Rgba8Unorm};
  }

  BufferReference encoded;
  std::vector<std::shared_ptr<const PagFont>> fonts;
  std::vector<BufferReference> image_bytes;
  std::shared_ptr<pag::PAGDecoder> decoder;
  render::ImageDesc image;
  int frames;
  float frame_rate;
  int last_index{-1};
  render::ColorSurface last_surface;
};

PagAnimation::PagAnimation(BufferReference encoded, std::span<const PagTextReplacement> texts,
                           std::span<const PagImageReplacement> images)
    : impl_(std::make_unique<Impl>(std::move(encoded), texts, images)) {}
PagAnimation::~PagAnimation() = default;

render::ColorSurface PagAnimation::Sample(media::MediaTimestamp elapsed, AnimationPlayback playback) {
  if (elapsed.ticks < 0 || !elapsed.timescale ||
      (playback != AnimationPlayback::HoldLast && playback != AnimationPlayback::Loop))
    throw std::invalid_argument("Invalid animation sample time or playback mode");
  auto& state = *impl_;
  const auto frame = std::floor(static_cast<long double>(elapsed.ticks) * state.frame_rate / elapsed.timescale);
  const auto index = playback == AnimationPlayback::Loop ? static_cast<int>(std::fmod(frame, state.frames)) :
                     frame >= state.frames ? state.frames - 1 : static_cast<int>(frame);
  if (index == state.last_index) return state.last_surface;

  const auto stride = std::size_t(state.image.size.width) * 4;
  auto pixels = std::make_shared<Bytes>(stride * state.image.size.height);
  if (!state.decoder->readFrame(index, pixels->data(), stride, pag::ColorType::RGBA_8888, pag::AlphaType::Premultiplied))
    throw std::runtime_error("PAG could not rasterize the sampled animation frame");
  auto surface = render::RetainImagePixels(state.image, {BufferReference(*pixels, pixels), stride});
  state.last_surface = {std::move(surface), render::AlphaMode::Premultiplied};
  state.last_index = index;
  return state.last_surface;
}

} // namespace huxerui::avkit
