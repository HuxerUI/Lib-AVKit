#include "workbench_media.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <utility>

#include "timeline_playback.h"
#include "workbench_player.h"
#include "app_resources.h"
#include <huxerui/avkit/file_movie.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;

double Seconds(media::MediaTimestamp time) { return double(time.ticks) / time.timescale; }

PixelSize PreviewSize(PixelSize canvas) {
  const auto short_edge = std::min(canvas.width, canvas.height);
  const auto long_edge = std::max(canvas.width, canvas.height);
  const auto scale = std::max(1U, std::min((540U + short_edge - 1U) / short_edge, 1280U / long_edge));
  return {canvas.width * scale, canvas.height * scale};
}

PixelSize ExportSize(PixelSize canvas, unsigned short_edge) {
  const auto scale = double(short_edge) / std::min(canvas.width, canvas.height);
  const auto even = [scale](unsigned size) {
    return static_cast<unsigned>(std::clamp(std::round(size * scale / 2.0), 1.0, 8192.0) * 2.0);
  };
  return {even(canvas.width), even(canvas.height)};
}

std::string ResolutionLabel(unsigned short_edge) { return short_edge == 2160 ? "4K" : std::to_string(short_edge) + "p"; }

bool ProbeExportSize(PixelSize size) {
  if (size.width > 8192 || size.height > 8192) return false;
  media::VideoEncoderConfig config;
  config.input = {size.width, size.height, {0, 0, size.width, size.height}, media::VideoPixelFormat::I420};
  config.input.primaries = media::ColorPrimaries::BT709;
  config.input.transfer = media::ColorTransfer::BT709;
  config.input.matrix = media::ColorMatrix::BT709;
  config.input.range = media::ColorRange::Limited;
  config.frame_rate_numerator = kTimelinePreviewRate;
  return CreateFFmpegCodecProvider()->Probe(config).Succeeded();
}

enum class ExportStage { Rendering, Cancelling, Generated, Saving, Saved, Failed };

struct ExportWork {
  std::stop_source stop;
  std::atomic<float> progress{};
};

struct ExportFile {
  explicit ExportFile(File location) : path(std::move(location)) {}
  ~ExportFile() {
    if (path.Delete()) {
      if (const auto directory = path.Parent()) (void)directory->Delete();
    }
  }
  File path;
};

struct ExportStatus {
  ExportStage stage;
  std::string project_title;
  PixelSize resolution;
  double duration;
  std::shared_ptr<ExportWork> work;
  std::shared_ptr<ExportFile> file;
  std::string error;
  bool operator==(const ExportStatus&) const = default;
};

View ExportPanel(const ThemeSpec& theme, const ExportStatus& status, float progress,
                 std::function<void()> close, std::function<void()> save, std::function<void()> retry) {
  const char* title{};
  const char* message{};
  View activity, actions;
  const auto button = [&](const char* label, const std::function<void()>& action, WorkbenchActionTone tone) {
    return WorkbenchControl(Button(label).OnClick(action), theme, tone);
  };
  switch (status.stage) {
    case ExportStage::Rendering:
      title = "正在导出视频";
      message = progress >= 0.99F ? "正在封装音视频" : "正在合成时间线";
      activity = Column{
        Row{Text(message, TextRole::Label), Spacer().With(Grow()),
            Text(std::to_string(int(progress * 100.0F)) + "%", TextRole::Label)}
            .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
        ProgressBar(progress),
      }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
      actions = button("取消导出", close, WorkbenchActionTone::Surface);
      break;
    case ExportStage::Cancelling:
      title = "正在取消导出";
      activity = ProgressBar();
      break;
    case ExportStage::Generated:
      title = "成片已生成";
      message = "选择保存位置";
      actions = Row{button("返回剪辑", close, WorkbenchActionTone::Surface).With(Grow()),
                    button("保存视频", save, WorkbenchActionTone::Primary).With(Grow())}.With(Spacing(8.0F));
      break;
    case ExportStage::Saving:
      title = "正在保存视频";
      activity = ProgressBar();
      break;
    case ExportStage::Saved:
      title = "视频已保存";
      actions = button("完成", close, WorkbenchActionTone::Primary);
      break;
    case ExportStage::Failed:
      title = "导出未完成";
      actions = Row{button("返回剪辑", close, WorkbenchActionTone::Surface).With(Grow()),
                    button("重新导出", retry, WorkbenchActionTone::Primary).With(Grow())}.With(Spacing(8.0F));
      break;
  }
  const auto spec = "MP4  ·  " + std::to_string(status.resolution.width) + " × " + std::to_string(status.resolution.height) +
      "  ·  " + std::to_string(kTimelinePreviewRate) + " fps";
  return Column{
    Text(title, TextRole::Title),
    Column{
      Text(status.project_title).LineLimit(1).Overflow(TextOverflow::Ellipsis),
      Text(spec, TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
      Text(WorkbenchTimelineTime(status.duration, true), TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    }.With(Padding(12.0F), Spacing(4.0F), Background(theme.colors.surface_container_low), CornerRadius(8.0F),
           CrossAlign(CrossAxisAlignment::Stretch)),
    !status.error.empty() ? View(Text(status.error).LineLimit(3).Overflow(TextOverflow::Ellipsis)
        .With(Foreground(theme.colors.error))) : message && !activity ? View(Text(message)) : View{},
    std::move(activity),
    std::move(actions),
  }.With(Frame{.width = 360.0F}, Padding(20.0F), Spacing(16.0F), Background(theme.colors.surface_container),
         CornerRadius(16.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

} // namespace

AssetReference ReadWorkbenchMusic(const File& file, const File& prepared, const ProjectSnapshot& project) {
  media::AudioFormat format{48000, media::AudioSampleFormat::S16, media::AudioPacking::Interleaved,
                            media::AudioChannelLayout::Stereo};
  const auto lane = std::ranges::find_if(project.tracks, [](const TrackInfo& track) {
    return track.kind == media::TrackKind::Audio && !track.clips.empty();
  });
  if (lane != project.tracks.end()) {
    const auto asset = std::ranges::find(project.assets, lane->clips.front().placement.source.asset, &AssetInfo::id);
    format = ImportMovieAudio(asset->source.file).pcm;
  }
  PrepareTimelineAudio(file, prepared, format);
  return InspectTimelineAudio(prepared);
}

[[huxerui::composable]]
View WorkbenchMonitor(WorkbenchContext context, std::shared_ptr<const ProjectSnapshot> project, EditorTransport timeline, bool wide,
                       std::function<void()> back, std::function<void()> project_menu, EditorMonitorViews views,
                       std::optional<VisualTransformOverride> layout, WorkbenchExport export_movie) {
  const auto& theme = UseTheme();
  const auto tasks = UseTaskScope();
  const bool has_media = project && project->Range().end.ticks > 0;
  const bool allowed = !context.busy.Get();
  View preview = Column{
    Text("导入素材开始剪辑", TextRole::Title).With(Foreground(theme.colors.on_surface)),
  }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Center));
  View transport{};
  using PreviewSession = std::pair<PixelSize, std::shared_ptr<WorkbenchPlayer>>;
  const auto player = UseState(PreviewSession{});
  const auto playback = UseState(WorkbenchPlayback{});
  const auto exporting = context.exporting;
  const auto resource_pack = views.assets;
  const auto size = has_media ? PreviewSize(project->canvas) : PixelSize{};
  const auto canvas = has_media && !exporting.Get() && resource_pack ? std::optional(size) : std::nullopt;
  Lifecycle([player, playback, canvas, resource_pack, timeline] {
    auto active = canvas ? std::make_shared<WorkbenchPlayer>(*canvas, *resource_pack) : nullptr;
    player = PreviewSession{canvas.value_or(PixelSize{}), active};
    playback = WorkbenchPlayback{timeline.position.Get()};
    return [active] { if (active) active->Close(); };
  }, canvas);
  const auto session = player.Get();
  const auto active = canvas && *canvas == session.first ? session.second : nullptr;
  using PreparedPicture = std::pair<std::shared_ptr<const ProjectSnapshot>, std::shared_ptr<const ProjectSnapshot>>;
  const auto prepared = UseState(PreparedPicture{});
  Lifecycle([prepared, project, size] {
    auto picture = PrepareWorkbenchPicture(project);
    if (picture && size.width && size != picture->canvas) {
      auto preview = std::make_shared<ProjectSnapshot>(*picture);
      preview->canvas = size;
      picture = std::move(preview);
    }
    prepared = PreparedPicture{project, std::move(picture)};
  }, project, size);
  const auto picture = prepared->first == project ? prepared->second : nullptr;
  Lifecycle([active, picture, timeline, layout] {
    if (active && picture) active->Show(picture, timeline.position.Get(), false, layout);
  }, active, picture, layout);
  const auto seek = timeline.seek.Get();
  Lifecycle([active, picture, timeline, playback, seek] {
    if (!active || !picture || !seek || active->Closed()) return;
    timeline.seek = std::nullopt;
    playback = WorkbenchPlayback{*seek};
    active->Show(picture, *seek, false);
  }, active, picture, seek);
  Lifecycle([active, tasks, playback, timeline] {
    if (!active) return;
    tasks.Launch([active, playback, timeline]() -> Task<void> {
      while (!active->Closed()) {
        const auto state = active->Status();
        if (state != playback.Get()) playback = state;
        if (state.ready && !timeline.scrubbing.Get() && !timeline.seek.Get() && state.seconds != timeline.position.Get())
          timeline.position = state.seconds;
        co_await Delay(std::chrono::milliseconds(60));
      }
    });
  }, active);
  const auto duration = has_media ? Seconds(project->Range().end) : 0.0;
  const auto status = playback.Get();
  if (has_media) {
    if (active) {
      preview = WorkbenchVideoPicture(active->Texture()).With(Grow());
    } else if (exporting.Get()) preview = {};
    else preview = Text("正在准备预览…");
  }
  const auto play = [active, picture, duration] {
    if (!active || !picture || active->Closed()) return;
    const auto displayed = active->Status();
    const bool at_end = displayed.seconds >= duration - 1.0 / kTimelinePreviewRate;
    active->Show(picture, !displayed.playing && at_end ? 0.0 : displayed.seconds, !displayed.playing);
  };
  const auto document = context.document.Get();
  const auto change_history = [context](bool redo) {
    auto next = context.document.Get();
    if (!next || context.busy.Get()) return;
    try {
      if (redo ? next->project->Redo() : next->project->Undo()) {
        next->revision = next->project->Snapshot();
        next->dirty = true;
        context.message = std::string{};
        context.document = std::move(next);
      }
    } catch (const std::exception& failure) {
      context.message = std::string(failure.what());
    }
  };
  const auto restart = [timeline] {
    timeline.position = 0.0;
    timeline.seek = 0.0;
  };
  const auto clock = WorkbenchTimelineTime(std::clamp(timeline.position.Get(), 0.0, duration), true) +
      " / " + WorkbenchTimelineTime(duration, false);
  View position = Text(clock).Style(TextStyle{.font = Font::Monospace(11.0F),
                                             .foreground = theme.colors.on_surface_variant}).LineLimit(1);
  if (active && allowed) {
    position = std::move(position).With(Padding(EdgeInsets::Symmetric(0.0F, 9.0F)),
        Semantics{.role = SemanticRole::Button, .label = clock + "，回到起点"}).OnClick(restart);
  }
  transport = Row{
    Row{std::move(position), Spacer()}.With(Grow(), CrossAlign(CrossAxisAlignment::Center)),
    WorkbenchTool(status.playing ? app::images::icons_pause : app::images::icons_play, status.playing ? "暂停" : "播放",
                  theme, play, allowed && bool(active), false),
    Row{
      Spacer(),
      WorkbenchTool(app::images::icons_undo, "撤销", theme, [change_history] { change_history(false); },
                    allowed && document && document->project->CanUndo(), false),
      WorkbenchTool(app::images::icons_redo, "重做", theme, [change_history] { change_history(true); },
                    allowed && document && document->project->CanRedo(), false),
    }.With(Grow(), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Frame{.height = 44.0F}, Padding(EdgeInsets::Symmetric(12.0F, 0.0F)),
        CrossAlign(CrossAxisAlignment::Center));

  const auto export_menu = UseMenu();
  const auto export_anchor = UseViewHandle();
  const auto export_height = UseState(1080U);
  const auto export_status = UseState(std::optional<ExportStatus>{});
  const auto export_progress = UseState(0.0F);
  const auto export_view = export_status.Get();
  const auto export_work = export_view ? export_view->work : nullptr;
  Lifecycle([tasks, export_status, export_progress, export_work] {
    TaskHandle poll;
    if (export_work) poll = tasks.Launch([=]() -> Task<void> {
      while (const auto current = export_status.Get()) {
        if (current->work != export_work) break;
        export_progress = export_work->progress.load(std::memory_order_relaxed);
        co_await Delay(std::chrono::milliseconds(100));
      }
    });
    return [poll] { poll.Cancel(); };
  }, export_work);
  const auto export_probe = UseState(std::optional<std::pair<PixelSize, bool>>{});
  const auto largest = has_media ? std::optional(ExportSize(project->canvas, 2160U)) : std::nullopt;
  Lifecycle([tasks, export_probe, largest] {
    TaskHandle probe;
    if (largest) probe = tasks.Launch([export_probe, largest]() -> Task<void> {
      const auto supported = co_await RunWorker([size = *largest] { return ProbeExportSize(size); });
      export_probe = std::pair{*largest, supported};
    });
    return [probe] { probe.Cancel(); };
  }, largest);
  const auto probed = export_probe.Get();
  const bool high_resolution = probed && largest == probed->first && probed->second;
  const auto selected_height = export_height.Get();
  const auto resolution = has_media ? ExportSize(project->canvas, selected_height) : PixelSize{};
  const auto fits_encoder = [high_resolution](PixelSize size, unsigned height) {
    return size.width <= 8192 && size.height <= 8192 && (height != 2160 || high_resolution);
  };
  const auto show_resolutions = [=] {
    std::vector<MenuEntry> choices;
    for (auto height : {720U, 1080U, 2160U}) {
      const auto size = ExportSize(project->canvas, height);
      choices.emplace_back(MenuItem(ResolutionLabel(height) + "  " + std::to_string(size.width) + "×" +
          std::to_string(size.height), [export_height, height] { export_height = height; })
          .Checked(selected_height == height).Enabled(fits_encoder(size, height)));
    }
    export_menu.Show(export_anchor, std::move(choices));
  };
  const auto begin_export = [=, revision = project] {
    if (!resource_pack || context.busy.Get()) return;
    const auto work = std::make_shared<ExportWork>();
    export_progress = 0.0F;
    export_status = ExportStatus{ExportStage::Rendering, document ? document->title : "剪辑工程", resolution, duration, work};
    RunWorkbenchAction(context, [=]() -> Task<void> {
      exporting = true;
      try {
        if (auto current = player.Get().second) current->Close();
        auto encoded = co_await RunWorker([cache = context.cache, export_movie, revision, resource_pack, resolution, duration, work] {
          const auto output = std::make_shared<ExportFile>(WorkbenchOutput(cache, "Exports", "成片.mp4"));
          auto render_revision = std::make_shared<ProjectSnapshot>(*revision);
          render_revision->canvas = resolution;
          export_movie(PrepareWorkbenchPicture(std::move(render_revision)), output->path, *resource_pack,
              [work, duration](media::MediaTimestamp pts) {
                if (work->stop.stop_requested()) throw std::runtime_error("Export cancelled");
                work->progress.store(std::clamp(float(Seconds(pts) / duration), 0.0F, 0.99F), std::memory_order_relaxed);
              });
          return output;
        });
        export_status.Update([&](auto& current) {
          current->stage = ExportStage::Generated;
          current->work.reset();
          current->file = std::move(encoded);
        });
      } catch (const std::exception& error) {
        export_status.Update([&](auto& current) {
          current->stage = ExportStage::Failed;
          current->work.reset();
          current->error = error.what();
        });
      }
      exporting = false;
      if (work->stop.stop_requested()) export_status = std::nullopt;
    });
  };
  const auto close_export = [export_status] {
    const auto current = export_status.Get();
    if (!current || current->stage == ExportStage::Saving || current->stage == ExportStage::Cancelling) return;
    if (current->work) {
      current->work->stop.request_stop();
      export_status.Update([](auto& next) { next->stage = ExportStage::Cancelling; });
    } else {
      export_status = std::nullopt;
    }
  };
  const auto save_export = [context, export_status] {
    const auto current = export_status.Get();
    if (!current || current->stage != ExportStage::Generated || context.busy.Get()) return;
    export_status.Update([](auto& next) {
      next->stage = ExportStage::Saving;
      next->error.clear();
    });
    RunWorkbenchAction(context, [context, export_status, file = current->file]() -> Task<void> {
      bool saved = false;
      std::string error;
      try {
        saved = co_await context.picker->SaveFileAsync(file->path, {"成片.mp4", {"MP4 视频", {"mp4"}, {"video/mp4"}}});
      } catch (const std::exception& failure) { error = failure.what(); }
      export_status.Update([&](auto& next) {
        next->stage = saved ? ExportStage::Saved : ExportStage::Generated;
        next->error = std::move(error);
        if (saved) next->file.reset();
      });
    });
  };
  const auto feedback = export_view ? std::string{} : !status.error.empty() ? status.error :
      context.busy.Get() ? "正在处理…" : context.message.Get();
  auto stage = Column{
    Stack{
      std::move(preview),
      active ? std::move(views.canvas) : View{},
      feedback.empty() ? View{} : View(Row{
        Text(feedback, TextRole::Label).LineLimit(2).With(Padding(EdgeInsets::Symmetric(10.0F, 5.0F)),
            Foreground(status.error.empty() ? theme.colors.on_surface : theme.colors.error),
            Background(theme.colors.surface_container_high), CornerRadius(6.0F)),
      }.With(Padding(8.0F), MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Start))),
    }
        .With(Grow(), Frame{.min_height = 110.0F}, Background(theme.colors.background),
              Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch), ClipChildren{}),
    std::move(transport),
  }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch));
  View monitor = Column{
    Row{
      wide ? View{} : WorkbenchBack("返回工作台", theme, std::move(back), allowed),
      Column{
        Text(document ? document->title : "剪辑工程", TextRole::Label)
            .LineLimit(1).Overflow(TextOverflow::Ellipsis),
        document ? View(Text(document->dirty ? "草稿未保存" : "草稿已保存")
             .Style(TextStyle{.font = Font::System(10.0F), .foreground = theme.colors.on_surface_variant})) : View{},
      }.With(Spacing(2.0F), Frame{.max_width = wide ? 220.0F : 116.0F}),
      WorkbenchCompactAction(Image(app::images::icons_more).With(InheritForeground{}, Frame{.width = 12.0F, .height = 12.0F}),
                             "工程菜单", theme,
                             std::move(project_menu), allowed),
      Spacer().With(Grow()),
      has_media ? WorkbenchCompactAction(Row{
        Text(ResolutionLabel(selected_height)).Style(TextStyle{.font = Font::System(12.0F).WithWeight(FontWeight::SemiBold)})
            .With(InheritForeground{}),
        Image(app::images::icons_dropdown).With(InheritForeground{}, Frame{.width = 7.0F, .height = 12.0F}),
      }.With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Center)), "导出分辨率 " + ResolutionLabel(selected_height),
          theme, show_resolutions, allowed).With(export_anchor) : View{},
      WorkbenchControl(Button(wide ? "导出 MP4" : "导出").OnClick(begin_export)
          .With(Enabled(allowed && has_media && resource_pack && fits_encoder(resolution, selected_height) &&
                        context.picker->CanSaveFiles())),
          theme, WorkbenchActionTone::Primary, true),
    }.With(Padding(EdgeInsets::Symmetric(wide ? 14.0F : 6.0F, 2.0F)), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)),
    Divider(),
    wide ? View(Row{std::move(views.media), std::move(stage), std::move(views.inspector)}
        .With(Grow(), Spacing(1.0F), CrossAlign(CrossAxisAlignment::Stretch))) : View(std::move(stage)),
    Divider(),
  }.With(Background(theme.colors.surface), CrossAlign(CrossAxisAlignment::Stretch));
  if (export_view) {
    const auto progress = export_progress.Get();
    monitor = std::move(monitor).With(Dialog{
        .visible = true,
        .content = [=] { return ExportPanel(theme, *export_view, progress, close_export, save_export, begin_export); },
        .dismiss_on_cancel = true,
        .on_dismiss_request = close_export,
    });
  }
  return monitor;
}

std::shared_ptr<const StripResult> UseWorkbenchStrip(std::shared_ptr<const ProjectSnapshot> project,
                                                       StripViewport visible, CreateStripWorker create_worker) {
  const auto tasks = UseTaskScope();
  const auto worker = UseState(std::shared_ptr<StripWorker>{});
  const auto generation = UseState(std::uint64_t{});
  using Sample = std::pair<std::shared_ptr<const ProjectSnapshot>, std::shared_ptr<const StripResult>>;
  struct PendingStrip {
    std::mutex mutex;
    std::shared_ptr<const StripResult> latest;
    bool queued{};
  };
  const auto result = UseState(Sample{});
  Lifecycle([worker, create_worker] { worker = create_worker(); }, create_worker);
  const auto sampler = worker.Get();
  Lifecycle([=] {
    std::uint64_t request{};
    if (result->first != project) result = Sample{};
    if (sampler && project && visible.range.start.Compare(visible.range.end) == -1) {
      auto pending = std::make_shared<PendingStrip>();
      const auto deliver = [tasks, generation, result, project, pending](std::shared_ptr<const StripResult> strip) {
        {
          std::scoped_lock lock(pending->mutex);
          pending->latest = std::move(strip);
          if (std::exchange(pending->queued, true)) return;
        }
        tasks.Post([generation, result, project, pending] {
          std::shared_ptr<const StripResult> strip;
          {
            std::scoped_lock lock(pending->mutex);
            strip = std::move(pending->latest);
            pending->queued = false;
          }
          if (generation.Get() == strip->generation) result = Sample{project, strip};
        });
      };
      request = sampler->Schedule(project, visible, deliver, deliver);
    }
    generation = request;
    return [sampler, request] { if (request) sampler->Cancel(request); };
  }, sampler, project, visible);
  const auto sample = result.Get();
  return sample.first == project ? sample.second : nullptr;
}

} // namespace avkit_preview
