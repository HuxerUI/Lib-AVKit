#include "timeline_resources.h"

#include "preview_sticker.h"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <utility>

#include <bimg/decode.h>
#include <bx/allocator.h>
#include <huxerui/avkit/bgfx.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using namespace huxerui::media;

constexpr MediaTimestamp kClipMaskFrameInterval{5, 48};

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

PagOverlayResources TimelinePagResources(const OverlayAssets& assets) {
  auto resources = WorkbenchPagResources(assets);
  resources.clip_animation = [assets](const auto& resource, const auto& replacements) {
    return ResolvePreviewPagSticker(assets, resource, replacements);
  };
  return resources;
}

ImageSurface LoadMask(const BufferReference& asset) {
  bx::DefaultAllocator allocator;
  const auto bytes = asset.AsBytes();
  // The PNG decoder preserves grayscale samples, which are coverage values rather than sRGB colors.
  const std::unique_ptr<bimg::ImageContainer, decltype(&bimg::imageFree)> image(
      bimg::imageParse(&allocator, bytes.data(), static_cast<std::uint32_t>(bytes.size())), bimg::imageFree);
  Check(image && image->m_parser == bimg::ImageParser::Png &&
        image->m_format == bimg::TextureFormat::R8 && !image->m_hasAlpha, "Expected an 8-bit grayscale PNG mask");
  const PixelSize size{image->m_width, image->m_height};
  const auto* coverage = static_cast<const std::byte*>(image->m_data);
  auto pixels = std::make_shared<Bytes>(coverage, coverage + std::size_t(size.width) * size.height);
  return RetainBgfxPixels({size, PixelFormat::R8Unorm}, BufferReference(*pixels, pixels), size.width);
}

template<std::size_t N>
MaskSequence ResolveMaskSequence(std::array<BufferReference, N> frames, MediaTimestamp interval = {}) {
  return {N, interval, [frames = std::move(frames)](std::uint32_t index) { return LoadMask(frames.at(index)); }};
}

ColorSurface CreateSticker(std::optional<analysis::TrackedPart> part) {
  auto pixels = std::make_shared<std::vector<PreviewPixel>>();
  pixels->reserve(kPreviewStickerSize.width * kPreviewStickerSize.height);
  for (int y = 0; y < static_cast<int>(kPreviewStickerSize.height); ++y) {
    for (int x = 0; x < static_cast<int>(kPreviewStickerSize.width); ++x) {
      pixels->push_back(part ? PreviewTrackingStickerPixel(x, y, *part) : PreviewStickerPixel(x, y));
    }
  }
  return ColorSurface{RetainBgfxPixels({kPreviewStickerSize, PixelFormat::Rgba8Unorm},
      BufferReference(std::as_bytes(std::span(*pixels)), pixels), kPreviewStickerSize.width * sizeof(PreviewPixel))};
}

} // namespace

TimelineRenderResources PreviewTimelineResources(const OverlayAssets& assets) {
  return {
    .overlays = TimelinePagResources(assets),
    .sticker = [](const ResourceRef& resource) {
      if (resource == PreviewStickerResource()) return CreateSticker({});
      for (const auto part : kPreviewTrackingParts) {
        if (resource == TrackingStickerResource(part)) return CreateSticker(part);
      }
      throw std::runtime_error("The preview sticker resource is unavailable: " + resource.key);
    },
    .static_mask = [frames = assets.clip_masks](const ResourceRef& resource) {
      const auto& references = PreviewStaticMaskResources();
      const auto found = std::ranges::find(references, resource);
      if (found == references.end()) throw std::runtime_error("The static mask resource is unavailable: " + resource.key);
      return LoadMask(frames[found - references.begin()]);
    },
    .mask_sequence = [clip = assets.clip_masks, wipe = assets.wipe_masks](const ResourceRef& resource) {
      if (resource == PreviewAnimatedMaskResource()) return ResolveMaskSequence(clip, kClipMaskFrameInterval);
      if (resource == PreviewWipeResource()) return ResolveMaskSequence(wipe);
      throw std::runtime_error("The mask sequence is unavailable: " + resource.key);
    },
    .pag_transition = [encoded = assets.transition](const ResourceRef& resource) {
      if (resource != PreviewPagTransitionResource())
        throw std::runtime_error("The PAG transition resource is unavailable: " + resource.key);
      return PagTransitionAsset{encoded, 1, 0};
    },
  };
}

MovieCodecProviders TimelineProviders() { return {CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()}; }

VideoEncoderConfig PreviewVideoConfig(PixelSize size) { return FileExportSettings::Sdr(size).video; }

const ResourceRef& PreviewStickerResource() {
  static const ResourceRef resource{"avkit.preview", "1", "sticker"};
  return resource;
}

const ResourceRef& TrackingStickerResource(analysis::TrackedPart part) {
  static const std::array<ResourceRef, kPreviewTrackingParts.size()> resources{{
      {"avkit.preview", "1", "glasses"}, {"avkit.preview", "1", "left_hand"}, {"avkit.preview", "1", "right_hand"}}};
  const auto item = std::ranges::find(kPreviewTrackingParts, part);
  Check(item != kPreviewTrackingParts.end(), "Unknown preview tracking sticker");
  return resources[item - kPreviewTrackingParts.begin()];
}

const ResourceRef& PreviewPagStickerResource() {
  static const ResourceRef resource{"avkit.preview", "1", "pag-like"};
  return resource;
}

const ResourceRef& PreviewEditablePagResource() {
  static const ResourceRef resource{"avkit.preview", "1", "pag-editable"};
  return resource;
}

const ResourceRef& PreviewPagImageResource() {
  static const ResourceRef resource{"avkit.preview", "1", "pag-replacement-image"};
  return resource;
}

const ResourceRef& PreviewPagTransitionResource() {
  static const ResourceRef resource{"avkit.preview", "1", "pag-crossfade"};
  return resource;
}

PagOverlayAsset ResolvePreviewPagSticker(const OverlayAssets& assets, const ResourceRef& resource,
                                       const StickerReplacements& replacements) {
  if (resource == PreviewPagStickerResource()) {
    Check(replacements.Empty(), "The like PAG fixture has no editable slots");
    return {assets.stickers.front()};
  }
  if (resource != PreviewEditablePagResource())
    throw std::runtime_error("The animated sticker resource is unavailable: " + resource.key);
  const auto font = std::make_shared<const PagFont>(assets.font);
  std::array<PagTextReplacement, 1> texts{{{0, std::nullopt, font}}};
  std::vector<PagImageReplacement> images;
  for (const auto& text : replacements.texts) {
    Check(text.slot == "title", "Unknown editable PAG text slot");
    texts.front().text = text.text;
  }
  for (const auto& image : replacements.images) {
    Check(image.slot == "icon", "Unknown editable PAG image slot");
    Check(image.image == PreviewPagImageResource(), "The PAG replacement image is unavailable");
    images.push_back({0, assets.replacement_image});
  }
  return {assets.editable, {texts.begin(), texts.end()}, std::move(images)};
}

const ResourceRef& PreviewWipeResource() {
  static const ResourceRef resource{"avkit.preview", "1", "wipe"};
  return resource;
}

const std::array<ResourceRef, 2>& PreviewStaticMaskResources() {
  static const std::array<ResourceRef, 2> resources{{
      {"avkit.preview", "1", "static-window"}, {"avkit.preview", "1", "static-offset"},
  }};
  return resources;
}

const ResourceRef& PreviewAnimatedMaskResource() {
  static const ResourceRef resource{"avkit.preview", "1", "clip-mask-sequence"};
  return resource;
}

} // namespace avkit_preview
