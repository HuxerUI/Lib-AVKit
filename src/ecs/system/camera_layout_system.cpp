#include "camera_layout_system.h"

#include "ecs/scene_evaluation.h"

#include <algorithm>
#include <stdexcept>

#include <huxerui/avkit/component.h>

namespace huxerui::avkit::detail {

void CameraLayoutSystem::update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) {
  const auto target = SceneRoot(entities).component<VideoFrameContext>()->target.size;
  entities.each<CameraFrameGeometry, render::Texture>([&](entityx::Entity layer, const CameraFrameGeometry& input,
                                                             const render::Texture& image) {
    const auto source = image.Info().size;
    const auto crop = input.crop.Intersection({0, 0, 1, 1});
    if (crop.IsEmpty()) throw std::invalid_argument("Camera frame has no visible area");
    float width = crop.width * source.width, height = crop.height * source.height;
    using enum media::VideoRotation;
    if (input.rotation == Clockwise90 || input.rotation == Clockwise270) std::swap(width, height);
    const auto scale = std::min(float(target.width) / width, float(target.height) / height);
    const float fitted_width = width * scale / target.width, fitted_height = height * scale / target.height;
    const auto source_point = [&](float x, float y) {
      if (input.mirrored) x = 1 - x;
      Point point{x, y};
      switch (input.rotation) {
        case None: break;
        case Clockwise90: point = {y, 1 - x}; break;
        case Clockwise180: point = {1 - x, 1 - y}; break;
        case Clockwise270: point = {1 - y, x}; break;
      }
      return Point{crop.x + point.x * crop.width, crop.y + point.y * crop.height};
    };
    const auto origin = source_point(0, 0);
    const auto horizontal = source_point(1, 0) - origin;
    const auto vertical = source_point(0, 1) - origin;
    layer.replace<Transform2D>(Transform2D{
        {(1 - fitted_width) * 0.5F, (1 - fitted_height) * 0.5F}, {fitted_width, fitted_height}});
    layer.replace<render::SourceSampling>(render::SourceSampling{
        {horizontal.x, horizontal.y, vertical.x, vertical.y, origin.x, origin.y}});
  });
}

} // namespace huxerui::avkit::detail
