#include "tracking.h"

#include "analysis_job.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>

#include "support/cancellation.h"
#include "support/media_time.h"

namespace huxerui::avkit::detail {
namespace {

using namespace analysis;
using media::VideoFrame;

const char* Name(AnalysisCapability capability) {
  switch (capability) {
    case AnalysisCapability::BodyLandmarks: return "body landmarks";
    case AnalysisCapability::FaceLandmarks: return "face landmarks";
    case AnalysisCapability::HandLandmarks: return "hand landmarks";
    case AnalysisCapability::PortraitMask: return "portrait mask";
    case AnalysisCapability::DepthMap: return "depth map";
  }
  throw std::invalid_argument("Unknown analysis capability");
}

std::optional<AnalysisCapability> Capability(TrackedPart part) {
  switch (part) {
    case TrackedPart::Face: return AnalysisCapability::FaceLandmarks;
    case TrackedPart::LeftHand:
    case TrackedPart::RightHand: return AnalysisCapability::HandLandmarks;
  }
  return std::nullopt;
}

struct Model {
  AnalysisConfig config;
  AnalysisWindow window;
  std::unique_ptr<AnalysisSession> session;
};

void ValidateWindow(const Model& model) {
  const auto& window = model.window;
  const auto count = std::uint64_t(window.past_frames) + window.future_frames;
  const bool unsupported_ordered = window.stateful &&
      (model.config.capability != AnalysisCapability::BodyLandmarks || model.config.uses_roi || count);
  if (unsupported_ordered || count > 8 || (count && !window.sample_interval) ||
      (window.sample_interval && (!window.sample_interval->timescale || window.sample_interval->ticks <= 0))) {
    throw std::runtime_error(std::string("Unsupported timeline analysis window for ") + Name(model.config.capability));
  }
}

std::vector<TrackingDetection> Run(Model& model, AnalysisJob job, std::stop_token stop) {
  auto completion = WaitForAnalysis<AnalysisCompletion>(std::move(job), stop, [&](auto request, auto token, auto done) {
    model.session->Submit(std::move(request), token, std::move(done));
  });
  auto result = std::move(completion->result);
  if (!result.Succeeded())
    throw std::runtime_error(std::string("Analysis failed for ") + Name(model.config.capability) + ": " + result.Error());
  return std::move(result).Value().value_or(std::vector<TrackingDetection>{});
}

std::optional<Point> FindLandmark(const TrackedPoints& points, std::string_view name) {
  const auto it = std::ranges::find(points.points, name, &Landmark::name);
  if (it == points.points.end()) return std::nullopt;
  const auto confidence = it->confidence.value_or(1.0F);
  const auto point = it->position;
  if (!(confidence >= 0.5F && confidence <= 1 && Rect{0, 0, 1, 1}.Contains(point))) return std::nullopt;
  return point;
}

std::string_view BodyPoint(TrackedPart part) {
  switch (part) {
    case TrackedPart::Face: return "head";
    case TrackedPart::LeftHand: return "left_wrist";
    case TrackedPart::RightHand: return "right_wrist";
  }
  throw std::invalid_argument("Unknown tracked part");
}

std::optional<PartSample>& Part(PersonSample& person, TrackedPart part) {
  switch (part) {
    case TrackedPart::Face: return person.face;
    case TrackedPart::LeftHand: return person.left_hand;
    case TrackedPart::RightHand: return person.right_hand;
  }
  throw std::invalid_argument("Unknown tracked part");
}

PartSample Freeze(FrameKey target, TrackedPoints points) {
  return {target, std::make_shared<const TrackedPoints>(std::move(points))};
}

bool MatchesRegion(Point point, std::span<const AnalysisRegion> regions, std::size_t index,
                   std::span<const AnalysisRegion> blockers = {}) {
  const auto distance = [&](const Rect& rect) {
    const auto x = (point.x - (rect.x + rect.width / 2)) / rect.width;
    const auto y = (point.y - (rect.y + rect.height / 2)) / rect.height;
    return x * x + y * y;
  };
  const auto& region = regions[index];
  if (!region.bounds.Contains(point)) return false;
  const auto nearest = distance(region.bounds);
  if (region.predicted && nearest > 0.05F) return false;
  const auto margin = region.predicted ? 0.04F : 1e-6F;
  const auto competes = [&](const AnalysisRegion& other) {
    return (other.person != region.person || other.part != region.part) &&
           other.bounds.Contains(point) && distance(other.bounds) <= nearest + margin;
  };
  return std::ranges::none_of(regions, competes) && std::ranges::none_of(blockers, competes);
}

void SuppressHandSwitch(PersonSample& person, const PersonSample& previous) {
  if (person.person != previous.person || Before({1, 16}, Difference(person.target.pts, previous.target.pts)) ||
      bool(person.left_hand) == bool(person.right_hand) || bool(previous.left_hand) == bool(previous.right_hand) ||
      bool(person.left_hand) == bool(previous.left_hand)) return;
  auto& current = person.left_hand ? person.left_hand : person.right_hand;
  const auto& last = previous.left_hand ? previous.left_hand : previous.right_hand;
  const auto a = FindLandmark(*current->points, "wrist");
  const auto b = FindLandmark(*last->points, "wrist");
  // A label flip on the only visible wrist is ambiguous when the wrist has barely moved.
  if (a && b && std::hypot(a->x - b->x, a->y - b->y) < 0.045F) current.reset();
}

constexpr std::array kHands{TrackedPart::LeftHand, TrackedPart::RightHand};

AnalysisRegion SearchRegion(const PersonSample& person, TrackedPart part, Point center, bool predicted,
                            std::optional<Point> forearm = {}) {
  const auto radius = predicted && !forearm ? Point{0.06F, 0.08F} : Point{0.08F, 0.10F};
  return {person.person, part, {center.x - radius.x, center.y - radius.y, 2 * radius.x, 2 * radius.y}, predicted, forearm};
}

std::optional<AnalysisRegion> ForearmRegion(const PersonSample& person, TrackedPart part, Point wrist) {
  const auto elbow = FindLandmark(*person.body->points, part == TrackedPart::LeftHand ? "left_elbow" : "right_elbow");
  if (!elbow) return std::nullopt;
  const auto direction = wrist - *elbow;
  const auto length = std::hypot(direction.x, direction.y);
  if (!(length >= 0.025F && length <= 0.30F)) return std::nullopt;
  const auto center = wrist + direction * std::min(0.60F, 0.065F / length);
  if (!Rect{0, 0, 1, 1}.Contains(center)) return std::nullopt;
  return SearchRegion(person, part, center, true, direction);
}

struct MeasuredWrist {
  Point point;
  bool body_wrist_available;
};

struct HandSearchReference {
  FrameKey target;
  PersonTrackId person;
  media::MediaTimestamp end;
  Point head;
  std::array<std::optional<MeasuredWrist>, kHands.size()> wrists;
};

} // namespace

struct SourceTracking::Impl {
  Impl(std::shared_ptr<AnalysisProvider> provider, std::uint64_t source)
      : provider(std::move(provider)), source(source) {}

  std::shared_ptr<AnalysisProvider> provider;
  std::uint64_t source, generation{NextAnalysisGeneration()}, next_job{1};
  std::vector<TrackedPart> parts;
  std::vector<Model> models;
  std::optional<FrameKey> last;
  TrackingFrame sampled;
  bool referenced{};
  ReadNext read_next;
  std::optional<VideoFrame> pending;
  bool end_of_input{};
  std::map<media::MediaTimestamp, TrackingFrame, decltype(&Before)> ordered_frames{Before};
  std::optional<HandSearchReference> hand_reference;
  const TrackingFrame missing;

  bool Ordered() const { return !models.empty() && models.front().window.stateful; }

  bool Uses(AnalysisCapability capability) const {
    return std::ranges::any_of(parts, [&](auto part) { return Capability(part) == capability; });
  }

  void Reset() {
    models.clear();
    last.reset();
    sampled = {};
    referenced = false;
    read_next = {};
    pending.reset();
    end_of_input = false;
    ordered_frames.clear();
    hand_reference.reset();
    generation = NextAnalysisGeneration();
  }

  void Prepare(const VideoFrame& image) {
    if (!models.empty() && models.front().config.format == image.format &&
        models.front().config.storage == image.Storage()) return;
    if (!models.empty()) Reset();
    const bool body = Uses(AnalysisCapability::HandLandmarks);
    for (const auto capability : {AnalysisCapability::BodyLandmarks, AnalysisCapability::FaceLandmarks,
                                  AnalysisCapability::HandLandmarks}) {
      if (capability == AnalysisCapability::BodyLandmarks ? !body : !Uses(capability)) continue;
      if (!provider) throw std::runtime_error(std::string("A tracking provider is required for ") + Name(capability));
      const AnalysisConfig config{capability, image.format, image.Storage(),
                                  body && capability != AnalysisCapability::BodyLandmarks};
      auto probe = provider->Probe(config);
      if (!probe.Succeeded())
        throw std::runtime_error(std::string("Unavailable analysis capability ") + Name(capability) + ": " + probe.Error());
      models.push_back({config, std::move(probe).Value(), {}});
      ValidateWindow(models.back());
    }
    for (auto& model : models) {
      model.session = provider->Open(model.config);
      if (!model.session) throw std::runtime_error(std::string("Cannot open ") + Name(model.config.capability));
    }
  }

  FrameKey Key(media::MediaTimestamp pts) const {
    // MovieReader emits unique presentation timestamps within a video cursor.
    return {source, generation, 0, pts};
  }

  std::vector<TrackingDetection> Detect(Model& model, const VideoFrame& frame, FrameKey target, const ReadWindow& read,
                                        std::vector<AnalysisRegion> regions, std::stop_token stop) {
    if (model.config.uses_roi && regions.empty()) return {};
    AnalysisJob job{next_job++, target, {}, std::move(regions)};
    const auto& window = model.window;
    std::uint32_t past = 0, future = 0;
    bool has_target = false;
    for (auto& image : read(frame, window)) {
      const auto key = Key(image.pts);
      if (image.format != model.config.format || image.Storage() != model.config.storage) continue;
      if (!job.frames.empty() && !Before(job.frames.back().key.pts, key.pts))
        throw std::logic_error("Analysis window must be in source presentation order");
      if (key == target) has_target = true;
      else if (Before(key.pts, target.pts)) ++past;
      else ++future;
      job.frames.push_back({key, std::move(image)});
    }
    if (!has_target || (!window.accepts_gaps && (past != window.past_frames || future != window.future_frames))) return {};
    return Run(model, std::move(job), stop);
  }

  void EstablishPeople(TrackingFrame& result, FrameKey target, AnalysisCapability capability,
                       std::vector<TrackingDetection> detections) {
    for (auto& detection : detections) {
      const auto id = detection.landmarks.subject_id;
      if (detection.region || std::ranges::count_if(detections, [&](const auto& other) {
            return !other.region && other.landmarks.subject_id == id;
          }) != 1) continue;
      auto& person = result.people.emplace_back(PersonSample{target, {id}});
      auto& part = capability == AnalysisCapability::BodyLandmarks ? person.body : person.face;
      part = Freeze(target, std::move(detection.landmarks));
    }
  }

  void DeriveParts(TrackingFrame& result, const VideoFrame& frame, FrameKey target, Model& model,
                   const ReadWindow& read, std::stop_token stop) {
    std::vector<AnalysisRegion> regions;
    std::vector<PersonSample*> owners;
    for (auto& person : result.people) {
      for (const auto part : parts) {
        if (Capability(part) != model.config.capability) continue;
        auto center = FindLandmark(*person.body, BodyPoint(part));
        const bool predicted = !center && model.config.capability == AnalysisCapability::HandLandmarks;
        if (predicted) center = PredictWrist(person, part);
        if (!center) continue;
        regions.push_back(SearchRegion(person, part, *center, predicted));
        owners.push_back(&person);
      }
    }

    const auto ambiguous = DetectParts(frame, target, model, read, regions, owners, stop);
    if (model.config.capability != AnalysisCapability::HandLandmarks || !Ordered()) return;

    std::vector<AnalysisRegion> retries;
    std::vector<PersonSample*> retry_owners;
    for (std::size_t i = 0; i < regions.size(); ++i) {
      auto& person = *owners[i];
      auto& region = regions[i];
      const auto& hand = Part(person, region.part);
      if (hand) {
        // A verified wrist from the first search still competes for ownership in the second search.
        if (const auto wrist = FindLandmark(*hand, "wrist")) region = SearchRegion(person, region.part, *wrist, false);
      } else if (person.person == result.primary && !region.predicted && !ambiguous[i]) {
        const auto body_wrist = FindLandmark(*person.body, BodyPoint(region.part));
        if (!body_wrist) continue;
        if (const auto center = PredictWrist(person, region.part, body_wrist)) {
          retries.push_back(SearchRegion(person, region.part, *center, true));
          retry_owners.push_back(&person);
        } else if (auto forearm = ForearmRegion(person, region.part, *body_wrist)) {
          retries.push_back(*forearm);
          retry_owners.push_back(&person);
        }
      }
    }
    if (!retries.empty()) DetectParts(frame, target, model, read, retries, retry_owners, stop, regions);
  }

  std::vector<bool> DetectParts(const VideoFrame& frame, FrameKey target, Model& model, const ReadWindow& read,
                                const std::vector<AnalysisRegion>& regions, const std::vector<PersonSample*>& owners,
                                std::stop_token stop, std::span<const AnalysisRegion> blockers = {}) {
    auto matches = Detect(model, frame, target, read, regions, stop);
    std::vector<bool> ambiguous(regions.size());
    const auto center_name = model.config.capability == AnalysisCapability::FaceLandmarks ? "face_center" : "wrist";
    for (auto& detection : matches) {
      if (!detection.region || *detection.region >= regions.size()) continue;
      const auto index = *detection.region;
      const auto center = FindLandmark(detection.landmarks, center_name);
      if (!center || !MatchesRegion(*center, regions, index, blockers) || ambiguous[index]) continue;
      auto& part = Part(*owners[index], regions[index].part);
      if (part) {
        ambiguous[index] = true;
        part.reset();
      } else {
        part = Freeze(target, std::move(detection.landmarks));
      }
    }
    return ambiguous;
  }

  std::optional<Point> PredictWrist(const PersonSample& person, TrackedPart part, std::optional<Point> body_wrist = {}) const {
    if (!hand_reference || hand_reference->person != person.person || hand_reference->end != person.target.pts ||
        Before({1, 12}, Difference(person.target.pts, hand_reference->target.pts))) return std::nullopt;
    const auto& wrist = hand_reference->wrists[part == TrackedPart::LeftHand ? 0 : 1];
    const auto head = FindLandmark(*person.body, "head");
    if (!wrist || !head || (!body_wrist && !wrist->body_wrist_available)) return std::nullopt;
    const auto motion = *head - hand_reference->head;
    auto center = wrist->point + motion;
    if (body_wrist) {
      // A nearby body estimate can bring the search center closer to the current arm position.
      const auto correction = *body_wrist - center;
      if (std::hypot(correction.x, correction.y) <= 0.09F) center = center + correction * 0.65F;
    }
    if (std::hypot(motion.x, motion.y) > 0.065F || !Rect{0, 0, 1, 1}.Contains(center)) return std::nullopt;
    return center;
  }

  void RememberHands(PersonSample& person, const VideoFrame& frame) {
    if (!person.body || (!person.left_hand && !person.right_hand) || !frame.duration) return;
    const auto head = FindLandmark(*person.body, "head");
    if (!head) return;
    HandSearchReference reference{person.target, person.person, Add(frame.pts, *frame.duration), *head, {}};
    for (std::size_t i = 0; i < kHands.size(); ++i) {
      const auto& hand = Part(person, kHands[i]);
      if (!hand || hand->observed != person.target) continue;
      if (const auto wrist = FindLandmark(*hand, "wrist"))
        reference.wrists[i] = MeasuredWrist{*wrist, FindLandmark(*person.body, BodyPoint(kHands[i])).has_value()};
    }
    if (std::ranges::any_of(reference.wrists, [](const auto& wrist) { return wrist.has_value(); }))
      hand_reference = reference;
  }

  std::optional<PersonTrackId> SelectPrimary(const TrackingFrame& result) const {
    const auto count = [&](const PersonSample& person) {
      if (!person.body) return person.face && FindLandmark(*person.face, "face_center") ? std::ptrdiff_t{1} : 0;
      return std::ranges::count_if(parts, [&](auto part) {
        return FindLandmark(*person.body, BodyPoint(part)).has_value();
      });
    };
    const auto best = std::ranges::min_element(result.people, {}, [&](const PersonSample& person) {
      return std::pair{-count(person), person.person.value};
    });
    return best != result.people.end() && count(*best) ? std::optional{best->person} : std::nullopt;
  }

  void EstablishPrimary(const VideoFrame& image, const ReadWindow& read, const ReadReference& reference,
                        std::stop_token stop) {
    if (referenced) return;
    referenced = true;
    const auto first = reference();
    if (!first || first->format != image.format || first->Storage() != image.Storage()) return;
    auto& model = models.front();
    const auto target = Key(first->pts);
    TrackingFrame initial;
    EstablishPeople(initial, target, model.config.capability, Detect(model, *first, target, read, {}, stop));
    sampled.primary = SelectPrimary(initial);
  }

  TrackingFrame Evaluate(const VideoFrame& image, const ReadWindow& read, std::optional<PersonTrackId> primary,
                         std::stop_token stop) {
    const auto target = Key(image.pts);
    TrackingFrame result{{}, primary};
    for (auto& model : models) {
      if (model.config.uses_roi) DeriveParts(result, image, target, model, read, stop);
      else EstablishPeople(result, target, model.config.capability, Detect(model, image, target, read, {}, stop));
    }
    if (!result.primary) result.primary = SelectPrimary(result);
    CheckCancelled(stop);
    return result;
  }

  const TrackingFrame& SampleOrdered(media::MediaTimestamp target, const ReadWindow& read,
                                     const OpenOrdered& open, std::stop_token stop) {
    if (!ordered_frames.empty() && !Before(ordered_frames.rbegin()->first, target)) {
      if (const auto it = ordered_frames.find(target); it != ordered_frames.end()) return it->second;
      return missing;
    }
    if (end_of_input) return missing;
    if (!read_next) read_next = open();
    auto primary = ordered_frames.empty() ? std::nullopt : ordered_frames.rbegin()->second.primary;
    while (!end_of_input) {
      CheckCancelled(stop);
      if (!pending) pending = read_next(stop);
      if (!pending) {
        end_of_input = true;
        read_next = {};
        break;
      }
      if (Before(target, pending->pts)) break;
      if (!ordered_frames.empty() && !Before(ordered_frames.rbegin()->first, pending->pts))
        throw std::logic_error("Ordered analysis cursor must advance in source presentation order");
      if (pending->format != models.front().config.format || pending->Storage() != models.front().config.storage)
        throw std::runtime_error("Ordered analysis input format changed inside the clip");
      const auto pts = pending->pts;
      auto result = Evaluate(*pending, read, primary, stop);
      primary = result.primary;
      TrackingFrame cached{{}, primary};
      const auto selected = std::ranges::find_if(result.people, [&](const auto& person) { return person.person == primary; });
      hand_reference.reset();
      if (selected != result.people.end()) {
        if (!ordered_frames.empty() && !ordered_frames.rbegin()->second.people.empty())
          SuppressHandSwitch(*selected, ordered_frames.rbegin()->second.people.front());
        RememberHands(*selected, *pending);
        selected->body.reset();
        cached.people.push_back(std::move(*selected));
      }
      pending.reset();
      const auto it = ordered_frames.emplace_hint(ordered_frames.end(), pts, std::move(cached));
      if (pts == target) return it->second;
    }
    return missing;
  }

  const TrackingFrame& Sample(const VideoFrame& image, const ReadWindow& read, const ReadReference& reference,
                              const OpenOrdered& open_ordered, std::stop_token stop) {
    CheckCancelled(stop);
    Prepare(image);
    if (Ordered()) return SampleOrdered(image.pts, read, open_ordered, stop);
    const auto target = Key(image.pts);
    if (last == target) return sampled;
    EstablishPrimary(image, read, reference, stop);
    sampled = Evaluate(image, read, sampled.primary, stop);
    last = target;
    return sampled;
  }
};

SourceTracking::SourceTracking(std::shared_ptr<AnalysisProvider> provider, std::uint64_t source,
                               std::span<const TrackedPart> parts)
    : impl_(std::make_unique<Impl>(std::move(provider), source)) {
  for (const auto part : parts) {
    if (std::ranges::find(impl_->parts, part) == impl_->parts.end()) impl_->parts.push_back(part);
  }
}

SourceTracking::~SourceTracking() = default;

const TrackingFrame& SourceTracking::Sample(const VideoFrame& image, const ReadWindow& read, const ReadReference& reference,
                                            const OpenOrdered& open_ordered, std::stop_token stop) {
  try {
    return impl_->Sample(image, read, reference, open_ordered, stop);
  } catch (...) {
    impl_->Reset();
    throw;
  }
}

void SourceTracking::Reset() {
  // A preview seek does not interrupt the ordered source cursor.
  if (!impl_->Ordered()) impl_->Reset();
}

std::optional<Point> FindLandmark(const PartSample& part, std::string_view name) {
  return FindLandmark(*part.points, name);
}

} // namespace huxerui::avkit::detail
