#include <huxerui/huxerui.h>
#include <huxerui/avkit.h>

#if defined(AVKIT_CAPTURE_PREVIEW)
#include <huxerui/camera.h>
#include "capture_preview.h"
#endif

#if defined(AVKIT_MACOS_PREVIEW)
#include <exception>

#include "metal_preview.h"
#if defined(AVKIT_FILE_MEDIA)
#include "file_preview.h"
#include "timeline_preview.h"
#include "timeline_strip.h"
#endif
#endif

using namespace huxerui;

View App() {
#if defined(AVKIT_CAPTURE_PREVIEW)
  if (avkit_preview::HasCapturePreview()) return avkit_preview::CapturePreview();
#endif
#if defined(AVKIT_MACOS_PREVIEW)
  try {
    static const auto texture = [] {
#if defined(AVKIT_FILE_MEDIA)
      if (auto timeline = avkit_preview::CreateTimelinePreview()) return timeline;
      if (auto movie = avkit_preview::CreateFilePreview()) return movie;
#endif
      return avkit_preview::CreateMetalPreview();
    }();
#if defined(AVKIT_FILE_MEDIA)
    if (avkit_preview::TimelineStripOptions()) return avkit_preview::TimelineStripPreview(texture);
    if (avkit_preview::HasTimelinePreview() || avkit_preview::HasFilePreview()) {
      const auto seek = avkit_preview::HasTimelinePreview() ? avkit_preview::SeekTimelinePreview : avkit_preview::SeekFilePreview;
      return Column {
        Image(texture).Fit(ImageFit::Contain).With(Grow()),
        Row {
          Button("Start").OnClick([seek] { seek(false); }),
          Button("Middle").OnClick([seek] { seek(true); }),
        }.With(Spacing(8.0F), MainAlign(MainAxisAlignment::Center)),
      }.With(Padding(8.0F), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
    }
#endif
    return Image(texture).Fit(ImageFit::Contain);
  } catch (const std::exception& error) {
    return Text(error.what());
  }
#else
  return Text("AVKit preview");
#endif
}

const Application application{
    App,
    {
        .window = {
            .title = "AVKit Preview",
#if defined(AVKIT_MACOS_PREVIEW)
            .initial_size = {640.0F, 360.0F},
#endif
        },
        .application_hooks = {
          huxerui::avkit::Install,
#if defined(AVKIT_CAPTURE_PREVIEW)
          huxerui::camera::Install,
          huxerui::audio::Install,
#endif
        },
    }
};
