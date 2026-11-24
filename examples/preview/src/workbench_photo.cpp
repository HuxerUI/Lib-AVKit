#include "workbench_media.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "render/bgfx/video_texture_internal.h"

namespace avkit_preview {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::media;

AssetReference PrepareWorkbenchPhoto(Bitmap image, const File& destination) {
  constexpr std::uint32_t max_edge = 1920;
  constexpr std::int64_t duration_seconds = 3;
  const auto scale = std::min(1.0, double(max_edge) / std::max(image.PixelWidth(), image.PixelHeight()));
  const auto even = [scale](std::uint32_t pixels) {
    return 2U * static_cast<std::uint32_t>(std::max(1.0, std::round(pixels * scale / 2.0)));
  };
  const auto width = even(image.PixelWidth()), height = even(image.PixelHeight());
  FileExportSettings settings;
  settings.policy = CodecPolicy::SoftwareOnly;
  settings.video.input = {.width = width, .height = height, .visible = {0, 0, width, height},
      .pixel_format = VideoPixelFormat::I420, .primaries = ColorPrimaries::BT709, .transfer = ColorTransfer::BT709,
      .matrix = ColorMatrix::BT709, .range = ColorRange::Limited};
  settings.video.frame_rate_numerator = 1;
  const AudioFormat clock{48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Mono};
  const auto converted = render::detail::EncodeCpuBitmap(image.Resize(width, height), settings.video.input, {0, 1},
                                                          MediaTimestamp{1, 1});
  if (!converted.Succeeded()) throw std::runtime_error(converted.Error().message);
  std::int64_t second{};
  WriteLiveMovie(destination, {{}, CreateFFmpegCodecProvider()}, settings, clock,
      [&](const auto&) -> std::optional<VideoFrame> {
        if (second == duration_seconds) return std::nullopt;
        auto frame = converted.Value();
        frame.pts = {second++, 1};
        return frame;
      }, {});
  return InspectTimelineMovie(destination);
}

} // namespace avkit_preview
