#include <huxerui/avkit/timeline.h>
#include <huxerui/avkit/scene.h>
#include <huxerui/avkit/component.h>
#include <huxerui/avkit/resource_catalog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>

#include "ecs/project_scene.h"
#include "ecs/scene_evaluation.h"
#include "ecs/system/audio_system.h"
#include "ecs/system/scene_systems.h"
#include "media/timeline_audio_source.h"
#include "media/timeline_internal.h"
#include "media/timeline_video_source.h"
#include "project/project_internal.h"
#include "support/cancellation.h"
#include "support/geometry.h"
#include "support/media_time.h"

namespace huxerui::avkit {
namespace {

using namespace media;
using detail::Add;
using detail::Before;
using detail::CheckCancelled;
using detail::ExactTicks;

TimeRange RangeOf(const MediaPresentationRange& range) { return {range.start, range.end}; }

bool Contains(TimeRange range, MediaTimestamp time) {
  return !Before(time, range.start) && Before(time, range.end);
}

std::shared_ptr<const ResourceCatalog> MergeResources(const ProjectSnapshot* project,
    const std::shared_ptr<const ResourceCatalog>& packaged) {
  if (!project || !project->resources || project->resources->Empty()) return packaged;
  if (!packaged || packaged->Empty()) return project->resources;
  return std::make_shared<const ResourceCatalog>(packaged->WithMedia(*project->resources));
}

} // namespace

AssetSource InspectTimelineMovie(const File& file) {
  const auto movie = ImportMovie(file);
  const auto& desc = movie.video.descriptions.front();
  auto width = desc.visible ? desc.visible->width : desc.coded_width.value_or(0);
  auto height = desc.visible ? desc.visible->height : desc.coded_height.value_or(0);
  if (const auto& aspect = desc.pixel_aspect_ratio; aspect && aspect->height) {
    const auto oriented = std::llround(double(width) * aspect->width / aspect->height);
    if (oriented > 0 && oriented <= std::numeric_limits<std::uint32_t>::max()) width = static_cast<std::uint32_t>(oriented);
  }
  if (movie.video.rotation == VideoRotation::Clockwise90 || movie.video.rotation == VideoRotation::Clockwise270)
    std::swap(width, height);
  const auto size = width && height ? std::optional{PixelSize{width, height}} : std::nullopt;
  AssetSource result{movie.file, {{movie.video.id, TrackKind::Video, RangeOf(movie.video_range), size}}};
  if (movie.HasAudio()) result.tracks.push_back({movie.audio.id, TrackKind::Audio, RangeOf(movie.audio_range)});
  return result;
}

AssetSource InspectTimelineAudio(const File& file) {
  const auto movie = ImportMovieAudio(file);
  return {movie.file, {{movie.audio.id, TrackKind::Audio, RangeOf(movie.audio_range)}}};
}

Project CreateTemplateProject(const ResourceCatalog& catalog, const ResourceRef& reference,
                              std::vector<AssetSource> assets) {
  const auto& recipe = OpenEditTemplate(catalog, reference);
  Project project(recipe.canvas_size);
  EditTemplateInputs inputs;
  project.Edit([&](ProjectEdit& edit) {
    for (auto& asset : assets) inputs.ordered_assets.push_back(edit.Import(std::move(asset)));
    for (const auto& slot : recipe.resource_slots) {
      if (const auto registered = catalog.FindMedia(slot.resource)) {
        inputs.bindings.push_back({slot.slot, edit.Import(*registered, slot.resource)});
        continue;
      }
      const auto file = catalog.FindLocalFile(slot.resource);
      if (!file) throw std::invalid_argument("A template media slot needs a local file: " + slot.resource.key);
      const auto visual = std::ranges::any_of(recipe.resource_slots, [&](const TemplateResourceSlot& other) {
        return other.resource == slot.resource && other.kind == TrackKind::Video;
      });
      auto asset = visual ? InspectTimelineVisual(*file) : InspectTimelineAudio(*file);
      inputs.bindings.push_back({slot.slot, edit.Import(std::move(asset), slot.resource)});
    }
  });
  const auto plan = PlanEditTemplate(project, recipe, inputs);
  return Project::FromSnapshot(*ApplyEditTemplate(project, plan));
}

void detail::ValidateMediaTrack(const ClipSource& source, const MovieAsset& movie, TrackKind kind) {
  const auto& track = kind == TrackKind::Video ? movie.video : movie.audio;
  const auto& available = kind == TrackKind::Video ? movie.video_range : movie.audio_range;
  if (source.track != track.id || Before(source.range.start, available.start) ||
      Before(available.end, source.range.end))
    throw std::runtime_error("Relink timeline source with changed tracks or duration: " + movie.file.Path());
}

struct TimelineSession::Impl {
  Impl(std::shared_ptr<const ProjectSnapshot> snapshot, CodecProviders codecs, CodecPolicy selection,
       OnCodecFallback fallback, std::shared_ptr<analysis::AnalysisProvider> analyzer,
       std::shared_ptr<const ResourceCatalog> resources, std::optional<PixelSize> output,
       std::shared_ptr<render::RenderBackend> backend, std::optional<render::PixelFormat> format,
       std::shared_ptr<const ResourceCatalog> scene_resources = {})
      : project(std::move(snapshot)), providers(std::move(codecs)), policy(selection), report(std::move(fallback)),
        device(std::move(backend)), packaged(std::move(resources)),
        scene(scene_resources ? std::move(scene_resources) : MergeResources(project.get(), packaged)),
        output_size(output), device_format(format) {
    if (!project) throw std::invalid_argument("Timeline sampling requires a project revision");
    detail::ValidateProject(*project);
    if (analyzer) scene.Root().assign<std::shared_ptr<analysis::AnalysisProvider>>(std::move(analyzer));
    scene.Root().assign<detail::TimelineServices>(video, audio, device.get(),
        [this](const ResourceRef& asset) -> const MovieAsset& { return Movie(asset); });
    detail::InstallTimelineSystems(scene);
    range = project->Range();
    bool has_audio = false;
    for (const auto& track : project->tracks) {
      for (const auto& clip : track.clips) {
        const auto& source = clip.placement.source;
        const auto& asset = detail::FindAsset(*project, source.asset);
        if (asset.still_image) {
          if (!asset.file.IsFile()) throw std::runtime_error("Missing timeline asset: " + asset.file.Path());
          continue;
        }
        const auto& movie = Movie(source.asset);
        detail::ValidateMediaTrack(source, movie, track.kind);
        if (track.kind != TrackKind::Audio) continue;
        if (!has_audio) {
          pcm = movie.pcm;
          has_audio = true;
        }
        pcm.sample_rate = std::max(pcm.sample_rate, movie.pcm.sample_rate);
        if (movie.pcm.ChannelCount() > pcm.ChannelCount()) pcm.channel_layout = movie.pcm.channel_layout;
      }
    }
    detail::ProjectTimeline(scene, *project);
  }

  const MovieAsset& Movie(const ResourceRef& resource) {
    const auto found = movies.find(resource);
    if (found != movies.end()) return found->second;
    const auto asset = SceneCatalog(scene.Root()).FindMedia(resource);
    if (!asset) throw std::invalid_argument("Unknown media resource: " + resource.key);
    const auto& file = asset->file;
    if (!file.IsFile()) throw std::runtime_error("Missing timeline asset: " + file.Path());
    const bool video = std::ranges::any_of(asset->tracks, [](const auto& track) { return track.kind == TrackKind::Video; });
    return movies.emplace(resource, video ? ImportMovie(file) : ImportMovieAudio(file)).first->second;
  }

  TimelineFrame SampleVideo(MediaTimestamp at, std::optional<render::PixelFormat> output,
                            std::stop_token stop, std::optional<std::chrono::milliseconds> analysis_wait,
                            std::optional<MediaTimestamp> continuous_step,
                            const std::function<void(const VideoFrame&)>& on_decode,
                            std::vector<detail::TargetLayoutEntry>* layouts) {
    last_video_plan.reset();
    CheckCancelled(stop);
    if (!Contains(range, at)) throw std::out_of_range("Sample time is outside the project interval");
    if (device_format && output && *output != *device_format)
      throw std::invalid_argument("The requested color format must match the bound renderer");
    const auto format = output.value_or(device_format.value_or(render::PixelFormat::Bgra8Unorm));
    detail::ClearSampledVideo(entities);
    TimelineFrame frame{.pts = at};
    auto root = scene.Root();
    root.assign<detail::TimelineEvaluation>(stop, analysis_wait, on_decode);
    struct ReleaseSamples {
      entityx::EntityManager& entities;
      entityx::Entity root;

      ~ReleaseSamples() {
        root.remove<detail::TimelineEvaluation>();
        entities.each<SampledVideo>([](entityx::Entity, SampledVideo& sample) { sample.picture = nullptr; });
        detail::ClearComponents<PrimarySubject>(entities);
      }
    } release{entities, root};
    frame.plan = scene.Evaluate({at, {output_size.value_or(project->canvas_size), format}, continuous_step},
        stop, layouts);
    last_video_plan = frame.plan;
    last_video_time = at;
    return frame;
  }

  TimelineFrame RecomposeVideo(const TimelineFrame& frame, EditTarget target, Affine2D delta,
                               std::vector<detail::TargetLayoutEntry>* layouts) {
    if (!frame.plan || frame.plan != last_video_plan.lock() || frame.pts != last_video_time)
      throw std::invalid_argument("Recomposition requires the most recently sampled video frame");
    if (!detail::IsInvertible(delta))
      throw std::invalid_argument("Recomposition requires an invertible finite transform");
    entityx::Entity entity;
    if (!delta.IsIdentity()) {
      const auto find_entity = [&]<class Id>(Id id) {
        for (auto layer : entities.entities_with_components<Id>()) {
          if (*layer.template component<Id>() == id) return layer;
        }
        return entityx::Entity{};
      };
      const auto clip = target.AsClip();
      entity = clip ? find_entity(*clip) : find_entity(*target.AsOverlay());
    }
    if (!entity && !layouts) return frame;
    return {frame.pts, scene.Compile(entity, delta, layouts)};
  }

  AudioFrame SampleAudio(MediaTimestamp at, std::uint32_t count, std::stop_token stop) {
    if (!count || !Contains(range, at) || Before(range.end, Add(at, {count, pcm.sample_rate})))
      throw std::out_of_range("Sample interval is outside the project audio range");
    try {
      return detail::EvaluateAudioBlock(scene, at, count, pcm, stop);
    } catch (...) {
      audio.Reset();
      throw;
    }
  }

  bool PrepareVideo(TimeRange lookahead, bool seek, std::stop_token stop = {}) {
    auto root = scene.Root();
    struct Release {
      entityx::Entity root;

      ~Release() { root.remove<detail::VideoPrerollRequest>(); }
    };
    const auto request = root.assign<detail::VideoPrerollRequest>(lookahead, seek, false, stop);
    const Release release{root};
    scene.RunPhase(SceneSystemPhase::ResolveTiming, stop);
    scene.RunPhase(SceneSystemPhase::Preroll, stop);
    return request->ready;
  }

  void Reset(std::optional<MediaTimestamp> seek, std::optional<MediaTimestamp> preroll_end = {}) {
    last_video_plan.reset();
    detail::ClearSampledVideo(entities);
    detail::ClearComponents<SampledStillImage, render::BackgroundMask, SampledStickerDepth,
                            render::MaskTransitionSample>(entities);
    if (seek) PrepareVideo({*seek, preroll_end.value_or(*seek)}, true);
    else video.Reset({}, {}, scene.Events());
    audio.Reset();
  }

  std::shared_ptr<const ProjectSnapshot> project;
  CodecProviders providers;
  CodecPolicy policy;
  OnCodecFallback report;
  TimeRange range;
  AudioFormat pcm{48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Mono};
  std::map<ResourceRef, MovieAsset> movies;
  std::shared_ptr<render::RenderBackend> device;
  detail::TimelineVideoSource video{providers, policy, report, device.get()};
  detail::TimelineAudioSource audio{providers, policy, report};
  std::shared_ptr<const ResourceCatalog> packaged;
  RenderScene scene;
  entityx::EntityManager& entities = scene.Entities();
  std::vector<SceneModule> modules;
  std::optional<PixelSize> output_size;
  std::optional<render::PixelFormat> device_format;
  std::weak_ptr<const render::RenderPlan> last_video_plan;
  MediaTimestamp last_video_time;
};

TimelineSession::TimelineSession(std::shared_ptr<const ProjectSnapshot> project, CodecProviders providers,
                                 CodecPolicy policy, OnCodecFallback on_fallback,
                                 std::shared_ptr<analysis::AnalysisProvider> analysis, std::shared_ptr<const ResourceCatalog> resources,
                                 std::optional<PixelSize> output_size)
    : TimelineSession(std::move(project), std::move(providers), policy, std::move(on_fallback),
                      std::move(analysis), std::move(resources), output_size, {}, {}) {}

TimelineSession::TimelineSession(std::shared_ptr<const ProjectSnapshot> project, CodecProviders providers,
    CodecPolicy policy, OnCodecFallback on_fallback, std::shared_ptr<analysis::AnalysisProvider> analysis,
    std::shared_ptr<const ResourceCatalog> resources, std::optional<PixelSize> output_size,
    std::shared_ptr<render::RenderBackend> device, std::optional<render::PixelFormat> device_format)
    : impl_(std::make_unique<Impl>(std::move(project), std::move(providers), policy, std::move(on_fallback),
                                   std::move(analysis), std::move(resources), output_size, std::move(device), device_format)) {}

TimelineSession::~TimelineSession() = default;

std::unique_ptr<TimelineSession> detail::TimelineSessionAccess::Open(std::shared_ptr<const ProjectSnapshot> project,
    CodecProviders providers, CodecPolicy policy, const SceneConfiguration& scene, OnCodecFallback fallback,
    std::shared_ptr<render::RenderBackend> device, render::ImageInfo target) {
  auto timeline = std::unique_ptr<TimelineSession>(new TimelineSession(std::move(project), std::move(providers), policy,
      std::move(fallback), scene.analysis, scene.resources, target.size, std::move(device), target.format));
  if (scene.module) timeline->InstallSceneModule(scene.module);
  return timeline;
}

void TimelineSession::ApplyProject(std::shared_ptr<const ProjectSnapshot> project) {
  auto resources = project && project->resources == impl_->project->resources ? impl_->scene.CatalogSnapshot() : nullptr;
  auto replacement = std::make_unique<Impl>(std::move(project), impl_->providers, impl_->policy, impl_->report,
      detail::SceneAnalysisProvider(impl_->scene.Root()), impl_->packaged, impl_->output_size, impl_->device,
      impl_->device_format, std::move(resources));
  replacement->modules = impl_->modules;
  for (const auto& module : replacement->modules) module(replacement->scene);
  replacement->scene.ReuseImageResources(impl_->scene);
  impl_ = std::move(replacement);
}

void TimelineSession::InstallSceneModule(SceneModule module) {
  if (!module) throw std::invalid_argument("Timeline scene module is empty");
  module(impl_->scene);
  impl_->last_video_plan.reset();
  impl_->modules.push_back(std::move(module));
}

TimelineFrame TimelineSession::SampleVideo(MediaTimestamp at, std::optional<render::PixelFormat> output,
                                           std::stop_token stop, std::optional<std::chrono::milliseconds> analysis_wait,
                                           std::optional<MediaTimestamp> continuous_step,
                                           const std::function<void(const VideoFrame&)>& on_decode) {
  return SampleVideo(at, output, stop, analysis_wait, continuous_step, on_decode, nullptr);
}

TimelineFrame TimelineSession::SampleVideo(MediaTimestamp at, std::optional<render::PixelFormat> output,
    std::stop_token stop, std::optional<std::chrono::milliseconds> analysis_wait,
    std::optional<MediaTimestamp> continuous_step, const std::function<void(const VideoFrame&)>& on_decode,
    std::vector<detail::TargetLayoutEntry>* layouts) {
  try {
    return impl_->SampleVideo(at, output, stop, analysis_wait, continuous_step, on_decode, layouts);
  } catch (const std::system_error& error) {
    if (error.code() == std::errc::operation_canceled) Reset();
    throw;
  }
}

TimelineFrame TimelineSession::RecomposeVideo(const TimelineFrame& frame, EditTarget target, Affine2D delta) {
  return RecomposeVideo(frame, target, delta, nullptr);
}

TimelineFrame TimelineSession::RecomposeVideo(const TimelineFrame& frame, EditTarget target, Affine2D delta,
    std::vector<detail::TargetLayoutEntry>* layouts) {
  return impl_->RecomposeVideo(frame, target, delta, layouts);
}

bool TimelineSession::PrerollVideoEntries(TimeRange lookahead, std::stop_token stop) {
  CheckCancelled(stop);
  const auto range = impl_->range;
  if (!Contains(range, lookahead.start) || !Before(lookahead.start, lookahead.end) || Before(range.end, lookahead.end))
    throw std::out_of_range("Video preroll interval is outside the project interval");
  return impl_->PrepareVideo(lookahead, false, stop);
}

AudioFormat TimelineSession::PcmFormat() const { return impl_->pcm; }

AudioFrame TimelineSession::SampleAudio(MediaTimestamp start, std::uint32_t samples, std::stop_token stop) {
  return impl_->SampleAudio(start, samples, stop);
}

void TimelineSession::Seek(MediaTimestamp at, std::optional<MediaTimestamp> preroll_end) {
  if (!Contains(impl_->range, at)) throw std::out_of_range("Seek time is outside the project interval");
  if (preroll_end && (!Before(at, *preroll_end) || Before(impl_->range.end, *preroll_end)))
    throw std::out_of_range("Seek lookahead end is outside the remaining project interval");
  impl_->Reset(at, preroll_end);
}

void TimelineSession::Reset() { impl_->Reset(std::nullopt); }

} // namespace huxerui::avkit
