#pragma once

#include <huxerui/avkit/timeline.h>

namespace huxerui::avkit::detail {

struct TimelineSessionAccess {
  /// @brief Opens a session bound to a device and installs the supplied scene configuration.
  /// @param project Immutable revision to sample.
  /// @param providers Candidate decoders for the session.
  /// @param policy Decoder selection and fallback policy.
  /// @param scene Resources, analysis provider, and scene extension.
  /// @param fallback Optional codec fallback observer.
  /// @param device Shared device used on its owning render sequence.
  /// @param target Composition extent and attachment format of the device.
  static std::unique_ptr<TimelineSession> Open(std::shared_ptr<const ProjectSnapshot> project,
      CodecProviders providers, CodecPolicy policy, const SceneConfiguration& scene, OnCodecFallback fallback,
      std::shared_ptr<render::RenderBackend> device, render::ImageInfo target);
};

using ComposeTimelineFrame = std::function<media::VideoFrame(
    const TimelineFrame&, const media::VideoEncoderConfig&, media::MediaTimestamp duration)>;

/// @brief Samples video and PCM on the export grids and publishes a finalized MP4.
/// @param project Immutable revision to export.
/// @param destination Local destination for the finalized MP4.
/// @param providers Codec candidates used for decoding and encoding.
/// @param settings Output profile and encoder policy.
/// @param device Render device retained through decoder import and frame composition.
/// @param target Device extent and attachment format.
/// @param compose Composes a sampled frame into a negotiated encoder picture.
/// @param options Scene setup, cancellation, and optional observers.
void ExportTimeline(std::shared_ptr<const ProjectSnapshot> project, const File& destination,
    const CodecProviders& providers, const FileExportSettings& settings,
    const std::shared_ptr<render::RenderBackend>& device, render::ImageInfo target,
    const ComposeTimelineFrame& compose, const render::TimelineExportOptions& options);

} // namespace huxerui::avkit::detail
