#include "ncnn_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace huxerui::avkit::analysis::detail {
namespace {

using namespace media;
using Detections = std::vector<LandmarkDetection>;
using DetectionResult = Result<std::optional<Detections>, std::string>;

constexpr Point kRight{1, 0}, kDown{0, 1};
constexpr Rect kImageBounds{0, 0, 1, 1};
constexpr std::size_t kMaxPeople = 6;

Point Add(Point a, Point b) { return {a.x + b.x, a.y + b.y}; }

Point Difference(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }

Point Scale(Point p, float factor) { return {p.x * factor, p.y * factor}; }

float Dot(Point a, Point b) { return a.x * b.x + a.y * b.y; }

float Length(Point p) { return std::hypot(p.x, p.y); }

Point Normalize(const CpuImage& image, Point p) { return {p.x / image.width, p.y / image.height}; }

float Sigmoid(float v) { return 1.F / (1.F + std::exp(-std::clamp(v, -80.F, 80.F))); }

std::array<float, 3> Pixel(const CpuImage& picture, int x, int y) {
  if (x < 0 || y < 0 || x >= picture.width || y >= picture.height) return {};
  const auto i = (std::size_t(y) * picture.width + x) * 3;
  return {float(picture.bgr[i + 2]), float(picture.bgr[i + 1]), float(picture.bgr[i])};
}

ncnn::Mat Output(ncnn::Extractor& extractor, const char* name, int width, int height = 1) {
  ncnn::Mat result;
  if (extractor.extract(name, result) || result.empty() || result.w != width || result.h != height ||
      result.c != 1 || result.elempack != 1 || result.elemsize != sizeof(float))
    throw std::runtime_error(std::string("Unexpected ncnn tensor: ") + name);
  return result;
}

struct Candidate {
  Rect box;
  std::array<Point, 7> points;
  float score;
};

float Overlap(const Rect& a, const Rect& b) {
  const float width = std::max(0.F, std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x));
  const float height = std::max(0.F, std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y));
  const float area = width * height;
  return area / (a.width * a.height + b.width * b.height - area);
}

struct AnchorGrid { int size, repeats; };

constexpr float kDetectionThreshold = 0.55F;

std::vector<Candidate> Detect(const CpuImage& image, const Network& model, bool person, const ModelCrop& crop,
                              float threshold = kDetectionThreshold) {
  constexpr std::array person_grids{AnchorGrid{28, 2}, AnchorGrid{14, 2}, AnchorGrid{7, 6}};
  constexpr std::array palm_grids{AnchorGrid{24, 2}, AnchorGrid{12, 6}};
  const auto grids = person ? std::span<const AnchorGrid>(person_grids) : std::span<const AnchorGrid>(palm_grids);
  const int size = person ? 224 : 192;
  const int anchors = person ? 2254 : 2016;
  const int landmarks = person ? 4 : 7;
  auto extraction = model.Extract(Warp(image, crop, size, person ? -1.F : 0.F));
  const auto scores = Output(extraction, person ? "out0" : "out1", 1, anchors);
  const auto regressions = Output(extraction, person ? "out1" : "out0", 4 + landmarks * 2, anchors);

  std::vector<Candidate> candidates;
  int index = 0;
  for (const auto [grid, repeats] : grids) {
    for (int y = 0; y < grid; ++y) {
      for (int x = 0; x < grid; ++x) {
        const Point anchor{(x + 0.5F) / grid, (y + 0.5F) / grid};
        for (int repeat = 0; repeat < repeats; ++repeat, ++index) {
          const float score = Sigmoid(scores.row(index)[0]);
          if (!(score >= threshold)) continue;
          const auto* values = regressions.row(index);
          const Point center = crop.ToSource(Add(anchor, {values[0] / size, values[1] / size}));
          const float local_width = values[2] * crop.width / size, local_height = values[3] * crop.height / size;
          const float width = std::abs(crop.right.x) * local_width + std::abs(crop.down.x) * local_height;
          const float height = std::abs(crop.right.y) * local_width + std::abs(crop.down.y) * local_height;
          if (!(width >= 2 && height >= 2 && std::isfinite(width * height) &&
                std::isfinite(center.x) && std::isfinite(center.y))) continue;
          Candidate candidate{{center.x - width / 2, center.y - height / 2, width, height}, {}, score};
          for (int point = 0; point < landmarks; ++point) {
            candidate.points[point] = crop.ToSource(Add(anchor, {values[4 + point * 2] / size,
                                                               values[5 + point * 2] / size}));
          }
          candidates.push_back(candidate);
        }
      }
    }
  }
  std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
  std::vector<Candidate> selected;
  for (auto& candidate : candidates) {
    if (std::ranges::none_of(selected, [&](const auto& kept) { return Overlap(kept.box, candidate.box) > 0.3F; }))
      selected.push_back(candidate);
    if (selected.size() == (person ? kMaxPeople : 12)) break;
  }
  return selected;
}

constexpr std::array kBodyNames{
    "head", "left_eye_inner", "left_eye", "left_eye_outer", "right_eye_inner", "right_eye", "right_eye_outer",
    "left_ear", "right_ear", "left_mouth_corner", "right_mouth_corner", "left_shoulder", "right_shoulder",
    "left_elbow", "right_elbow", "left_wrist", "right_wrist", "left_pinky", "right_pinky", "left_index", "right_index",
    "left_thumb", "right_thumb", "left_hip", "right_hip", "left_knee", "right_knee", "left_ankle", "right_ankle",
    "left_heel", "right_heel", "left_foot_index", "right_foot_index"};

using Appearance = std::array<float, 3>;
struct Body {
  std::vector<Landmark> points;
  Appearance appearance;
};

std::optional<Appearance> Torso(const CpuImage& image, const std::vector<Landmark>& points) {
  constexpr std::array indices{11, 12, 23, 24};
  for (const auto index : indices) {
    if (*points[index].confidence < 0.55F || !kImageBounds.Contains(points[index].position)) return std::nullopt;
  }
  const auto between = [](Point a, Point b, float t) { return Add(a, Scale(Difference(b, a), t)); };
  Appearance color{};
  constexpr std::array horizontal{0.35F, 0.5F, 0.65F};
  constexpr std::array vertical{0.2F, 0.35F, 0.5F};
  for (const float v : vertical) {
    for (const float u : horizontal) {
      const auto shoulders = between(points[11].position, points[12].position, u);
      const auto hips = between(points[23].position, points[24].position, u);
      const auto sample = between(shoulders, hips, v);
      const auto channels = Pixel(image, int(sample.x * image.width), int(sample.y * image.height));
      for (std::size_t c = 0; c < color.size(); ++c) color[c] += channels[c] / (9 * 255.F);
    }
  }
  return color;
}

std::optional<Body> Pose(const CpuImage& image, const Network& model, const Candidate& person, const ModelCrop& crop) {
  constexpr int size = 256;
  auto extraction = model.Extract(Warp(image, crop, size));
  const auto score = Output(extraction, "out1", 1);
  if (!(score[0] >= 0.55F)) return std::nullopt;
  const auto data = Output(extraction, "out0", 195);
  std::vector<Landmark> landmarks;
  landmarks.reserve(kBodyNames.size());
  for (std::size_t i = 0; i < kBodyNames.size(); ++i) {
    const auto point = Normalize(image, crop.ToSource({data[i * 5] / size, data[i * 5 + 1] / size}));
    const float certainty = std::min(Sigmoid(data[i * 5 + 3]), Sigmoid(data[i * 5 + 4]));
    landmarks.push_back({kBodyNames[i], point, std::isfinite(certainty) && kImageBounds.Contains(point) ? certainty : 0.F});
  }
  // An overlapping crop can return a pose for the other person even when the detector found distinct heads.
  const auto& head = landmarks.front();
  if (*head.confidence < 0.5F || !person.box.Contains({head.position.x * image.width, head.position.y * image.height}))
    return std::nullopt;
  const auto torso = Torso(image, landmarks);
  if (!torso) return std::nullopt;
  return Body{std::move(landmarks), *torso};
}

Point BoxCenter(const Rect& box) { return {box.x + box.width / 2, box.y + box.height / 2}; }

bool TorsoNearNeighbor(const Body& body, const Candidate& person, std::span<const Candidate> candidates, int width) {
  const auto center = BoxCenter(person.box);
  const auto hips = (body.points[23].position.x + body.points[24].position.x) * width / 2;
  return std::ranges::any_of(candidates, [&](const auto& other) {
    const auto delta = Difference(BoxCenter(other.box), center);
    return std::abs(delta.x) >= 0.55F * std::max(person.box.width, other.box.width) &&
           std::abs(delta.y) <= std::max(person.box.height, other.box.height) &&
           (hips - center.x) / delta.x > 0.35F;
  });
}

constexpr std::array kHandNames{
    "wrist", "thumb_cmc", "thumb_mcp", "thumb_ip", "thumb_tip", "index_mcp", "index_pip", "index_dip", "index_tip",
    "middle_mcp", "middle_pip", "middle_dip", "middle_tip", "ring_mcp", "ring_pip", "ring_dip", "ring_tip",
    "pinky_mcp", "pinky_pip", "pinky_dip", "pinky_tip"};

std::optional<std::vector<Landmark>> Hand(const CpuImage& image, const Network& model, const Candidate& palm,
                                          bool mirror = false, float crop_scale = 3.F, float rotation = 0.F,
                                          float forward_shift = 0.4F) {
  const auto fingers = Difference(palm.points[2], palm.points[0]);
  const auto length = Length(fingers);
  if (!(length > 2 && length < std::max(image.width, image.height))) return std::nullopt;
  auto down = Scale(fingers, -1.F / length);
  if (rotation != 0.F) {
    const auto cosine = std::cos(rotation), sine = std::sin(rotation);
    down = {cosine * down.x - sine * down.y, sine * down.x + cosine * down.y};
  }
  const Point right{down.y, -down.x};
  const auto pivot = palm.points[0];
  float left = 0, top = 0, right_edge = 0, bottom = 0;
  for (const auto point : palm.points) {
    const auto local = Difference(point, pivot);
    const auto x = Dot(local, right), y = Dot(local, down);
    left = std::min(left, x);
    right_edge = std::max(right_edge, x);
    top = std::min(top, y);
    bottom = std::max(bottom, y);
  }
  const auto center = Add(pivot, Add(Scale(right, (left + right_edge) / 2),
                                     Scale(down, (top + bottom) / 2 - forward_shift * (bottom - top))));
  const float size = crop_scale * std::max(right_edge - left, bottom - top);
  const ModelCrop crop{center, mirror ? Scale(right, -1.F) : right, down, size, size};
  constexpr int input = 224;
  auto extraction = model.Extract(Warp(image, crop, input));
  const auto score = Output(extraction, "out1", 1);
  if (!(score[0] >= 0.65F)) return std::nullopt;
  const auto data = Output(extraction, "out0", 63);
  std::vector<Landmark> points;
  points.reserve(kHandNames.size());
  for (std::size_t i = 0; i < kHandNames.size(); ++i) {
    const auto point = Normalize(image, crop.ToSource({data[i * 3] / input, data[i * 3 + 1] / input}));
    if (kImageBounds.Contains(point)) points.push_back({kHandNames[i], point, score[0]});
  }
  if (points.empty() || points.front().name != "wrist") return std::nullopt;
  return points;
}

float RegionDistance(Point point, const Rect& region) {
  if (!(region.width > 0 && region.height > 0) || !region.Contains(point)) return std::numeric_limits<float>::infinity();
  const auto x = (point.x - region.x) / region.width - 0.5F, y = (point.y - region.y) / region.height - 0.5F;
  return x * x + y * y;
}

std::optional<std::size_t> HandRegion(Point wrist, const std::vector<AnalysisRegion>& regions) {
  float nearest = std::numeric_limits<float>::infinity(), next = nearest;
  std::optional<std::size_t> index;
  for (std::size_t i = 0; i < regions.size(); ++i) {
    if (regions[i].part == TrackedPart::Face) continue;
    const auto distance = RegionDistance(wrist, regions[i].bounds);
    if (distance < nearest) {
      next = nearest;
      nearest = distance;
      index = i;
    } else next = std::min(next, distance);
  }
  if (!(next - nearest > 0.04F)) return std::nullopt;
  return index;
}

float ChromaDistance(const Appearance& a, const Appearance& b) {
  const auto brightness = (a[0] + a[1] + a[2] - b[0] - b[1] - b[2]) / 3;
  float sum = 0;
  for (std::size_t i = 0; i < a.size(); ++i) sum += std::abs(a[i] - b[i] - brightness);
  return sum / a.size();
}

constexpr unsigned kRecoveryFrames = 3;
constexpr float kUnmatched = 1.F, kAmbiguity = 0.12F;
constexpr std::array kPoseJoints{0, 11, 12, 23, 24};

struct BodyFeatures {
  Point center;
  float scale;
  float hip_offset;
  std::array<Point, kPoseJoints.size()> pose;
  Appearance appearance;
};

BodyFeatures Features(const Body& body) {
  const auto position = [&](int joint) { return body.points[joint].position; };
  const auto shoulders = Scale(Add(position(11), position(12)), 0.5F);
  const auto hips = Scale(Add(position(23), position(24)), 0.5F);
  const auto head = position(0);
  // Keep the motion center close to the head when a crop misplaces the hips during a turn.
  const Point center{0.9F * head.x + 0.1F * hips.x, 0.85F * head.y + 0.15F * hips.y};
  const auto scale = std::max(0.04F, Length(Difference(shoulders, hips)));
  BodyFeatures result{center, scale, hips.x - head.x, {}, body.appearance};
  for (std::size_t i = 0; i < kPoseJoints.size(); ++i)
    result.pose[i] = Scale(Difference(position(kPoseJoints[i]), shoulders), 1.F / scale);
  return result;
}

float PoseDistance(const BodyFeatures& a, const BodyFeatures& b) {
  float sum = 0;
  for (std::size_t i = 0; i < a.pose.size(); ++i) sum += Length(Difference(a.pose[i], b.pose[i]));
  return sum / a.pose.size() + 0.5F * std::abs(std::log(a.scale / b.scale));
}

struct Motion {
  BodyFeatures body;
  Point velocity{};
  double time;

  Point Predict(double at, double horizon = 0.25) const {
    const auto elapsed = float(std::min(horizon, at - time));
    return Add(body.center, Scale({velocity.x, 0.5F * velocity.y}, elapsed));
  }

  Motion Follow(const BodyFeatures& next, double at) const {
    const auto observed = Scale(Difference(next.center, body.center), float(1 / (at - time)));
    const auto bounded = Scale(observed, 2.5F / std::max(2.5F, Length(observed)));
    auto filtered = next;
    filtered.scale = 0.65F * body.scale + 0.35F * next.scale;
    return {filtered, Add(Scale(velocity, 0.55F), Scale(bounded, 0.45F)), at};
  }
};

struct BodyTrack {
  Motion confirmed;
  Appearance reference;
  std::optional<Motion> candidate;
  unsigned confirmations{};
  bool hidden{}, contested{};

  const Motion& Prediction() const { return candidate ? *candidate : confirmed; }

  bool Update(const BodyFeatures* body, double at) {
    if (!body) {
      hidden = true;
      candidate.reset();
      confirmations = 0;
      return false;
    }
    const auto next = Prediction().Follow(*body, at);
    if (hidden && ++confirmations < kRecoveryFrames) {
      candidate = next;
      return false;
    }
    confirmed = next;
    candidate.reset();
    confirmations = 0;
    hidden = contested = false;
    return true;
  }
};

using MatchCosts = std::array<std::array<float, kMaxPeople>, kMaxPeople>;

struct Assignment {
  float cost{};
  std::array<int, kMaxPeople> bodies{};
};

Assignment Assign(const MatchCosts& costs, std::size_t tracks, std::size_t bodies,
                  std::optional<std::pair<std::size_t, std::size_t>> excluded = {}) {
  // The detector caps the number of people, so a mask can represent every used observation.
  std::array<Assignment, 1 << kMaxPeople> suffix;
  for (std::size_t i = tracks; i-- > 0;) {
    for (std::size_t used = 0; used < std::size_t(1 << bodies); ++used) {
      auto best = suffix[used];
      best.cost += kUnmatched;
      best.bodies[i] = -1;
      for (std::size_t j = 0; j < bodies; ++j) {
        const auto bit = std::size_t(1) << j;
        if ((used & bit) || excluded == std::pair{i, j} || costs[i][j] >= kUnmatched) continue;
        const auto& next = suffix[used | bit];
        const auto cost = costs[i][j] + next.cost;
        if (cost < best.cost) {
          best = next;
          best.cost = cost;
          best.bodies[i] = int(j);
        }
      }
      suffix[used] = best;
    }
  }
  return suffix.front();
}

class BodyAssociation {
public:
  Detections Advance(std::vector<Body> bodies, const FrameKey& frame) {
    if (last_) {
      if (last_->source != frame.source || last_->generation != frame.generation || last_->pts.Compare(frame.pts) != -1)
        throw std::invalid_argument("ncnn body analysis requires ascending source PTS in one clip session");
      time_ += double((static_cast<long double>(frame.pts.ticks) * last_->pts.timescale -
                       static_cast<long double>(last_->pts.ticks) * frame.pts.timescale) /
                      (static_cast<long double>(frame.pts.timescale) * last_->pts.timescale));
    }
    last_ = frame;
    const auto at = time_;
    if (tracks_.empty()) return Initialize(bodies, at);

    std::vector<BodyFeatures> features;
    for (const auto& body : bodies) features.push_back(Features(body));
    const auto n = tracks_.size();
    MatchCosts costs{};
    std::array<std::array<bool, kMaxPeople>, kMaxPeople> restart{};
    for (std::size_t i = 0; i < n; ++i) {
      const auto& track = tracks_[i];
      for (std::size_t j = 0; j < features.size(); ++j) {
        if (at - track.confirmed.time <= 0.45 && InconsistentTorso(track.confirmed.body, features[j])) {
          costs[i][j] = kUnmatched;
          continue;
        }
        auto cost = Cost(track.Prediction(), features[j], at);
        if (track.candidate && !track.contested && at - track.confirmed.time <= 0.25 && cost >= kUnmatched - kAmbiguity) {
          const auto confirmed = Cost(track.confirmed, features[j], at) + kAmbiguity;
          if (confirmed < cost) {
            cost = confirmed;
            restart[i][j] = true;
          }
        }
        costs[i][j] = cost;
      }
    }
    auto assignment = Assign(costs, n, features.size());
    auto reliable = Reliable(costs, assignment, n, features.size());
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = i + 1; j < n; ++j) {
        if (reliable[i] && reliable[j]) continue;
        if (at - tracks_[i].confirmed.time > 0.95 || at - tracks_[j].confirmed.time > 0.95) continue;
        const auto& a = tracks_[i].Prediction();
        const auto& b = tracks_[j].Prediction();
        if (Length(Difference(a.Predict(at), b.Predict(at))) > 1.2F * std::max(a.body.scale, b.body.scale)) continue;
        if (!reliable[i] || tracks_[i].hidden) tracks_[i].contested = true;
        if (!reliable[j] || tracks_[j].hidden) tracks_[j].contested = true;
      }
    }
    bool reconsider = false;
    for (std::size_t i = 0; i < n; ++i) {
      const auto& track = tracks_[i];
      if (!track.contested && at - track.confirmed.time <= 0.45) continue;
      reconsider = true;
      restart[i].fill(false);
      for (std::size_t j = 0; j < features.size(); ++j)
        costs[i][j] = RecoveryCost(track, features[j], at);
    }
    if (reconsider) {
      assignment = Assign(costs, n, features.size());
      reliable = Reliable(costs, assignment, n, features.size());
    }

    Detections result;
    std::array<bool, kMaxPeople> assigned{};
    for (std::size_t i = 0; i < n; ++i) {
      auto& track = tracks_[i];
      const auto j = assignment.bodies[i];
      const BodyFeatures* match = reliable[i] ? &features[std::size_t(j)] : nullptr;
      if (j >= 0) assigned[std::size_t(j)] = true;
      if (match && restart[i][std::size_t(j)]) {
        track.candidate.reset();
        track.confirmations = 0;
      }
      if (track.Update(match, at)) result.push_back({{i + 1, std::move(bodies[std::size_t(j)].points)}, {}});
    }
    // A separated entrant can supply competing wrist regions without replacing the selected person.
    if (std::ranges::none_of(tracks_, &BodyTrack::contested)) {
      for (std::size_t j = 0; j < features.size() && tracks_.size() < kMaxPeople; ++j) {
        const auto& body = features[j];
        if (assigned[j] || std::ranges::any_of(tracks_, [&](const auto& track) {
              const auto& motion = track.Prediction();
              return Length(Difference(motion.Predict(at), body.center)) < std::max(0.18F, 1.4F * motion.body.scale);
            })) continue;
        tracks_.push_back({{body, {}, at}, body.appearance, {}, 1, true});
      }
    }
    return result;
  }

private:
  static bool InconsistentTorso(const BodyFeatures& previous, const BodyFeatures& body) {
    const auto sideways = std::abs(body.hip_offset - previous.hip_offset);
    const bool distorted = sideways > 0.25F * (previous.scale + body.scale) || body.scale < 0.55F * previous.scale;
    return distorted && ChromaDistance(previous.appearance, body.appearance) > 0.09F;
  }

  static std::array<bool, kMaxPeople> Reliable(const MatchCosts& costs, const Assignment& best,
                                               std::size_t tracks, std::size_t bodies) {
    std::array<bool, kMaxPeople> reliable{};
    for (std::size_t i = 0; i < tracks; ++i) {
      const auto j = best.bodies[i];
      if (j >= 0)
        reliable[i] = Assign(costs, tracks, bodies, std::pair{i, std::size_t(j)}).cost - best.cost >= kAmbiguity;
    }
    return reliable;
  }

  static float Cost(const Motion& motion, const BodyFeatures& body, double at) {
    const auto elapsed = float(at - motion.time);
    const auto gate = std::clamp(0.42F * motion.body.scale + 0.42F * elapsed, 0.045F, 0.18F);
    return Cost(motion.body, body, motion.Predict(at), gate);
  }

  static float Cost(const BodyFeatures& previous, const BodyFeatures& body, Point predicted, float gate) {
    const auto delta = Difference(predicted, body.center);
    const auto spatial = std::hypot(delta.x, 0.65F * delta.y) / gate;
    const auto pose = std::min(PoseDistance(previous, body), 1.F);
    const auto chroma = std::min(ChromaDistance(previous.appearance, body.appearance), 0.2F);
    return spatial + 0.18F * pose + 0.2F * chroma;
  }

  float RecoveryCost(const BodyTrack& track, const BodyFeatures& body, double at) const {
    const auto& last = track.confirmed;
    const auto gap = float(at - last.time);
    // A tentative trajectory must remain consistent with the last confirmed person as well as its own latest frame.
    if (PoseDistance(last.body, body) > 0.70F || gap > 0.95F || InconsistentTorso(last.body, body)) return kUnmatched;
    const auto delta = Difference(body.center, last.body.center);
    const auto traveled = Length(delta);
    // Head height can change abruptly when someone turns or crouches. Give lateral travel more weight across gaps.
    const auto heading = Point{last.velocity.x, last.velocity.y * 0.5F};
    const auto displacement = Point{delta.x, delta.y * 0.5F};
    const auto speed = Length(heading);
    const auto distance_along = speed > 0 ? Dot(displacement, heading) / speed : 0.F;
    const bool follows_heading = speed >= 0.38F && Length(displacement) >= 0.12F &&
                                 distance_along >= 0.90F * Length(displacement) &&
                                 distance_along >= 0.65F * speed * gap && distance_along <= 1.9F * speed * gap;
    const auto reach = 0.06F + gap * 0.90F;
    const bool distinct = traveled <= reach && DistinctChroma(track, body);
    const bool isolated = !track.contested && traveled < 0.5F * last.body.scale &&
        std::ranges::none_of(tracks_, [&](const auto& other) {
          const auto& motion = other.Prediction();
          return &other != &track &&
                 Length(Difference(motion.Predict(at), body.center)) < 1.4F * std::max(body.scale, motion.body.scale);
        });
    if (!follows_heading && !distinct && !isolated) return kUnmatched;
    if (track.candidate) return Cost(*track.candidate, body, at);
    // Distinct chroma can corroborate a recovery anywhere inside the motion gate after a missed turn.
    return Cost(last.body, body, isolated ? last.body.center : last.Predict(at, 0.55),
                distinct ? reach : 0.10F + gap * 0.55F);
  }

  Detections Initialize(std::vector<Body>& bodies, double at) {
    std::ranges::sort(bodies, {}, [](const Body& body) {
      const auto head = body.points.front().position;
      return std::pair{head.x, head.y};
    });
    Detections result;
    for (auto& body : bodies) {
      const auto features = Features(body);
      tracks_.push_back({{features, {}, at}, features.appearance});
      result.push_back({{tracks_.size(), std::move(body.points)}, {}});
    }
    return result;
  }

  bool DistinctChroma(const BodyTrack& track, const BodyFeatures& body) const {
    const auto distance = ChromaDistance(track.reference, body.appearance);
    if (distance > 0.055F) return false;
    return tracks_.size() > 1 && std::ranges::all_of(tracks_, [&](const auto& other) {
      return &other == &track || (ChromaDistance(other.reference, track.reference) > 0.09F &&
                                 ChromaDistance(other.reference, body.appearance) > distance + 0.055F);
    });
  }

  std::vector<BodyTrack> tracks_;
  std::optional<FrameKey> last_;
  double time_{};
};

} // namespace

Point ModelCrop::ToSource(Point position) const {
  return Add(center, Add(Scale(right, (position.x - 0.5F) * width), Scale(down, (position.y - 0.5F) * height)));
}

ncnn::Mat Warp(const CpuImage& picture, const ModelCrop& crop, int size, float low, float high, bool bgr) {
  ncnn::Mat result(size, size, 3);
  if (result.empty()) throw std::bad_alloc();
  const auto start = crop.ToSource({0.5F / size, 0.5F / size});
  const auto dx = Scale(crop.right, crop.width / size);
  const auto dy = Scale(crop.down, crop.height / size);
  const auto scale = (high - low) / 255.F;
  for (int y = 0; y < size; ++y) {
    std::array<float*, 3> rows{result.channel(0).row(y), result.channel(1).row(y), result.channel(2).row(y)};
    if (bgr) std::swap(rows.front(), rows.back());
    for (int x = 0; x < size; ++x) {
      const auto at = Add(start, Add(Scale(dx, float(x)), Scale(dy, float(y))));
      for (auto row : rows) row[x] = 0;
      if (!(at.x > -1 && at.y > -1 && at.x < picture.width + 1.F && at.y < picture.height + 1.F)) continue;
      const int left = int(std::floor(at.x - 0.5F)), top = int(std::floor(at.y - 0.5F));
      const auto u = at.x - 0.5F - left, v = at.y - 0.5F - top;
      for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
          if (left + i < 0 || left + i >= picture.width || top + j < 0 || top + j >= picture.height) continue;
          const auto sample = Pixel(picture, left + i, top + j);
          const auto weight = (i ? u : 1 - u) * (j ? v : 1 - v);
          for (std::size_t c = 0; c < rows.size(); ++c) rows[c][x] += weight * (low + scale * sample[c]);
        }
      }
    }
  }
  return result;
}

struct BodyHandModels {
  explicit BodyHandModels(const NcnnBodyHandModels& assets)
      : person(assets.person_detector, kDetectorOutputs, "person detector"),
        pose(assets.body_landmarks, kPoseOutputs, "body landmarks"),
        palm(assets.palm_detector, kDetectorOutputs, "palm detector"),
        hand(assets.hand_landmarks, kHandOutputs, "hand landmarks") {}

  static constexpr std::array kDetectorOutputs{"out0", "out1"};
  static constexpr std::array kPoseOutputs{"out0", "out1", "out2", "out3", "out4"};
  static constexpr std::array kHandOutputs{"out0", "out1", "out2", "out3"};
  Network person, pose, palm, hand;
};

namespace {

class BodyHandSession final : public AnalysisSession {
public:
  BodyHandSession(std::shared_ptr<const BodyHandModels> models, AnalysisConfig config)
      : models_(std::move(models)), config_(config) {}

  void Submit(AnalysisJob job, std::stop_token stop, Completion complete) override {
    auto result = Analyze(job, stop);
    job.frames.clear();
    complete({job.id, job.target, std::move(result)});
  }

private:
  DetectionResult Analyze(const AnalysisJob& job, std::stop_token stop) {
    if (stop.stop_requested()) return DetectionResult::Success(std::nullopt);
    if (job.frames.size() != 1 || job.frames.front().key != job.target || (!config_.uses_roi && !job.regions.empty()))
      return DetectionResult::Failure("ncnn body and hand models need one exact target picture");
    const auto& frame = job.frames.front().image;
    if (frame.format != config_.format || frame.Storage() != config_.storage || frame.pts != job.target.pts ||
        !frame.Validate()) return DetectionResult::Failure("ncnn picture does not match the opened input format or PTS");
    try {
      const auto image = ConvertPicture(frame, stop);
      if (stop.stop_requested()) return DetectionResult::Success(std::nullopt);
      auto detections = config_.uses_roi ? Hands(image, job.regions, stop) : Bodies(image, job.target, stop);
      if (stop.stop_requested()) return DetectionResult::Success(std::nullopt);
      return DetectionResult::Success(std::move(detections));
    } catch (const std::exception& error) {
      return DetectionResult::Failure(error.what());
    }
  }

  Detections Bodies(const CpuImage& image, const FrameKey& target, std::stop_token stop) {
    std::vector<Body> bodies;
    const float extent = float(std::max(image.width, image.height));
    const ModelCrop crop{{image.width / 2.F, image.height / 2.F}, kRight, kDown, extent, extent};
    const auto candidates = Detect(image, models_->person, true, crop);
    for (const auto& candidate : candidates) {
      if (stop.stop_requested()) return {};
      const auto direction = Difference(candidate.points[0], candidate.points[1]);
      const auto radius = Length(direction);
      if (!(radius > 8.F && radius < 2.F * extent)) continue;
      const auto down = Scale(direction, 1.F / radius);
      auto body = Pose(image, models_->pose, candidate,
                       {candidate.points[0], {down.y, -down.x}, down, 2 * radius, 2 * radius});
      if (!body) continue;
      const auto head_size = std::sqrt(candidate.box.width * candidate.box.height);
      if (radius < 0.5F * head_size || TorsoNearNeighbor(*body, candidate, candidates, image.width)) {
        if (stop.stop_requested()) return {};
        // Recover a torso crop from the detected head when the virtual body points collapse or pull toward a neighbor.
        const auto head = BoxCenter(candidate.box);
        const auto size = std::min(4.F * head_size, extent);
        body = Pose(image, models_->pose, candidate, {{head.x, head.y + size / 4}, kRight, kDown, size, size});
        if (!body || TorsoNearNeighbor(*body, candidate, candidates, image.width)) continue;
      }
      bodies.push_back(std::move(*body));
    }
    if (stop.stop_requested()) return {};
    return association_.Advance(std::move(bodies), target);
  }

  Detections Hands(const CpuImage& image, const std::vector<AnalysisRegion>& regions, std::stop_token stop) const {
    std::vector<Candidate> palms;
    for (const auto& region : regions) {
      if (stop.stop_requested()) return {};
      const auto& bounds = region.bounds;
      const float extent = 1.5F * std::max(bounds.width * image.width, bounds.height * image.height);
      if (region.part == TrackedPart::Face || region.predicted ||
          !(extent >= 8.F && extent <= 2.F * std::max(image.width, image.height)))
        continue;
      const ModelCrop crop{{(bounds.x + bounds.width / 2) * image.width, (bounds.y + bounds.height / 2) * image.height},
                           kRight, kDown, extent, extent};
      for (auto& palm : Detect(image, models_->palm, false, crop)) {
        if (HandRegion(Normalize(image, palm.points[0]), regions)) palms.push_back(std::move(palm));
      }
    }
    std::stable_sort(palms.begin(), palms.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    Detections candidates;
    std::vector<Rect> visited;
    for (const auto& palm : palms) {
      if (stop.stop_requested()) return {};
      if (std::ranges::any_of(visited, [&](const Rect& box) { return Overlap(box, palm.box) > 0.3F; })) continue;
      visited.push_back(palm.box);
      const auto coarse = HandRegion(Normalize(image, palm.points[0]), regions);
      if (!coarse || regions[*coarse].predicted) continue;
      auto hand = Hand(image, models_->hand, palm);
      if (!hand || HandRegion(hand->front().position, regions) != coarse) continue;
      candidates.push_back({{regions[*coarse].person.value, std::move(*hand)}, *coarse});
    }
    Detections result;
    for (std::size_t i = 0; i < regions.size(); ++i) {
      if (stop.stop_requested()) return {};
      const auto count = std::ranges::count(candidates, i, &LandmarkDetection::region);
      if (count == 1) {
        result.push_back(std::move(*std::ranges::find(candidates, i, &LandmarkDetection::region)));
      } else if (count == 0 && regions[i].part != TrackedPart::Face) {
        if (auto hand = RetryHand(image, regions, i, stop))
          result.push_back({{regions[i].person.value, std::move(*hand)}, i});
      }
    }
    return result;
  }

  std::optional<std::vector<Landmark>> RetryHand(const CpuImage& image, const std::vector<AnalysisRegion>& regions,
                                                std::size_t index, std::stop_token stop) const {
    const auto& region = regions[index];
    const auto& bounds = region.bounds;
    const auto extent = std::max(bounds.width * image.width, bounds.height * image.height);
    if (!(extent >= 8.F && extent <= std::max(image.width, image.height))) return std::nullopt;
    const Point center{(bounds.x + bounds.width / 2) * image.width, (bounds.y + bounds.height / 2) * image.height};
    const auto near_wrist = [&](Point point) {
      return RegionDistance(point, bounds) <= 0.05F && HandRegion(point, regions) == index;
    };
    struct SearchPass {
      float scale;
      std::array<Point, 2> directions;
      bool mirror{};
      float landmarks_scale{3.F};
    };
    constexpr std::array measured{SearchPass{0.93F, {kRight, kDown}},
                                  SearchPass{0.78F, {kRight, Point{-1, 0}}}};
    constexpr std::array predicted{SearchPass{0.63F, {kRight, kDown}},
                                   SearchPass{1.5F, {kRight, Point{0, -1}}, true, 3.7F}};
    const auto threshold = region.predicted ? kDetectionThreshold : 0.35F;
    std::optional<std::vector<Landmark>> best;
    const auto refine = [&](const Candidate& palm, bool mirror, float landmarks_scale, float rotation = 0.F,
                            float forward_shift = 0.4F) {
      auto hand = Hand(image, models_->hand, palm, mirror, landmarks_scale, rotation, forward_shift);
      if (!hand || !near_wrist(hand->front().position)) return true;
      const auto& wrist = hand->front();
      // An alternate crop or a weak palm proposal needs more evidence from the landmark model.
      const auto minimum = (!region.predicted && rotation != 0.F) || palm.score < kDetectionThreshold
          ? 0.75F : (region.predicted ? 0.70F : 0.65F);
      if (*wrist.confidence < minimum) return true;
      if (best) {
        const auto delta = Difference(wrist.position, best->front().position);
        if (std::hypot(delta.x / bounds.width, delta.y / bounds.height) > 0.10F) return false;
        if (wrist.confidence <= best->front().confidence) return true;
      }
      best = std::move(hand);
      return true;
    };
    if (region.forearm && region.predicted) {
      const Point arm{region.forearm->x * image.width, region.forearm->y * image.height};
      const auto length = Length(arm);
      if (!(length >= 8.F && length <= 2.F * extent)) return std::nullopt;
      const Point across{-arm.y, arm.x};
      const auto base = Add(center, Scale(arm, -0.15F));
      const auto spread = std::min(0.48F, 0.30F * extent / length);
      // Check both sides of the projected forearm while keeping the acceptance region centered on its tip.
      for (const float lateral : {-spread, spread}) {
        if (stop.stop_requested()) return std::nullopt;
        const auto crop_center = Add(base, Scale(across, lateral));
        const auto size = 0.63F * extent;
        const ModelCrop crop{crop_center, {0, -1}, kRight, size, size};
        for (const auto& palm : Detect(image, models_->palm, false, crop, kDetectionThreshold)) {
          if (stop.stop_requested()) return std::nullopt;
          if (!near_wrist(Normalize(image, palm.points[0]))) continue;
          if (!refine(palm, true, 2.8F, 0.38F, 0.48F)) return std::nullopt;
        }
      }
      return best;
    }
    std::vector<Candidate> compact_palms;
    for (const auto& [scale, directions, mirror, landmarks_scale] : region.predicted ? predicted : measured) {
      for (const auto right : directions) {
        if (stop.stop_requested()) return std::nullopt;
        const ModelCrop crop{center, right, {-right.y, right.x}, extent * scale, extent * scale};
        for (const auto& palm : Detect(image, models_->palm, false, crop, threshold)) {
          if (stop.stop_requested()) return std::nullopt;
          if (!near_wrist(Normalize(image, palm.points[0]))) continue;
          if (!mirror && palm.score >= kDetectionThreshold) compact_palms.push_back(palm);
          if (!refine(palm, mirror, landmarks_scale)) return std::nullopt;
        }
      }
      if (best) return best;
    }
    // Reuse strong current-frame palms when the initial crop has no verified landmarks.
    for (const auto& palm : compact_palms) {
      if (stop.stop_requested()) return std::nullopt;
      if (region.predicted) {
        if (!refine(palm, false, 3.7F)) return std::nullopt;
      } else {
        for (const auto rotation : {-0.22F, 0.22F}) {
          if (stop.stop_requested() || !refine(palm, false, 2.5F, rotation)) return std::nullopt;
        }
      }
    }
    return best;
  }

  std::shared_ptr<const BodyHandModels> models_;
  AnalysisConfig config_;
  BodyAssociation association_;
};

} // namespace

std::shared_ptr<const BodyHandModels> LoadBodyHand(const NcnnBodyHandModels& models) {
  return std::make_shared<BodyHandModels>(models);
}

std::unique_ptr<AnalysisSession> OpenBodyHand(std::shared_ptr<const BodyHandModels> models, const AnalysisConfig& config) {
  return std::make_unique<BodyHandSession>(std::move(models), config);
}

} // namespace huxerui::avkit::analysis::detail
