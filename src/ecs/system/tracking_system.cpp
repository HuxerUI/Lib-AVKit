#include "tracking_system.h"

#include <huxerui/avkit/component.h>

#include <algorithm>
#include <cmath>

#include "ecs/scene_evaluation.h"
#include "ecs/scene_events.h"
#include "media/timeline_video_source.h"

namespace huxerui::avkit::detail {
namespace {

std::optional<Affine2D> AnchorTransform(const analysis::TrackingAnchor& anchor, const analysis::TrackedPersonSample& person,
                                           const Affine2D& source_to_clip, PixelSize canvas) {
  const auto& part = anchor.part == analysis::TrackedPart::Face ? person.face
                     : anchor.part == analysis::TrackedPart::LeftHand ? person.left_hand : person.right_hand;
  if (!part || part->observed != person.target) return std::nullopt;
  const auto origin = detail::FindLandmark(*part, anchor.point);
  if (!origin) return std::nullopt;
  const auto position = source_to_clip.Apply(*origin);
  if (!anchor.axis_point)
    return Affine2D{std::hypot(source_to_clip.m11, source_to_clip.m12), 0, 0,
                       std::hypot(source_to_clip.m21, source_to_clip.m22), position.x, position.y};
  const auto axis = detail::FindLandmark(*part, *anchor.axis_point);
  if (!axis || *axis == *origin) return std::nullopt;
  const auto dx = axis->x - origin->x, dy = axis->y - origin->y;
  const auto x = dx * source_to_clip.m11 + dy * source_to_clip.m21;
  const auto y = dx * source_to_clip.m12 + dy * source_to_clip.m22;
  return Affine2D{x, y, -y * canvas.height / canvas.width, x * canvas.width / canvas.height, position.x, position.y};
}

} // namespace

void TrackingSystem::configure(entityx::EventManager& events) {
  events.subscribe<VideoSourceInvalidatedEvent>(*this);
}

void TrackingSystem::receive(const VideoSourceInvalidatedEvent& event) {
  const auto tracker = trackers_.find(event.clip);
  if (tracker == trackers_.end()) return;
  if (event.source_changed) trackers_.erase(tracker);
  else tracker->second.source->Reset();
}

void TrackingSystem::update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) {
  auto root = SceneRoot(entities);
  const auto& sources = *root.component<const TimelineServices>();
  const auto provider = SceneAnalysisProvider(root);
  const auto canvas = root.component<const VideoFrameContext>()->target.size;
  const auto stop = root.component<const TimelineEvaluation>()->stop;
  std::map<entityx::Entity, std::vector<analysis::TrackedPart>> requirements;
  for (auto entity : entities.entities_with_components<ClipAttachment, TrackedSticker>()) {
    const auto clip = entity.component<ClipAttachment>()->clip;
    if (clip && clip.has_component<ClipId>()) requirements[clip].push_back(entity.component<TrackedSticker>()->binding.part);
  }
  std::erase_if(trackers_, [&](const auto& entry) { return !requirements.contains(entry.first); });
  for (auto entity : entities.entities_with_components<SampledVideo>()) {
    const auto required = requirements.find(entity);
    const auto sample = entity.component<SampledVideo>();
    if (required == requirements.end() || !sample->picture) continue;
    const auto& image = *sample->picture;
    const auto input = sources.video.TrackingInput(entity, image, stop);
    if (!input) continue;
    auto& parts = required->second;
    std::ranges::sort(parts);
    const auto duplicates = std::ranges::unique(parts);
    parts.erase(duplicates.begin(), duplicates.end());
    const auto id = *entity.component<const ClipId>();
    auto& tracker = trackers_[entity];
    if (!tracker.source || tracker.id != id || tracker.parts != parts)
      tracker = {id, parts, std::make_unique<SourceTracking>(provider, id.value, parts)};
    const auto& tracking = tracker.source->Sample(image, input->read, input->reference, input->open_ordered, stop);
    if (!tracking.primary) continue;
    const auto person = std::ranges::find(tracking.people, *tracking.primary, &analysis::TrackedPersonSample::person);
    if (person != tracking.people.end()) entity.assign<PrimarySubject>(&*person);
  }
  entities.each<ClipAttachment, TrackedSticker>([&](entityx::Entity, ClipAttachment& attachment, TrackedSticker& tracking) {
    if (!tracking.anchor) return;
    const auto subject = attachment.clip ? attachment.clip.component<PrimarySubject>() : entityx::ComponentHandle<PrimarySubject>{};
    const auto mapping = subject ? attachment.clip.component<ClipSourceMapping>() : entityx::ComponentHandle<ClipSourceMapping>{};
    const auto transform = mapping ? AnchorTransform(tracking.binding, *subject->person,
        mapping->source_to_clip, canvas) : std::nullopt;
    if (transform) tracking.anchor.replace<Affine2D>(*transform);
    else if (tracking.anchor.has_component<Affine2D>()) tracking.anchor.remove<Affine2D>();
  });
}

} // namespace huxerui::avkit::detail
