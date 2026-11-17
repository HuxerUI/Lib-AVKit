#include "tracking.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

#include "support/cancellation.h"
#include "support/media_time.h"

namespace huxerui::avkit::detail {
namespace {

using namespace analysis;
using media::VideoFrame;
std::atomic<std::uint64_t> next_generation{1};

const char* Name(AnalysisCapability capability) {
  switch (capability) {
    case AnalysisCapability::BodyLandmarks: return "body landmarks";
    case AnalysisCapability::FaceLandmarks: return "face landmarks";
    case AnalysisCapability::HandLandmarks: return "hand landmarks";
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
  if (window.stateful || count > 8 || (count && !window.sample_interval) ||
      (window.sample_interval && (!window.sample_interval->timescale || window.sample_interval->ticks <= 0))) {
    throw std::runtime_error(std::string("Unsupported timeline analysis window for ") + Name(model.config.capability));
  }
}

struct Mailbox {
  std::mutex mutex;
  std::condition_variable changed;
  std::optional<AnalysisCompletion> completion;
};

std::vector<TrackingDetection> Run(Model& model, AnalysisJob job, std::stop_token stop) {
  CheckCancelled(stop);
  const auto box = std::make_shared<Mailbox>();
  const std::stop_callback wake_on_stop(stop, [box] {
    const std::lock_guard lock(box->mutex);
    box->changed.notify_one();
  });
  const auto target = job.target;
  const auto id = job.id;
  // A provider can finish after cancellation releases this job's mailbox.
  model.session->Submit(std::move(job), stop, [mailbox = std::weak_ptr(box), target, id](AnalysisCompletion result) {
    if (result.job_id != id || result.target != target) return;
    if (const auto queued = mailbox.lock()) {
      const std::lock_guard lock(queued->mutex);
      if (queued->completion) return;
      queued->completion.emplace(std::move(result));
      queued->changed.notify_one();
    }
  });
  std::unique_lock lock(box->mutex);
  box->changed.wait(lock, [&] { return box->completion || stop.stop_requested(); });
  CheckCancelled(stop);
  auto result = std::move(box->completion->result);
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

bool MatchesRegion(Point point, std::span<const AnalysisRegion> regions, std::size_t index) {
  const auto distance = [&](const Rect& rect) {
    const auto x = (point.x - (rect.x + rect.width / 2)) / rect.width;
    const auto y = (point.y - (rect.y + rect.height / 2)) / rect.height;
    return x * x + y * y;
  };
  const auto& region = regions[index].bounds;
  if (!region.Contains(point)) return false;
  const auto nearest = distance(region);
  for (std::size_t other = 0; other < regions.size(); ++other) {
    const auto& bounds = regions[other].bounds;
    if (other != index && bounds.Contains(point) && distance(bounds) <= nearest + 1e-6F) return false;
  }
  return true;
}

} // namespace

struct SourceTracking::Impl {
  Impl(std::shared_ptr<AnalysisProvider> provider, std::uint64_t source)
      : provider(std::move(provider)), source(source) {}

  std::shared_ptr<AnalysisProvider> provider;
  std::uint64_t source, generation{next_generation.fetch_add(1, std::memory_order_relaxed)}, next_job{1};
  std::vector<TrackedPart> parts;
  std::vector<Model> models;
  std::optional<FrameKey> last;
  TrackingFrame sampled;
  bool referenced{};

  bool Uses(AnalysisCapability capability) const {
    return std::ranges::any_of(parts, [&](auto part) { return Capability(part) == capability; });
  }

  void Reset() {
    models.clear();
    last.reset();
    sampled = {};
    referenced = false;
    generation = next_generation.fetch_add(1, std::memory_order_relaxed);
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
        const auto center = FindLandmark(*person.body, BodyPoint(part));
        if (!center) continue;
        constexpr float radius_x = 0.08F, radius_y = 0.10F;
        regions.push_back({person.person, part, {center->x - radius_x, center->y - radius_y, 2 * radius_x, 2 * radius_y}});
        owners.push_back(&person);
      }
    }
    auto matches = Detect(model, frame, target, read, regions, stop);
    std::vector<bool> ambiguous(regions.size());
    const auto center_name = model.config.capability == AnalysisCapability::FaceLandmarks ? "face_center" : "wrist";
    for (auto& detection : matches) {
      if (!detection.region || *detection.region >= regions.size()) continue;
      const auto index = *detection.region;
      const auto center = FindLandmark(detection.landmarks, center_name);
      if (!center || !MatchesRegion(*center, regions, index) || ambiguous[index]) continue;
      auto& part = Part(*owners[index], regions[index].part);
      if (part) {
        ambiguous[index] = true;
        part.reset();
      } else {
        part = Freeze(target, std::move(detection.landmarks));
      }
    }
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

  const TrackingFrame& Sample(const VideoFrame& image, const ReadWindow& read, const ReadReference& reference,
                              std::stop_token stop) {
    CheckCancelled(stop);
    Prepare(image);
    const auto target = Key(image.pts);
    if (last == target) return sampled;
    EstablishPrimary(image, read, reference, stop);
    TrackingFrame result{{}, sampled.primary};
    for (auto& model : models) {
      if (model.config.uses_roi) DeriveParts(result, image, target, model, read, stop);
      else EstablishPeople(result, target, model.config.capability, Detect(model, image, target, read, {}, stop));
    }
    if (!result.primary) result.primary = SelectPrimary(result);
    CheckCancelled(stop);
    sampled = std::move(result);
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
                                            std::stop_token stop) {
  try {
    return impl_->Sample(image, read, reference, stop);
  } catch (...) {
    Reset();
    throw;
  }
}

void SourceTracking::Reset() { impl_->Reset(); }

std::optional<Point> FindLandmark(const PartSample& part, std::string_view name) {
  return FindLandmark(*part.points, name);
}

} // namespace huxerui::avkit::detail
