#include "workbench_player.h"

#include "timeline_resources.h"
#include "workbench_platform.h"

#include <array>
#include <cmath>
#include <utility>


namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using media::MediaTimestamp;

double Seconds(MediaTimestamp time) { return time.timescale ? double(time.ticks) / time.timescale : 0.0; }

void ReportFallback(const CodecFallback& fallback) {
  constexpr std::array operations{"Video decode", "Audio decode", "Video encode", "Audio encode"};
  LogWorkbench(WorkbenchLogLevel::Warning, "%s, %s to %s, PTS %.3f: %s [%s]",
      operations[static_cast<std::size_t>(fallback.operation)], fallback.from.c_str(), fallback.to.c_str(),
      fallback.resume_at ? Seconds(*fallback.resume_at) : -1, fallback.cause.message.c_str(),
      fallback.cause.platform_code.c_str());
}

} // namespace

WorkbenchPlayer::WorkbenchPlayer(PixelSize texture_size, std::shared_ptr<const ResourceCatalog> resources, Present present)
    : session_({texture_size, PixelFormat::Rgba8Unorm}, TimelineProviders(),
        [this, present = std::move(present)](PreviewFrame frame) {
          std::optional<WorkbenchDrag> drag;
          if (frame.transform_request) {
            const std::scoped_lock lock(drag_mutex_);
            if (*frame.transform_request == drag_request_) drag = drag_;
          }
          present({std::move(frame), std::move(drag)});
        }, {.resources = std::move(resources)}, ReportFallback) {}

WorkbenchPlayback WorkbenchPlayer::Status() const {
  const auto status = session_.Status();
  return {Seconds(status.pts), status.playing, status.ready, status.error};
}

bool WorkbenchPlayer::Closed() const { return session_.Closed(); }

void WorkbenchPlayer::Close() { session_.Close(); }

void WorkbenchPlayer::Seek(double seconds) {
  session_.Seek({std::llround(seconds * kTimelinePreviewRate), kTimelinePreviewRate});
}

void WorkbenchPlayer::SetDrag(std::optional<WorkbenchDrag> drag) {
  const std::scoped_lock lock(drag_mutex_);
  drag_request_ = drag ? session_.SetTransformDelta(drag->target, drag->delta) : 0;
  drag_ = std::move(drag);
  if (!drag_) session_.ClearTransform();
}

void ExportWorkbenchMovie(std::shared_ptr<const ProjectSnapshot> revision, const File& file,
                          const FileExportSettings& settings, std::shared_ptr<const ResourceCatalog> resources,
                          const WorkbenchExportFrame& on_frame, std::stop_token stop, WorkbenchPlayer* preview) {
  ExportProject(std::move(revision), file, TimelineProviders(), settings, {
      .scene = {.resources = std::move(resources)},
      .on_fallback = ReportFallback,
      .on_frame = on_frame,
      .stop = stop,
      .preview = preview ? &preview->Session() : nullptr,
  });
}

} // namespace avkit_preview
