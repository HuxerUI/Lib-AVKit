#include "workbench_project.h"

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

const AssetTrack& Track(const AssetReference& asset, TrackKind kind) {
  const auto match = std::ranges::find(asset.tracks, kind, &AssetTrack::kind);
  if (match == asset.tracks.end())
    throw std::invalid_argument(kind == TrackKind::Audio ? "所选素材没有音轨" : "所选素材没有视频轨");
  return *match;
}

MediaTimestamp Available(const AssetReference& asset, double limit, std::optional<TrackKind> only = {}) {
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

bool TrackAvailable(const TrackInfo& track, TimeRange span) {
  return std::ranges::none_of(track.clips, [&](const ClipInfo& clip) {
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

WorkbenchDocument CreateWorkbenchProject(std::vector<AssetReference> assets) {
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

void AppendWorkbenchMovie(WorkbenchDocument& document, AssetReference asset) {
  const auto duration = Available(asset, 24 * 60 * 60);
  const auto before = document.revision;
  const auto main = std::ranges::find(before->tracks, TrackKind::Video, &TrackInfo::kind);
  const auto begin = main != before->tracks.end() && !main->clips.empty() ?
      main->clips.back().placement.timeline.end : MediaTimestamp{};
  const TimeRange timeline{begin, AddWorkbenchTime(begin, duration)};
  const auto audio = std::ranges::find_if(before->tracks, [&](const TrackInfo& track) {
    return track.kind == TrackKind::Audio && TrackAvailable(track, timeline);
  });
  const auto video_source = Track(asset, TrackKind::Video);
  const auto audio_match = std::ranges::find(asset.tracks, TrackKind::Audio, &AssetTrack::kind);
  const auto audio_source = audio_match == asset.tracks.end() ? std::optional<AssetTrack>{} : *audio_match;
  const auto name = asset.file.Stem();
  document.revision = document.project->Edit([&](ProjectEdit& edit) {
    const auto id = edit.Import(std::move(asset));
    if (before->tracks.empty() && video_source.display_size) {
      const auto size = *video_source.display_size;
      edit.SetCanvas({std::max(2U, size.width & ~1U), std::max(2U, size.height & ~1U)});
    }
    const auto insert = [&](const AssetTrack& source, auto track) {
      const auto output = track == before->tracks.end() ? edit.AddTrack(source.kind) : track->id;
      const auto start = source.presentation.start;
      return edit.InsertClip(output, {{id, source.id, {start, AddWorkbenchTime(start, duration)}}, timeline});
    };
    const auto owner = insert(video_source, main);
    if (audio_source) edit.BindAudio(insert(*audio_source, audio), owner);
  });
  if (before->assets.empty()) document.title = name;
  document.dirty = true;
}

void InsertWorkbenchLayer(WorkbenchDocument& document, AssetReference asset, TrackKind kind, MediaTimestamp position) {
  const auto duration = Available(asset, 24 * 60 * 60, kind);
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
      edit.SetLayout(clip, FitWorkbenchInset(bounds, document.revision->canvas, source.display_size));
      edit.SetClipFill(clip, true);
    }
  });
  document.dirty = true;
}

std::optional<PixelSize> WorkbenchVideoSize(const ProjectSnapshot& project, const ClipInfo& clip) {
  const auto source = clip.placement.source;
  const auto asset = std::ranges::find(project.assets, source.asset, &AssetInfo::id);
  if (asset == project.assets.end()) return {};
  const auto track = std::ranges::find(asset->source.tracks, source.track, &AssetTrack::id);
  return track == asset->source.tracks.end() ? std::nullopt : track->display_size;
}

Rect FitWorkbenchInset(Rect bounds, PixelSize canvas, std::optional<PixelSize> source) {
  if (!source || !source->width || !source->height) return bounds;
  const auto ratio = float(source->width) * canvas.height / (float(source->height) * canvas.width);
  if (std::abs(bounds.width - bounds.height * ratio) < 1e-5F) return bounds;
  const auto width = std::min(bounds.width, bounds.height * ratio);
  const auto height = width / ratio;
  return {bounds.x + (bounds.width - width) / 2, bounds.y + (bounds.height - height) / 2, width, height};
}

std::shared_ptr<const ProjectSnapshot> PrepareWorkbenchPicture(std::shared_ptr<const ProjectSnapshot> project) {
  if (!project) return {};
  std::shared_ptr<ProjectSnapshot> fitted;
  bool main = true;
  for (std::size_t lane = 0; lane < project->tracks.size(); ++lane) {
    const auto& track = project->tracks[lane];
    if (track.kind != TrackKind::Video || std::exchange(main, false)) continue;
    for (std::size_t index = 0; index < track.clips.size(); ++index) {
      const auto& clip = track.clips[index];
      if (clip.destination == Rect{0, 0, 1, 1} || std::ranges::any_of(project->transitions, [&](const auto& transition) {
            return transition.from == clip.id || transition.to == clip.id;
          })) continue;
      const auto bounds = FitWorkbenchInset(clip.destination, project->canvas, WorkbenchVideoSize(*project, clip));
      if (clip.fill && clip.destination == bounds) continue;
      if (!fitted) fitted = std::make_shared<ProjectSnapshot>(*project);
      auto& picture = fitted->tracks[lane].clips[index];
      picture.destination = bounds;
      picture.fill = true;
    }
  }
  return fitted ? fitted : project;
}

void ApplyWorkbenchTrim(ProjectEdit& edit, const ProjectSnapshot& project, ClipId id,
                        ClipPlacement placement, bool main_track) {
  edit.SetClip(id, placement);
  if (!main_track) return;
  const auto primary = std::ranges::find(project.tracks, TrackKind::Video, &TrackInfo::kind);
  if (primary == project.tracks.end()) throw std::invalid_argument("The project has no main video track");
  const auto clip = std::ranges::find(primary->clips, id, &ClipInfo::id);
  if (clip == primary->clips.end()) throw std::invalid_argument("The clip is not on the main video track");
  const auto previous_end = clip->placement.timeline.end;
  const auto shift = AddWorkbenchTime(placement.timeline.end, {-previous_end.ticks, previous_end.timescale});
  const auto shifted = [shift](TimeRange range) {
    return TimeRange{AddWorkbenchTime(range.start, shift), AddWorkbenchTime(range.end, shift)};
  };
  for (auto following = clip + 1; following != primary->clips.end(); ++following) {
    auto next = following->placement;
    next.timeline = shifted(next.timeline);
    edit.SetClip(following->id, next);
    for (const auto& transition : project.transitions) {
      if (transition.from == following->id) edit.SetTransitionRange(transition.id, shifted(transition.spec.range));
    }
  }
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
  const auto has_audio = [](const AssetReference& asset) {
    return std::ranges::find(asset.tracks, TrackKind::Audio, &AssetTrack::kind) != asset.tracks.end();
  };
  const TemplateClip a{.slot = "first", .source = {{}, first}, .timeline = {{}, first},
                       .include_original_audio = has_audio(assets[0])};
  const TemplateClip b{.slot = "second", .source = {{}, second},
                       .timeline = inset ? TimeRange{{}, second} : TimeRange{first, AddWorkbenchTime(first, second)},
                       .destination = inset ? FitWorkbenchInset({0.66F, 0.63F, 0.30F, 0.30F}, canvas,
                           Track(assets[1], TrackKind::Video).display_size) : Rect{0, 0, 1, 1},
                       .include_original_audio = !inset && has_audio(assets[1])};
  EditTemplateRecipe recipe{{"avkit.workbench", "1", key}, canvas, {"first", "second"}};
  if (inset) recipe.tracks = {{TrackKind::Video, {a}}, {TrackKind::Video, {b}}};
  else recipe.tracks = {{TrackKind::Video, {a, b}}};
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
