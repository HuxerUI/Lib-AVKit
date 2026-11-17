#pragma once

#include <span>
#include <string_view>

#include <huxerui/avkit/analysis.h>

namespace huxerui::avkit::detail {

struct TrackingFrame {
  std::vector<analysis::PersonSample> people;
  std::optional<analysis::PersonTrackId> primary;
};

class SourceTracking {
public:
  using ReadWindow = std::function<std::vector<media::VideoFrame>(const media::VideoFrame&, const analysis::AnalysisWindow&)>;
  using ReadReference = std::function<std::optional<media::VideoFrame>()>;

  SourceTracking(std::shared_ptr<analysis::AnalysisProvider> provider, std::uint64_t source,
                 std::span<const analysis::TrackedPart> parts);
  ~SourceTracking();

  const TrackingFrame& Sample(const media::VideoFrame& image, const ReadWindow& read, const ReadReference& reference,
                              std::stop_token stop);
  void Reset();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::optional<Point> FindLandmark(const analysis::PartSample& part, std::string_view name);

} // namespace huxerui::avkit::detail
