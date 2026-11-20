#include "workbench_media.h"

#include <algorithm>
#include <utility>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;

double Seconds(media::MediaTimestamp time) { return double(time.ticks) / time.timescale; }

enum class EditorPane { Timeline, Assets, Inspector };

} // namespace

[[huxerui::composable]]
View WorkbenchEditor(WorkbenchContext context) {
  const auto& theme = UseTheme();
  const bool wide = UseViewportClass() == ViewportClass::Expanded;
  const auto pane = UseState(EditorPane::Timeline);
  const auto selected = UseState(std::optional<ClipId>{});
  const auto document = context.document.Get();
  const auto project = document ? document->revision : nullptr;
  const bool has_media = project && project->Range().end.ticks > 0;
  const bool has_file_media = HasWorkbenchMedia();
  const bool allowed = !context.busy.Get();
  const auto edit = [context](auto change) {
    auto next = context.document.Get();
    if (!next || context.busy.Get()) return;
    try {
      context.message = std::string{};
      if (change(*next->project)) {
        next->revision = next->project->Snapshot();
        next->dirty = true;
        context.document = std::move(next);
      }
    } catch (const std::exception& error) {
      context.message = std::string(error.what());
    }
  };

  const auto missing = UseState(std::vector<AssetId>{});
  const auto tasks = UseTaskScope();
  Lifecycle([tasks, project, missing] {
    missing = std::vector<AssetId>{};
    TaskHandle request;
    if (project) {
      request = tasks.Launch([project, missing]() -> Task<void> {
        missing = co_await RunWorker([project] { return project->MissingAssets(); });
      });
    }
    return [request] { request.Cancel(); };
  }, project);

  const auto add_movie = [context] {
    RunWorkbenchAction(context, [context]() -> Task<void> {
      const auto picked = co_await PickWorkbenchFile(context, WorkbenchMovieFilter(), "Media", "movie.mp4");
      if (!picked) co_return;
      auto asset = co_await RunWorker([file = *picked] { return ReadWorkbenchMovie(file); });
      auto next = context.document.Get();
      if (!next) next = EmptyWorkbenchProject();
      AppendWorkbenchMovie(*next, std::move(asset));
      context.document = std::move(next);
    });
  };


  View strip = Text("素材加入后显示各轨道。");
  if (has_media && has_file_media) strip = EditorStrip(project);

  std::vector<View> sources;
  sources.push_back(Text("素材库", TextRole::Title));
  if (has_file_media) sources.push_back(Button("导入视频").OnClick(add_movie).With(Enabled(allowed && context.picker->CanOpenFiles())));
  else sources.push_back(Text("当前平台的示例构建尚未接入视频预览和导出。"));
  if (project) {
    const auto missing_ids = missing.Get();
    for (const auto& asset : project->assets) {
      const bool absent = std::ranges::find(missing_ids, asset.id) != missing_ids.end();
      sources.push_back(Text(asset.source.file.Name(), TextRole::Label).LineLimit(1).Overflow(TextOverflow::Ellipsis));
      if (absent) {
        if (has_file_media) sources.push_back(Button("重新链接").OnClick([context, id = asset.id] {
          RunWorkbenchAction(context, [context, id]() -> Task<void> {
            const auto picked = co_await PickWorkbenchFile(context, WorkbenchMovieFilter(), "Media", "movie.mp4");
            if (!picked) co_return;
            auto replacement = co_await RunWorker([file = *picked] { return ReadWorkbenchMovie(file); });
            auto next = context.document.Get();
            if (!next) co_return;
            next->revision = next->project->Edit([&](ProjectEdit& draft) { draft.Relink(id, std::move(replacement)); });
            next->dirty = true;
            context.document = std::move(next);
          });
        }).With(Enabled(allowed && context.picker->CanOpenFiles())));
        else sources.push_back(Text("素材丢失"));
      }
    }
  }
  if (!project || project->assets.empty()) sources.push_back(Text("尚无素材"));
  View library = WorkbenchPanel(Column(std::move(sources)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme);

  std::vector<View> timeline;
  timeline.push_back(Text("时间线", TextRole::Title));
  timeline.push_back(std::move(strip));
  std::optional<ClipInfo> selection;
  if (project) {
    bool first_video = true;
    for (const auto& track : project->tracks) {
      const bool audio = track.kind == media::TrackKind::Audio;
      const bool video = track.kind == media::TrackKind::Video;
      std::vector<View> clips;
      for (const auto& clip : track.clips) {
        const bool is_selected = selected.Get() == clip.id;
        if (is_selected) selection = clip;
        const auto asset = std::ranges::find(project->assets, clip.placement.source.asset, &AssetInfo::id);
        const auto label = asset == project->assets.end() ? std::string("片段") : asset->source.file.Stem();
        clips.push_back(Chip(label, is_selected).OnChanged([selected, id = clip.id](bool on) {
          selected = on ? std::optional(id) : std::nullopt;
        }).With(Enabled(allowed)).Key(std::to_string(clip.id.value)));
      }
      const auto lane = audio ? "音频" : video ? first_video ? "主轨" : "叠加" : "字幕";
      if (video) first_video = false;
      timeline.push_back(Row{
        Text(lane, TextRole::Label).With(Frame{.width = 42.0F}),
        ScrollView(Row(std::move(clips)).With(Spacing(6.0F))).ScrollAxis(Axis::Horizontal).With(Grow()),
      }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)).Key(std::to_string(track.id.value)));
    }
  }
  View timeline_panel = WorkbenchPanel(Column(std::move(timeline)).With(Spacing(8.0F),
      CrossAlign(CrossAxisAlignment::Stretch)), theme);
  View inspector = WorkbenchPanel(Column{
    Text("片段属性", TextRole::Title),
    selection ? View(Text::Format("{}–{} 秒", WorkbenchSeconds(Seconds(selection->placement.timeline.start)),
                                   WorkbenchSeconds(Seconds(selection->placement.timeline.end)))) : View(Text("在时间线选择一个片段。")),
    Button(selection && selection->placement.source.reverse ? "恢复正放" : "倒放").OnClick([edit, selection] {
      if (!selection) return;
      edit([&](Project& value) {
        auto placement = selection->placement;
        placement.source.reverse = !placement.source.reverse;
        value.Edit([&](ProjectEdit& draft) { draft.SetClip(selection->id, placement); });
        return true;
      });
    }).With(Enabled(allowed && selection.has_value())),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme);

  View monitor = EditorMonitor(context, project, wide);

  View panels = wide ? View(Column{
      Row{std::move(library).With(Frame{.width = 188.0F}), std::move(monitor).With(Grow()),
          std::move(inspector).With(Frame{.width = 190.0F})}.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)),
      std::move(timeline_panel),
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch))) :
    View(Column{
      std::move(monitor),
      std::move(timeline_panel),
      SegmentedButton({"时间线", "素材库", "片段属性"}, static_cast<std::size_t>(pane.Get()))
          .OnChanged([pane](std::size_t index) { pane = static_cast<EditorPane>(index); }),
      pane.Get() == EditorPane::Assets ? std::move(library) :
          pane.Get() == EditorPane::Inspector ? std::move(inspector) : View(Text("")),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)));

  std::vector<View> toolbar;
  toolbar.push_back(Button("撤销").OnClick([edit] { edit([](Project& current) { return current.Undo(); }); })
                        .With(Enabled(allowed && document.has_value())));
  toolbar.push_back(Button("重做").OnClick([edit] { edit([](Project& current) { return current.Redo(); }); })
                        .With(Enabled(allowed && document.has_value())));
  if (!wide && has_file_media) toolbar.push_back(Button("导入").OnClick(add_movie).With(Enabled(allowed && context.picker->CanOpenFiles())));
  toolbar.push_back(Button("保存").OnClick([context] {
    RunWorkbenchAction(context, [context]() -> Task<void> {
      co_await SaveWorkbenchDocument(context);
      context.message = std::string("工程已保存");
    });
  }).With(Enabled(allowed && document.has_value())));
  toolbar.push_back(Button("新建").OnClick([context, selected] {
    RunWorkbenchAction(context, [context, selected]() -> Task<void> {
      if (const auto current = context.document.Get(); current && current->dirty)
        co_await SaveWorkbenchDocument(context);
      context.document = EmptyWorkbenchProject();
      selected = std::nullopt;
    });
  }).With(Enabled(allowed && document.has_value())));
  return ScrollView(Column{
    Text(document ? document->title + (document->dirty ? " · 未保存" : "") : "剪辑",
         TextRole::Title).LineLimit(1).Overflow(TextOverflow::Ellipsis),
    ScrollView(Row(std::move(toolbar)).With(Spacing(6.0F))).ScrollAxis(Axis::Horizontal),
    std::move(panels),
  }.With(Padding(wide ? 20.0F : 10.0F), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)));
}

} // namespace avkit_preview
