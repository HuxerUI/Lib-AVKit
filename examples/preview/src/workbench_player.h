#pragma once

#include <functional>
#include <memory>
#include <string>

#include <huxerui/avkit/project.h>
#include <huxerui/external_texture.h>

#include "workbench_overlay.h"

namespace avkit_preview {

struct WorkbenchPlayback {
  double seconds{};
  bool playing{}, ready{};
  std::string error;
  bool operator==(const WorkbenchPlayback&) const = default;
};

class WorkbenchPlayer {
public:
  WorkbenchPlayer(huxerui::avkit::PixelSize canvas, OverlayAssets assets);
  ~WorkbenchPlayer();
  std::shared_ptr<huxerui::ExternalTexture> Texture() const;
  WorkbenchPlayback Status() const;
  bool Closed() const;
  void Close();
  void Show(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> revision, double seconds, bool play,
            std::optional<huxerui::avkit::VisualTransformOverride> layout = {});

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

using WorkbenchExportFrame = std::function<void(huxerui::media::MediaTimestamp)>;
void ExportWorkbenchMovie(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> revision, const huxerui::File& file,
                          const OverlayAssets& assets, const WorkbenchExportFrame& on_frame);

} // namespace avkit_preview
