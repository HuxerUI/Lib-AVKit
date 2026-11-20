#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/avkit/project.h>

namespace avkit_preview {

struct WorkbenchDocument {
  std::shared_ptr<huxerui::avkit::Project> project;
  std::shared_ptr<const huxerui::avkit::ProjectSnapshot> revision;
  std::optional<huxerui::File> file;
  std::string title;
  bool dirty{};
  bool operator==(const WorkbenchDocument&) const = default;
};

enum class WorkbenchTemplate { StageCut, Portrait, PictureInPicture };

huxerui::File WorkbenchDirectory(const huxerui::File& base, std::string_view name);
huxerui::File WorkbenchOutput(const huxerui::File& base, std::string_view directory, std::string_view filename);
std::vector<huxerui::File> RecentWorkbenchProjects(const huxerui::File& data);
WorkbenchDocument EmptyWorkbenchProject();
WorkbenchDocument OpenWorkbenchProject(const huxerui::File& file);
void AppendWorkbenchMovie(WorkbenchDocument& document, huxerui::avkit::AssetReference asset);
WorkbenchDocument MakeWorkbenchTemplate(WorkbenchTemplate kind, std::array<huxerui::avkit::AssetReference, 2> assets);

} // namespace avkit_preview
