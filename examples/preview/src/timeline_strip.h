#pragma once

#include <huxerui/avkit/asset_sampling.h>
#include <huxerui/paint.h>
#include <huxerui/view.h>

namespace avkit_preview {

struct StripViewport {
  huxerui::avkit::TimeRange range;
  huxerui::media::MediaTimestamp thumbnail_step, bucket_step;
  bool operator==(const StripViewport&) const = default;
};

struct StripThumbnail {
  huxerui::avkit::TrackId track;
  huxerui::avkit::ClipId clip;
  huxerui::avkit::TimeRange project;
  huxerui::media::MediaTimestamp source_time;
  std::optional<huxerui::avkit::Thumbnail> thumbnail;
  bool placeholder_source{};
};

struct StripBucket {
  huxerui::avkit::TrackId track;
  huxerui::avkit::ClipId clip;
  huxerui::avkit::TimeRange project, source;
  std::vector<huxerui::avkit::WaveformPeak> peaks;
};

struct StripResult {
  std::uint64_t generation, revision;
  StripViewport viewport;
  std::vector<StripThumbnail> thumbnails;
  std::vector<StripBucket> waveform;
  std::string error;
};

struct StripWorkerStats { std::uint64_t started{}, cancelled{}, delivered{}; };

class StripWorker {
public:
  using Publish = std::function<void(std::shared_ptr<const StripResult>)>;
  StripWorker(huxerui::avkit::MovieCodecProviders providers, huxerui::avkit::MakeThumbnail make_thumbnail);
  ~StripWorker();
  std::uint64_t Schedule(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project, StripViewport viewport,
                         Publish publish, Publish progress = {});
  void Cancel(std::uint64_t generation);
  StripWorkerStats Stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

inline constexpr std::uint32_t kStripClock = 192;
StripViewport VisibleStrip(const huxerui::avkit::ProjectSnapshot& project, std::int64_t left, unsigned zoom);
huxerui::avkit::ThumbnailImage MakeStripThumbnail(const huxerui::media::VideoFrame& frame, huxerui::avkit::PixelSize size);
inline void DrawStripThumbnail(huxerui::PaintContext& paint, const huxerui::avkit::Thumbnail& thumbnail, huxerui::Rect rect) {
  std::visit([&](const auto& image) { paint.DrawImage(image, rect); }, thumbnail.image);
}
void PaintTimelineStrip(huxerui::PaintContext& paint, huxerui::Size size,
                        const huxerui::avkit::ProjectSnapshot& project, const StripResult& strip);

struct StripPreviewOptions {
  huxerui::avkit::Project project;
  huxerui::File replacement;
  StripWorker worker;
};

void ConfigureTimelineStrip(std::string_view project, std::string_view replacement);
std::shared_ptr<StripPreviewOptions> TimelineStripOptions();
huxerui::View TimelineStripPreview(std::shared_ptr<huxerui::ExternalTexture> picture);
void VerifyTimelineStrip(std::string_view first, std::string_view replacement, std::string_view directory);

} // namespace avkit_preview
