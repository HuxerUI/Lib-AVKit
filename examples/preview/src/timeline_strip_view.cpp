#include "timeline_preview.h"
#include "timeline_strip.h"

#include <algorithm>
#include <cmath>

#include <huxerui/huxerui.h>
#include <huxerui/avkit/timeline.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using huxerui::media::MediaTimestamp;

double Seconds(MediaTimestamp at) { return double(at.ticks) / at.timescale; }

struct StripPosition {
  std::int64_t left{};
  unsigned zoom{1};
  bool operator==(const StripPosition&) const = default;
};

constexpr float kTrackHeight = 52.0F;

} // namespace

void PaintTimelineStrip(PaintContext& paint, Size size, const ProjectSnapshot& project, const StripResult& strip) {
  const auto begin = Seconds(strip.viewport.range.start), end = Seconds(strip.viewport.range.end);
  const auto x = [&](MediaTimestamp at) { return float(std::clamp((Seconds(at) - begin) / (end - begin), 0.0, 1.0) * size.width); };
  const auto row = [&](TrackId track) {
    return float(std::ranges::find(project.tracks, track, &TrackInfo::id) - project.tracks.begin()) * kTrackHeight;
  };
  paint.DrawRect({0, 0, size.width, size.height}, Color::Rgb(27, 34, 43));
  for (const auto& item : strip.thumbnails) {
    const auto left = x(item.project.start), right = x(item.project.end);
    if (item.thumbnail && right > left) paint.DrawImage(item.thumbnail->image, {left, row(item.track) + 1, right - left, kTrackHeight - 2});
  }
  for (const auto& item : strip.waveform) {
    const auto left = x(item.project.start), right = x(item.project.end);
    if (right <= left || item.peaks.empty()) continue;
    const float lane = (kTrackHeight - 6) / item.peaks.size();
    for (std::size_t channel = 0; channel < item.peaks.size(); ++channel) {
      const auto& peak = item.peaks[channel];
      const auto middle = row(item.track) + 3 + (float(channel) + 0.5F) * lane;
      const auto top = middle - peak.maximum * lane * 0.5F;
      const auto height = std::max(1.0F, (peak.maximum - peak.minimum) * lane * 0.5F);
      paint.DrawRect({left, top, std::max(1.0F, right - left - 0.5F), height}, Color::Rgb(103, 216, 187));
    }
  }
}

[[huxerui::composable]] View TimelineStripPreview(std::shared_ptr<ExternalTexture> picture) {
  const auto options = TimelineStripOptions();
  auto snapshot = UseState(options->project.Snapshot());
  auto position = UseState(StripPosition{});
  auto result = UseState(std::shared_ptr<const StripResult>{});
  auto token = UseState(std::uint64_t{});
  auto error = UseState(std::string{});
  const auto tasks = UseTaskScope();
  const auto project = snapshot.Get();
  const auto visible = VisibleStrip(*project, position->left, position->zoom);
  Lifecycle([=] {
    if (!(Seconds(visible.range.end) > Seconds(visible.range.start))) return std::function<void()>{};
    const auto generation = options->worker.Schedule(project, visible, [=](std::shared_ptr<const StripResult> delivery) {
      tasks.Post([=] {
        if (token.Get() == delivery->generation) result = delivery;
      });
    });
    token = generation;
    return std::function<void()>([options, generation] { options->worker.Cancel(generation); });
  }, project, visible);

  const auto scroll = [position, visible](std::int64_t delta) {
    position.Update([&](auto& value) { value.left = visible.range.start.ticks + delta; });
  };
  const auto zoom = [position, visible](int direction) {
    position.Update([&](auto& value) {
      value.left = visible.range.start.ticks;
      value.zoom = unsigned(std::clamp(int(value.zoom) + direction, 0, 2));
    });
  };
  auto relink = [=] {
    tasks.Launch([=]() -> Task<void> {
      try {
        const auto source = co_await RunWorker([file = options->replacement] { return InspectTimelineMovie(file); });
        const auto current = options->project.Snapshot();
        if (current->assets.empty()) throw std::runtime_error("Project has no asset to relink");
        auto next = options->project.Edit([&](ProjectEdit& edit) { edit.Relink(current->assets.front().id, source); });
        ReplaceTimelinePreviewProject(next);
        snapshot = next;
        error = std::string{};
      } catch (const std::exception& failure) {
        error = std::string(failure.what());
      }
    });
  };
  const auto current = result.Get();
  const auto ready = current && current->revision == project->revision && current->viewport == visible;
  const auto message = !error->empty() ? error.Get() : ready ? current->error : std::string("Sampling timeline…");
  return Column {
    Image(picture).Fit(ImageFit::Contain).With(Grow()),
    Canvas([project, current, ready](PaintContext& paint, Size size) {
      if (ready) PaintTimelineStrip(paint, size, *project, *current);
      else paint.DrawRect({0, 0, size.width, size.height}, Color::Rgb(27, 34, 43));
    }).On<ViewEvents::ScrollInput>([scroll](const ScrollInputEvent& event) {
      const auto delta = event.delta_x == 0 ? event.delta_y : event.delta_x;
      if (std::isfinite(delta)) scroll(std::llround(delta));
      return true;
    }).With(Frame{.height = std::max(1.0F, float(project->tracks.size())) * kTrackHeight}),
    Row {
      Button("Start").OnClick([] { SeekTimelinePreview(false); }),
      Button("Scroll left").OnClick([scroll] { scroll(-std::int64_t(kStripClock) / 2); }),
      Button("Scroll right").OnClick([scroll] { scroll(kStripClock / 2); }),
      Button("Zoom out").OnClick([zoom] { zoom(-1); }),
      Button("Zoom in").OnClick([zoom] { zoom(1); }),
      Button("Relink first asset").OnClick(relink),
    }.With(Spacing(5.0F)),
    message.empty() ? Text::Format("{:.2f}–{:.2f} s", Seconds(visible.range.start), Seconds(visible.range.end)) : Text(message),
  }.With(Padding(8.0F), Spacing(5.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

} // namespace avkit_preview
