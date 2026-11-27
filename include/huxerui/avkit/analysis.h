#pragma once

/// @file
/// @brief In-process inference requests, retained frame windows, and tracking anchors.
///
/// Providers may run inference asynchronously. The sampling session validates result identity on its
/// owning sequence before using analysis results in a render plan.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

#include <huxerui/data.h>
#include <huxerui/geometry.h>
#include <huxerui/media_format.h>
#include <huxerui/avkit/render.h>
#include <huxerui/avkit/tracking_anchor.h>

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

/// @brief One named model observation in oriented source image coordinates.
struct Landmark {
  /// Semantic name used by tracking anchors, such as right_eye or wrist.
  std::string name;
  /// Normalized in the oriented visible source image, with a top-left origin.
  Point position;
  std::optional<float> confidence; ///< Score in [0, 1] when supplied by the provider.
};

/// @brief Output requested from one model session.
enum class AnalysisCapability { BodyLandmarks, FaceLandmarks, HandLandmarks, PortraitMask, DepthMap };

/// @brief Exact picture format, storage, and region input requirements for a model.
struct AnalysisConfig {
  /// Output requested from this session.
  AnalysisCapability capability;
  /// Exact picture layout, orientation, and color metadata.
  media::VideoFormat format;
  /// CPU planes or platform-backed storage accepted by the session.
  media::VideoStorage storage;
  /// True when each request supplies caller-selected regions.
  bool uses_roi{};
  /// Stable model ID. An empty value selects the provider's default for this capability.
  std::string model_id;
};

/// @brief Runtime person identity scoped to a source and analysis generation.
struct PersonTrackId {
  /// Runtime identity valid only for the source and analysis generation.
  std::uint64_t value;

  bool operator==(const PersonTrackId&) const = default;
};

/// @brief Source region proposed by the caller for a particular person and body part.
struct AnalysisRegion {
  /// Subject identity assigned by the caller.
  PersonTrackId person;
  /// Requested body part, with hand sides from the subject's perspective.
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
  /// Generation, sequence, and source PTS of this picture.
  FrameKey key;
  /// Retained decoded input in the probed format.
  media::VideoFrame image;
};

/// @brief One analysis request with the exact frame to report and its temporal context.
struct AnalysisJob {
  /// Unique job identifier within the submitting session.
  std::uint64_t id;
  /// Exact picture to report in the completion.
  FrameKey target;
  /// Source presentation order. All images retain their decoded storage.
  std::vector<AnalysisFrame> frames;
  /// Optional target-frame regions. Detections refer to these by index.
  std::vector<AnalysisRegion> regions;
};

/// @brief Semantic landmarks reported for a subject by one model session.
struct SubjectLandmarks {
  /// Provider identity scoped to one model session. Full-frame IDs are stable within that session.
  std::uint64_t subject_id;
  /// Named observations from the target picture.
  std::vector<Landmark> points;
};

/// @brief One full-frame or region-based detection.
struct LandmarkDetection {
  /// Observed points for this detection.
  SubjectLandmarks landmarks;
  /// Echoes the index in AnalysisJob::regions for an ROI result.
  std::optional<std::size_t> region;
};

/// @brief Completion for a submitted job. The ID and target must match the request.
struct LandmarkCompletion {
  /// Identifier copied from the submitted job.
  std::uint64_t job_id;
  /// Target identity copied from the submitted job.
  FrameKey target;
  /// An empty optional is a skipped frame. An empty list is a frame with no detections.
  Result<std::optional<std::vector<LandmarkDetection>>, std::string> result;
};

/// @brief Numeric image for one exact job. An empty optional is a skipped frame.
struct AnalysisImageCompletion {
  /// Identifier copied from the submitted job.
  std::uint64_t job_id;
  /// Target identity copied from the submitted job.
  FrameKey target;
  /// Retained numeric image, skipped sample, or provider diagnostic.
  Result<std::optional<render::PixelInput>, std::string> result;
  /// Maps coordinates in the oriented source to the result's logical UVs.
  Affine2D source_to_texture_uv;
  /// Region of the result with usable measurements.
  Rect valid_uv{0, 0, 1, 1};
};

/// @brief Executes jobs with the format and capability selected by the provider.
class AnalysisSession {
public:
  using Completion = std::function<void(LandmarkCompletion)>;
  using ImageCallback = std::function<void(AnalysisImageCompletion)>;

  virtual ~AnalysisSession() = default;

  /// @brief Submits a landmark job on the session's owning sequence.
  /// The default implementation completes with an unsupported diagnostic.
  ///
  /// @param job Retained input frames and region assignments.
  /// @param stop Cooperative cancellation token. Providers should release input leases promptly when cancelled.
  /// @param complete Receives the matching completion. It may run inline or on another sequence, including after session destruction.
  virtual void Submit(AnalysisJob job, [[maybe_unused]] std::stop_token stop, Completion complete) {
    complete({job.id, job.target, decltype(LandmarkCompletion::result)::Failure("Landmark submission is unavailable")});
  }

  /// @brief Submits an R8Unorm portrait mask job under the lifetime and cancellation rules of Submit.
  /// A valid all-zero mask reports a frame with no foreground.
  /// @param job Window and target identity for the requested foreground mask.
  /// @param stop Cooperative cancellation token.
  /// @param complete Receives the mask, a skipped result, or a diagnostic with the same job ID and target.
  virtual void SubmitMask(AnalysisJob job, [[maybe_unused]] std::stop_token stop, ImageCallback complete) {
    complete({job.id, job.target, decltype(AnalysisImageCompletion::result)::Failure("Portrait mask submission is unavailable")});
  }

  /// @brief Submits an R32Float optical-axis depth job on the same terms as Submit.
  /// Depth is registered to the target frame in meters. Zero and nonfinite pixels have no usable depth.
  /// @param job Window and target identity for the requested depth map.
  /// @param stop Cooperative cancellation token.
  /// @param complete Receives the depth image, a skipped result, or a diagnostic with the same job ID and target.
  virtual void SubmitDepth(AnalysisJob job, [[maybe_unused]] std::stop_token stop, ImageCallback complete) {
    complete({job.id, job.target, decltype(AnalysisImageCompletion::result)::Failure("Depth submission is unavailable")});
  }
};

/// @brief Probes an input contract and opens independent model sessions.
class AnalysisProvider {
public:
  virtual ~AnalysisProvider() = default;

  /// @brief Checks whether the provider can run the requested capability with this input.
  /// @param config Exact capability, format, storage, and ROI contract.
  /// @return Temporal requirements on success or a diagnostic on failure.
  virtual Result<AnalysisWindow, std::string> Probe(const AnalysisConfig& config) const = 0;

  /// @brief Opens a model session using a previously probed contract.
  /// @param config The successfully probed input contract and selected model.
  /// @return A session for the caller's sequence, or null if it could not be opened.
  virtual std::unique_ptr<AnalysisSession> Open(const AnalysisConfig& config) = 0;
};

/// @brief Retains a person's landmarks and the exact frame on which they were observed.
struct TrackedPartSample {
  /// Exact picture from which these landmarks were measured.
  FrameKey observed;
  /// Retained observation shared by consumers of this part.
  std::shared_ptr<const SubjectLandmarks> points;
};

/// @brief Parts associated with one person for one target frame. Missing parts are optional.
struct TrackedPersonSample {
  /// Picture for which the association was evaluated.
  FrameKey target;
  /// Associated subject on the current source.
  PersonTrackId person;
  /// Available observations by semantic body part.
  std::optional<TrackedPartSample> face, body, left_hand, right_hand;
};

} // namespace huxerui::avkit::analysis
