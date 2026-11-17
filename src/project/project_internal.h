#pragma once

#include <huxerui/avkit/project.h>

namespace huxerui::avkit::detail {

void ValidateProject(const ProjectSnapshot& snapshot);
const AssetInfo& FindAsset(const ProjectSnapshot& snapshot, AssetId id);
const AssetTrack& FindSourceTrack(const AssetReference& asset, std::uint32_t id);

} // namespace huxerui::avkit::detail
