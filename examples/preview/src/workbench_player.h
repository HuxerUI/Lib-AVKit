#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

#include <huxerui/avkit/timeline.h>

namespace avkit_preview {

struct WorkbenchPlayback {
  double seconds{};
  bool playing{}, ready{};
  std::string error;

  bool operator==(const WorkbenchPlayback&) const = default;
};

struct CanvasGuides {
  std::optional<float> x, y;
  bool rotating{}, angle_snapped{};

  bool operator==(const CanvasGuides&) const = default;
};

struct WorkbenchDrag {
  /// Stable project target for this gesture.
  huxerui::avkit::EditTarget target;
  /// Transform delta in normalized project canvas coordinates.
  huxerui::avkit::Affine2D delta;
  /// Alignment feedback for the same drag position.
  CanvasGuides guides;

  bool operator==(const WorkbenchDrag&) const = default;
};

struct WorkbenchFrame {
  huxerui::avkit::render::PreviewFrame preview;
  /// Transient edit sampled into this frame, if the user is dragging an overlay.
  std::optional<WorkbenchDrag> drag;

  bool operator==(const WorkbenchFrame& other) const {
    return preview.texture == other.preview.texture && preview.sequence == other.preview.sequence;
  }
};

/// @brief Adds editor gesture feedback to the library preview session.
class WorkbenchPlayer {
public:
  /// Receives a frame and the matching editor state on the preview worker.
  using Present = std::function<void(WorkbenchFrame)>;

  /// @brief Starts the worker and opens its preview texture.
  /// @param texture_size Physical width and height of the preview texture.
  /// @param resources Catalog of resources for text, stickers, and masks.
  /// @param present Callback that delivers the frame and its matching editor state.
  WorkbenchPlayer(huxerui::avkit::PixelSize texture_size, std::shared_ptr<const huxerui::avkit::ResourceCatalog> resources, Present present);

  /// @brief Returns a synchronized snapshot of preview progress and errors.
  WorkbenchPlayback Status() const;

  bool Closed() const;

  /// @brief Cancels pending work and joins the preview worker. Call from outside that worker.
  void Close();

  /// @brief Moves the preview playhead while preserving playback state.
  /// @param seconds Requested project time, clamped to a valid frame.
  void Seek(double seconds);

  /// @brief Sets the transient layout and its associated guides.
  /// @param drag Optional drag state to match against the next published frame.
  void SetDrag(std::optional<WorkbenchDrag> drag);

  huxerui::avkit::render::TimelinePreviewSession& Session() { return session_; }

private:
  std::mutex drag_mutex_;
  std::optional<WorkbenchDrag> drag_;
  std::uint64_t drag_request_{};
  huxerui::avkit::render::TimelinePreviewSession session_;
};

using WorkbenchExportFrame = std::function<void(huxerui::media::MediaTimestamp)>;

/// @brief Renders and writes an SDR MP4 from the selected project revision.
/// @param revision Snapshot retained for the export.
/// @param file Local destination for the finalized movie.
/// @param settings Export dimensions, frame rate, color, and codec policy.
/// @param resources Catalog of resources used to resolve overlays and masks.
/// @param on_frame Optional progress callback receiving each frame's project timestamp before rendering.
/// @param stop Cancels the render handoff, frame evaluation, or encoding waits.
void ExportWorkbenchMovie(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> revision, const huxerui::File& file,
                          const huxerui::avkit::FileExportSettings& settings,
                          std::shared_ptr<const huxerui::avkit::ResourceCatalog> resources, const WorkbenchExportFrame& on_frame,
                          std::stop_token stop, WorkbenchPlayer* preview);

} // namespace avkit_preview
