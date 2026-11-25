#include "sticker_scene.h"

#include <algorithm>
#include <stdexcept>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using media::MediaTimestamp;

constexpr float kStickerTravel = 3.0F / 16;

struct StickerMotion {
  float travel;
};

float MotionPhase(MediaTimestamp time) {
  auto remainder = time.ticks % time.timescale;
  if (remainder < 0) remainder += time.timescale;
  return float(2 * std::min(remainder, time.timescale - remainder)) / float(time.timescale);
}

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

} // namespace

void InstallStickerScene(SceneEvaluator& scene) {
  auto& world = scene.World();
  world.component<StickerMotion>();
  world.system<const StickerMotion, LocalTransform2D, const FrameContext>("SampleStickerMotion")
      .kind<EffectEvaluation>()
      .each([](const StickerMotion& motion, LocalTransform2D& transform, const FrameContext& frame) {
        transform.translation.x = motion.travel * MotionPhase(frame.time);
      });

  auto sticker = world.entity("Sticker").set<CompositeLayer>({1, 1, Blend::PremultipliedOver});
  auto anchor = world.entity("StickerAnchor").set<LocalTransform2D>({}).set<StickerMotion>({kStickerTravel});
  scene.SetSpatialParent(sticker, anchor);
  world.entity("Source").set<CompositeLayer>({0, 0, Blend::Replace});
}

huxerui::Point StickerCueCenter(MediaTimestamp time) {
  return {196.0F / 320 + kStickerTravel * MotionPhase(time), 80.0F / 180};
}

bool HasStickerCue(const media::VideoFrame& frame) {
  if (frame.format.pixel_format != media::VideoPixelFormat::I420 || !frame.Validate()) return false;
  const auto center = StickerCueCenter(frame.pts);
  const auto& visible = frame.format.visible;
  const auto x = visible.x + static_cast<std::uint32_t>(visible.width * center.x);
  const auto y = visible.y + static_cast<std::uint32_t>(visible.height * center.y);
  const auto channel = [&](std::size_t index, std::uint32_t px, std::uint32_t py) {
    const auto& plane = frame.planes[index];
    return std::to_integer<unsigned>(plane.data.AsBytes()[py * plane.row_stride + px * plane.pixel_stride]);
  };
  return channel(0, x, y) > 145 && channel(1, x / 2, y / 2) < 85 && channel(2, x / 2, y / 2) < 80;
}

void VerifyStickerScene() {
  SceneEvaluator preview, recording, export_scene;
  InstallStickerScene(preview);
  InstallStickerScene(recording);
  InstallStickerScene(export_scene);
  const ImageDesc target{{320, 180}, render::PixelFormat::Bgra8Unorm};
  const auto initial = preview.Evaluate({}, target);
  Check(initial->draws == std::vector<Draw>{{0, Blend::Replace}, {1, Blend::PremultipliedOver}},
        "ECS did not produce the initial layer order");

  const auto middle = preview.Evaluate({1, 2}, target);
  Check(middle->draws == std::vector<Draw>{{0, Blend::Replace}, {1, Blend::PremultipliedOver, {3.0F / 16, 0, 1, 1}}},
        "The external system did not move the parent's sticker at the requested PTS");
  Check(recording.Evaluate({22050, 44100}, target)->draws == middle->draws &&
            export_scene.Evaluate({6144, 12288}, target)->draws == middle->draws,
        "Independent worlds did not produce identical draws for equivalent media times");
  Check(preview.Evaluate({1, 4}, target)->draws.back().destination.x == 3.0F / 32 &&
            preview.Evaluate({}, target)->draws == initial->draws && middle->draws.back().destination.x == 3.0F / 16,
        "Backward sampling changed a previous plan or retained the prior effect position");

  auto& world = preview.World();
  const auto sticker = world.lookup("Sticker");
  const auto anchor = world.lookup("StickerAnchor");
  bool rejected_cycle = false;
  try { preview.SetSpatialParent(anchor, sticker); }
  catch (const std::invalid_argument&) { rejected_cycle = true; }
  Check(rejected_cycle && sticker.target<SpatialParent>() == anchor, "Cyclic reparenting changed the spatial links");

  auto parent = world.entity().set<LocalTransform2D>({{0.25F, 0.25F}, {0.5F, 0.5F}});
  sticker.set<LocalTransform2D>({{0.125F, 0.25F}, {0.5F, 0.5F}});
  preview.SetSpatialParent(sticker, parent);
  Check(preview.Evaluate({1, 2}, target)->draws.back().destination == Rect{0.3125F, 0.375F, 0.25F, 0.25F},
        "Parent and child transforms did not compose after reparenting");
  auto owned = world.entity().child_of(parent);
  parent.destruct();
  Check(sticker.is_alive() && !sticker.target<SpatialParent>() && !owned.is_alive() &&
            preview.Evaluate({1, 2}, target)->draws.back().destination == Rect{0.125F, 0.25F, 0.5F, 0.5F},
        "Spatial and owned children did not keep their respective lifetimes");
}

} // namespace avkit_preview
