#include "workbench_project.h"

#include <huxerui/avkit/resource_catalog.h>

#include <huxerui/avkit/edit_template.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::media;

double Seconds(MediaTimestamp time) { return double(time.ticks) / time.timescale; }

const AssetTrack& Track(const AssetSource& asset, TrackKind kind) {
  const auto match = std::ranges::find(asset.tracks, kind, &AssetTrack::kind);
  if (match == asset.tracks.end())
    throw std::invalid_argument(kind == TrackKind::Audio ? "所选素材没有音轨" : "所选素材没有视频轨");
  return *match;
}

MediaTimestamp Available(const AssetSource& asset, double limit, std::optional<TrackKind> only = {}) {
  for (const auto& track : asset.tracks) {
    if (only && track.kind != *only) continue;
    const auto range = track.presentation;
    limit = std::min(limit, Seconds(range.end) - Seconds(range.start));
  }
  // An even number of 24 fps frames also lands on the common 44.1 and 48 kHz PCM grids.
  auto ticks = static_cast<std::int64_t>(std::floor(limit * 12 + 1e-7));
  for (const auto& track : asset.tracks) {
    if (ticks <= 0) break;
    if (only && track.kind != *only) continue;
    const auto range = track.presentation;
    if (AddWorkbenchTime(range.start, {ticks, 12}).Compare(range.end) == 1) --ticks;
  }
  if (ticks <= 0) throw std::invalid_argument("The selected movie is too short");
  return {ticks, 12};
}

WorkbenchDocument NewDocument(PixelSize canvas, std::string title) {
  auto project = std::make_shared<Project>(canvas);
  return {project, project->Snapshot(), {}, std::move(title)};
}

bool TrackAvailable(const TimelineTrack& track, TimeRange span) {
  return std::ranges::none_of(track.clips, [&](const TimelineClip& clip) {
    const auto range = clip.placement.timeline;
    return range.start.Compare(span.end) == -1 && span.start.Compare(range.end) == -1;
  });
}

} // namespace

MediaTimestamp AddWorkbenchTime(MediaTimestamp a, MediaTimestamp b) {
  const auto scale = std::lcm<std::uint64_t>(a.timescale, b.timescale);
  if (scale > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("Incompatible media clocks");
  const auto first = a.Rescale(static_cast<std::uint32_t>(scale));
  const auto second = b.Rescale(static_cast<std::uint32_t>(scale));
  if (!first || !second ||
      (second->ticks > 0 && first->ticks > std::numeric_limits<std::int64_t>::max() - second->ticks) ||
      (second->ticks < 0 && first->ticks < std::numeric_limits<std::int64_t>::min() - second->ticks))
    throw std::runtime_error("Clip time exceeds the project range");
  return {first->ticks + second->ticks, first->timescale};
}

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

WorkbenchDocument CreateWorkbenchProject(std::vector<AssetSource> assets) {
  if (assets.empty()) throw std::invalid_argument("A new edit needs initial media");
  auto document = EmptyWorkbenchProject();
  for (auto& asset : assets) AppendWorkbenchMovie(document, std::move(asset));
  document.project = std::make_shared<Project>(Project::FromSnapshot(*document.revision));
  document.revision = document.project->Snapshot();
  return document;
}

WorkbenchDocument OpenWorkbenchProject(const File& file) {
  auto project = std::make_shared<Project>(Project::Open(file));
  return {project, project->Snapshot(), file, file.Stem()};
}

std::vector<WorkbenchCaption> ReadWorkbenchCaptions(const File& file) {
  auto contents = file.ReadString();
  if (!contents.Succeeded()) throw std::runtime_error(contents.Error().message);
  const auto timestamp = [](std::string_view value) {
    const auto number = [&](std::size_t start, std::size_t count) {
      unsigned n{};
      const auto end = value.data() + start + count;
      const auto parsed = std::from_chars(value.data() + start, end, n);
      if (parsed.ec != std::errc{} || parsed.ptr != end) throw std::invalid_argument("SRT 时间格式无效");
      return n;
    };
    if (value.size() < 12 || value[2] != ':' || value[5] != ':' || value[8] != ',')
      throw std::invalid_argument("SRT 时间格式无效");
    const auto minutes = number(3, 2), seconds = number(6, 2);
    if (minutes >= 60 || seconds >= 60) throw std::invalid_argument("SRT 时间格式无效");
    return number(0, 2) * 3600.0 + minutes * 60.0 + seconds + number(9, 3) / 1000.0;
  };
  std::vector<WorkbenchCaption> captions;
  std::istringstream input(std::move(contents).Value());
  const auto line = [&input](std::string& value) {
    if (!std::getline(input, value)) return false;
    if (!value.empty() && value.back() == '\r') value.pop_back();
    return true;
  };
  for (std::string value; line(value);) {
    if (value.empty()) continue;
    if (value.find(" --> ") == std::string::npos && !line(value))
      throw std::invalid_argument("SRT 字幕缺少时间范围");
    const auto arrow = value.find(" --> ");
    if (arrow == std::string::npos) throw std::invalid_argument("SRT 字幕缺少时间范围");
    const auto time = std::string_view(value);
    WorkbenchCaption cue{timestamp(time.substr(0, arrow)), timestamp(time.substr(arrow + 5)), {}};
    while (line(value) && !value.empty()) {
      if (!cue.text.empty()) cue.text += '\n';
      cue.text += value;
    }
    if (cue.begin >= cue.end || cue.text.empty()) throw std::invalid_argument("SRT 字幕为空或时间范围无效");
    captions.push_back(std::move(cue));
  }
  std::ranges::stable_sort(captions, {}, &WorkbenchCaption::begin);
  return captions;
}

void AppendWorkbenchMovie(WorkbenchDocument& document, AssetSource asset) {
  const auto duration = asset.still_image ? asset.tracks.front().presentation.end : Available(asset, 24 * 60 * 60);
  const auto before = document.revision;
  const auto main = std::ranges::find(before->tracks, TrackKind::Video, &TimelineTrack::kind);
  const auto begin = main != before->tracks.end() && !main->clips.empty() ?
      main->clips.back().placement.timeline.end : MediaTimestamp{};
  const TimeRange timeline{begin, AddWorkbenchTime(begin, duration)};
  const auto audio = std::ranges::find_if(before->tracks, [&](const TimelineTrack& track) {
    return track.kind == TrackKind::Audio && TrackAvailable(track, timeline);
  });
  const auto still_image = asset.still_image;
  const auto video_source = Track(asset, TrackKind::Video);
  const auto audio_match = std::ranges::find(asset.tracks, TrackKind::Audio, &AssetTrack::kind);
  const auto audio_source = audio_match == asset.tracks.end() ? std::optional<AssetTrack>{} : *audio_match;
  const auto name = asset.file.Stem();
  document.revision = document.project->Edit([&](ProjectEdit& edit) {
    const auto id = edit.Import(std::move(asset));
    if (before->tracks.empty() && video_source.display_size) {
      const auto size = *video_source.display_size;
      edit.SetCanvasSize({std::max(2U, size.width & ~1U), std::max(2U, size.height & ~1U)});
    }
    const auto insert = [&](const AssetTrack& source, auto track) {
      const auto output = track == before->tracks.end() ? edit.AddTrack(source.kind) : track->id;
      const auto start = source.presentation.start;
      return edit.InsertClip(output, {{id, source.id, {start, AddWorkbenchTime(start, duration)}}, timeline});
    };
    const auto owner = still_image ?
        edit.InsertImage(main == before->tracks.end() ? edit.AddTrack(TrackKind::Video) : main->id, id, begin) :
        insert(video_source, main);
    if (audio_source) edit.BindAudio(insert(*audio_source, audio), owner);
  });
  if (before->resources->Empty()) document.title = name;
  document.dirty = true;
}

void InsertWorkbenchLayer(WorkbenchDocument& document, AssetSource asset, TrackKind kind, MediaTimestamp position,
                          bool include_original_audio) {
  const auto sound = std::ranges::find(asset.tracks, TrackKind::Audio, &AssetTrack::kind);
  const auto linked = kind == TrackKind::Video && include_original_audio && sound != asset.tracks.end()
      ? std::optional{*sound} : std::nullopt;
  const auto duration = Available(asset, 24 * 60 * 60, linked ? std::nullopt : std::optional{kind});
  const auto source = Track(asset, kind);
  const auto begin = position.Compare(document.revision->Range().end) == 1 ? document.revision->Range().end : position;
  const TimeRange timeline{begin, AddWorkbenchTime(begin, duration)};
  std::optional<TrackId> lane;
  std::array<bool, kWorkbenchInsetPositions.size()> occupied{};
  bool reserve_main = kind == TrackKind::Video;
  for (const auto& track : document.revision->tracks) {
    if (track.kind != kind) continue;
    if (std::exchange(reserve_main, false)) continue;
    if (!lane && TrackAvailable(track, timeline)) lane = track.id;
    if (kind == TrackKind::Video) {
      for (const auto& clip : track.clips) {
        const auto range = clip.placement.timeline;
        if (range.start.Compare(timeline.end) != -1 || timeline.start.Compare(range.end) != -1) continue;
        for (std::size_t i = 0; i < occupied.size(); ++i) {
          const auto corner = kWorkbenchInsetPositions[i];
          const auto rect = clip.destination;
          occupied[i] |= rect.x < corner.x + corner.width && corner.x < rect.x + rect.width &&
                         rect.y < corner.y + corner.height && corner.y < rect.y + rect.height;
        }
      }
    }
  }
  document.revision = document.project->Edit([&](ProjectEdit& edit) {
    const auto id = edit.Import(std::move(asset));
    const auto output = lane ? *lane : edit.AddTrack(kind);
    const auto clip = edit.InsertClip(output, {{id, source.id, {source.presentation.start, AddWorkbenchTime(source.presentation.start, duration)}},
                                                      timeline});
    if (kind == TrackKind::Video) {
      auto corner = std::ranges::find(occupied, false);
      const auto bounds = kWorkbenchInsetPositions[corner == occupied.end() ? 0 : corner - occupied.begin()];
      edit.SetLayout(clip, bounds);
      edit.SetClipFill(clip, true);
    }
    if (linked) {
      const auto lane = std::ranges::find_if(document.revision->tracks, [&](const TimelineTrack& track) {
        return track.kind == TrackKind::Audio && TrackAvailable(track, timeline);
      });
      const auto track = lane == document.revision->tracks.end() ? edit.AddTrack(TrackKind::Audio) : lane->id;
      const auto start = linked->presentation.start;
      const auto original = edit.InsertClip(track, {{id, linked->id, {start, AddWorkbenchTime(start, duration)}}, timeline});
      edit.BindAudio(original, clip);
    }
  });
  document.dirty = true;
}

std::optional<PixelSize> WorkbenchVideoSize(const ProjectSnapshot& project, const TimelineClip& clip) {
  const auto source = clip.placement.source;
  const auto asset = project.resources->FindMedia(source.asset);
  if (!asset) return {};
  const auto track = std::ranges::find(asset->tracks, source.track, &AssetTrack::id);
  return track == asset->tracks.end() ? std::nullopt : track->display_size;
}

Rect FitWorkbenchInset(Rect bounds, PixelSize canvas, std::optional<PixelSize> source) {
  return source ? FitCanvasAspect(bounds, canvas, {static_cast<float>(source->width), static_cast<float>(source->height)}) : bounds;
}

WorkbenchDocument MakeWorkbenchTemplate(const ResourceCatalog& resources, const ResourceRef& reference,
                                        std::string title, std::vector<AssetSource> assets) {
  auto project = std::make_shared<Project>(CreateTemplateProject(resources, reference, std::move(assets)));
  return {project, project->Snapshot(), {}, std::move(title), true};
}

} // namespace avkit_preview
