#pragma once

#include <functional>
#include <memory>
#include <stop_token>
#include <string>

#include <huxerui/avkit/project.h>
#include <huxerui/avkit/timeline.h>
#include <huxerui/avkit/video_texture.h>
#include <huxerui/external_texture.h>

#include "workbench_overlay.h"

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
  huxerui::avkit::VisualTransformOverride layout;
  huxerui::Rect bounds;
  float degrees;
  CanvasGuides guides;
  bool operator==(const WorkbenchDrag&) const = default;
};

struct WorkbenchFrame {
  std::shared_ptr<huxerui::ExternalTexture> texture;
  std::shared_ptr<const huxerui::avkit::ProjectSnapshot> project;
  std::optional<WorkbenchDrag> drag;
  double seconds;
  bool playing;
  bool operator==(const WorkbenchFrame&) const = default;
};

class WorkbenchPlayer {
public:
  using Present = std::function<void(WorkbenchFrame, std::stop_token, huxerui::avkit::render::PreviewFrameCommit)>;
  WorkbenchPlayer(huxerui::avkit::PixelSize canvas, OverlayAssets assets, Present present);
  ~WorkbenchPlayer();
  WorkbenchPlayback Status() const;
  bool Closed() const;
  void Close();
  void Show(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> revision, double seconds, bool play,
            std::optional<WorkbenchDrag> drag = {});

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

using WorkbenchExportFrame = std::function<void(huxerui::media::MediaTimestamp)>;
void ExportWorkbenchMovie(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> revision, const huxerui::File& file,
                          const OverlayAssets& assets, const WorkbenchExportFrame& on_frame);

} // namespace avkit_preview
