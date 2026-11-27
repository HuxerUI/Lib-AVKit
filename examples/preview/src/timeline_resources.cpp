#include "timeline_resources.h"

#include <array>
#include <utility>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::media;

constexpr MediaTimestamp kClipMaskFrameInterval{5, 48};

} // namespace

std::shared_ptr<const ResourceCatalog> LoadWorkbenchResources(const Resources& resources) {
  ResourceCatalog catalog;
  const auto add_template = [&](std::string key, PixelSize canvas, std::int64_t seconds, bool inset) {
    ResourceRef reference{std::move(key)};
    const TemplateClip primary{.slot = "主镜头", .source = {{}, {seconds, 1}}, .include_original_audio = true};
    TemplateClip second{.slot = "第二镜头", .source = {{}, {seconds, 1}}, .include_original_audio = !inset};
    EditTemplateRecipe recipe{reference, canvas, {primary.slot, second.slot}};
    recipe.placement = inset ? TemplatePlacement::Parallel : TemplatePlacement::Sequence;
    if (inset) {
      second.destination = {0.66F, 0.63F, 0.30F, 0.30F};
      recipe.tracks = {{TrackKind::Video, {primary}}, {TrackKind::Video, {second}}};
    } else {
      recipe.tracks = {{TrackKind::Video, {primary, second}}};
    }
    catalog.Register(std::move(reference), std::move(recipe));
  };
  add_template("stage-cut", {640, 360}, 6, false);
  add_template("portrait", {360, 640}, 5, false);
  add_template("picture-in-picture", {640, 360}, 6, true);
  const auto raw = [&](RawResource resource) { return resources.GetRawResource(resource); };
  for (const auto& sticker : kWorkbenchStickers) {
    catalog.Register({sticker.key}, raw(sticker.pag), PagAsset{});
  }
  const ResourceRef font{"preview-sans"};
  catalog.Register(font, raw(app::raw::stickers_preview_sans_ttf));
  catalog.Register(kPreviewPointTextResource, raw(app::raw::stickers_point_text_pag), PagAsset{
      .text_slots = {{kPreviewPointTextSlot, 0}}, .fit_text_to_canvas = true});
  catalog.Register({"pag-editable"}, raw(app::raw::stickers_editable_pag), PagAsset{
      .text_slots = {{"title", 0}}, .image_slots = {{"icon", 0}}, .font = font});
  catalog.Register({"pag-crossfade"}, raw(app::raw::transitions_crossfade_pag), PagAsset{
      .image_slots = {{"outgoing", 1}, {"incoming", 0}},
      .transition_slots = std::array<std::string, 2>{"outgoing", "incoming"}});
  catalog.Register({"pag-replacement-image"}, raw(app::raw::stickers_replacement_png));

  const ResourceRef window{"static-window"};
  const ResourceRef offset{"static-offset"};
  const ResourceRef corner{"sequence-corner"};
  catalog.Register(window, raw(app::raw::masks_static_window_png));
  catalog.Register(offset, raw(app::raw::masks_static_offset_png));
  catalog.Register(corner, raw(app::raw::masks_sequence_corner_png));
  catalog.Register({"clip-mask-sequence"}, MaskSequenceData{{{window}, {offset}, {corner}}, kClipMaskFrameInterval});

  const std::array wipe{app::raw::masks_wipe_0_png, app::raw::masks_wipe_1_png, app::raw::masks_wipe_2_png,
                        app::raw::masks_wipe_3_png, app::raw::masks_wipe_4_png, app::raw::masks_wipe_5_png,
                        app::raw::masks_wipe_6_png, app::raw::masks_wipe_7_png};
  MaskSequenceData frames;
  for (std::size_t index = 0; index < wipe.size(); ++index) {
    const ResourceRef reference{"wipe/" + std::to_string(index)};
    catalog.Register(reference, raw(wipe[index]));
    frames.frames.push_back({reference});
  }
  catalog.Register({"wipe"}, std::move(frames));
  return std::make_shared<const ResourceCatalog>(std::move(catalog));
}

CodecProviders TimelineProviders() { return {CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()}; }

} // namespace avkit_preview
