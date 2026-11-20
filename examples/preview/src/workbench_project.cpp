#include "workbench_project.h"

#include <huxerui/avkit/edit_template.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::media;

double Seconds(MediaTimestamp time) { return double(time.ticks) / time.timescale; }

MediaTimestamp Add(MediaTimestamp a, MediaTimestamp b) {
  const auto scale = std::lcm<std::uint64_t>(a.timescale, b.timescale);
  if (scale > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("Incompatible media clocks");
  const auto first = a.Rescale(static_cast<std::uint32_t>(scale));
  const auto second = b.Rescale(static_cast<std::uint32_t>(scale));
  if (!first || !second || second->ticks < 0 || first->ticks > std::numeric_limits<std::int64_t>::max() - second->ticks)
    throw std::runtime_error("Clip time exceeds the project range");
  return {first->ticks + second->ticks, first->timescale};
}

const AssetTrack& Track(const AssetReference& asset, TrackKind kind) {
  const auto match = std::ranges::find(asset.tracks, kind, &AssetTrack::kind);
  if (match == asset.tracks.end()) throw std::invalid_argument("The selected movie must contain video and audio");
  return *match;
}

MediaTimestamp Available(const AssetReference& asset, double limit) {
  for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
    const auto range = Track(asset, kind).presentation;
    limit = std::min(limit, Seconds(range.end) - Seconds(range.start));
  }
  // An even number of 24 fps frames also lands on the common 44.1 and 48 kHz PCM grids.
  auto ticks = static_cast<std::int64_t>(std::floor(limit * 12 + 1e-7));
  for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
    if (ticks <= 0) break;
    const auto range = Track(asset, kind).presentation;
    if (Add(range.start, {ticks, 12}).Compare(range.end) == 1) --ticks;
  }
  if (ticks <= 0) throw std::invalid_argument("The selected movie is too short");
  return {ticks, 12};
}

WorkbenchDocument NewDocument(PixelSize canvas, std::string title) {
  auto project = std::make_shared<Project>(canvas);
  return {project, project->Snapshot(), {}, std::move(title)};
}

} // namespace

File WorkbenchDirectory(const File& base, std::string_view name) {
  File directory(base, name);
  if (!directory.IsDirectory() && !directory.CreateDirectories())
    throw std::runtime_error("Cannot create " + directory.Path());
  return directory;
}

File WorkbenchOutput(const File& base, std::string_view directory, std::string_view filename) {
  static std::atomic_uint64_t counter{};
  const auto now = std::chrono::system_clock::now().time_since_epoch().count();
  const auto key = std::to_string(now) + "-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
  return File(WorkbenchDirectory(WorkbenchDirectory(base, directory), key), filename);
}

std::vector<File> RecentWorkbenchProjects(const File& data) {
  const File directory(data, "Projects");
  if (!directory.IsDirectory()) return {};
  auto children = directory.ListChildren();
  if (!children.Succeeded()) throw std::runtime_error(children.Error().message);
  auto entries = std::move(children).Value();
  std::ranges::sort(entries, std::greater{}, &File::Name);
  std::vector<File> result;
  for (const auto& folder : entries) {
    auto docs = folder.ListChildren();
    if (!docs.Succeeded()) continue;
    for (const auto& file : docs.Value()) {
      if (file.Extension() == ".avkit" && file.IsFile()) result.push_back(file);
      if (result.size() >= 8) return result;
    }
  }
  return result;
}

WorkbenchDocument EmptyWorkbenchProject() { return NewDocument({640, 360}, "未命名项目"); }

WorkbenchDocument OpenWorkbenchProject(const File& file) {
  auto project = std::make_shared<Project>(Project::Open(file));
  return {project, project->Snapshot(), file, file.Stem()};
}

void AppendWorkbenchMovie(WorkbenchDocument& document, AssetReference asset) {
  const auto duration = Available(asset, 24 * 60 * 60);
  const auto begin = document.revision->Range().end;
  const TimeRange timeline{begin, Add(begin, duration)};
  const auto before = document.revision;
  const std::array tracks{Track(asset, TrackKind::Video), Track(asset, TrackKind::Audio)};
  const auto name = asset.file.Stem();
  document.revision = document.project->Edit([&](ProjectEdit& edit) {
    const auto id = edit.Import(std::move(asset));
    for (const auto& source : tracks) {
      const auto track = std::ranges::find(before->tracks, source.kind, &TrackInfo::kind);
      const auto output = track == before->tracks.end() ? edit.AddTrack(source.kind) : track->id;
      const auto start = source.presentation.start;
      edit.InsertClip(output, {{id, source.id, {start, Add(start, duration)}}, timeline});
    }
  });
  if (before->assets.empty()) document.title = name;
  document.dirty = true;
}

WorkbenchDocument MakeWorkbenchTemplate(WorkbenchTemplate kind, std::array<AssetReference, 2> assets) {
  const bool portrait = kind == WorkbenchTemplate::Portrait;
  const bool inset = kind == WorkbenchTemplate::PictureInPicture;
  const auto title = portrait ? "竖屏记录" : inset ? "双镜头画中画" : "舞台快剪";
  const auto key = portrait ? "portrait" : inset ? "picture-in-picture" : "stage-cut";
  const PixelSize canvas = portrait ? PixelSize{360, 640} : PixelSize{640, 360};
  auto document = NewDocument(canvas, title);
  auto first = Available(assets[0], portrait ? 5 : 6);
  auto second = Available(assets[1], portrait ? 5 : 6);
  if (inset) first = second = first.Compare(second) == -1 ? first : second;
  const TemplateClip a{.slot = "first", .source = {{}, first}, .timeline = {{}, first}};
  const TemplateClip b{.slot = "second", .source = {{}, second},
                       .timeline = inset ? TimeRange{{}, second} : TimeRange{first, Add(first, second)},
                       .destination = inset ? Rect{0.66F, 0.63F, 0.30F, 0.30F} : Rect{0, 0, 1, 1}};
  EditTemplateRecipe recipe{{"avkit.workbench", "1", key}, canvas, {"first", "second"}};
  if (inset) recipe.tracks = {{TrackKind::Video, {a}}, {TrackKind::Video, {b}}, {TrackKind::Audio, {a}}};
  else recipe.tracks = {{TrackKind::Video, {a, b}}, {TrackKind::Audio, {a, b}}};
  EditTemplateInputs binding;
  document.project->Edit([&](ProjectEdit& edit) {
    for (std::size_t i = 0; i < assets.size(); ++i)
      binding.bindings.push_back({recipe.slots[i], edit.Import(std::move(assets[i]))});
  });
  const auto plan = PlanEditTemplate(*document.project, recipe, binding);
  document.revision = ApplyEditTemplate(*document.project, plan);
  document.dirty = true;
  return document;
}

} // namespace avkit_preview
