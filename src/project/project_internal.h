#pragma once

#include <huxerui/avkit/project.h>

namespace huxerui::avkit::detail {

void ValidateProject(const ProjectSnapshot& snapshot);
const AssetSource& FindAsset(const ProjectSnapshot& snapshot, const ResourceRef& reference);
const AssetTrack& FindSourceTrack(const AssetSource& asset, std::uint32_t id);

} // namespace huxerui::avkit::detail
