#include "media/image_asset.h"

#include <huxerui/avkit/timeline.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace huxerui::avkit {

AssetSource InspectTimelineImage(const File& file, media::MediaTimestamp default_duration) {
  if (!default_duration.timescale || default_duration.ticks <= 0)
    throw std::invalid_argument("An image insertion duration must be positive");
  const auto image = render::detail::DecodeResourceImage(file);
  return {file, {{1, media::TrackKind::Video, {{}, default_duration}, image.info.size}}, true};
}

AssetSource InspectTimelineVisual(const File& file, media::MediaTimestamp image_duration) {
  auto extension = file.Extension();
  std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return std::tolower(c); });
  if (extension == ".png" || extension == ".jpg" || extension == ".jpeg")
    return InspectTimelineImage(file, image_duration);
  return InspectTimelineMovie(file);
}

} // namespace huxerui::avkit
