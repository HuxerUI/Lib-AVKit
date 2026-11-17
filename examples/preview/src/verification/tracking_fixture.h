#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>

#include <huxerui/avkit/analysis.h>
#include <huxerui/avkit/file_movie.h>
#include <huxerui/avkit/project.h>

namespace avkit_preview::verification {

inline constexpr huxerui::avkit::PixelSize kCanvas{320, 180};
inline constexpr int kRate = 24, kLength = 96, kClipLength = 72, kCrossing = 48;
inline constexpr int kMissingLeft = 42, kWrongFace = 47, kCoincidentEyes = 50, kMissingBody = 52, kWeakRight = 55;
inline constexpr int kAmbiguousWrist = 66;
inline constexpr std::uint64_t kPrimary = 101, kSecondary = 707;
constexpr huxerui::media::MediaTimestamp Tick(int frame) { return {frame, kRate}; }
constexpr huxerui::avkit::TimeRange Interval(int first, int end) { return {Tick(first), Tick(end)}; }

struct Actor {
  huxerui::Point head, right_eye, left_eye, left_wrist, right_wrist;
};

Actor Pose(std::uint64_t person, int frame);

using DetectionResult =
    huxerui::Result<std::optional<std::vector<huxerui::avkit::analysis::TrackingDetection>>, std::string>;

struct Pending {
  std::uint64_t id;
  huxerui::avkit::analysis::FrameKey target;
  std::stop_token stop;
  huxerui::avkit::analysis::AnalysisSession::Completion complete;
  std::vector<huxerui::avkit::analysis::TrackingDetection> detections;
};

struct Trace {
  huxerui::avkit::analysis::AnalysisCapability capability;
  huxerui::avkit::analysis::FrameKey key;
  std::size_t window_size;
};

struct MockState {
  explicit MockState(bool has_hands = true, int held = 0) : hands(has_hands), holds(held) {}

  Pending TakeHeld();

  bool hands;
  int holds;
  std::size_t opens{};
  std::vector<Trace> trace;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Pending> pending;
};

std::shared_ptr<huxerui::avkit::analysis::AnalysisProvider> MakeMockTrackingProvider(std::shared_ptr<MockState> state);
void WriteFixture(const huxerui::File& file, const huxerui::avkit::MovieCodecProviders& providers,
                  huxerui::media::VideoEncoderConfig video);

} // namespace avkit_preview::verification
