#include "workbench_media.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <huxerui/avkit/timeline.h>

#include "file_preview.h"
#include "timeline_preview.h"
#include "timeline_strip.h"
#include "workbench_player.h"

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;

double Seconds(media::MediaTimestamp time) { return double(time.ticks) / time.timescale; }

} // namespace

bool HasWorkbenchMedia() { return true; }

AssetReference ReadWorkbenchMovie(const File& file) { return InspectTimelineMovie(file); }

std::optional<View> RequestedFilePreview() {
  const auto timeline = HasTimelinePreview();
  if (!timeline && !HasFilePreview()) return std::nullopt;
  static const auto texture = timeline ? CreateTimelinePreview() : CreateFilePreview();
  if (TimelineStripOptions()) return TimelineStripPreview(texture);
  const auto seek = timeline ? SeekTimelinePreview : SeekFilePreview;
  return Column {
    Image(texture).Fit(ImageFit::Contain).With(Grow()),
    Row {
      Button("Start").OnClick([seek] { seek(false); }),
      Button("Middle").OnClick([seek] { seek(true); }),
    }.With(Spacing(8.0F), MainAlign(MainAxisAlignment::Center)),
  }.With(Padding(8.0F), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View EditorStrip(std::shared_ptr<const ProjectSnapshot> project) {
  const auto tasks = UseTaskScope();
  const auto worker = UseState(std::shared_ptr<StripWorker>{});
  const auto result = UseState(std::shared_ptr<const StripResult>{});
  const auto generation = UseState(std::uint64_t{});
  const auto left = UseState(std::int64_t{});
  const auto zoom = UseState(1U);
  Lifecycle([worker] {
    worker = std::make_shared<StripWorker>(TimelineProviders(), EncodeStripThumbnail);
  });
  const auto visible = VisibleStrip(*project, left.Get(), zoom.Get());
  const auto sampler = worker.Get();
  Lifecycle([=] {
    std::uint64_t request{};
    if (sampler && visible.range.start.Compare(visible.range.end) == -1) {
      request = sampler->Schedule(project, visible, [=](std::shared_ptr<const StripResult> strip) {
        tasks.Post([=] {
          if (generation.Get() == strip->generation) result = strip;
        });
      });
    }
    generation = request;
    return [sampler, request] { if (request) sampler->Cancel(request); };
  }, sampler, project, visible);
  const auto strip = result.Get();
  const bool complete = strip && strip->revision == project->revision && strip->viewport == visible;
  return Column{
    Canvas([project, strip, complete](PaintContext& paint, Size size) {
      if (complete) PaintTimelineStrip(paint, size, *project, *strip);
      else paint.DrawRect({0, 0, size.width, size.height}, Color::Rgb(27, 34, 43));
    }).With(Frame{.height = 52.0F * float(project->tracks.size())}, CornerRadius(8.0F)),
    Row{
      Button("向前").OnClick([left, visible] { left = visible.range.start.ticks - kStripClock; }),
      Button("向后").OnClick([left, visible] { left = visible.range.start.ticks + kStripClock; }),
      Button(zoom.Get() == 2 ? "缩小" : "放大").OnClick([zoom, visible, left] {
        left = visible.range.start.ticks;
        zoom = zoom.Get() == 2 ? 0U : zoom.Get() + 1;
      }),
    }.With(Spacing(6.0F)),
    complete && !strip->error.empty() ? View(Text(strip->error)) : View(Text::Format(
        "{}–{} 秒", WorkbenchSeconds(Seconds(visible.range.start), 1), WorkbenchSeconds(Seconds(visible.range.end), 1))),
  }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View EditorMonitor(WorkbenchContext context, std::shared_ptr<const ProjectSnapshot> project, bool wide) {
  const auto& theme = UseTheme();
  const auto tasks = UseTaskScope();
  const bool has_media = project && project->Range().end.ticks > 0;
  const bool allowed = !context.busy.Get();
  View preview = Text("导入一段视频开始剪辑。").With(Foreground(theme.colors.on_surface_variant));
  View transport = Text("");
  const auto player = UseState(std::shared_ptr<WorkbenchPlayer>{});
  const auto playback = UseState(WorkbenchPlayback{});
  const auto exporting = UseState(false);
  const auto canvas = has_media && !exporting.Get() ? std::optional(project->canvas) : std::nullopt;
  Lifecycle([player, playback, canvas] {
    auto active = canvas ? std::make_shared<WorkbenchPlayer>(*canvas) : nullptr;
    player = active;
    playback = WorkbenchPlayback{};
    return [active] { if (active) active->Close(); };
  }, canvas);
  const auto active = canvas ? player.Get() : nullptr;
  Lifecycle([active, project, playback] {
    if (active && project) active->Show(project, playback->seconds, false);
  }, active, project);
  Lifecycle([active, tasks, playback] {
    if (!active) return;
    tasks.Launch([active, playback]() -> Task<void> {
      while (!active->Closed()) {
        const auto state = active->Status();
        if (state != playback.Get()) playback = state;
        co_await Delay(std::chrono::milliseconds(60));
      }
    });
  }, active);
  if (has_media) {
    const auto duration = static_cast<float>(Seconds(project->Range().end));
    const auto status = playback.Get();
    if (active) preview = Image(active->Texture()).Fit(ImageFit::Contain).With(Grow());
    else preview = Text(exporting.Get() ? "正在导出视频…" : "正在准备预览…");
    transport = Column{
      Row{Button(status.playing ? "暂停" : "播放").OnClick([active, project, duration] {
              if (!active || active->Closed()) return;
              const auto displayed = active->Status();
              const bool at_end = displayed.seconds >= duration - 1.0 / 24.0;
              active->Show(project, !displayed.playing && at_end ? 0.0 : displayed.seconds, !displayed.playing);
            }).With(Enabled(allowed && static_cast<bool>(active))),
          Text::Format("{} / {} 秒", WorkbenchSeconds(std::clamp(status.seconds, 0.0, double(duration))), WorkbenchSeconds(duration))
              .With(Foreground(theme.colors.on_surface_variant)),
      }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)),
      Slider(std::clamp(static_cast<float>(status.seconds), 0.0F, duration)).Range(0.0F, duration)
          .OnChanged([active, project, playback](float time) {
            if (!active || active->Closed()) return;
            playback = WorkbenchPlayback{time};
            active->Show(project, time, false);
          }).With(Enabled(allowed && static_cast<bool>(active))),
      status.error.empty() ? View(Text("")) : View(Text(status.error)),
    }.With(CrossAlign(CrossAxisAlignment::Stretch), Spacing(4.0F));
  }

  const auto export_movie = [context, active, exporting, revision = project] {
    RunWorkbenchAction(context, [context, active, exporting, revision]() -> Task<void> {
      if (active) active->Close();
      exporting = true;
      try {
        auto encoded = co_await RunWorker([cache = context.cache, revision] {
          const auto output = WorkbenchOutput(cache, "Exports", "成片.mp4");
          ExportWorkbenchMovie(revision, output);
          return output;
        });
        const bool saved = co_await context.picker->SaveFileAsync(encoded, {"成片.mp4", WorkbenchMovieFilter()});
        context.message = saved ? "已导出 MP4" : "未保存导出文件";
      } catch (...) {
        exporting = false;
        throw;
      }
      exporting = false;
    });
  };
  View monitor = WorkbenchPanel(Column{
    Column{std::move(preview)}.With(Frame{.height = wide ? 300.0F : 196.0F}, CornerRadius(8.0F),
        Background(Color::Rgb(8, 11, 17)), MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center)),
    std::move(transport),
    Button("导出 MP4").OnClick(export_movie).With(Enabled(allowed && has_media && context.picker->CanSaveFiles())),
  }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme);
  return monitor;
}

} // namespace avkit_preview
