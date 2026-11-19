#pragma once

/// @file
/// @brief In-process landmark inference requests, retained frame windows, and tracking anchors.
///
/// Providers may run inference asynchronously. The sampling session validates result identity on its
/// owning sequence before using landmarks in a render plan.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <huxerui/data.h>
#include <huxerui/geometry.h>
#include <huxerui/media_format.h>

namespace huxerui::avkit::analysis {

/// @brief Identity of one analyzed picture within a source instance.
struct FrameKey {
  std::uint64_t source;     ///< Distinguishes source instances, including clips of the same asset.
  std::uint64_t generation; ///< Retired by a seek, source change, or analysis reconfiguration.
  std::uint64_t sequence;   ///< Distinguishes pictures with equal PTS. Zero is valid for unique file PTS.
  media::MediaTimestamp pts; ///< Exact presentation time on the source media clock.
  bool operator==(const FrameKey&) const = default;
};

/// @brief Describes the temporal input a model needs for a target frame.
struct AnalysisWindow {
  std::uint32_t past_frames{}, future_frames{}; ///< Counts on either side of the target.
  /// Positive sampling interval on the source clock. File sampling requires it for neighboring frames.
  std::optional<media::MediaTimestamp> sample_interval;
  /// The session carries state between requests. File tracking feeds source PTS in ascending order per clip.
  bool stateful{};
  bool accepts_gaps{}; ///< The model accepts missing window samples.
};

/// @brief Parts available for sticker anchors, with hand sides from the subject's perspective.
/// Values are persisted in project documents.
enum class TrackedPart { Face = 0, LeftHand = 1, RightHand = 2 };

/// @brief Binds a sticker to semantic landmarks on the selected person.
struct TrackingAnchor {
  TrackedPart part;
  /// Origin landmark. If axis_point is present, the vector to it gives the direction and pixel scale.
  std::string point;
  std::optional<std::string> axis_point;
  bool operator==(const TrackingAnchor&) const = default;
};

/// @brief One named model observation in oriented source image coordinates.
struct Landmark {
  std::string name;
  /// Normalized in the oriented visible source image, with a top-left origin.
  Point position;
  std::optional<float> confidence; ///< Score in [0, 1] when supplied by the provider.
};

/// @brief Output requested from one model session.
enum class AnalysisCapability { BodyLandmarks, FaceLandmarks, HandLandmarks };

/// @brief Exact picture format, storage, and region input requirements for a model.
struct AnalysisConfig {
  AnalysisCapability capability;
  media::VideoFormat format;
  media::VideoStorage storage;
  bool uses_roi{};
};

/// @brief Runtime person identity scoped to a source and analysis generation.
struct PersonTrackId {
  std::uint64_t value;
  bool operator==(const PersonTrackId&) const = default;
};

/// @brief Source region proposed by the caller for a particular person and body part.
struct AnalysisRegion {
  PersonTrackId person;
  TrackedPart part;
  /// Normalized oriented source coordinates. Edge regions may extend beyond the source.
  Rect bounds;
  /// The center was inferred from pose or prior observations. Report a landmark only when verified in the target picture.
  bool predicted{};
  /// Optional target-frame elbow-to-wrist vector in normalized source coordinates for a hand search.
  std::optional<Point> forearm;
};

/// @brief Retains one decoded picture and its analysis identity.
struct AnalysisFrame {
  FrameKey key;
  media::VideoFrame image;
};

/// @brief One landmark request with the exact frame to report and its temporal context.
struct AnalysisJob {
  std::uint64_t id;
  FrameKey target;
  /// Source presentation order. All images retain their decoded storage.
  std::vector<AnalysisFrame> frames;
  std::vector<AnalysisRegion> regions;
};

/// @brief Semantic landmarks reported for a subject by one model session.
struct TrackedPoints {
  /// Provider identity scoped to one model session. Full-frame IDs are stable within that session.
  std::uint64_t subject_id;
  std::vector<Landmark> points;
};

/// @brief One full-frame or region-based detection.
struct TrackingDetection {
  TrackedPoints landmarks;
  /// Echoes the index in AnalysisJob::regions for an ROI result.
  std::optional<std::size_t> region;
};

/// @brief Completion for a submitted job. The ID and target must match the request.
struct AnalysisCompletion {
  std::uint64_t job_id;
  FrameKey target;
  /// An empty optional is a skipped frame. An empty list is a frame with no detections.
  Result<std::optional<std::vector<TrackingDetection>>, std::string> result;
};

/// @brief Executes jobs with the format and capability selected by the provider.
class AnalysisSession {
public:
  using Completion = std::function<void(AnalysisCompletion)>;

  virtual ~AnalysisSession() = default;
  /// @brief Submits owned frame references on the session's owning sequence.
  /// @param job Retained input frames and region assignments.
  /// @param stop Cooperative cancellation for this job. Release input leases promptly when cancelled.
  /// @param complete May run inline or on another sequence, including after session destruction.
  /// Deliver one matching completion for each job that reaches completion.
  virtual void Submit(AnalysisJob job, std::stop_token stop, Completion complete) = 0;
};

/// @brief Probes an input contract and opens independent model sessions.
class AnalysisProvider {
public:
  virtual ~AnalysisProvider() = default;
  /// @brief Checks whether the provider can run the requested capability with this input.
  /// @return Temporal requirements on success or a diagnostic on failure.
  virtual Result<AnalysisWindow, std::string> Probe(const AnalysisConfig& config) const = 0;
  /// @brief Opens a model session using a previously probed contract.
  /// @return A session for the caller's sequence, or null if it could not be opened.
  virtual std::unique_ptr<AnalysisSession> Open(const AnalysisConfig& config) = 0;
};

/// @brief Retains a person's landmarks and the exact frame on which they were observed.
struct PartSample {
  FrameKey observed;
  std::shared_ptr<const TrackedPoints> points;
};

/// @brief Parts associated with one person for one target frame. Missing parts are optional.
struct PersonSample {
  FrameKey target;
  PersonTrackId person;
  std::optional<PartSample> face, body, left_hand, right_hand;
};

} // namespace huxerui::avkit::analysis
