#include "tracking_fixture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <span>
#include <stdexcept>
#include <utility>

namespace avkit_preview::verification {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::analysis;
using namespace huxerui::media;
using enum AnalysisCapability;
using enum TrackedPart;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int FrameNumber(MediaTimestamp pts) {
  const auto frame = pts.Rescale(kRate);
  Check(frame && *frame == pts && frame->ticks >= 0 && frame->ticks < kLength, "Unexpected fixture source PTS");
  return static_cast<int>(frame->ticks);
}

Landmark PointAt(std::string name, Point point, float confidence = 1) {
  return {std::move(name), point, confidence};
}

TrackedPoints FaceAt(std::uint64_t person, const Actor& pose) {
  return {8000 + person, {PointAt("face_center", pose.head), PointAt("right_eye", pose.right_eye),
                           PointAt("left_eye", pose.left_eye)}};
}

AnalysisWindow Window(AnalysisCapability capability) {
  switch (capability) {
    case BodyLandmarks: return {2, 1, Tick(1), false, true};
    case FaceLandmarks: return {};
    case HandLandmarks: return {1, 2, Tick(1), false, false};
  }
  throw std::invalid_argument("Unknown mock model");
}

std::vector<TrackingDetection> Detect(const AnalysisJob& job, AnalysisCapability capability) {
  const int frame = FrameNumber(job.target.pts);
  std::vector<TrackingDetection> results;
  if (capability == BodyLandmarks) {
    Check(job.regions.empty(), "Body model received regions");
    for (const auto person : {kSecondary, kPrimary}) {
      if (person == kPrimary && frame == kMissingBody) continue;
      const auto pose = Pose(person, frame);
      results.push_back({{person, {PointAt("head", pose.head), PointAt("left_wrist", pose.left_wrist),
                                  PointAt("right_wrist", pose.right_wrist)}}, {}});
    }
    if (frame % 2) std::ranges::reverse(results);
    return results;
  }
  if (capability == FaceLandmarks && job.regions.empty()) {
    for (const auto person : {kSecondary, kPrimary}) results.push_back({FaceAt(person, Pose(person, frame)), {}});
    return results;
  }
  for (std::size_t index = job.regions.size(); index-- > 0;) {
    const auto& region = job.regions[index];
    const auto person = region.person.value;
    const auto primary = person == kPrimary;
    auto pose = Pose(person, frame);
    if (capability == FaceLandmarks) {
      Check(region.part == Face && region.bounds.Contains(pose.head), "Face region is attached to the wrong person");
      if (primary && frame == kWrongFace) pose = Pose(kSecondary, frame);
      if (primary && frame == kCoincidentEyes) pose.left_eye = pose.right_eye;
      results.push_back({FaceAt(person, pose), index});
    } else {
      Check(region.part == LeftHand || region.part == RightHand, "Hand request lacks a subject side");
      const bool left = region.part == LeftHand;
      auto center = left ? pose.left_wrist : pose.right_wrist;
      Check(region.bounds.Contains(center), "Hand region is attached to the wrong person or side");
      if (primary && left && frame == kMissingLeft) continue;
      if (primary && !left && frame == kAmbiguousWrist) {
        center = (center + Pose(kSecondary, frame).left_wrist) * 0.5F;
        Check(region.bounds.Contains(center) && std::ranges::any_of(job.regions, [&](const auto& other) {
          return other.person.value == kSecondary && other.part == LeftHand && other.bounds.Contains(center);
        }), "Ambiguous wrist fixture must overlap both tracking regions");
      }
      const auto confidence = primary && !left && frame == kWeakRight ? 0.2F : 1.0F;
      results.push_back({{16000 + person * 2 + left, {PointAt("wrist", center, confidence)}}, index});
    }
  }
  return results;
}

class MockSession final : public AnalysisSession {
public:
  MockSession(std::shared_ptr<MockState> state, AnalysisConfig config) : state_(std::move(state)), config_(config) {}

  void Submit(AnalysisJob job, std::stop_token stop, Completion complete) override {
    const auto frame = FrameNumber(job.target.pts);
    const auto window = Window(config_.capability);
    const auto first = std::max(0, frame - int(window.past_frames));
    const auto end = std::min(kLength, frame + int(window.future_frames) + 1);
    Check(window.accepts_gaps || end - first == int(window.past_frames + window.future_frames + 1),
          "Strict model was submitted an incomplete window");
    Check(job.frames.size() == std::size_t(end - first), "Analysis history or lookahead is missing");
    for (int i = first; i < end; ++i) {
      const auto& input = job.frames[i - first];
      Check(input.key.source == job.target.source && input.key.generation == job.target.generation &&
                input.key.pts == Tick(i) && input.image.pts == Tick(i) &&
                input.image.format == config_.format && input.image.Storage() == config_.storage && input.image.Validate(),
            "Analysis input leaked across sources or does not match its retained presentation frame");
    }
    auto detections = Detect(job, config_.capability);
    state_->trace.push_back({config_.capability, job.target, job.frames.size()});
    if (config_.capability == BodyLandmarks && state_->holds > 0 && frame == kCrossing) {
      --state_->holds;
      const std::lock_guard lock(state_->mutex);
      state_->pending.push_back({job.id, job.target, stop, std::move(complete), std::move(detections)});
      state_->changed.notify_one();
      return;
    }
    complete({job.id, job.target, DetectionResult::Success(std::move(detections))});
  }

private:
  std::shared_ptr<MockState> state_;
  AnalysisConfig config_;
};

class MockProvider final : public AnalysisProvider {
public:
  explicit MockProvider(std::shared_ptr<MockState> state) : state_(std::move(state)) {}

  Result<AnalysisWindow, std::string> Probe(const AnalysisConfig& config) const override {
    using ProbeResult = Result<AnalysisWindow, std::string>;
    if (!state_->hands && config.capability == HandLandmarks) return ProbeResult::Failure("Mock hands are disabled");
    if ((config.capability == BodyLandmarks && config.uses_roi) ||
        (config.capability == HandLandmarks && !config.uses_roi) || !config.format.Validate())
      return ProbeResult::Failure("Mock model cannot use the requested input");
    return ProbeResult::Success(Window(config.capability));
  }

  std::unique_ptr<AnalysisSession> Open(const AnalysisConfig& config) override {
    ++state_->opens;
    return std::make_unique<MockSession>(state_, config);
  }

private:
  std::shared_ptr<MockState> state_;
};

bool InArm(Point point, Point shoulder, Point wrist) {
  const auto pixel = [](Point p) { return Point{p.x * kCanvas.width, p.y * kCanvas.height}; };
  const auto line = pixel(wrist - shoulder);
  const auto offset = pixel(point - shoulder);
  const auto t = std::clamp((line.x * offset.x + line.y * offset.y) / (line.x * line.x + line.y * line.y), 0.0F, 1.0F);
  const auto distance = offset - line * t;
  return distance.x * distance.x + distance.y * distance.y <= 22;
}

VideoFrame Picture(const VideoEncoderConfig& config, int frame) {
  Check(config.input_storage == VideoStorage::Cpu && config.input.pixel_format == VideoPixelFormat::I420,
        "Tracking fixture requires I420 encoder input");
  const auto width = kCanvas.width, height = kCanvas.height;
  const auto area = width * height;
  auto bytes = std::make_shared<Bytes>(area * 3 / 2);
  const auto a = Pose(kPrimary, frame), b = Pose(kSecondary, frame);
  const auto write = [&](std::uint32_t x, std::uint32_t y, std::array<int, 3> color) {
    const auto [luma, u, v] = color;
    (*bytes)[y * width + x] = std::byte(luma);
    if (!(x % 2) && !(y % 2)) {
      const auto index = y / 2 * (width / 2) + x / 2;
      (*bytes)[area + index] = std::byte(u);
      (*bytes)[area + area / 4 + index] = std::byte(v);
    }
  };
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const Point p{(x + 0.5F) / width, (y + 0.5F) / height};
      std::array color{35 + int(y / 12), 139, 118};
      for (const auto& pose : {b, a}) {
        const auto d = p - pose.head;
        if (Rect{-0.04F, 0.09F, 0.08F, 0.24F}.Contains(d) ||
            InArm(p, pose.head + Point{0.024F, 0.12F}, pose.left_wrist) ||
            InArm(p, pose.head + Point{-0.024F, 0.12F}, pose.right_wrist))
          color = pose.head.y == a.head.y ? std::array{117, 101, 190} : std::array{129, 89, 91};
        if (std::hypot(d.x * 320, d.y * 180) < 17) color = {171, 114, 147};
        for (const auto eye : {pose.right_eye, pose.left_eye}) {
          if (std::hypot((p.x - eye.x) * 320, (p.y - eye.y) * 180) < 2) color = {40, 128, 128};
        }
      }
      write(x, y, color);
    }
  }
  const auto span = std::span<const std::byte>(*bytes);
  return {config.input, Tick(frame), Tick(1), {
      {BufferReference(span.first(area), bytes), width, 1},
      {BufferReference(span.subspan(area, area / 4), bytes), width / 2, 1},
      {BufferReference(span.last(area / 4), bytes), width / 2, 1}}};
}

} // namespace

Actor Pose(std::uint64_t person, int frame) {
  const bool primary = person == kPrimary;
  Check(primary || person == kSecondary, "Unknown mock person");
  const auto t = float(frame - kCrossing);
  const auto x = 0.5F + (primary ? t : -t) / 160;
  const auto y = primary ? 0.255F : 0.49F;
  const auto eye = 0.026F + t / 10000, tilt = 0.013F + t / 15000;
  return {{x, y}, {x - eye, y - tilt}, {x + eye, y + tilt},
          {x + 0.112F, y + 0.158F}, {x - 0.112F, y + 0.22F}};
}

Pending MockState::TakeHeld() {
  std::unique_lock lock(mutex);
  Check(changed.wait_for(lock, std::chrono::seconds(8), [&] { return !pending.empty(); }),
        "Timed out waiting for the mock analysis job");
  auto result = std::move(pending.front());
  pending.pop_front();
  return result;
}

std::shared_ptr<AnalysisProvider> MakeMockTrackingProvider(std::shared_ptr<MockState> state) {
  return std::make_shared<MockProvider>(std::move(state));
}

void WriteFixture(const File& file, const MovieCodecProviders& providers, VideoEncoderConfig video) {
  FileExportSettings settings{std::move(video)};
  settings.video.frame_rate_numerator = kRate;
  settings.policy = CodecPolicy::SoftwareOnly;
  const AudioFormat pcm{48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Mono};
  int frame = 0, audio_at = 0;
  WriteLiveMovie(file, providers, settings, pcm, [&](const auto& config) -> std::optional<VideoFrame> {
    if (frame == kLength) return std::nullopt;
    return Picture(config, frame++);
  }, [&]() -> std::optional<AudioFrame> {
    const auto count = std::min(1024, kLength * int(pcm.sample_rate) / kRate - audio_at);
    if (!count) return std::nullopt;
    auto silence = std::make_shared<Bytes>(count * sizeof(std::int16_t));
    AudioFrame output{pcm, {audio_at, pcm.sample_rate}, std::uint32_t(count), {BufferReference(*silence, silence)}};
    audio_at += count;
    return output;
  });
}

} // namespace avkit_preview::verification
