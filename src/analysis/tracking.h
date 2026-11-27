#pragma once

#include <span>
#include <string_view>

#include <huxerui/avkit/analysis.h>

namespace huxerui::avkit::detail {

struct TrackingFrame {
  std::vector<analysis::TrackedPersonSample> people;
  std::optional<analysis::PersonTrackId> primary;
};

class SourceTracking {
public:
  using ReadWindow = std::function<std::vector<media::VideoFrame>(const media::VideoFrame&, const analysis::AnalysisWindow&)>;
  using ReadReference = std::function<std::optional<media::VideoFrame>()>;
  using ReadNext = std::function<std::optional<media::VideoFrame>(std::stop_token)>;
  using OpenOrdered = std::function<ReadNext()>;
  struct Input {
    ReadWindow read;
    ReadReference reference;
    OpenOrdered open_ordered;
  };

  SourceTracking(std::shared_ptr<analysis::AnalysisProvider> provider, std::uint64_t source,
                 std::span<const analysis::TrackedPart> parts);

  ~SourceTracking();

  const TrackingFrame& Sample(const media::VideoFrame& image, const ReadWindow& read, const ReadReference& reference,
                              const OpenOrdered& open_ordered, std::stop_token stop);

  void Reset();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::optional<Point> FindLandmark(const analysis::TrackedPartSample& part, std::string_view name);

} // namespace huxerui::avkit::detail
