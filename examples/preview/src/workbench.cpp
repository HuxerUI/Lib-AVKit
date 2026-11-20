#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "app_resources.h"

#include "capture_preview.h"
#include "workbench_media.h"

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;

enum class Page : std::size_t { Home, Capture, Edit, Templates };
enum class TemplateStep { Browse, Preview, Bind };

struct Preset {
  WorkbenchTemplate kind;
  const char* title;
  const char* description;
  const char* layout;
  ImageResource image;
};

const std::array kPresets{
    Preset{WorkbenchTemplate::StageCut, "舞台快剪", "两段视频依次拼接，每段最多六秒。", "16:9 · 顺序拼接", app::images::workbench_stage},
    Preset{WorkbenchTemplate::Portrait, "竖屏记录", "两段视频排进竖屏画布，每段最多五秒。", "9:16 · 顺序拼接", app::images::workbench_portrait},
    Preset{WorkbenchTemplate::PictureInPicture, "双镜头画中画", "两段视频同时播放，保留主镜头音轨。", "16:9 · 画中画", app::images::workbench_dissolve},
};

ThemeSpec WorkbenchTheme() {
  auto theme = MaterialDarkThemeSpec();
  theme.colors.background = Color::Rgb(13, 16, 24);
  theme.colors.surface = Color::Rgb(23, 28, 41);
  theme.colors.surface_container_low = Color::Rgb(19, 23, 35);
  theme.colors.surface_container = Color::Rgb(32, 40, 57);
  theme.colors.surface_container_high = Color::Rgb(39, 48, 66);
  theme.colors.surface_container_highest = Color::Rgb(47, 57, 77);
  theme.colors.on_surface = Color::Rgb(241, 245, 253);
  theme.colors.on_surface_variant = Color::Rgb(164, 179, 203);
  theme.colors.primary = Color::Rgb(118, 228, 216);
  theme.colors.on_primary = Color::Rgb(15, 29, 36);
  theme.colors.secondary_container = Color::Rgb(44, 79, 83);
  theme.colors.on_secondary_container = Color::Rgb(203, 247, 242);
  theme.colors.outline = Color::Rgb(65, 78, 100);
  return theme;
}

View Cover(ImageResource image, float height) {
  return Image(image).Fit(ImageFit::Cover).With(Frame{.height = height}, CornerRadius(12.0F));
}

void OpenDocument(const WorkbenchContext& context, State<Page> page, std::optional<File> stored = {}) {
  RunWorkbenchAction(context, [context, page, stored = std::move(stored)]() -> Task<void> {
    auto file = stored;
    if (!file) file = co_await PickWorkbenchFile(context, WorkbenchProjectFilter(), "Projects", "project.avkit");
    if (!file) co_return;
    if (const auto current = context.document.Get(); current && current->dirty)
      co_await SaveWorkbenchDocument(context);
    auto opened = co_await RunWorker([source = *file] { return OpenWorkbenchProject(source); });
    context.document = std::move(opened);
    page = Page::Edit;
  });
}

View Entry(const char* title, const char* detail, const ThemeSpec& theme, const std::function<void()>& open, bool busy) {
  return WorkbenchPanel(Row{
    Column{Text(title, TextRole::Title), Text(detail).With(Foreground(theme.colors.on_surface_variant))}
        .With(Spacing(4.0F), Grow()),
    Button("进入").OnClick(open).With(Enabled(!busy)),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)), theme);
}

[[huxerui::composable]]
View Home(WorkbenchContext context, State<Page> page, bool wide) {
  const auto& theme = UseTheme();
  const auto tasks = UseTaskScope();
  const auto recent = UseState(std::vector<File>{});
  Lifecycle([context, tasks, recent] {
    tasks.Launch([context, recent]() -> Task<void> {
      try {
        recent = co_await RunWorker([data = context.data] { return RecentWorkbenchProjects(data); });
      } catch (const std::exception& failure) {
        context.message = std::string(failure.what());
      }
    });
  });

  const bool busy = context.busy.Get();
  const auto files = recent.Get();
  const auto current = context.document.Get();
  View entries = Column{
    Entry("录制", "拍照、贴纸录像和实时变声", theme, [page] { page = Page::Capture; }, busy),
    Entry("剪辑", "导入素材、编辑轨道和导出成片", theme, [page] { page = Page::Edit; }, busy),
    Entry("模板", "选两个镜头，一键生成可编辑工程", theme, [page] { page = Page::Templates; }, busy),
  }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch));
  View feature = WorkbenchPanel(Column{
    Cover(kPresets.front().image, wide ? 226.0F : 138.0F),
    Text(current ? current->title : files.empty() ? "从一段视频开始" : files.front().Stem(), TextRole::Title)
        .LineLimit(1).Overflow(TextOverflow::Ellipsis),
    Button(current || !files.empty() ? "继续剪辑" : "新建剪辑").OnClick([context, page, current, files] {
      if (current || files.empty()) page = Page::Edit;
      else OpenDocument(context, page, files.front());
    }).With(Enabled(!busy)),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme);
  View overview = wide ? View(Row{std::move(feature).With(Grow(2.0F)), std::move(entries).With(Grow())}
                             .With(Spacing(16.0F), CrossAlign(CrossAxisAlignment::Stretch)))
                       : View(Column{std::move(entries), std::move(feature)}
                             .With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)));
  std::vector<View> rows;
  for (const auto& file : files) {
    rows.push_back(Button(file.Stem()).OnClick([context, page, file] { OpenDocument(context, page, file); })
                       .With(Enabled(!busy)).Key(file.Path()));
  }
  if (rows.empty()) rows.push_back(Text("保存后的工程会出现在这里。").With(Foreground(theme.colors.on_surface_variant)));
  return ScrollView(Column{
    Text("音视频工作台").Style(TextStyle{.font = Font::System(wide ? 38.0F : 28.0F), .foreground = theme.colors.on_surface}),
    std::move(overview),
    WorkbenchPanel(Column{
      Row{Text("最近项目", TextRole::Title).With(Grow()),
          Button("打开工程").OnClick([context, page] { OpenDocument(context, page); })
              .With(Enabled(!busy && context.picker->CanOpenFiles()))}
          .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
      Column(std::move(rows)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme),
  }.With(Spacing(16.0F), Padding(wide ? 24.0F : 12.0F), CrossAlign(CrossAxisAlignment::Stretch)));
}

[[huxerui::composable]]
View Templates(WorkbenchContext context, State<Page> page, bool wide) {
  const auto& theme = UseTheme();
  const auto step = UseState(TemplateStep::Browse);
  const auto which = UseState(std::size_t{});
  const auto inputs = UseState(std::array<std::optional<AssetReference>, 2>{});
  const auto& preset = kPresets[which.Get()];
  const bool busy = context.busy.Get();
  std::vector<View> choices;
  for (std::size_t index = 0; index < kPresets.size(); ++index) {
    const auto& item = kPresets[index];
    choices.push_back(WorkbenchPanel(Column{
       Cover(item.image, wide ? 100.0F : 125.0F),
       Button(item.title).OnClick([which, step, index] {
         which = index;
         step = TemplateStep::Preview;
       }).With(Enabled(!busy)),
    }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme));
  }
  View catalog = Column(std::move(choices)).With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch));
  View preview = WorkbenchPanel(Column{
    Cover(preset.image, wide ? 282.0F : 208.0F),
    Text(preset.title, TextRole::Title),
    Text(preset.layout).With(Foreground(theme.colors.primary)),
    Text(preset.description),
    wide ? View(Text("示例封面 · 生成后可在剪辑页预览你的素材", TextRole::Label)) :
           View(Button("选择素材").OnClick([step] { step = TemplateStep::Bind; }).With(Enabled(!busy))),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme);
  std::vector<View> slots;
  slots.push_back(Text("绑定镜头", TextRole::Title));
  if (HasWorkbenchMedia()) {
    for (std::size_t i = 0; i < inputs.Get().size(); ++i) {
      const auto& asset = inputs.Get()[i];
      slots.push_back(Text(i == 0 ? "主镜头" : "第二镜头", TextRole::Label));
      slots.push_back(Button(asset ? asset->file.Name() : "选择视频").OnClick([context, inputs, i] {
        RunWorkbenchAction(context, [context, inputs, i]() -> Task<void> {
          const auto picked = co_await PickWorkbenchFile(context, WorkbenchMovieFilter(), "Media", "movie.mp4");
          if (!picked) co_return;
          auto asset = co_await RunWorker([file = *picked] { return ReadWorkbenchMovie(file); });
          inputs.Update([&](auto& current) { current[i] = std::move(asset); });
        });
      }).With(Enabled(!busy && context.picker->CanOpenFiles())));
    }
    slots.push_back(Button("生成并进入剪辑").OnClick([context, inputs, page, kind = preset.kind] {
      RunWorkbenchAction(context, [context, inputs, page, kind]() -> Task<void> {
        auto selected = inputs.Get();
        if (!selected[0] || !selected[1]) co_return;
        auto document = co_await RunWorker([assets = std::array{*selected[0], *selected[1]}, kind]() mutable {
          return MakeWorkbenchTemplate(kind, std::move(assets));
        });
        if (const auto current = context.document.Get(); current && current->dirty)
          co_await SaveWorkbenchDocument(context);
        context.document = std::move(document);
        page = Page::Edit;
      });
    }).With(Enabled(!busy && inputs.Get()[0].has_value() && inputs.Get()[1].has_value())));
  } else {
    slots.push_back(Text("当前平台的示例构建尚未接入视频素材处理。"));
  }
  View binding = WorkbenchPanel(Column(std::move(slots)).With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme);
  const auto current = step.Get();
  View body = wide ? View(Row{std::move(catalog).With(Frame{.width = 194.0F}), std::move(preview).With(Grow()),
                             std::move(binding).With(Frame{.width = 230.0F})}.With(Spacing(14.0F)))
                   : current == TemplateStep::Browse ? std::move(catalog)
                   : current == TemplateStep::Preview ? std::move(preview) : std::move(binding);
  return ScrollView(Column{
    Row{Text(current == TemplateStep::Bind && !wide ? preset.title : "模板", TextRole::Title).With(Grow()),
        !wide && current != TemplateStep::Browse ?
            View(Button(current == TemplateStep::Bind ? "返回预览" : "返回模板")
                .OnClick([step, current] { step = current == TemplateStep::Bind ? TemplateStep::Preview : TemplateStep::Browse; })
                .With(Enabled(!busy))) : View(Text(""))}
        .With(CrossAlign(CrossAxisAlignment::Center)),
    std::move(body),
  }.With(Spacing(14.0F), Padding(wide ? 24.0F : 12.0F), CrossAlign(CrossAxisAlignment::Stretch)));
}

[[huxerui::composable]]
View CapturePage(WorkbenchContext context, State<Page> page, State<bool> capturing) {
  return WorkbenchCapture(context.data, capturing, [context, page](File file) {
    RunWorkbenchAction(context, [context, page, file = std::move(file)]() -> Task<void> {
      auto asset = co_await RunWorker([file] { return ReadWorkbenchMovie(file); });
      auto edited = context.document.Get();
      if (!edited) edited = EmptyWorkbenchProject();
      AppendWorkbenchMovie(*edited, std::move(asset));
      context.document = std::move(edited);
      page = Page::Edit;
    });
  });
}

[[huxerui::composable]]
View Workbench() {
  const auto& theme = UseTheme();
  const auto wide = UseViewportClass() == ViewportClass::Expanded;
  const auto directories = UseApplication().Directories();
  const auto page = UseState(Page::Home);
  const auto document = UseState(std::optional<WorkbenchDocument>{});
  const auto message = UseState(std::string{});
  const auto busy = UseState(false);
  const auto capturing = UseState(false);
  const WorkbenchContext context{UseTaskScope(), UseService<FilePicker>(),
                                directories.data_directory, directories.cache_directory, document, message, busy};
  const auto selected = page.Get();
  View content = Text("");
  switch (selected) {
    case Page::Home: content = Home(context, page, wide); break;
    case Page::Edit: content = WorkbenchEditor(context); break;
    case Page::Templates: content = Templates(context, page, wide); break;
    case Page::Capture:
      content = CapturePage(context, page, capturing);
      break;
  }
  View navigation = SegmentedButton({"工作台", "录制", "剪辑", "模板"}, static_cast<std::size_t>(selected))
      .OnChanged([page, message](std::size_t index) {
        message = std::string{};
        page = static_cast<Page>(index);
      }).With(Enabled(!busy.Get() && !capturing.Get()));
  View header = Row{Text("AVKit", TextRole::Title).With(Grow()),
                    wide ? std::move(navigation) : View(Text(""))}
      .With(Padding(EdgeInsets::Symmetric(wide ? 24.0F : 12.0F, 8.0F)),
            Background(theme.colors.surface), CrossAlign(CrossAxisAlignment::Center));
  std::vector<View> layout;
  layout.push_back(std::move(header));
  layout.push_back(std::move(content).With(Grow()).Key(std::to_string(static_cast<unsigned>(selected))));
  if (busy.Get() || !message->empty()) {
    layout.push_back(Text(busy.Get() ? "正在处理…" : message.Get())
        .With(Foreground(theme.colors.primary), Padding(EdgeInsets::Symmetric(14.0F, 8.0F))));
  }
  if (!wide) layout.push_back(Row{std::move(navigation)}
      .With(Padding(8.0F), MainAlign(MainAxisAlignment::Center), Background(theme.colors.surface)));
  return Column(std::move(layout)).With(CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.background));
}

} // namespace

View WorkbenchRoot() { return MaterialTheme(WorkbenchTheme(), Workbench()); }

View WorkbenchPanel(View view, const ThemeSpec& theme) {
  return std::move(view).With(Padding(12.0F), Background(theme.colors.surface), CornerRadius(14.0F));
}

void RunWorkbenchAction(const WorkbenchContext& context, std::function<Task<void>()> action) {
  if (context.busy.Get()) return;
  context.busy = true;
  context.message = std::string{};
  context.tasks.Launch([context, action = std::move(action)]() -> Task<void> {
    try { co_await action(); }
    catch (const std::exception& error) { context.message = std::string(error.what()); }
    context.busy = false;
  });
}

Task<void> SaveWorkbenchDocument(WorkbenchContext context) {
  auto next = context.document.Get();
  if (!next) co_return;
  next->file = co_await RunWorker([next, data = context.data] {
    const auto target = next->file ? *next->file : WorkbenchOutput(data, "Projects", next->title + ".avkit");
    next->project->Save(target);
    return target;
  });
  next->dirty = false;
  context.document = std::move(next);
}

Task<std::optional<File>> PickWorkbenchFile(
    WorkbenchContext context, FilePickerFilter filter, std::string directory, std::string fallback) {
  const auto picked = co_await context.picker->OpenFileAsync(std::move(filter));
  if (!picked) co_return std::nullopt;
  auto name = picked->Name();
  if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos)
    name = std::move(fallback);
  const auto destination = WorkbenchOutput(context.data, directory, name);
  if (!co_await picked->ImportToAsync(destination))
    throw std::runtime_error("无法复制所选文件");
  co_return destination;
}

FilePickerFilter WorkbenchMovieFilter() { return {"视频", {"mp4", "mov"}, {"video/mp4", "video/quicktime"}}; }
FilePickerFilter WorkbenchProjectFilter() { return {"AVKit 工程", {"avkit"}, {}}; }

std::string WorkbenchSeconds(double seconds, int precision) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(precision) << seconds;
  return output.str();
}

} // namespace avkit_preview
