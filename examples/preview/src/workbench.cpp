#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <huxerui/media_library.h>

#include "app_resources.h"

#include "capture_preview.h"
#include "workbench_media.h"

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;

Task<File> ImportWorkbenchReference(const File& data, const FileReference& picked,
                                   std::string directory, std::string fallback) {
  auto name = picked.Name();
  if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos)
    name = std::move(fallback);
  const auto destination = WorkbenchOutput(data, directory, name);
  if (!co_await picked.ImportToAsync(destination)) throw std::runtime_error("无法复制所选文件");
  co_return destination;
}

enum class Page : std::size_t { Home, Capture, Edit, Templates };

struct Preset {
  WorkbenchTemplate kind;
  const char* title;
  const char* description;
  const char* layout;
  ImageResource image;
};

const std::array kPresets{
    Preset{WorkbenchTemplate::StageCut, "舞台快剪", "两段视频依次拼接，每段最多六秒。", "16:9  ·  2 段素材  ·  12 秒", app::images::workbench_stage},
    Preset{WorkbenchTemplate::Portrait, "竖屏记录", "两段视频排进竖屏画布，每段最多五秒。", "9:16  ·  2 段素材", app::images::workbench_portrait},
    Preset{WorkbenchTemplate::PictureInPicture, "双镜头", "两段视频同时播放，保留主镜头音轨。", "画中画  ·  2 段素材", app::images::workbench_dissolve},
};

ThemeSpec WorkbenchTheme() {
  auto theme = MaterialDarkThemeSpec();
  theme.colors.background = Color::Rgb(10, 11, 13);
  theme.colors.surface = Color::Rgb(19, 21, 25);
  theme.colors.surface_container_low = Color::Rgb(22, 24, 28);
  theme.colors.surface_container = Color::Rgb(27, 30, 35);
  theme.colors.surface_container_high = Color::Rgb(39, 43, 51);
  theme.colors.surface_container_highest = Color::Rgb(48, 53, 62);
  theme.colors.on_surface = Color::Rgb(244, 245, 246);
  theme.colors.on_surface_variant = Color::Rgb(171, 177, 188);
  theme.colors.primary = Color::Rgb(207, 247, 118);
  theme.colors.on_primary = Color::Rgb(21, 28, 12);
  theme.colors.secondary_container = Color::Rgb(37, 46, 29);
  theme.colors.on_secondary_container = theme.colors.primary;
  theme.colors.outline = Color::Rgb(40, 43, 50);
  return theme;
}

View Cover(ImageResource image, float height) {
  return Image(image).Fit(ImageFit::Cover).With(Frame{.height = height}, CornerRadius(9.0F));
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

View Entry(const char* title, const char* detail, const ThemeSpec& theme, const std::function<void()>& open,
           bool busy, bool wide) {
  return View(Row{
    Column{
      Text(title).Style(TextStyle{.font = Font::System(18.0F).WithWeight(FontWeight::SemiBold),
                                  .foreground = theme.colors.on_surface}),
      Text(detail).With(Foreground(theme.colors.on_surface_variant)),
    }.With(Spacing(5.0F), Grow()),
    Image(app::images::icons_chevron_right).Tint(theme.colors.primary),
  }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center), Padding(wide ? 17.0F : 12.0F),
        Background(theme.colors.surface_container_low), CornerRadius(9.0F), Enabled(!busy),
        Semantics{.label = title})).OnClick(open);
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
  const auto destination = [&](const char* title, const char* summary, Page target) {
    return Entry(title, summary, theme, [page, target] { page = target; }, busy, wide).With(Grow());
  };
  View entries = wide ? View(Row{
      destination("录制", "相机、麦克风和变声", Page::Capture),
      destination("剪辑", "素材、多轨时间线和导出", Page::Edit),
      destination("模板", "替换镜头并生成工程", Page::Templates),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch))) :
    View(Column{
      destination("录制", "相机、麦克风和变声", Page::Capture),
      destination("剪辑", "素材、多轨时间线和导出", Page::Edit),
      destination("模板", "替换镜头并生成工程", Page::Templates),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)));
  const auto resume = [context, page, current, files] {
    if (current || files.empty()) page = Page::Edit;
    else OpenDocument(context, page, files.front());
  };
  const auto feature_height = wide ? 248.0F : 172.0F;
  View feature = Stack{
    Cover(kPresets.front().image, feature_height),
    Column{Row{Column{
      Text(current ? current->title : files.empty() ? "新建视频工程" : files.front().Stem())
          .Style(TextStyle{.font = Font::System(wide ? 26.0F : 19.0F).WithWeight(FontWeight::SemiBold),
                           .foreground = Color::White()}).LineLimit(1).Overflow(TextOverflow::Ellipsis),
      WorkbenchControl(Button(current || !files.empty() ? "继续剪辑" : "进入剪辑").OnClick(resume)
                           .With(Enabled(!busy)), theme, WorkbenchActionTone::Primary),
    }.With(Frame{.max_width = 263.0F}, Padding(12.0F), Spacing(8.0F),
        Background(Color::Rgb(10, 11, 13, 0.88F)), CornerRadius(7.0F))}
        .With(Padding(10.0F))}.With(MainAlign(MainAxisAlignment::End)),
  }.With(Frame{.height = feature_height}, Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch),
         CornerRadius(10.0F), ClipChildren{});
  std::vector<View> rows;
  for (const auto& file : files) {
    rows.push_back(WorkbenchControl(Button(file.Stem()).OnClick([context, page, file] { OpenDocument(context, page, file); })
                       .With(Enabled(!busy)), theme).Key(file.Path()));
  }
  if (rows.empty()) rows.push_back(Text("保存后的工程会出现在这里。").With(Foreground(theme.colors.on_surface_variant)));
  return ScrollView(Column{
    Text("音视频工作台").Style(TextStyle{.font = Font::System(wide ? 38.0F : 28.0F), .foreground = theme.colors.on_surface}),
    std::move(feature),
    std::move(entries),
    WorkbenchPanel(Column{
      Row{Text("最近项目", TextRole::Title).With(Grow()),
          WorkbenchControl(Button("打开工程").OnClick([context, page] { OpenDocument(context, page); })
              .With(Enabled(!busy && context.picker->CanOpenFiles())), theme)}
          .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
      Column(std::move(rows)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme),
  }.With(Spacing(16.0F), Padding(wide ? 24.0F : 12.0F), CrossAlign(CrossAxisAlignment::Stretch)));
}

[[huxerui::composable]]
View Templates(WorkbenchContext context, State<Page> page, State<bool> detail, bool wide) {
  const auto& theme = UseTheme();
  const auto which = UseState(std::size_t{});
  const auto inputs = UseState(std::array<std::optional<AssetReference>, 2>{});
  const auto& preset = kPresets[which.Get()];
  const bool busy = context.busy.Get();
  const auto card = [&](std::size_t index, float height) {
    const auto& item = kPresets[index];
    return View(Stack{
      Cover(item.image, height),
      Column{Column{
        Text(item.title).Style(TextStyle{.font = Font::System(18.0F).WithWeight(FontWeight::SemiBold),
                                        .foreground = Color::White()}),
        Text(item.layout).Style(TextStyle{.font = Font::System(11.0F), .foreground = Color::Rgb(223, 225, 229)}),
      }.With(Padding(11.0F), Spacing(5.0F),
          Background(LinearGradient{.start = {0.0F, -0.25F}, .end = {0.0F, 1.0F},
              .stops = {{0.0F, Color::Rgb(7, 9, 13, 0.0F)}, {1.0F, Color::Rgb(7, 9, 13, 0.94F)}}})),
      }.With(MainAlign(MainAxisAlignment::End), CrossAlign(CrossAxisAlignment::Stretch)),
    }.With(Frame{.height = height}, Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch),
        CornerRadius(9.0F), Border{wide && index == which.Get() ? theme.colors.primary : theme.colors.outline, 1.0F},
        ClipChildren{}, Enabled(!busy), Semantics{.role = SemanticRole::Button, .label = item.title}))
        .OnClick([which, detail, inputs, index] {
          if (which.Get() != index) inputs = std::array<std::optional<AssetReference>, 2>{};
          which = index;
          detail = true;
        });
  };
  View catalog = wide ? View(Column{card(0, 124.0F), card(1, 124.0F), card(2, 124.0F)}
      .With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch))) :
      View(Column{
        card(0, 185.0F),
        Row{card(1, 151.0F).With(Grow()), card(2, 151.0F).With(Grow())}.With(Spacing(8.0F)),
      }.With(Spacing(9.0F), CrossAlign(CrossAxisAlignment::Stretch)));
  if (!wide && !detail.Get())
    return ScrollView(std::move(catalog).With(Padding(12.0F)));
  const auto bound = inputs.Get();
  const auto selected = int(bool(bound[0])) + int(bool(bound[1]));
  const auto portrait = preset.kind == WorkbenchTemplate::Portrait;
  const auto height = wide ? 320.0F : portrait ? 236.0F : 182.0F;
  View artwork = portrait ? View(Row{Cover(preset.image, height).With(
      Frame{.width = height * 9.0F / 16.0F, .height = height})}.With(MainAlign(MainAxisAlignment::Center))) :
      Cover(preset.image, height);
  View preview = Column{
    std::move(artwork).With(Background(theme.colors.background), ClipChildren{}, CornerRadius(8.0F)),
    Text(preset.layout, TextRole::Label).With(Foreground(theme.colors.primary)),
    Text(preset.description).With(Foreground(theme.colors.on_surface_variant)),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));

  std::vector<View> slots;
  slots.push_back(Row{
    Text("素材", TextRole::Label).With(Grow()),
    Text(std::to_string(selected) + " / 2").Style(
        TextStyle{.font = Font::Monospace(11.0F),
                  .foreground = selected == 2 ? theme.colors.primary : theme.colors.on_surface_variant}),
  }.With(CrossAlign(CrossAxisAlignment::Center)));
  const bool has_media = HasWorkbenchMedia();
  for (std::size_t i = 0; has_media && i < bound.size(); ++i) {
    const auto& asset = bound[i];
    const auto name = i == 0 ? "主镜头" : "第二镜头";
    const auto action = [context, inputs, i, name] {
      SelectWorkbenchVideo(context, name, 1, [inputs, i](std::vector<File> files) -> Task<void> {
        auto asset = co_await RunWorker([file = files.front()] { return ReadWorkbenchMovie(file); });
        inputs.Update([&](auto& current) { current[i] = std::move(asset); });
      });
    };
    slots.push_back(View(Row{
      Column{
        Text(name, TextRole::Label),
        Text(asset ? asset->file.Name() : "待选").LineLimit(1).Overflow(TextOverflow::Ellipsis)
          .With(Foreground(theme.colors.on_surface_variant)),
      }.With(Grow(), Spacing(3.0F)),
      Row{
        Text(asset ? "替换" : "选择", TextRole::Label).With(Foreground(theme.colors.primary)),
        Image(app::images::icons_chevron_right).Tint(theme.colors.primary),
      }.With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Center)),
    }.With(Frame{.min_height = 56.0F}, Padding(10.0F), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center),
           Background(theme.colors.surface_container_low), CornerRadius(8.0F),
           Enabled(!busy && CanSelectWorkbenchVideo(context)), Semantics{.role = SemanticRole::Button, .label = name}))
        .OnClick(action).Key(std::to_string(i)));
  }
  if (has_media && context.browse_library.Get()) {
    slots.push_back(WorkbenchControl(Button("选择两个镜头").OnClick([context, inputs] {
      SelectWorkbenchVideo(context, "选择模板镜头", 2, [inputs](std::vector<File> files) -> Task<void> {
        auto assets = co_await RunWorker([files = std::move(files)] {
          std::vector<AssetReference> result;
          for (const auto& file : files) result.push_back(ReadWorkbenchMovie(file));
          return result;
        });
        inputs.Update([&](auto& current) {
          for (std::size_t index = 0; index < assets.size(); ++index) current[index] = std::move(assets[index]);
        });
      });
    }).With(Enabled(!busy)), theme));
  }
  if (!has_media) slots.push_back(Text("此构建无法导入视频素材。").With(Foreground(theme.colors.on_surface_variant)));
  View binding = Column(std::move(slots)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
  View apply = WorkbenchControl(Button("生成并进入剪辑").OnClick([context, inputs, page, kind = preset.kind] {
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
  }).With(Enabled(!busy && selected == 2)), theme, WorkbenchActionTone::Primary);

  if (wide) {
    return ScrollView(Row{
      std::move(catalog).With(Frame{.width = 210.0F}),
      Column{Text(preset.title, TextRole::Title), std::move(preview)}
          .With(Grow(), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)),
      Column{std::move(binding), std::move(apply)}
          .With(Frame{.width = 258.0F}, Spacing(14.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    }.With(Padding(16.0F), Spacing(16.0F)));
  }
  return Column{
    ScrollView(Column{Text(preset.title, TextRole::Title), std::move(preview), std::move(binding)}
        .With(Spacing(16.0F), Padding(12.0F), CrossAlign(CrossAxisAlignment::Stretch))).With(Grow()),
    Divider(),
    Column{std::move(apply)}.With(Padding(12.0F), Background(theme.colors.surface),
                                   CrossAlign(CrossAxisAlignment::Stretch)),
  }.With(Background(theme.colors.background), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View CapturePage(WorkbenchContext context, State<Page> page, State<bool> capturing) {
  const auto append = [context, page](File file) -> Task<void> {
    auto asset = co_await RunWorker([file] { return ReadWorkbenchMovie(file); });
    auto edited = context.document.Get();
    if (!edited) edited = EmptyWorkbenchProject();
    AppendWorkbenchMovie(*edited, std::move(asset));
    context.document = std::move(edited);
    page = Page::Edit;
  };
  std::function<void()> import;
  if (HasWorkbenchMedia() && context.picker->CanOpenFiles()) {
    import = [context, append] {
      RunWorkbenchAction(context, [context, append]() -> Task<void> {
        const auto picked = co_await PickWorkbenchFile(context, WorkbenchMovieFilter(), "Media", "movie.mp4");
        if (picked) co_await append(*picked);
      });
    };
  }
  return WorkbenchCapture(context.data, capturing, [context, append](File file) {
    RunWorkbenchAction(context, [append, file = std::move(file)] { return append(file); });
  }, std::move(import));
}

[[huxerui::composable]]
View Workbench() {
  const auto& theme = UseTheme();
  const auto wide = UseViewportClass() == ViewportClass::Expanded;
  const auto directories = UseApplication().Directories();
  const auto page = UseState(Page::Home);
  const auto template_detail = UseState(false);
  const auto document = UseState(std::optional<WorkbenchDocument>{});
  const auto message = UseState(std::string{});
  const auto busy = UseState(false);
  const auto exporting = UseState(false);
  const auto capturing = UseState(false);
  const auto video_request = UseState(std::shared_ptr<const WorkbenchVideoRequest>{});
  const auto browse_library = UseState(false);
  const auto library = media::UseMediaLibrary();
  const auto tasks = UseTaskScope();
  Lifecycle([tasks, library, browse_library] {
    tasks.Launch([library, browse_library]() -> Task<void> {
      auto result = co_await library.CapabilitiesAsync();
      if (result.Succeeded()) browse_library = result.Value().can_browse;
    });
  });
  const WorkbenchContext context{tasks, UseService<FilePicker>(), directories.data_directory, directories.cache_directory,
                                document, message, busy, exporting, video_request, browse_library};
  const auto selected = page.Get();
  const bool can_switch = !busy.Get() && !capturing.Get();
  const auto navigate = [page, message, template_detail](Page target) {
    message = std::string{};
    template_detail = false;
    page = target;
  };
  const auto back = [navigate] { navigate(Page::Home); };
  View content;
  switch (selected) {
    case Page::Home: content = Home(context, page, wide); break;
    case Page::Edit: content = WorkbenchEditor(context, back); break;
    case Page::Templates: content = Templates(context, page, template_detail, wide); break;
    case Page::Capture: content = CapturePage(context, page, capturing); break;
  }
  const auto nav_item = [&](const char* label, Page target) {
    return WorkbenchTab(label, theme, [navigate, target] { navigate(target); }, selected == target, can_switch);
  };
  View navigation = Row{nav_item("工作台", Page::Home), nav_item("录制", Page::Capture),
                        nav_item("剪辑", Page::Edit), nav_item("模板", Page::Templates)}
        .With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Center));
  View header = Row{
      !wide && selected != Page::Home ? WorkbenchBack(selected == Page::Templates && template_detail.Get() ?
          "返回模板" : "返回工作台", theme,
          [back, template_detail, selected] {
            if (selected == Page::Templates && template_detail.Get()) template_detail = false;
            else back();
          }, can_switch) : View{},
      Image(app::images::icons_brand).Tint(theme.colors.primary),
      Text("AVKit").Style(TextStyle{.font = Font::System(17.0F).WithWeight(FontWeight::Bold),
                                   .foreground = theme.colors.on_surface}),
      !wide && selected == Page::Capture ? View(Text("录制", TextRole::Label).With(Foreground(theme.colors.on_surface_variant))) :
          !wide && selected == Page::Templates ? View(Text("模板", TextRole::Label)) : View{},
      Spacer(),
      wide ? std::move(navigation) : View{},
    }.With(Padding(EdgeInsets::Symmetric(wide ? 16.0F : 9.0F, 2.0F)), Spacing(7.0F),
        Background(theme.colors.surface), CrossAlign(CrossAxisAlignment::Center));
  std::vector<View> layout;
  if (wide || selected != Page::Edit) {
    layout.push_back(std::move(header));
    layout.emplace_back(Divider());
  }
  layout.push_back(std::move(content).With(Grow()).Key(std::to_string(static_cast<unsigned>(selected))));
  if (selected != Page::Edit && (busy.Get() || !message->empty())) {
    layout.push_back(Text(busy.Get() ? "正在处理…" : message.Get())
        .With(Foreground(theme.colors.primary), Padding(EdgeInsets::Symmetric(14.0F, 7.0F))));
  }
  const auto request = video_request.Get();
  View workbench = Column(std::move(layout)).With(CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.background),
      Enabled(!request), Semantics{.hidden = bool(request)});
  View picker;
  if (request) {
    picker = media::MediaLibraryPicker({.filter = media::MediaFilter::Videos, .selection_limit = request->limit,
        .title = request->title, .on_confirm = [context, request](std::vector<FileReference> references) {
          context.videos = nullptr;
          if (references.empty()) return;
          RunWorkbenchAction(context, [context, request, references = std::move(references)]() -> Task<void> {
            std::vector<File> files;
            for (const auto& reference : references)
              files.push_back(co_await ImportWorkbenchReference(context.data, reference, "Media", "movie.mp4"));
            co_await request->confirmed(std::move(files));
          });
        }, .on_cancel = [video_request] { video_request = nullptr; }});
    picker = std::move(picker).With(Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch),
                                    Background(theme.colors.background));
  }
  return Stack{std::move(workbench), std::move(picker)};
}

} // namespace

View WorkbenchRoot() {
  auto tokens = WorkbenchTheme();
  auto definition = MaterialThemeDefinition(tokens);
  definition.Set(DividerStyle{.color = tokens.colors.outline, .thickness = 1.0F});
  return Theme{std::move(definition), Workbench()};
}

View WorkbenchPanel(View view, const ThemeSpec& theme) {
  return std::move(view).With(Padding(12.0F), Background(theme.colors.surface_container_low));
}

View WorkbenchBack(std::string label, const ThemeSpec& theme, std::function<void()> action, bool enabled) {
  return View(Row {
    Image(app::images::icons_back).With(InheritForeground{}, Frame{.width = 18.0F, .height = 18.0F}),
  }.With(Frame{.width = 44.0F, .height = 44.0F}, MainAlign(MainAxisAlignment::Center),
        CrossAlign(CrossAxisAlignment::Center),
        Foreground(enabled ? theme.colors.on_surface : theme.colors.on_surface_variant),
        Indication{.focus = IndicationLayer{.foreground = theme.colors.primary},
                   .hover = IndicationLayer{.foreground = theme.colors.on_surface},
                   .press = IndicationLayer{.foreground = theme.colors.primary}},
        Enabled(enabled), Semantics{.role = SemanticRole::Button, .label = std::move(label)})).OnClick(std::move(action));
}

View WorkbenchControl(View view, const ThemeSpec& theme, WorkbenchActionTone tone, bool compact) {
  ButtonStyle style;
  const auto primary = tone == WorkbenchActionTone::Primary;
  style.background = primary ? theme.colors.primary :
      tone == WorkbenchActionTone::Surface ? theme.colors.surface_container : Color::Rgb(0, 0, 0, 0.0F);
  style.disabled_background = primary ? theme.colors.surface_container : Color::Rgb(0, 0, 0, 0.0F);
  style.label_style.foreground = primary ? theme.colors.on_primary : theme.colors.on_surface;
  style.label_style.font = Font::System(compact ? 12.0F : 14.0F).WithWeight(FontWeight::SemiBold);
  style.disabled_label = Color::Rgb(116, 124, 141);
  style.minimum_width = 0.0F;
  style.minimum_height = compact ? 36.0F : 44.0F;
  style.padding = EdgeInsets::Symmetric(compact ? 10.0F : 13.0F, 6.0F);
  style.corner_radii = CornerRadii{7.0F};
  ThemeDefinition overrides;
  overrides.Set(style);
  return Column{Theme{std::move(overrides), std::move(view)}}.With(CrossAlign(CrossAxisAlignment::Stretch));
}

View WorkbenchTab(std::string label, const ThemeSpec& theme, std::function<void()> action, bool active, bool enabled) {
  auto underline = Canvas([active, primary = theme.colors.primary](PaintContext& paint, Size) {
    if (active) paint.DrawRect({0, 0, 14.0F, 2.0F}, primary, CornerRadii{1.0F});
  });
  return View(Column{
    Text(std::move(label)).Style(TextStyle{.font = Font::System(13.0F).WithWeight(FontWeight::Medium),
        .foreground = active ? theme.colors.on_surface : theme.colors.on_surface_variant}),
    std::move(underline).With(Frame{.width = 14.0F, .height = 2.0F}),
  }.With(Frame{.height = 40.0F, .min_width = 55.0F}, Spacing(4.0F), MainAlign(MainAxisAlignment::Center),
        CrossAlign(CrossAxisAlignment::Center), Enabled(enabled))).OnClick(std::move(action));
}

View WorkbenchCompactAction(View content, std::string label, const ThemeSpec& theme,
                            std::function<void()> action, bool enabled) {
  return View(Column{std::move(content)}.With(Frame{.height = 36.0F}, Padding(EdgeInsets::Symmetric(10.0F, 6.0F)),
      MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center), CornerRadius(7.0F),
      Foreground(enabled ? theme.colors.on_surface : Color::Rgb(116, 124, 141)),
      Enabled(enabled), Semantics{.role = SemanticRole::Button, .label = std::move(label)})).OnClick(std::move(action));
}

View WorkbenchTool(ImageResource icon, std::string label, const ThemeSpec& theme,
                   std::function<void()> action, bool enabled, bool show_label, std::optional<bool> selected) {
  const auto ink = !enabled ? Color::Rgb(94, 100, 111) : selected.value_or(false) ? theme.colors.primary : theme.colors.on_surface;
  View title = show_label ? View(Text(label).Style(TextStyle{.font = Font::System(11.0F), .foreground = ink}).LineLimit(1)) : View{};
  return View(Column{Image(icon).Tint(ink).With(Frame{.width = 20.0F, .height = 20.0F}), std::move(title)}
      .With(Frame{.min_width = 42.0F, .min_height = show_label ? 53.0F : 42.0F}, Spacing(5.0F),
            MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center),
            Semantics{.role = SemanticRole::Button, .label = std::move(label), .selected = selected}, Enabled(enabled)))
      .OnClick(std::move(action));
}

View WorkbenchShutter(const ThemeSpec& theme, bool recording, bool enabled, std::string label,
                      std::function<void()> action) {
  const auto ring = enabled ? theme.colors.on_surface : theme.colors.outline;
  const auto fill = enabled ? Color::Rgb(253, 77, 107) : theme.colors.surface_container_high;
  return View(Stack{
    Image(app::images::icons_shutter_ring).Tint(ring),
    Image(recording ? app::images::icons_shutter_stop : app::images::icons_shutter_record).Tint(fill),
  }.With(Frame{.width = 66.0F, .height = 66.0F}, Enabled(enabled),
      Semantics{.role = SemanticRole::Button, .label = std::move(label)})).OnClick(std::move(action));
}

View WorkbenchRecordLayout(const ThemeSpec& theme, View picture, View overlay, View modes, View controls, View status) {
  return Column{
    Stack{std::move(picture), std::move(overlay)}.With(Grow(), ClipChildren{}, Background(theme.colors.background),
        Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch)),
    std::move(modes),
    std::move(controls),
    std::move(status),
  }.With(Padding(EdgeInsets{.bottom = 7.0F}), Background(theme.colors.background),
        CrossAlign(CrossAxisAlignment::Stretch));
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
  co_return co_await ImportWorkbenchReference(context.data, *picked, std::move(directory), std::move(fallback));
}

FilePickerFilter WorkbenchMovieFilter() { return {"视频", {"mp4", "mov", "m4v"}, {"video/mp4", "video/quicktime", "video/x-m4v"}}; }
FilePickerFilter WorkbenchAudioFilter() {
  return {"音乐", {"mp3", "m4a", "wav"}, {"audio/mpeg", "audio/mp4", "audio/wav", "audio/x-wav"}};
}
FilePickerFilter WorkbenchProjectFilter() { return {"AVKit 工程", {"avkit"}, {}}; }

bool CanSelectWorkbenchVideo(const WorkbenchContext& context) {
  return context.browse_library.Get() || context.picker->CanOpenFiles();
}

void SelectWorkbenchVideo(const WorkbenchContext& context, std::string title, std::uint32_t limit,
                          WorkbenchVideoSelection confirmed) {
  if (context.browse_library.Get()) {
    context.message = std::string{};
    context.videos = std::make_shared<const WorkbenchVideoRequest>(WorkbenchVideoRequest{std::move(title), limit, std::move(confirmed)});
    return;
  }
  RunWorkbenchAction(context, [context, confirmed = std::move(confirmed)]() -> Task<void> {
    const auto selected = co_await PickWorkbenchFile(context, WorkbenchMovieFilter(), "Media", "movie.mp4");
    if (selected) co_await confirmed(std::vector<File>{*selected});
  });
}

std::string WorkbenchSeconds(double seconds, int precision) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(precision) << seconds;
  return output.str();
}

std::string WorkbenchTimecode(double seconds) {
  const auto frames = std::llround(std::max(0.0, seconds) * 24);
  std::ostringstream output;
  output << std::setfill('0') << std::setw(2) << frames / 86400 << ':' << std::setw(2) << frames / 1440 % 60
         << ':' << std::setw(2) << frames / 24 % 60 << ':' << std::setw(2) << frames % 24;
  return output.str();
}

std::string WorkbenchTimelineTime(double seconds, bool fraction) {
  const auto time = std::llround(std::max(0.0, seconds) * 10.0);
  std::ostringstream output;
  output << std::setfill('0') << std::setw(2) << time / 600 << ':' << std::setw(2) << time / 10 % 60;
  if (fraction) output << '.' << time % 10;
  return output.str();
}

} // namespace avkit_preview
