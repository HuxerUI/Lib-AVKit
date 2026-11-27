#include "clip_sampling_system.h"

#include "ecs/scene_evaluation.h"

#include <huxerui/avkit/component.h>

#include <limits>
#include <set>

#include "support/geometry.h"
#include "support/video_geometry.h"

namespace huxerui::avkit::detail {
namespace {

using media::VideoTextureOptions;

Affine2D DrawToSource(const SourceCrop& crop) {
  auto transform = UvRect(crop.area);
  if (crop.mirrored) {
    transform.translate_x += transform.m11;
    transform.m11 = -transform.m11;
  }
  return transform;
}

} // namespace

void ClipSamplingSystem::update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) {
  const auto canvas = SceneRoot(entities).component<VideoFrameContext>()->target.size;
  ClearComponents<ClipSourceMapping>(entities);
  ClearComponents<FittedVideoTransform>(entities);
  for (auto image : entities.entities_with_components<ProjectImageSource>())
    if (image.has_component<render::SourceSampling>()) image.remove<render::SourceSampling>();
  std::set<entityx::Entity> analysis_sources;
  for (auto entity : entities.entities_with_components<ClipAttachment>()) {
    if (entity.has_component<TrackedSticker>() || entity.has_component<StickerDepth>())
      analysis_sources.insert(entity.component<ClipAttachment>()->clip);
  }
  entities.each<ClipSampleTime, ProjectImageSource, VideoLayout, Transform2D>([&](entityx::Entity entity,
      const ClipSampleTime&, const ProjectImageSource& image, const VideoLayout& layout, const Transform2D& transform) {
    const auto crop = entity.component<SourceCrop>();
    auto draw_to_source = crop ? DrawToSource(*crop) : Affine2D{};
    const auto region = crop ? crop->area : Rect{0, 0, 1, 1};
    const Size source{image.size.width * region.width, image.size.height * region.height};
    const auto bounds = Rect{transform.translation.x, transform.translation.y, transform.scale.width, transform.scale.height};
    if (!layout.fill || layout.match_source_aspect) {
      const auto fitted = FitCanvasAspect(bounds, canvas, source);
      entity.assign<FittedVideoTransform>(Transform2D{{fitted.x, fitted.y}, {fitted.width, fitted.height},
                                                       transform.rotation_degrees});
    } else {
      const auto destination_aspect = (canvas.width * bounds.width) / (canvas.height * bounds.height);
      const auto source_aspect = source.width / source.height;
      const auto width = std::min(1.0F, destination_aspect / source_aspect);
      const auto height = std::min(1.0F, source_aspect / destination_aspect);
      draw_to_source = Compose(draw_to_source, UvRect({(1 - width) / 2, (1 - height) / 2, width, height}));
    }
    entity.assign<ClipSourceMapping>(draw_to_source, InverseAffine(draw_to_source));
    entity.replace<render::SourceSampling>(render::SourceSampling{draw_to_source});
  });
  const auto dimension = [](std::uint32_t canvas_size, float fraction) {
    return static_cast<std::uint32_t>(std::clamp(double(canvas_size) * fraction, 1.0,
        double(std::numeric_limits<std::uint32_t>::max())));
  };
  for (auto entity : entities.entities_with_components<SampledVideo, VideoLayout, Transform2D>()) {
    const auto& picture = *entity.component<SampledVideo>();
    if (!picture.picture) continue;
    const auto& layout = *entity.component<VideoLayout>();
    auto bounds = *entity.component<Transform2D>();
    if (layout.match_source_aspect) {
      const auto [width, height] = VideoDisplaySize(picture.picture->format);
      const auto fit = FitCanvasAspect({bounds.translation.x, bounds.translation.y, bounds.scale.width, bounds.scale.height},
                                       canvas, {static_cast<float>(width), static_cast<float>(height)});
      bounds.translation = {fit.x, fit.y};
      bounds.scale = {fit.width, fit.height};
      entity.assign<FittedVideoTransform>(bounds);
    }
    const auto source_crop = entity.component<SourceCrop>();
    const bool fill = layout.fill || layout.match_source_aspect;
    const bool samples_draw = source_crop || fill || entity.has_component<BackgroundReplacement>();
    if (!samples_draw) {
      if (entity.has_component<render::SourceSampling>()) entity.remove<render::SourceSampling>();
      if (!entity.has_component<SourceGeometryRequired>() && !analysis_sources.contains(entity)) continue;
    }
    const auto& content = picture.source_to_texture_uv;
    auto draw_to_source = source_crop ? DrawToSource(*source_crop) : Affine2D{};
    if (fill) {
      const VideoTextureOptions cover{dimension(canvas.width, bounds.scale.width),
                                      dimension(canvas.height, bounds.scale.height), ImageFit::Cover};
      draw_to_source = Compose(draw_to_source, InverseAffine(FitVideoTexture(picture.picture->format, cover).SourceToTextureUv(cover)));
    }
    if (samples_draw) {
      entity.assign<ClipSourceMapping>(draw_to_source, InverseAffine(draw_to_source));
      entity.replace<render::SourceSampling>(render::SourceSampling{Compose(content, draw_to_source)});
    } else {
      entity.assign<ClipSourceMapping>(InverseAffine(content), content);
    }
  }
}

} // namespace huxerui::avkit::detail
