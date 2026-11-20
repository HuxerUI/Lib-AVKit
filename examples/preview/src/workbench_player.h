#pragma once

#include <memory>
#include <string>

#include <huxerui/avkit/project.h>
#include <huxerui/external_texture.h>

namespace avkit_preview {

struct WorkbenchPlayback {
  double seconds{};
  bool playing{}, ready{};
  std::string error;
  bool operator==(const WorkbenchPlayback&) const = default;
};

class WorkbenchPlayer {
public:
  explicit WorkbenchPlayer(huxerui::avkit::PixelSize canvas);
  ~WorkbenchPlayer();
  std::shared_ptr<huxerui::ExternalTexture> Texture() const;
  WorkbenchPlayback Status() const;
  bool Closed() const;
  void Close();
  void Show(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> revision, double seconds, bool play);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

void ExportWorkbenchMovie(std::shared_ptr<const huxerui::avkit::ProjectSnapshot> revision, const huxerui::File& file);

} // namespace avkit_preview
