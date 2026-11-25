#include "timeline_strip.h"
#include "timeline_preview.h"

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;

std::shared_ptr<StripPreviewOptions> options;

} // namespace

void ConfigureTimelineStrip(std::string_view path, std::string_view replacement) {
  ConfigureTimelinePreview(path, "0");
  options.reset(new StripPreviewOptions{Project::Open(File(path)), File(replacement),
                                         {TimelineProviders(), OpenCanvasThumbnailSource()}});
}

std::shared_ptr<StripPreviewOptions> TimelineStripOptions() { return options; }

} // namespace avkit_preview
