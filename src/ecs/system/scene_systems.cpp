#include "scene_systems.h"

#include "audio_system.h"
#include "camera_layout_system.h"
#include "clip_sampling_system.h"
#include "coverage_system.h"
#include "timeline_system.h"
#include "tracking_system.h"
#include "transition_system.h"
#include "video_source_system.h"

namespace huxerui::avkit::detail {
void InstallTimelineSystems(RenderScene& scene) {
  using enum SceneSystemPhase;
  scene.RegisterSystemAt<ClipPresentationRangeSystem>(ResolveTiming);
  scene.RegisterSystemAt<TimelineSystem>(SampleSources);
  scene.RegisterSystemAt<VideoPrerollSystem>(Preroll);
  scene.RegisterSystemAt<VideoSourceSystem>(SampleSources);
  scene.RegisterSystemAt<ClipSamplingSystem>(ResolveLayers);
  scene.RegisterSystemAt<TrackingSystem>(ResolveLayers);
  scene.RegisterSystemAt<CoverageSystem>(ResolveLayers);
  scene.RegisterSystemAt<TransitionSystem>(ResolveLayers);
  InstallAudioSystem(scene);
}

void InstallCaptureSystems(RenderScene& scene) {
  scene.RegisterSystemAt<CameraLayoutSystem>(SceneSystemPhase::ResolveLayers);
}

void InstallAudioSystem(RenderScene& scene) { scene.RegisterSystemAt<AudioSystem>(SceneSystemPhase::Audio); }

} // namespace huxerui::avkit::detail
