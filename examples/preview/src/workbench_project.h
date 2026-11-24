#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/avkit/project.h>

namespace avkit_preview {

struct WorkbenchCaption {
  double begin, end;
  std::string text;
  bool operator==(const WorkbenchCaption&) const = default;
};

struct WorkbenchDocument {
  std::shared_ptr<huxerui::avkit::Project> project;
  std::shared_ptr<const huxerui::avkit::ProjectSnapshot> revision;
  std::optional<huxerui::File> file;
  std::string title;
  bool dirty{};
  bool operator==(const WorkbenchDocument&) const = default;
};

enum class WorkbenchTemplate { StageCut, Portrait, PictureInPicture };

huxerui::media::MediaTimestamp AddWorkbenchTime(huxerui::media::MediaTimestamp time, huxerui::media::MediaTimestamp offset);

inline constexpr std::array<huxerui::Rect, 4> kWorkbenchInsetPositions{{
    {0.61F, 0.06F, 0.33F, 0.33F}, {0.61F, 0.61F, 0.33F, 0.33F},
    {0.06F, 0.06F, 0.33F, 0.33F}, {0.06F, 0.61F, 0.33F, 0.33F},
}};

huxerui::File WorkbenchDirectory(const huxerui::File& base, std::string_view name);
huxerui::File WorkbenchOutput(const huxerui::File& base, std::string_view directory, std::string_view filename);
std::vector<huxerui::File> RecentWorkbenchProjects(const huxerui::File& data);
WorkbenchDocument EmptyWorkbenchProject();
WorkbenchDocument CreateWorkbenchProject(std::vector<huxerui::avkit::AssetReference> assets);
WorkbenchDocument OpenWorkbenchProject(const huxerui::File& file);
std::vector<WorkbenchCaption> ReadWorkbenchCaptions(const huxerui::File& file);
void AppendWorkbenchMovie(WorkbenchDocument& document, huxerui::avkit::AssetReference asset);
void InsertWorkbenchLayer(WorkbenchDocument& document, huxerui::avkit::AssetReference asset,
                          huxerui::media::TrackKind kind, huxerui::media::MediaTimestamp position);
std::optional<huxerui::avkit::PixelSize> WorkbenchVideoSize(const huxerui::avkit::ProjectSnapshot& project,
                                                         const huxerui::avkit::ClipInfo& clip);
huxerui::Rect FitWorkbenchInset(huxerui::Rect bounds, huxerui::avkit::PixelSize canvas,
                                std::optional<huxerui::avkit::PixelSize> source);
std::shared_ptr<const huxerui::avkit::ProjectSnapshot> PrepareWorkbenchPicture(
    std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project);
void ApplyWorkbenchTrim(huxerui::avkit::ProjectEdit& edit, const huxerui::avkit::ProjectSnapshot& project,
                        huxerui::avkit::ClipId clip, huxerui::avkit::ClipPlacement placement, bool main_track);
WorkbenchDocument MakeWorkbenchTemplate(WorkbenchTemplate kind, std::array<huxerui::avkit::AssetReference, 2> assets);

} // namespace avkit_preview
