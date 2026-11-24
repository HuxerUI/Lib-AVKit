#include "app_resources.h"
#include "workbench_media.h"
#include "workbench_overlay.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <numeric>
#include <utility>
#include <variant>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;

double Seconds(media::MediaTimestamp time) { return double(time.ticks) / time.timescale; }
bool Before(media::MediaTimestamp a, media::MediaTimestamp b) { return a.Compare(b) == -1; }

constexpr float kRulerHeight = 28.0F;
constexpr float kLaneHeader = 64.0F;
constexpr float kTimelineGutter = 14.0F;
constexpr double kMaxZoom = 3.0;

float LaneHeight(media::TrackKind kind) {
  if (kind == media::TrackKind::Video) return 48.0F;
  if (kind == media::TrackKind::Audio) return 34.0F;
  return 28.0F;
}

enum class EditorTools { Main, Clip, Media, Audio, Captions, Stickers, PictureInPicture, Overlay, Canvas };
enum class OverlayPage { Font, Style, Background, Position };
enum class EditorPanel { None, Overlay, Picture, Audio, Stickers };

struct TimelineLane {
  const TrackInfo* track{};
  media::TrackKind kind{media::TrackKind::Video};
  std::string label;
  std::vector<const CanvasOverlay*> overlays;
};

bool VisibleClip(const ClipInfo& clip) { return !clip.audio_of; }

const char* OverlayLabel(CanvasOverlayKind kind) {
  switch (kind) {
    case CanvasOverlayKind::Text: return "文字";
    case CanvasOverlayKind::Subtitle: return "字幕";
    case CanvasOverlayKind::Sticker: return "贴纸";
    case CanvasOverlayKind::PagText: return "PAG 文字";
  }
  return "叠加";
}

std::vector<TimelineLane> TimelineLanes(const ProjectSnapshot* project) {
  std::vector<TimelineLane> rows{{.label = "主轨"}};
  unsigned video_count = 0, audio_count = 0;
  if (project) {
    for (const auto& track : project->tracks) {
      const bool video = track.kind == media::TrackKind::Video;
      if (video && !rows.front().track) {
        rows.front().track = &track;
      } else if (std::ranges::any_of(track.clips, VisibleClip)) {
        auto& count = video ? video_count : audio_count;
        const bool inset = video && std::ranges::all_of(track.clips, [](const auto& clip) {
          return clip.destination != Rect{0, 0, 1, 1};
        });
        auto label = std::string(inset ? "画中画" : video ? "叠加" : "音频");
        if (++count > 1) label += " " + std::to_string(count);
        rows.push_back({&track, track.kind, std::move(label)});
      }
    }
  }
  if (project) {
    for (const auto kind : {CanvasOverlayKind::Text, CanvasOverlayKind::Subtitle, CanvasOverlayKind::PagText,
                            CanvasOverlayKind::Sticker}) {
      const auto first_lane = rows.size();
      std::vector<const CanvasOverlay*> overlays;
      for (const auto& overlay : project->overlays) if (overlay.kind == kind) overlays.push_back(&overlay);
      std::ranges::stable_sort(overlays, [](const auto* a, const auto* b) {
        return a->range.start.Compare(b->range.start) == -1;
      });
      for (const auto* overlay : overlays) {
        auto lane = std::ranges::find_if(rows.begin() + first_lane, rows.end(), [&](const TimelineLane& row) {
          return row.overlays.back()->range.end.Compare(overlay->range.start) != 1;
        });
        if (lane == rows.end()) rows.push_back({{}, media::TrackKind::Subtitle, OverlayLabel(kind), {overlay}});
        else lane->overlays.push_back(overlay);
      }
    }
  }
  return rows;
}


struct TimelineWindow {
  double begin, span;
  bool centered{};

  float X(double time, float width) const {
    return Origin() + float((time - begin) / span) * Width(width);
  }

  double Time(float x, float width) const {
    return begin + span * (x - Origin()) / Width(width);
  }
  static float Width(float width) { return std::max(1.0F, width - kLaneHeader - 2 * kTimelineGutter); }

private:
  float Origin() const { return kTimelineGutter + (centered ? kLaneHeader * 0.5F : kLaneHeader); }
};

StripViewport SamplingWindow(TimelineWindow display, double base_span, double duration, double zoom) {
  const auto level = unsigned(std::floor(zoom));
  const auto chunk = base_span / (1U << level);
  const auto first = std::floor(std::max(0.0, display.begin) / chunk) * chunk;
  const auto last = std::min(duration, first + chunk * 3.0);
  return {{{std::llround(first * kStripClock), kStripClock}, {std::llround(last * kStripClock), kStripClock}},
          {1, 1U << level}, {1, 25U << std::min(level, 2U)}};
}

enum class TrimEdge { Start, End, Body };
enum class TrimPhase { Move, Commit, Cancel };

using CanvasTarget = std::variant<ClipId, InstanceId>;

Rect ArtworkBounds(Rect frame, Rect artwork) {
  return {frame.x + artwork.x * frame.width, frame.y + artwork.y * frame.height,
          frame.width * artwork.width, frame.height * artwork.height};
}

Rect FrameFromArtwork(Rect bounds, Rect artwork) {
  const auto width = bounds.width / artwork.width, height = bounds.height / artwork.height;
  return {bounds.x - artwork.x * width, bounds.y - artwork.y * height, width, height};
}

struct CanvasItem {
  CanvasTarget target;
  Rect bounds;
  Rect artwork{0, 0, 1, 1};
  float degrees{};
  bool text{};
  Rect Visible() const { return ArtworkBounds(bounds, artwork); }
  bool operator==(const CanvasItem&) const = default;
};

Rect CanvasArea(Size available, PixelSize canvas) {
  const auto scale = std::min(available.width / canvas.width, available.height / canvas.height);
  const auto width = canvas.width * scale, height = canvas.height * scale;
  return {(available.width - width) / 2, (available.height - height) / 2, width, height};
}

Rect PlaceCanvasItem(Rect bounds, float x, float y, float scale = 1.0F) {
  const auto largest = 1.0F / std::max(bounds.width, bounds.height);
  scale = std::clamp(scale, std::min(largest, 0.025F * largest), largest);
  const auto width = std::min(1.0F, bounds.width * scale), height = std::min(1.0F, bounds.height * scale);
  return {std::clamp(x, 0.0F, 1.0F - width), std::clamp(y, 0.0F, 1.0F - height), width, height};
}

enum class CanvasCorner { Delete, Edit, Transform, Duplicate };
constexpr float kCanvasButtonSize = 18.0F;
constexpr float kCanvasButtonHitRadius = 16.0F;

std::array<Point, 4> CanvasCorners(const CanvasItem& item, Rect area, PixelSize canvas) {
  auto rect = item.Visible();
  if (item.text) {
    constexpr float padding = 6.0F;
    const auto horizontal = padding / area.width, vertical = padding / area.height;
    rect = {rect.x - horizontal, rect.y - vertical, rect.width + 2 * horizontal, rect.height + 2 * vertical};
  }
  const auto rotate = CanvasRotation(item.bounds, canvas, item.degrees);
  std::array points{Point{rect.x, rect.y}, Point{rect.x + rect.width, rect.y},
      Point{rect.x + rect.width, rect.y + rect.height}, Point{rect.x, rect.y + rect.height}};
  for (auto& point : points) {
    point = rotate.Apply(point);
    point = {area.x + point.x * area.width, area.y + point.y * area.height};
  }
  return points;
}

struct CanvasPointer {
  class Extension;
  PixelSize canvas;
  std::vector<CanvasItem> items;
  std::optional<CanvasItem> active;
  State<std::optional<CanvasCorner>> pressed;
  std::function<void(std::optional<CanvasTarget>)> select;
  std::function<void(CanvasItem, TrimPhase)> update;
  std::function<void(CanvasCorner)> action;
};

class CanvasPointer::Extension final : public NodeExtension {
public:
  Extension(ViewNode&, const CanvasPointer& value) : config_(value) {}
  void Update(ViewNode&, const CanvasPointer& value) { config_ = value; }

  bool HitTest(ViewNode& node, Point point) const override {
    return Rect{0, 0, node.Bounds().width, node.Bounds().height}.Contains(point);
  }

  PointerResult OnPointer(ViewNode& node, const PointerEvent& event) override {
    const auto area = CanvasArea({node.Bounds().width, node.Bounds().height}, config_.canvas);
    if (area.IsEmpty()) return PointerResult::Ignored;
    if (event.type == PointerEventType::Down) return Begin(event, area);
    if (!first_ || (first_ != event.pointer_id && second_ != event.pointer_id)) return PointerResult::Ignored;
    if (event.type == PointerEventType::Cancel) {
      if (changed_) update_(*current_, TrimPhase::Cancel);
      Clear();
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Move) {
      (first_ == event.pointer_id ? first_point_ : second_point_) = event.position;
      if (button_ && *button_ != CanvasCorner::Transform) {
        const bool inside = std::hypot(event.position.x - origin_.x, event.position.y - origin_.y) <= kCanvasButtonHitRadius;
        config_.pressed = inside ? button_ : std::nullopt;
      }
      Move(area);
      return PointerResult::Handled;
    }
    if (event.type != PointerEventType::Up) return PointerResult::Handled;
    if (second_) {
      if (first_ == event.pointer_id) {
        first_ = second_;
        first_point_ = second_point_;
      }
      second_.reset();
      base_ = current_;
      origin_ = first_point_;
      move_body_ = true;
      return PointerResult::Handled;
    }
    if (changed_) {
      update_(*current_, TrimPhase::Commit);
    } else if (button_ && *button_ != CanvasCorner::Transform &&
               std::hypot(event.position.x - origin_.x, event.position.y - origin_.y) <= kCanvasButtonHitRadius) {
      config_.action(*button_);
    } else if (background_ && !multi_ && std::hypot(event.position.x - origin_.x, event.position.y - origin_.y) < 5.0F) {
      config_.select(tap_target_);
    }
    Clear();
    return PointerResult::Handled;
  }

private:
  static Point Midpoint(Point first, Point second) { return {(first.x + second.x) / 2, (first.y + second.y) / 2}; }

  static Point Center(const CanvasItem& item, Rect area) {
    return {area.x + (item.bounds.x + item.bounds.width / 2) * area.width,
            area.y + (item.bounds.y + item.bounds.height / 2) * area.height};
  }

  PointerResult Begin(const PointerEvent& event, Rect area) {
    if (event.changed_button == PointerButton::Secondary) return PointerResult::Ignored;
    if (first_) {
      if (second_ || !current_ || event.device_kind != PointerDeviceKind::Touch) return PointerResult::Ignored;
      button_.reset();
      config_.pressed = std::nullopt;
      second_ = event.pointer_id;
      second_point_ = event.position;
      base_ = current_;
      origin_ = Midpoint(first_point_, second_point_);
      span_ = {second_point_.x - first_point_.x, second_point_.y - first_point_.y};
      multi_ = true;
      return PointerResult::Capture;
    }
    const Point normalized{(event.position.x - area.x) / area.width, (event.position.y - area.y) / area.height};
    const auto hits_artwork = [&](const CanvasItem& item) {
      const auto position = CanvasRotation(item.bounds, config_.canvas, item.degrees).Inverse(normalized);
      return position && item.Visible().Contains(*position);
    };
    if (config_.active) {
      const auto corners = CanvasCorners(*config_.active, area, config_.canvas);
      auto near = hits_artwork(*config_.active) ? kCanvasButtonSize / 2 : kCanvasButtonHitRadius;
      for (std::size_t index = 0; index < corners.size(); ++index) {
        const auto distance = std::hypot(corners[index].x - event.position.x, corners[index].y - event.position.y);
        if (distance < near) {
          near = distance;
          button_ = static_cast<CanvasCorner>(index);
        }
      }
      if (button_) base_ = config_.active;
    }
    if (!base_) {
      const auto hit = std::ranges::find_if(config_.items.rbegin(), config_.items.rend(), hits_artwork);
      if (hit != config_.items.rend() && config_.active && hit->target != config_.active->target &&
          event.device_kind == PointerDeviceKind::Touch) {
        base_ = config_.active;
        tap_target_ = hit->target;
        background_ = true;
      } else if (hit != config_.items.rend()) {
        base_ = *hit;
        move_body_ = true;
        config_.select(hit->target);
      } else if (config_.active) {
        base_ = config_.active;
        background_ = true;
      } else {
        return PointerResult::Handled;
      }
    }
    current_ = base_;
    update_ = config_.update;
    first_ = event.pointer_id;
    origin_ = first_point_ = event.position;
    config_.pressed = button_;
    return PointerResult::Capture;
  }

  void Move(Rect area) {
    if (!move_body_ && !second_ && button_ != CanvasCorner::Transform) return;
    Point translation{};
    Point before{}, after{};
    if (second_) {
      const auto center = Midpoint(first_point_, second_point_);
      translation = {center.x - origin_.x, center.y - origin_.y};
      before = span_;
      after = {second_point_.x - first_point_.x, second_point_.y - first_point_.y};
    } else if (button_ == CanvasCorner::Transform) {
      const auto center = Center(*base_, area);
      before = {origin_.x - center.x, origin_.y - center.y};
      after = {first_point_.x - center.x, first_point_.y - center.y};
    } else {
      translation = {first_point_.x - origin_.x, first_point_.y - origin_.y};
    }
    const auto distance = std::hypot(before.x, before.y);
    const bool transform = distance >= 4.0F;
    const auto scale = transform ? std::hypot(after.x, after.y) / distance : 1.0F;
    const auto angle = transform ? std::remainder(
        (std::atan2(after.y, after.x) - std::atan2(before.y, before.x)) * 57.2957795131F, 360.0F) : 0.0F;
    if (!changed_ && std::hypot(translation.x, translation.y) < 2.0F && std::abs(scale - 1.0F) < 0.01F && std::abs(angle) < 1.0F)
      return;
    auto next = *base_;
    const auto rect = next.Visible();
    const auto maximum = 1.0F / std::max(rect.width, rect.height);
    const auto bounded = std::clamp(scale, 0.025F * maximum, maximum);
    const auto placed = PlaceCanvasItem(rect, rect.x + (1 - bounded) * rect.width / 2 + translation.x / area.width,
        rect.y + (1 - bounded) * rect.height / 2 + translation.y / area.height, bounded);
    next.bounds = FrameFromArtwork(placed, next.artwork);
    next.degrees = std::remainder(next.degrees + angle, 360.0F);
    if (current_ == next) return;
    current_ = next;
    changed_ = true;
    update_(next, TrimPhase::Move);
  }

  void Clear() {
    first_.reset();
    second_.reset();
    base_.reset();
    current_.reset();
    button_.reset();
    config_.pressed = std::nullopt;
    tap_target_.reset();
    update_ = {};
    changed_ = background_ = move_body_ = multi_ = false;
  }

  CanvasPointer config_;
  std::optional<std::int64_t> first_, second_;
  Point origin_, first_point_, second_point_, span_;
  std::optional<CanvasItem> base_, current_;
  std::optional<CanvasCorner> button_;
  std::optional<CanvasTarget> tap_target_;
  std::function<void(CanvasItem, TrimPhase)> update_;
  bool changed_{}, background_{}, move_body_{}, multi_{};
};

void PaintCanvasSelection(PaintContext& paint, Rect area, const CanvasItem& item, PixelSize canvas,
                          bool show_controls, std::optional<CanvasCorner> pressed, const VectorAsset& plate_icon,
                          const VectorAsset& pressed_plate_icon,
                          const std::array<VectorAsset, 4>& action_icons) {
  if (area.IsEmpty()) return;
  const auto corners = CanvasCorners(item, area, canvas);
  auto outline = Path{}.MoveTo(corners[0]).LineTo(corners[1]).LineTo(corners[2]).LineTo(corners[3]).Close();
  paint.StrokePath(outline, Color::White(), StrokeStyle{.width = 1.0F});
  if (!show_controls) return;
  for (std::size_t index = 0; index < corners.size(); ++index) {
    const auto corner = static_cast<CanvasCorner>(index);
    const auto point = corners[index];
    const bool down = pressed == corner;
    const Rect plate{point.x - kCanvasButtonSize / 2, point.y - kCanvasButtonSize / 2,
                     kCanvasButtonSize, kCanvasButtonSize};
    const auto ink = down ? Color::Rgb(19, 21, 24) : Color::White();
    paint.DrawImage(down ? pressed_plate_icon : plate_icon, plate);
    paint.DrawImage(action_icons[index], plate, ink);
  }
}

using GeometryInput = std::array<TextEditingValue, 3>;

GeometryInput GeometryValues(Rect bounds, PixelSize canvas) {
  return {TextEditingValue::FromText(std::to_string(std::lround(bounds.x * canvas.width))),
          TextEditingValue::FromText(std::to_string(std::lround(bounds.y * canvas.height))),
          TextEditingValue::FromText(std::to_string(std::lround(bounds.width * canvas.width)))};
}

std::optional<Rect> ReadGeometry(const GeometryInput& fields, Rect bounds, PixelSize canvas) {
  std::array<int, 3> pixels{};
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto& text = fields[index].text;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), pixels[index]);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return {};
  }
  if (pixels[2] <= 0) return {};
  const auto x = pixels[0] == std::lround(bounds.x * canvas.width) ? bounds.x : float(pixels[0]) / canvas.width;
  const auto y = pixels[1] == std::lround(bounds.y * canvas.height) ? bounds.y : float(pixels[1]) / canvas.height;
  const auto scale = pixels[2] == std::lround(bounds.width * canvas.width) ? 1.0F :
      float(pixels[2]) / (bounds.width * canvas.width);
  if (x == bounds.x && y == bounds.y && scale == 1.0F) return bounds;
  const auto width = bounds.width * scale, height = bounds.height * scale;
  if (x < 0 || y < 0 || x + width > 1.00001F || y + height > 1.00001F) return {};
  return Rect{x, y, width, height};
}

View EditorField(TextField field, const ThemeSpec& theme, bool caption = false) {
  auto style = TextFieldStyle::Default();
  style.filled = {.background = theme.colors.surface_container_high, .border = Color::Transparent(),
                  .hovered_border = Color::Transparent(), .focused_border = Color::Transparent(),
                  .corner_radii = CornerRadii{6.0F}, .minimum_height = caption ? 40.0F : 32.0F};
  style.border_width = 0;
  style.focused_border_width = 0;
  style.padding = EdgeInsets::Symmetric(8.0F, caption ? 5.0F : 4.0F);
  style.show_label = false;
  style.text_style = TextStyle{.font = caption ? Font::System(14.0F) : Font::Monospace(12.0F),
                               .foreground = theme.colors.on_surface};
  style.placeholder_style = {style.text_style.font, theme.colors.on_surface_variant};
  style.caret = theme.colors.primary;
  style.composition = theme.colors.primary;
  style.selection = theme.colors.primary;
  style.selection.alpha = 0.24F;
  ThemeDefinition overrides;
  overrides.Set(style);
  return Column{Theme{std::move(overrides), std::move(field).Variant(TextFieldVariant::Filled)
      .Align(caption ? TextAlign::Leading : TextAlign::Center).VerticalAlign(TextVerticalAlign::Center)}}
      .With(CrossAlign(CrossAxisAlignment::Stretch));
}

View GeometryFields(State<GeometryInput> fields, Rect bounds, PixelSize canvas, const ThemeSpec& theme) {
  const auto values = fields.Get();
  std::vector<View> inputs;
  constexpr std::array labels{"X", "Y", "宽度"};
  for (std::size_t index = 0; index < labels.size(); ++index) {
    auto field = TextField(values[index]).Label(labels[index]).MaxLength(7)
        .InputConfiguration({.type = TextInputType::Number})
        .OnChanged([fields, index](TextEditingValue value) {
          fields.Update([&](GeometryInput& next) { next[index] = std::move(value); });
        });
    inputs.push_back(Column{
      Text(labels[index], TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
      EditorField(std::move(field), theme),
    }.With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Stretch), Grow()).Key(labels[index]));
  }
  const auto displayed = ReadGeometry(values, bounds, canvas);
  return Column{
    Row(std::move(inputs)).With(Spacing(6.0F)),
    Text(displayed ? "像素 · 高度 " + std::to_string(std::lround(displayed->height * canvas.height)) + " · 等比缩放"
                   : "请输入画布内的位置和宽度", TextRole::Label),
  }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

View EditorRow(const ThemeSpec& theme, View controls, std::function<void()> done, bool valid = true) {
  return Row{
    std::move(controls).With(Grow()),
    WorkbenchTool(app::images::icons_confirm, "完成编辑", theme, std::move(done), valid, false)
        .With(Frame{.width = 44.0F, .height = 44.0F}),
  }.With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center));
}

View EditorDock(const ThemeSpec& theme, View controls, View details = {}, std::optional<float> detail_height = {}) {
  if (details || detail_height) {
    details = ScrollView(std::move(details)).With(Frame{.height = detail_height, .max_height = 150.0F});
  }
  return Column{
    std::move(controls),
    std::move(details),
  }.With(Padding(EdgeInsets::Symmetric(12.0F, 4.0F)), Spacing(4.0F),
        Background(theme.colors.surface_container_low), CrossAlign(CrossAxisAlignment::Stretch));
}

View EditorSlider(const ThemeSpec& theme, const char* label, std::string display, float value, float low, float high,
                  std::function<void(float)> changed) {
  auto style = SliderStyle::Default();
  style.height = 32.0F;
  style.track_height = 2.0F;
  style.inactive_track = theme.colors.surface_container_highest;
  style.active_track = theme.colors.primary;
  style.thumb = theme.colors.on_surface;
  style.thumb_width = style.thumb_height = 10.0F;
  style.hovered_thumb_width = style.hovered_thumb_height = 12.0F;
  style.pressed_thumb_width = style.pressed_thumb_height = 14.0F;
  style.thumb_track_gap = 0.0F;
  style.stop_indicator_size = style.tick_size = 0.0F;
  ThemeDefinition overrides;
  overrides.Set(style);
  return Row{
    Text(label, TextRole::Label).With(Frame{.width = 54.0F}, Foreground(theme.colors.on_surface_variant)),
    Column{Theme{std::move(overrides), Slider(value).Range(low, high).Step(0.01F).OnChanged(std::move(changed))
        .With(Semantics{.label = label})}}.With(CrossAlign(CrossAxisAlignment::Stretch), Grow()),
    Text(std::move(display)).Style(TextStyle{.font = Font::Monospace(12.0F),
        .foreground = theme.colors.on_surface}).Align(TextAlign::Trailing).With(Frame{.width = 48.0F}),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
}

struct TrimPreview {
  ClipId clip;
  std::optional<ClipId> linked;
  TimeRange range;
  std::optional<InstanceId> overlay;
  std::optional<media::MediaTimestamp> ripple_from;
  media::MediaTimestamp ripple;
  double thumbnail_shift{};
  bool operator==(const TrimPreview&) const = default;
};

struct TrimLimits {
  double minimum, maximum;

  void Intersect(TrimLimits other) {
    minimum = std::max(minimum, other.minimum);
    maximum = std::min(maximum, other.maximum);
  }
};

TrimLimits LimitTrim(const ProjectSnapshot& project, const ClipInfo& clip, TrimEdge edge, double minimum_length, bool ripple) {
  const auto source = clip.placement.source;
  const auto interval = clip.placement.timeline;
  const auto& asset = *std::ranges::find(project.assets, source.asset, &AssetInfo::id);
  const auto& available = std::ranges::find(asset.source.tracks, source.track, &AssetTrack::id)->presentation;
  const auto length = Seconds(interval.end) - Seconds(interval.start);
  const auto remaining = edge == TrimEdge::Start
      ? source.reverse ? Seconds(available.end) - Seconds(source.range.end)
                       : Seconds(source.range.start) - Seconds(available.start)
      : source.reverse ? Seconds(source.range.start) - Seconds(available.start)
                       : Seconds(available.end) - Seconds(source.range.end);
  if (ripple) return edge == TrimEdge::Start ? TrimLimits{-remaining, std::max(0.0, length - minimum_length)}
                                           : TrimLimits{std::min(0.0, minimum_length - length), remaining};
  for (const auto& lane : project.tracks) {
    const auto at = std::ranges::find(lane.clips, clip.id, &ClipInfo::id);
    if (at == lane.clips.end()) continue;
    if (edge == TrimEdge::Start) {
      const auto previous = at == lane.clips.begin() ? 0.0 : Seconds((at - 1)->placement.timeline.end);
      return {std::min(0.0, std::max(-remaining, previous - Seconds(interval.start))), std::max(0.0, length - minimum_length)};
    }
    const auto next = at + 1;
    const auto gap = next == lane.clips.end() ? remaining : Seconds(next->placement.timeline.start) - Seconds(interval.end);
    return {std::min(0.0, minimum_length - length), std::max(0.0, std::min(remaining, gap))};
  }
  return {};
}

ClipPlacement MoveClipEdge(ClipPlacement placement, TrimEdge edge, media::MediaTimestamp delta, bool ripple) {
  const bool start = edge == TrimEdge::Start;
  auto& project_edge = start && !ripple ? placement.timeline.start : placement.timeline.end;
  const auto project_delta = start && ripple ? media::MediaTimestamp{-delta.ticks, delta.timescale} : delta;
  project_edge = AddWorkbenchTime(project_edge, project_delta);
  auto& source_edge = start != placement.source.reverse ? placement.source.range.start : placement.source.range.end;
  if (placement.source.reverse) delta.ticks = -delta.ticks;
  source_edge = AddWorkbenchTime(source_edge, delta);
  return placement;
}

struct TrimHandles {
  TimeRange range;
  Rect lane;
  std::function<void(TrimEdge, double, TrimPhase)> update;
  bool movable{};
};

constexpr float kTrimHandleWidth = 12.0F;

std::optional<TrimEdge> HitTrimHandle(const TrimHandles& handles, TimelineWindow window, Point point, float width) {
  if (point.y < handles.lane.y || point.y >= handles.lane.y + handles.lane.height) return {};
  const auto left = window.X(Seconds(handles.range.start), width);
  const auto right = window.X(Seconds(handles.range.end), width);
  const auto near_edge = std::min(3.0F, (right - left) * 0.48F);
  if (left >= kLaneHeader && left < width && point.x >= left - kTrimHandleWidth - 4.0F && point.x <= left + near_edge)
    return TrimEdge::Start;
  if (right >= kLaneHeader && right < width && point.x >= right - near_edge && point.x <= right + kTrimHandleWidth + 4.0F)
    return TrimEdge::End;
  if (handles.movable && point.x >= std::max(kLaneHeader, left) && point.x < right) return TrimEdge::Body;
  return {};
}

struct TimelinePointer {
  class Extension;
  TimelineWindow window;
  bool centered;
  double duration;
  EditorTransport transport;
  State<double> zoom;
  State<float> track_scroll;
  float track_scroll_limit;
  std::function<void(double, float)> select;
  std::optional<TrimHandles> trim;
  WindowHandle window_handle;
  TaskScope tasks;
  std::function<void(double)> zoom_by;
};

class TimelinePointer::Extension final : public NodeExtension {
public:
  Extension(ViewNode& node, const TimelinePointer& value) : config_(value) {
    stop_magnify_ = ObserveWorkbenchMagnification(value.window_handle, [this, &node](Point point, double amount) {
      if (!node.IsEnabled() || first_ || !node.PresentationBounds().Contains(point)) return false;
      config_.tasks.Post([zoom = config_.zoom_by, amount] { zoom(amount); });
      return true;
    });
  }
  ~Extension() override {
    if (stop_magnify_) stop_magnify_();
  }
  void Update(ViewNode&, const TimelinePointer& value) { config_ = value; }

  bool HitTest(ViewNode& node, Point point) const override {
    return (point.x >= kLaneHeader || (config_.track_scroll_limit > 0.0F &&
            point.y >= kRulerHeight + LaneHeight(media::TrackKind::Video))) && point.x <= node.Bounds().width;
  }

  PointerResult OnPointer(ViewNode& node, const PointerEvent& event) override {
    const auto width = node.Bounds().width;
    if (event.type == PointerEventType::Down) {
      if (event.changed_button == PointerButton::Secondary) return PointerResult::Ignored;
      if (!first_) {
        first_ = event.pointer_id;
        origin_ = first_point_ = event.position;
        start_time_ = config_.transport.position.Get();
        start_window_ = config_.window;
        start_scroll_ = config_.track_scroll.Get();
        last_seek_ = {};
        moved_ = zoomed_ = scrolling_ = false;
        trim_edge_ = config_.trim ? HitTrimHandle(*config_.trim, start_window_, origin_, width) : std::nullopt;
        trim_ = trim_edge_ ? config_.trim : std::nullopt;
        config_.transport.scrubbing = true;
      } else if (!second_ && event.device_kind == PointerDeviceKind::Touch && (!trim_ || !moved_)) {
        trim_.reset();
        trim_edge_.reset();
        second_ = event.pointer_id;
        second_point_ = event.position;
        start_distance_ = std::max(1.0, Distance());
        start_zoom_ = config_.zoom.Get();
        zoomed_ = true;
      } else {
        return PointerResult::Ignored;
      }
      return PointerResult::Capture;
    }
    const bool first = first_ == event.pointer_id, second = second_ == event.pointer_id;
    if (!first && !second) return PointerResult::Ignored;
    if (event.type == PointerEventType::Move) {
      (first ? first_point_ : second_point_) = event.position;
      if (first_ && second_) {
        const auto zoom = std::clamp(start_zoom_ + std::log2(std::max(1.0, Distance()) / start_distance_), 0.0, kMaxZoom);
        config_.zoom_by(zoom - config_.zoom.Get());
      } else if (trim_) {
        const auto dx = event.position.x - origin_.x;
        moved_ |= std::abs(dx) > 2.0F;
        if (moved_) trim_->update(*trim_edge_, dx * start_window_.span / TimelineWindow::Width(width), TrimPhase::Move);
      } else if (!zoomed_) {
        const auto dx = std::abs(event.position.x - origin_.x);
        const auto dy = std::abs(event.position.y - origin_.y);
        if (!moved_ && std::max(dx, dy) > 5.0F) {
          moved_ = true;
          scrolling_ = origin_.y >= kRulerHeight + LaneHeight(media::TrackKind::Video) &&
              config_.track_scroll_limit > 0.0F && (dy > dx || origin_.x < kLaneHeader);
        }
        if (scrolling_) {
          config_.track_scroll = std::clamp(start_scroll_ + origin_.y - event.position.y, 0.0F, config_.track_scroll_limit);
        } else if (moved_ && origin_.x >= kLaneHeader) {
          Seek(DragTime(event.position.x, width), false);
        }
      }
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
      if (first) first_.reset();
      if (second) second_.reset();
      if (!first_ && !second_) {
        if (trim_) {
          if (moved_) {
            const auto dx = event.position.x - origin_.x;
            const auto phase = event.type == PointerEventType::Up ? TrimPhase::Commit : TrimPhase::Cancel;
            trim_->update(*trim_edge_, dx * start_window_.span / TimelineWindow::Width(width), phase);
          }
          trim_.reset();
          trim_edge_.reset();
        } else if (!zoomed_ && !scrolling_ && origin_.x >= kLaneHeader && event.type == PointerEventType::Up) {
          const auto time = moved_ ? DragTime(event.position.x, width) : start_window_.Time(event.position.x, width);
          if (!moved_ && event.position.y >= kRulerHeight) config_.select(time, event.position.y);
          Seek(time, true);
        } else if (moved_ && !zoomed_ && !scrolling_ && origin_.x >= kLaneHeader) {
          Seek(config_.transport.position.Get(), true);
        }
        config_.transport.scrubbing = false;
      }
      return PointerResult::Handled;
    }
    return PointerResult::Ignored;
  }

private:
  double Distance() const {
    return std::hypot(double(first_point_.x - second_point_.x), double(first_point_.y - second_point_.y));
  }

  double DragTime(float x, float width) const {
    return config_.centered ? start_time_ - (start_window_.Time(x, width) - start_window_.Time(origin_.x, width))
                            : start_window_.Time(x, width);
  }

  void Seek(double time, bool commit) {
    time = std::clamp(time, 0.0, config_.duration);
    config_.transport.position = time;
    const auto now = std::chrono::steady_clock::now();
    if (commit || now - last_seek_ >= std::chrono::milliseconds(55)) {
      config_.transport.seek = time;
      last_seek_ = now;
    }
  }

  TimelinePointer config_;
  std::function<void()> stop_magnify_;
  std::optional<TrimHandles> trim_;
  std::optional<TrimEdge> trim_edge_;
  TimelineWindow start_window_{};
  std::optional<std::int64_t> first_, second_;
  Point origin_, first_point_, second_point_;
  std::chrono::steady_clock::time_point last_seek_;
  double start_time_{}, start_zoom_{}, start_distance_{};
  float start_scroll_{};
  bool moved_{}, zoomed_{}, scrolling_{};
};

struct TimelineResize {
  class Extension;
  State<std::optional<float>> height;
  float visible_height;
};

class TimelineResize::Extension final : public NodeExtension {
public:
  Extension(ViewNode&, const TimelineResize& resize) : config_(resize) {}
  void Update(ViewNode&, const TimelineResize& resize) { config_ = resize; }

  bool HitTest(ViewNode& node, Point point) const override {
    return Rect{0, 0, node.Bounds().width, node.Bounds().height}.Contains(point);
  }

  PointerResult OnPointer(ViewNode& node, const PointerEvent& event) override {
    const auto y = node.PresentationBounds().y + event.position.y;
    if (event.type == PointerEventType::Down && event.changed_button == PointerButton::Primary) {
      pointer_ = event.pointer_id;
      start_y_ = y;
      start_height_ = config_.visible_height;
      previous_ = config_.height.Get();
      return PointerResult::Capture;
    }
    if (pointer_ != event.pointer_id) return PointerResult::Ignored;
    if (event.type == PointerEventType::Move)
      config_.height = std::clamp(start_height_ + start_y_ - y, 64.0F, 320.0F);
    if (event.type == PointerEventType::Cancel) config_.height = previous_;
    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) pointer_.reset();
    return PointerResult::Handled;
  }

private:
  TimelineResize config_;
  std::optional<float> previous_;
  std::optional<std::int64_t> pointer_;
  float start_y_{}, start_height_{};
};

void PaintEditorTimeline(PaintContext& paint, Size size, const ProjectSnapshot* project, TimelineWindow window,
                         const StripResult* samples, const std::vector<TimelineLane>& lanes,
                         float track_scroll, float scroll_limit,
                         std::optional<ClipId> selected, std::optional<InstanceId> selected_overlay, bool show_handles,
                         const std::optional<TrimPreview>& trim,
                         double position, const ColorScheme& colors) {
  const auto x = [&](double time) { return window.X(time, size.width); };
  const auto grips = [&](Rect clip) {
    const auto right = clip.x + clip.width, bottom = clip.y + clip.height;
    const auto ink = colors.on_surface;
    const Rect rim{clip.x - kTrimHandleWidth, clip.y, clip.width + 2.0F * kTrimHandleWidth, clip.height};
    auto shape = Path::RoundedRect(rim, CornerRadii{5.0F});
    shape.MoveTo({clip.x, clip.y + 1.5F}).LineTo({right, clip.y + 1.5F})
        .LineTo({right, bottom - 1.5F}).LineTo({clip.x, bottom - 1.5F}).Close();
    paint.FillPath(std::move(shape), ink, PathFillRule::EvenOdd);
    for (const auto center : {clip.x - kTrimHandleWidth * 0.5F, right + kTrimHandleWidth * 0.5F}) {
      paint.DrawRect({center - 0.85F, clip.y + (clip.height - 10.0F) / 2, 1.7F, 10.0F},
                     colors.surface_container_highest, CornerRadii{0.85F});
    }
  };
  const auto label = [&](Rect rect, std::string text, Color color, bool mono = false) {
    paint.DrawText(rect, std::move(text), TextStyle{.font = mono ? Font::Monospace(10.0F) : Font::System(11.0F),
                                                  .foreground = color});
  };
  const float track_width = std::max(1.0F, size.width - kLaneHeader);
  paint.DrawRect({0, 0, size.width, size.height}, colors.background);
  paint.DrawRect({0, kRulerHeight, kLaneHeader - 2.0F, size.height - kRulerHeight}, colors.surface_container_low);
  paint.DrawRect({0, 0, size.width, kRulerHeight}, colors.surface_container_low);
  const auto duration = project ? Seconds(project->Range().end) : 0.0;
  paint.PushClip({kLaneHeader, 0, std::max(1.0F, size.width - kLaneHeader), size.height});
  constexpr std::array periods{0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0};
  const auto content_width = std::max(1.0F, size.width - kLaneHeader);
  const auto period = std::ranges::find_if(periods, [&](double seconds) {
    return seconds * content_width / window.span >= (seconds < 1.0 ? 53.0 : 36.0);
  });
  const auto label_period = period == periods.end() ? periods.back() : *period;
  const auto label_width = label_period < 1.0 ? 47.0F : 35.0F;
  const auto end = std::min(duration, window.begin + window.span);
  const auto tick_step = label_period / 4.0;
  for (auto index = static_cast<std::int64_t>(std::ceil(std::max(0.0, window.begin) / tick_step));
       index * tick_step <= end; ++index) {
    const auto time = index * tick_step;
    const auto px = x(time);
    const bool major = index % 4 == 0;
    paint.DrawRect({px, major ? 20.0F : 24.0F, 1.0F, major ? 7.0F : 3.0F},
                   major ? Color::Rgb(112, 121, 135) : Color::Rgb(59, 65, 75));
    if (major && px + label_width < size.width) {
      label({px + 3.0F, 4.0F, label_width, 14.0F}, WorkbenchTimelineTime(time, label_period < 1.0),
            colors.on_surface_variant, true);
    }
  }
  paint.PopClip();

  auto y = kRulerHeight;
  const auto draw_track = [&](const TimelineLane& row) {
    const auto* track = row.track;
    const bool audio = row.kind == media::TrackKind::Audio;
    const bool captions = row.kind == media::TrackKind::Subtitle;
    const auto height = LaneHeight(row.kind);
    const auto marker = audio ? Color::Rgb(111, 202, 184) :
        captions ? Color::Rgb(191, 163, 223) : Color::Rgb(160, 179, 203);
    const auto middle = y + height * 0.5F;
    paint.DrawRect({8.0F, middle - 5.0F, 2.0F, 10.0F}, marker, CornerRadii{1.0F});
    label({15.0F, middle - 7.0F, kLaneHeader - 16.0F, 16.0F}, row.label, colors.on_surface_variant);
    paint.PushClip({kLaneHeader, y, track_width, height});
    paint.DrawRect({kLaneHeader, y + 2.0F, track_width, height - 4.0F}, colors.surface, CornerRadii{4.0F});
    std::optional<Rect> selected_grips;
    if (track) {
      for (const auto& clip : track->clips) {
        if (!VisibleClip(clip)) continue;
        const auto range = trim && (trim->clip == clip.id || trim->linked == clip.id) ? trim->range : clip.placement.timeline;
        const auto shift = trim && trim->ripple_from && &row == &lanes.front() &&
            range.start.Compare(*trim->ripple_from) != -1 ? Seconds(trim->ripple) : 0.0;
        const auto thumbnail_shift = trim && trim->clip == clip.id ? trim->thumbnail_shift : shift;
        const auto left = x(Seconds(range.start) + shift), right = x(Seconds(range.end) + shift);
        if (right <= kLaneHeader || left >= size.width) continue;
        const bool trimming = show_handles && selected == clip.id;
        const auto gap = trimming ? 0.0F : std::min(1.0F, (right - left) * 0.12F);
        const Rect rect{left + gap, y + 3.0F, right - left - 2.0F * gap, height - 6.0F};
        const auto corner = CornerRadii{trimming ? 0.0F : 4.0F};
        paint.DrawRect(rect, audio ? Color::Rgb(21, 53, 49) : colors.surface_container_high, corner);
        paint.PushClip(rect, corner);
        if (samples && !audio) {
          const StripThumbnail* first_ready = nullptr;
          for (const auto& item : samples->thumbnails) {
            if (item.clip != clip.id || !item.thumbnail) continue;
            if (!first_ready || item.placeholder_source) first_ready = &item;
            if (item.placeholder_source) break;
          }
          for (const auto& item : samples->thumbnails) {
            if (item.clip != clip.id || !first_ready) continue;
            const auto first = x(Seconds(item.project.start) + thumbnail_shift);
            const auto last = x(Seconds(item.project.end) + thumbnail_shift);
            if (last <= kLaneHeader || first >= size.width) continue;
            const auto scale = std::max((last - first) / 16.0F, rect.height / 9.0F);
            const auto image_width = scale * 16.0F;
            const auto image_height = scale * 9.0F;
            paint.PushClip({first, rect.y, last - first, rect.height});
            DrawStripThumbnail(paint, item.thumbnail ? *item.thumbnail : *first_ready->thumbnail,
                {(first + last - image_width) * 0.5F, rect.y + (rect.height - image_height) * 0.5F, image_width, image_height});
            paint.PopClip();
          }
        }
        if (samples && audio) {
          const auto baseline = rect.y + rect.height * 0.68F;
          paint.DrawRect({rect.x, baseline, rect.width, 0.6F}, Color::Rgb(73, 113, 104));
          for (const auto& item : samples->waveform) {
            if (item.clip != clip.id || item.peaks.empty()) continue;
            const auto first = x(Seconds(item.project.start)), last = x(Seconds(item.project.end));
            if (last <= kLaneHeader || first >= size.width) continue;
            float low = 0.0F, high = 0.0F;
            for (const auto& channel : item.peaks) {
              low = std::min(low, channel.minimum);
              high = std::max(high, channel.maximum);
            }
            const auto amplitude = rect.height * 0.25F;
            const auto start = baseline - std::clamp(high, 0.0F, 1.0F) * amplitude;
            const auto end = baseline - std::clamp(low, -1.0F, 0.0F) * amplitude;
            paint.DrawRect({first, start, std::max(0.65F, last - first - 0.6F), std::max(0.85F, end - start)}, marker);
          }
        }
        const auto asset = std::ranges::find(project->assets, clip.placement.source.asset, &AssetInfo::id);
        const auto title_x = std::max(kLaneHeader, rect.x) + 5.0F;
        const auto title_width = std::min(size.width, right) - title_x - 4.0F;
        if (title_width > 19.0F) {
          auto name = asset == project->assets.end() ? std::string("片段") : asset->source.file.Stem();
          const auto top = audio ? rect.y + 1.0F : rect.y + rect.height - 15.0F;
          if (!audio) paint.DrawRect({rect.x, rect.y + rect.height * 0.45F, rect.width, rect.height * 0.55F},
              LinearGradient{.start = {0, 0}, .end = {0, 1},
                .stops = {{0.0F, Color::Rgb(6, 8, 11, 0.0F)}, {1.0F, Color::Rgb(6, 8, 11, 0.87F)}}});
          paint.PushClip({title_x, top, title_width, 14.0F});
          label({title_x, top, title_width, 14.0F}, std::move(name), audio ? Color::Rgb(198, 234, 225) : Color::White());
          paint.PopClip();
        }
        paint.PopClip();
        if (trimming) selected_grips = rect;
        else if (selected == clip.id) paint.DrawBorder(rect, colors.on_surface, StrokeStyle{.width = 1.5F}, corner);
      }
    }
    for (const auto* cue : row.overlays) {
      const auto range = trim && trim->overlay == cue->id ? trim->range : cue->range;
      const auto left = x(Seconds(range.start)), right = x(Seconds(range.end));
      if (right <= std::max(kLaneHeader, left) || left >= size.width) continue;
      const Rect rect{left, y + 3.0F, right - left, height - 7.0F};
      const auto corner = CornerRadii{selected_overlay == cue->id ? 0.0F : 4.0F};
      paint.DrawRect(rect, Color::Rgb(63, 49, 81), corner);
      paint.PushClip(rect);
      const auto title = std::max(left, kLaneHeader) + 5.0F;
      label({title, y + 5.0F, std::max(0.0F, right - title), 15.0F},
            cue->text.empty() ? OverlayLabel(cue->kind) : cue->text, Color::Rgb(228, 213, 239));
      paint.PopClip();
      if (selected_overlay == cue->id) selected_grips = rect;
    }
    if (selected_grips) grips(*selected_grips);
    paint.PopClip();
    y += height;
  };
  draw_track(lanes.front());
  const auto pinned_bottom = y;
  paint.PushClip({0, pinned_bottom, size.width, std::max(0.0F, size.height - pinned_bottom)});
  y -= track_scroll;
  for (auto row = lanes.begin() + 1; row != lanes.end() && y < size.height; ++row) {
    if (y + LaneHeight(row->kind) > pinned_bottom) draw_track(*row);
    else y += LaneHeight(row->kind);
  }
  paint.PopClip();

  if (scroll_limit > 0.0F) {
    const auto viewport = size.height - pinned_bottom;
    const auto thumb = std::min(viewport, std::max(13.0F, viewport * viewport / (viewport + scroll_limit)));
    const auto offset = track_scroll / scroll_limit * (viewport - thumb);
    paint.DrawRect({size.width - 3.0F, pinned_bottom + offset, 3.0F, thumb},
                   colors.on_surface_variant, CornerRadii{1.0F});
  }

  const auto playhead = x(position);
  if (project && playhead >= kLaneHeader && playhead <= size.width) {
    paint.DrawRect({playhead - 0.5F, 22.0F, 1.0F, size.height - 22.0F}, Color::White());
    paint.DrawRect({playhead - 3.0F, 20.0F, 6.0F, 5.0F}, Color::White(), CornerRadii{1.0F});
  }
}


} // namespace

[[huxerui::composable]]
View StickerChoice(const WorkbenchSticker& sticker, State<int> frame, const ThemeSpec& theme, std::function<void()> action) {
  const auto image = UseImage(sticker.preview);
  auto preview = Canvas([image, index = frame.Get()](PaintContext& paint, Size size) {
    const auto side = std::min(size.width, size.height);
    paint.DrawImageRect(image, {80.0F * index, 0, 80, 80}, {(size.width - side) / 2, 0, side, side});
  });
  return View(Column{
    std::move(preview).With(Frame{.width = 40.0F, .height = 44.0F}),
    Text(sticker.title, TextRole::Label),
  }.With(Spacing(3.0F), Padding(EdgeInsets::Symmetric(4.0F, 6.0F)), Grow(), CrossAlign(CrossAxisAlignment::Center),
      Background(theme.colors.surface_container_low), CornerRadius(8.0F),
      Semantics{.role = SemanticRole::Button, .label = sticker.title})).OnClick(std::move(action));
}

[[huxerui::composable]]
View StickerCatalog(std::function<void(const WorkbenchSticker&)> select) {
  const auto& theme = UseTheme();
  const auto tasks = UseTaskScope();
  const auto frame = UseState(0);
  Lifecycle([tasks, frame] {
    auto timer = tasks.Launch([frame]() -> Task<void> {
      for (;;) {
        co_await Delay(std::chrono::milliseconds(120));
        frame = (frame.Get() + 1) % 8;
      }
    });
    return [timer] { timer.Cancel(); };
  });
  std::vector<View> choices;
  for (const auto& sticker : kWorkbenchStickers) {
    choices.push_back(StickerChoice(sticker, frame, theme, [select, sticker] { select(sticker); }).Key(sticker.key));
  }
  return Row(std::move(choices)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View WorkbenchEditor(WorkbenchContext context, std::function<void()> back) {
  const auto& theme = UseTheme();
  const bool wide = UseViewportClass() == ViewportClass::Expanded;
  const auto sheets = UseBottomSheet();
  const auto section = UseState(EditorTools::Main);
  const auto panel = UseState(EditorPanel::None);
  const auto selected = UseState(std::optional<ClipId>{});
  const auto selected_overlay = UseState(std::optional<InstanceId>{});
  const auto text_draft = UseState(TextEditingValue::FromText(""));
  const auto overlay_draft = UseState(CanvasOverlay{});
  const auto overlay_page = UseState(OverlayPage::Style);
  const auto text_focused = UseState(false);
  const auto text_input = UseViewHandle();
  const auto geometry = UseState(GeometryInput{});
  const auto overlay_artwork_draft = UseState(Rect{0, 0, 1, 1});
  const auto picture_draft = UseState(Rect{});
  const auto fill_draft = UseState(false);
  const auto dragging = UseState(std::optional<CanvasItem>{});
  const auto pressed_corner = UseState(std::optional<CanvasCorner>{});
  const auto gain_draft = UseState(1.0F);
  const auto mute_draft = UseState(false);
  const auto fade_in_draft = UseState(0.0F);
  const auto fade_out_draft = UseState(0.0F);
  const auto transport = EditorTransport{UseState(0.0), UseState(std::optional<double>{}), UseState(false)};
  const auto timeline_left = UseState(0.0);
  const auto timeline_zoom = UseState(0.0);
  const auto track_scroll = UseState(0.0F);
  const auto desktop_tracks = UseState(std::optional<float>{});
  const auto trim_preview = UseState(std::optional<TrimPreview>{});
  const auto window_handle = UseWindow();
  const auto document = context.document.Get();
  const auto project = document ? document->revision : nullptr;
  const bool has_media = project && project->Range().end.ticks > 0;
  const bool has_file_media = HasWorkbenchMedia();
  const bool allowed = !context.busy.Get();
  const auto close_panel = [panel, window_handle] {
    if (panel.Get() == EditorPanel::None) return;
    window_handle.HideKeyboard();
    panel = EditorPanel::None;
  };
  const auto choose_canvas = [selected, selected_overlay, close_panel](std::optional<CanvasTarget> target) {
    close_panel();
    const auto* clip = target ? std::get_if<ClipId>(&*target) : nullptr;
    const auto* overlay = target ? std::get_if<InstanceId>(&*target) : nullptr;
    selected = clip ? std::optional{*clip} : std::nullopt;
    selected_overlay = overlay ? std::optional{*overlay} : std::nullopt;
  };
  const auto enter = [section, choose_canvas](EditorTools next) {
    choose_canvas({});
    section = next;
  };
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
  const auto resources = UseService<Resources>();
  const auto assets = UseState(std::shared_ptr<const OverlayAssets>{});
  Lifecycle([assets, resources, has_file_media] {
    assets = has_file_media ? std::make_shared<const OverlayAssets>(LoadOverlayAssets(*resources)) : nullptr;
  }, has_file_media);
  const auto resource_pack = assets.Get();
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

  const auto import_movie = [context, transport](std::optional<media::TrackKind> layer) {
    const auto position = media::MediaTimestamp{std::llround(transport.position.Get() * 12.0), 12};
    const auto title = !layer ? "导入主轨" : *layer == media::TrackKind::Video ? "选择画中画" : "从视频提取音频";
    SelectWorkbenchVideo(context, title, layer ? 1 : 10, [context, layer, position](std::vector<File> files) -> Task<void> {
      auto assets = co_await RunWorker([files = std::move(files)] {
        std::vector<std::variant<AssetReference, std::string>> items;
        for (const auto& file : files) {
          try { items.emplace_back(ReadWorkbenchMovie(file)); }
          catch (const std::exception& error) { items.emplace_back(file.Name() + "：" + error.what()); }
        }
        return items;
      });
      auto next = context.document.Get();
      if (!next) next = EmptyWorkbenchProject();
      std::size_t imported = 0;
      std::string first_failure;
      for (auto& item : assets) {
        if (auto* failure = std::get_if<std::string>(&item)) {
          if (first_failure.empty()) first_failure = std::move(*failure);
          continue;
        }
        auto& asset = std::get<AssetReference>(item);
        const auto name = asset.file.Name();
        try {
          if (layer) InsertWorkbenchLayer(*next, std::move(asset), *layer, position);
          else AppendWorkbenchMovie(*next, std::move(asset));
          ++imported;
        } catch (const std::exception& error) {
          if (first_failure.empty()) first_failure = name + "：" + error.what();
        }
      }
      if (imported) context.document = std::move(next);
      if (!first_failure.empty()) {
        const auto failed = assets.size() - imported;
        context.message = "已导入 " + std::to_string(imported) + " 个，失败 " + std::to_string(failed) + " 个。\n" +
                          first_failure;
      }
    });
  };
  const auto add_movie = [import_movie] { import_movie(std::nullopt); };
  const auto add_inset = [import_movie] { import_movie(media::TrackKind::Video); };
  const auto add_audio = [import_movie] { import_movie(media::TrackKind::Audio); };
  const auto add_music = [context, transport] {
    const auto position = media::MediaTimestamp{std::llround(transport.position.Get() * 12.0), 12};
    RunWorkbenchAction(context, [context, position]() -> Task<void> {
      auto next = context.document.Get();
      if (!next) co_return;
      const auto picked = co_await PickWorkbenchFile(context, WorkbenchAudioFilter(), "AudioImports", "audio.m4a");
      if (!picked) co_return;
      const auto output = WorkbenchOutput(context.data, "Media", picked->Stem() + ".wav");
      auto asset = co_await RunWorker([file = *picked, output, snapshot = next->revision] {
        return ReadWorkbenchMusic(file, output, *snapshot);
      });
      InsertWorkbenchLayer(*next, std::move(asset), media::TrackKind::Audio, position);
      context.document = std::move(next);
    });
  };

  std::vector<View> sources;
  sources.push_back(Text("项目素材", TextRole::Label));
  if (has_file_media && wide) {
    sources.push_back(WorkbenchControl(Button("导入到主轨").OnClick(add_movie)
        .With(Enabled(allowed && CanSelectWorkbenchVideo(context))), theme, WorkbenchActionTone::Surface, true));
    if (wide && has_media) {
      sources.push_back(WorkbenchControl(Button("添加画中画").OnClick(add_inset)
          .With(Enabled(allowed && CanSelectWorkbenchVideo(context))), theme, WorkbenchActionTone::Quiet, true));
      sources.push_back(WorkbenchControl(Button("从视频提取音频").OnClick(add_audio)
          .With(Enabled(allowed && CanSelectWorkbenchVideo(context))), theme, WorkbenchActionTone::Quiet, true));
      sources.push_back(WorkbenchControl(Button("添加音乐").OnClick(add_music)
          .With(Enabled(allowed && context.picker->CanOpenFiles())), theme, WorkbenchActionTone::Quiet, true));
    }
  } else if (!has_file_media) {
    sources.push_back(Text("此构建无法导入视频。"));
  }
  if (project) {
    const auto missing_ids = missing.Get();

    for (const auto& asset : project->assets) {
      const bool absent = std::ranges::find(missing_ids, asset.id) != missing_ids.end();
      const bool video = std::ranges::any_of(asset.source.tracks, [](const AssetTrack& track) {
        return track.kind == media::TrackKind::Video;
      });
      sources.push_back(Column{
        Text(asset.source.file.Name(), TextRole::Label).LineLimit(1).Overflow(TextOverflow::Ellipsis),
        Text(absent ? "素材离线" : video ? "视频素材" : "音频素材").With(Foreground(absent ? theme.colors.error : theme.colors.on_surface_variant)),
      }.With(Padding(8.0F), Spacing(3.0F), Background(theme.colors.surface_container_low), CornerRadius(6.0F),
          CrossAlign(CrossAxisAlignment::Stretch)).Key(std::to_string(asset.id.value)));
      if (absent) {
        if (has_file_media) sources.push_back(WorkbenchControl(Button("重新链接").OnClick([context, video, id = asset.id] {
          const auto replace = [context, id](AssetReference replacement) {
            auto next = context.document.Get();
            if (!next) return;
            next->revision = next->project->Edit([&](ProjectEdit& draft) { draft.Relink(id, std::move(replacement)); });
            next->dirty = true;
            context.document = std::move(next);
          };
          if (video) {
            SelectWorkbenchVideo(context, "重新链接视频", 1, [replace](std::vector<File> files) -> Task<void> {
              replace(co_await RunWorker([file = files.front()] { return ReadWorkbenchMovie(file); }));
            });
          } else {
            RunWorkbenchAction(context, [context, replace]() -> Task<void> {
              const auto picked = co_await PickWorkbenchFile(context, WorkbenchAudioFilter(), "AudioImports", "audio.m4a");
              const auto project = context.document.Get();
              if (!picked || !project) co_return;
              const auto output = WorkbenchOutput(context.data, "Media", picked->Stem() + ".wav");
              replace(co_await RunWorker([file = *picked, output, snapshot = project->revision] {
                return ReadWorkbenchMusic(file, output, *snapshot);
              }));
            });
          }
        }).With(Enabled(allowed && (video ? CanSelectWorkbenchVideo(context) : context.picker->CanOpenFiles()))), theme));
        else sources.push_back(Text("素材丢失"));
      }
    }
  }
  if (!project || project->assets.empty()) sources.push_back(Text("导入视频后在此显示。").With(Foreground(theme.colors.on_surface_variant)));
  View library = WorkbenchPanel(Column(std::move(sources)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme);

  std::optional<ClipInfo> selection;
  std::optional<CanvasOverlay> overlay_selection;
  if (project && selected_overlay.Get()) {
    const auto it = std::ranges::find(project->overlays, *selected_overlay.Get(), &CanvasOverlay::id);
    if (it != project->overlays.end()) overlay_selection = *it;
  }
  auto selected_kind = media::TrackKind::Video;
  if (project && selected.Get()) {
    for (const auto& track : project->tracks) {
      const auto clip = std::ranges::find(track.clips, *selected.Get(), &ClipInfo::id);
      if (clip != track.clips.end()) {
        selection = *clip;
        selected_kind = track.kind;
        break;
      }
    }
  }
  const auto editing_text = !wide && panel.Get() == EditorPanel::Overlay && overlay_selection &&
      overlay_selection->kind != CanvasOverlayKind::Sticker ? std::optional{overlay_selection->id} : std::nullopt;
  Lifecycle([tasks, panel, selected_overlay, editing_text, text_input, window_handle] {
    if (!editing_text) return;
    tasks.Launch([=]() -> Task<void> {
      if (panel.Get() == EditorPanel::Overlay && selected_overlay.Get() == editing_text && text_input.RequestFocus())
        window_handle.ShowKeyboard();
      co_return;
    });
  }, editing_text);
  const auto duration = has_media ? Seconds(project->Range().end) : 0.0;
  const auto position = std::clamp(transport.position.Get(), 0.0, duration);
  const auto zoom = timeline_zoom.Get();
  const auto base_span = wide ? std::clamp(duration, 8.0, 20.0) : 8.0;
  const auto span = base_span / std::exp2(zoom);
  const auto left = std::clamp(timeline_left.Get(), 0.0, std::max(0.0, duration - span));
  const auto follow = position >= left && position <= left + span ? left : position - span * 0.5;
  const TimelineWindow visible{wide ? std::clamp(follow, 0.0, std::max(0.0, duration - span)) : position - span * 0.5,
                               span, !wide};
  const auto samples = UseEditorStrip(has_media ? project : nullptr,
                                      SamplingWindow(visible, base_span, duration, zoom));
  const auto move_to = [transport, duration](double time) {
    time = std::clamp(time, 0.0, duration);
    transport.position = time;
    transport.seek = time;
  };
  auto active_section = section.Get();
  if (active_section == EditorTools::Canvas && !has_media) active_section = EditorTools::Main;
  const auto current = selection ? EditorTools::Clip : overlay_selection ? EditorTools::Overlay : active_section;
  const auto lanes = TimelineLanes(project.get());
  const float pinned_height = kRulerHeight + LaneHeight(media::TrackKind::Video);
  const auto auxiliary_height = std::accumulate(lanes.begin() + 1, lanes.end(), 0.0F,
      [](float sum, const TimelineLane& row) { return sum + LaneHeight(row.kind); });
  constexpr float compact_auxiliary_height = 120.0F;
  constexpr float desktop_auxiliary_height = 156.0F;
  const auto auxiliary_visible = wide ? desktop_tracks.Get().value_or(desktop_auxiliary_height) : compact_auxiliary_height;
  const auto scroll_limit = std::max(0.0F, auxiliary_height - auxiliary_visible);
  const auto scrolled = std::clamp(track_scroll.Get(), 0.0F, scroll_limit);
  Lifecycle([track_scroll, scrolled] { track_scroll = scrolled; }, scroll_limit);

  const auto select = [lanes, choose_canvas, pinned_height, scrolled](double time, float y) {
    const TimelineLane* lane = &lanes.front();
    if (y >= pinned_height) {
      auto edge = pinned_height - scrolled;
      lane = nullptr;
      for (auto row = lanes.begin() + 1; row != lanes.end(); ++row) {
        edge += LaneHeight(row->kind);
        if (y < edge) { lane = &*row; break; }
      }
    }
    if (lane && lane->track) {
      for (const auto& clip : lane->track->clips) {
        if (!VisibleClip(clip)) continue;
        if (time >= Seconds(clip.placement.timeline.start) && time < Seconds(clip.placement.timeline.end)) {
          choose_canvas(clip.id);
          return;
        }
      }
    }
    if (lane) {
      for (const auto* overlay : lane->overlays) {
        if (time >= Seconds(overlay->range.start) && time < Seconds(overlay->range.end)) {
          choose_canvas(overlay->id);
          return;
        }
      }
    }
    choose_canvas({});
  };
  const auto tools = [&](const char* label, std::function<void()> action, bool enabled, const char* accessible = nullptr) {
    View button = Button(label).OnClick(std::move(action)).With(Enabled(allowed && enabled));
    if (accessible) button = std::move(button).With(Semantics{.label = accessible});
    return WorkbenchControl(std::move(button), theme, WorkbenchActionTone::Quiet, true);
  };
  const auto reverse = [edit, selection] {
    if (!selection) return;
    edit([&](Project& value) {
      auto placement = selection->placement;
      placement.source.reverse = !placement.source.reverse;
      value.Edit([&](ProjectEdit& draft) { draft.SetClip(selection->id, placement); });
      return true;
    });
  };
  const auto inset = selection && selected_kind == media::TrackKind::Video && lanes.front().track &&
      std::ranges::find(lanes.front().track->clips, selection->id, &ClipInfo::id) == lanes.front().track->clips.end();
  const auto source_size = inset ? WorkbenchVideoSize(*project, *selection) : std::nullopt;
  const auto layout = [=] {
    if (!selection) return;
    const auto bounds = inset ? FitWorkbenchInset(selection->destination, project->canvas, source_size) : selection->destination;
    picture_draft = bounds;
    fill_draft = inset || selection->fill;
    geometry = GeometryValues(bounds, project->canvas);
    panel = EditorPanel::Picture;
  };
  const auto picture_editor = [&]() -> View {
    if (!selection || selected_kind != media::TrackKind::Video) return {};
    const auto basis = picture_draft.Get();
    const auto bounds = ReadGeometry(geometry.Get(), basis, project->canvas);
    const auto apply = [=] {
      if (!bounds) return;
      edit([&](Project& value) {
        if (selection->destination == *bounds && selection->fill == fill_draft.Get()) return false;
        value.Edit([&](ProjectEdit& draft) {
          draft.SetLayout(selection->id, *bounds);
          draft.SetClipFill(selection->id, fill_draft.Get());
        });
        return true;
      });
      close_panel();
    };
    const auto option = [&](const char* title, Rect rect) {
      const auto fitted = inset ? FitWorkbenchInset(rect, project->canvas, source_size) : rect;
      return WorkbenchTab(title, theme, [picture_draft, geometry, fitted, canvas = project->canvas] {
        picture_draft = fitted;
        geometry = GeometryValues(fitted, canvas);
      }, bounds == fitted).With(Grow());
    };
    auto fields = GeometryFields(geometry, basis, project->canvas, theme);
    auto choices = Row{option("左上", kWorkbenchInsetPositions[2]), option("右上", kWorkbenchInsetPositions[0]),
                       option("左下", kWorkbenchInsetPositions[3]), option("右下", kWorkbenchInsetPositions[1])}.With(Spacing(3.0F));
    if (inset) return EditorDock(theme, EditorRow(theme, std::move(fields), apply, bounds.has_value()), std::move(choices));
    return EditorDock(theme, EditorRow(theme, Row{
        WorkbenchTab("适应", theme, [fill_draft] { fill_draft = false; }, !fill_draft.Get()).With(Grow()),
        WorkbenchTab("铺满", theme, [fill_draft] { fill_draft = true; }, fill_draft.Get()).With(Grow()),
        option("全画布", {0, 0, 1, 1}),
      }.With(Spacing(6.0F)), apply, bounds.has_value()), Column{
      std::move(fields), std::move(choices),
    }.With(Spacing(9.0F), CrossAlign(CrossAxisAlignment::Stretch)));
  };
  // Two preview frames also align to the supported 44.1 and 48 kHz PCM grids.
  const auto now = media::MediaTimestamp{std::llround(position * 12.0), 12};
  std::optional<ClipInfo> linked;
  if (selection && selected_kind == media::TrackKind::Video) {
    for (const auto& track : project->tracks) {
      if (track.kind != media::TrackKind::Audio) continue;
      for (const auto& clip : track.clips) {
        if (clip.audio_of == selection->id) linked = clip;
      }
    }
  }
  const auto audio_clip = selected_kind == media::TrackKind::Audio ? selection : linked;
  const auto adjust_audio = [=] {
    if (!audio_clip) return;
    const auto clip = *audio_clip;
    gain_draft = clip.audio_mix.gain;
    mute_draft = clip.audio_mix.muted;
    fade_in_draft = static_cast<float>(Seconds(clip.audio_mix.fade_in));
    fade_out_draft = static_cast<float>(Seconds(clip.audio_mix.fade_out));
    panel = EditorPanel::Audio;
  };
  const auto audio_editor = [&]() -> View {
    if (!audio_clip) return {};
    const auto clip = *audio_clip;
    const auto row = [&](const char* label, State<float> value, float high) {
      return EditorSlider(theme, label, WorkbenchSeconds(value.Get()), value.Get(), 0.0F, high,
                          [value](float next) { value = next; });
    };
    const auto apply = [=](bool detach) {
      edit([&](Project& value) {
        value.Edit([&](ProjectEdit& draft) {
          draft.SetAudioMix(clip.id, {gain_draft.Get(), mute_draft.Get(),
              {std::llround(fade_in_draft.Get() * 1000), 1000}, {std::llround(fade_out_draft.Get() * 1000), 1000}});
          if (detach) draft.BindAudio(clip.id, std::nullopt);
        });
        return true;
      });
      close_panel();
    };
    return EditorDock(theme, EditorRow(theme, row("音量", gain_draft, 2.0F), [apply] { apply(false); }), Column{
      row("淡入", fade_in_draft, 5.0F),
      row("淡出", fade_out_draft, 5.0F),
      WorkbenchControl(Button(mute_draft.Get() ? "取消静音" : "静音").OnClick([=] { mute_draft = !mute_draft.Get(); }), theme),
      clip.audio_of ? WorkbenchControl(Button("分离原声").OnClick([apply] { apply(true); }), theme) : View{},
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)));
  };
  const auto has_transition = [&](ClipId id) {
    return std::ranges::any_of(project->transitions, [id](const auto& transition) {
      return transition.from == id || transition.to == id;
    });
  };
  const bool can_layout = selection && selected_kind == media::TrackKind::Video && !has_transition(selection->id);
  std::vector<CanvasItem> canvas_items;
  const auto visible_at = [position](TimeRange range) {
    return position >= Seconds(range.start) && position < Seconds(range.end);
  };
  std::vector<InstanceId> visible_overlays;
  if (project) {
    for (const auto& overlay : project->overlays)
      if (visible_at(overlay.range)) visible_overlays.push_back(overlay.id);
  }
  using MeasuredBounds = std::vector<std::pair<InstanceId, Rect>>;
  const auto text_bounds = UseState(MeasuredBounds{});
  Lifecycle([=] {
    MeasuredBounds next;
    if (resource_pack && project) {
      for (const auto id : visible_overlays) {
        const auto overlay = std::ranges::find(project->overlays, id, &CanvasOverlay::id);
        try { next.emplace_back(id, MeasureWorkbenchOverlay(*overlay, project->canvas, *resource_pack)); }
        catch (const std::exception&) { next.emplace_back(id, Rect{0, 0, 1, 1}); }
      }
    }
    text_bounds = std::move(next);
  }, project, resource_pack, visible_overlays);
  const auto overlay_artwork = [measured = text_bounds.Get()](InstanceId id) {
    const auto bounds = std::ranges::find(measured, id, &MeasuredBounds::value_type::first);
    return bounds == measured.end() ? Rect{0, 0, 1, 1} : bounds->second;
  };
  if (has_media) {
    for (const auto& track : project->tracks) {
      if (track.kind != media::TrackKind::Video || &track == lanes.front().track) continue;
      for (const auto& clip : track.clips) {
        if (visible_at(clip.placement.timeline) && !has_transition(clip.id)) {
          const auto bounds = FitWorkbenchInset(clip.destination, project->canvas, WorkbenchVideoSize(*project, clip));
          canvas_items.push_back({clip.id, bounds, {0, 0, 1, 1}, clip.rotation_degrees});
        }
      }
    }
    for (const auto& overlay : project->overlays) {
      if (visible_at(overlay.range)) {
        canvas_items.push_back({overlay.id, overlay.destination, overlay_artwork(overlay.id), overlay.rotation_degrees,
                                overlay.kind != CanvasOverlayKind::Sticker});
      }
    }
  }
  std::optional<CanvasItem> canvas_item;
  for (const auto& item : canvas_items) {
    if (const auto* clip = std::get_if<ClipId>(&item.target); clip && selected.Get() == *clip) canvas_item = item;
    if (const auto* overlay = std::get_if<InstanceId>(&item.target); overlay && selected_overlay.Get() == *overlay)
      canvas_item = item;
  }
  const auto change_canvas = [context, project, edit, dragging](CanvasItem item, TrimPhase phase) {
    const auto document = context.document.Get();
    if (phase == TrimPhase::Cancel || !document || document->revision != project || context.busy.Get()) {
      dragging = std::nullopt;
      return;
    }
    if (phase == TrimPhase::Move) {
      dragging = item;
      return;
    }
    dragging = std::nullopt;
    edit([&](Project& value) {
      if (const auto* id = std::get_if<ClipId>(&item.target)) {
        value.Edit([&](ProjectEdit& draft) {
          draft.SetLayout(*id, item.bounds);
          draft.SetClipRotation(*id, item.degrees);
          draft.SetClipFill(*id, true);
        });
      } else {
        const auto instance = std::get<InstanceId>(item.target);
        const auto original = std::ranges::find(project->overlays, instance, &CanvasOverlay::id);
        if (original == project->overlays.end() ||
            (original->destination == item.bounds && original->rotation_degrees == item.degrees)) return false;
        auto changed = *original;
        changed.destination = item.bounds;
        changed.rotation_degrees = item.degrees;
        value.Edit([&](ProjectEdit& draft) { draft.SetOverlay(std::move(changed)); });
      }
      return true;
    });
  };
  const auto drag = dragging.Get();
  std::optional<VisualTransformOverride> preview_layout;
  if (drag && canvas_item && drag->target == canvas_item->target) {
    const auto basis = canvas_item->bounds;
    const auto destination = drag->bounds;
    const auto scale = destination.width / basis.width;
    auto transform = CanvasRotation(basis, project->canvas, drag->degrees - canvas_item->degrees);
    const auto old_center = Point{basis.x + basis.width / 2, basis.y + basis.height / 2};
    transform.m11 *= scale;
    transform.m12 *= scale;
    transform.m21 *= scale;
    transform.m22 *= scale;
    transform.translate_x = destination.x + destination.width / 2 - transform.m11 * old_center.x - transform.m21 * old_center.y;
    transform.translate_y = destination.y + destination.height / 2 - transform.m12 * old_center.x - transform.m22 * old_center.y;
    preview_layout = VisualTransformOverride{drag->target, transform};
  }
  const auto zoom_by = [timeline_zoom, timeline_left, transport, base_span, duration, wide](double steps) {
    const auto current = timeline_zoom.Get();
    const auto next = std::clamp(current + steps, 0.0, kMaxZoom);
    if (next == current) return;
    if (wide) {
      const auto span = base_span / std::exp2(current);
      const auto position = transport.position.Get();
      const auto left = std::clamp(timeline_left.Get(), 0.0, std::max(0.0, duration - span));
      const auto visible = position >= left && position <= left + span ? left :
          std::clamp(position - span * 0.5, 0.0, std::max(0.0, duration - span));
      timeline_left = position - (position - visible) * std::exp2(current - next);
    }
    timeline_zoom = next;
  };
  std::optional<TrimHandles> handles;
  const bool trimmable = selection && !has_transition(selection->id) && (!linked || !has_transition(linked->id));
  if (trimmable) {
    const auto main = lanes.front().track;
    const bool ripple = main && std::ranges::find(main->clips, selection->id, &ClipInfo::id) != main->clips.end();
    const auto clock = linked || selected_kind == media::TrackKind::Audio ? 12U : 24U;
    const auto length = 1.0 / clock;
    auto start = LimitTrim(*project, *selection, TrimEdge::Start, length, ripple);
    auto end = LimitTrim(*project, *selection, TrimEdge::End, length, ripple);
    if (linked) {
      start.Intersect(LimitTrim(*project, *linked, TrimEdge::Start, length, ripple));
      end.Intersect(LimitTrim(*project, *linked, TrimEdge::End, length, ripple));
    }
    auto y = kRulerHeight;
    for (const auto& lane : lanes) {
      const auto height = LaneHeight(lane.kind);
      const auto top = &lane == &lanes.front() ? y : y - scrolled;
      if (lane.track && std::ranges::find(lane.track->clips, selection->id, &ClipInfo::id) != lane.track->clips.end()) {
        const auto visible_top = std::max(top, &lane == &lanes.front() ? kRulerHeight : pinned_height);
        const auto visible_bottom = std::min(top + height, pinned_height + auxiliary_visible);
        handles = TrimHandles{selection->placement.timeline, {0, visible_top, 0, std::max(0.0F, visible_bottom - visible_top)},
            [context, project, edit, transport, trim_preview, selection, linked, clock, start, end, position, ripple]
            (TrimEdge edge, double seconds, TrimPhase phase) {
          const auto document = context.document.Get();
          if (phase == TrimPhase::Cancel || !document || document->revision != project || context.busy.Get()) {
            trim_preview = std::nullopt;
            transport.seek = position;
            return;
          }
          const auto limits = edge == TrimEdge::Start ? start : end;
          const auto delta = media::MediaTimestamp{
              std::clamp(std::llround(seconds * clock), std::llround(std::ceil(limits.minimum * clock - 1e-7)),
                         std::llround(std::floor(limits.maximum * clock + 1e-7))), clock};
          const auto placement = MoveClipEdge(selection->placement, edge, delta, ripple);
          const auto original = selection->placement.timeline;
          const auto first = Seconds(placement.timeline.start);
          const auto last = Seconds(placement.timeline.end) - 1.0 / 24.0;
          const auto preview = edge == TrimEdge::Start ? first : std::max(first, last);
          if (phase == TrimPhase::Move) {
            const TrimPreview next{selection->id, linked ? std::optional(linked->id) : std::nullopt, placement.timeline, {},
                                   ripple ? std::optional(original.end) : std::nullopt,
                                   AddWorkbenchTime(placement.timeline.end, {-original.end.ticks, original.end.timescale}),
                                   ripple && edge == TrimEdge::Start ? -Seconds(delta) : 0.0};
            if (trim_preview.Get() == next) return;
            trim_preview = next;
            const auto source_preview = ripple && edge == TrimEdge::Start ? first + Seconds(delta) : preview;
            transport.seek = std::clamp(source_preview, Seconds(original.start),
                std::max(Seconds(original.start), Seconds(original.end) - 1.0 / 24.0));
            return;
          }
          trim_preview = std::nullopt;
          if (delta.ticks == 0) {
            transport.seek = position;
            return;
          }
          edit([&](Project& value) {
            value.Edit([&](ProjectEdit& draft) {
              ApplyWorkbenchTrim(draft, *project, selection->id, placement, ripple);
            });
            transport.position = preview;
            transport.seek = preview;
            return true;
          });
        }};
        break;
      }
      y += height;
    }
  }
  if (overlay_selection) {
    auto top = pinned_height - scrolled;
    for (auto lane = lanes.begin() + 1; lane != lanes.end(); ++lane) {
      const auto height = LaneHeight(lane->kind);
      if (std::ranges::any_of(lane->overlays, [&](const auto* overlay) { return overlay->id == overlay_selection->id; })) {
        const auto shown_top = std::max(top, pinned_height);
        const auto shown_end = std::min(top + height, pinned_height + auxiliary_visible);
        const auto overlay = *overlay_selection;
        handles = TrimHandles{overlay.range, {0, shown_top, 0, std::max(0.0F, shown_end - shown_top)},
            [=](TrimEdge edge, double seconds, TrimPhase phase) {
              const auto next = context.document.Get();
              if (phase == TrimPhase::Cancel || !next || next->revision != project || context.busy.Get()) {
                trim_preview = std::nullopt;
                return;
              }
              const auto start = Seconds(overlay.range.start), end = Seconds(overlay.range.end);
              const auto length = end - start;
              const auto quantum = std::min(length, 1.0 / 12.0);
              const auto lower = edge == TrimEdge::End ? quantum - length : -start;
              const auto upper = edge == TrimEdge::Start ? length - quantum : duration - end;
              const auto delta = media::MediaTimestamp{
                  std::clamp(std::llround(seconds * 12), std::llround(std::ceil(lower * 12 - 1e-7)),
                             std::llround(std::floor(upper * 12 + 1e-7))), 12};
              auto changed = overlay;
              if (edge != TrimEdge::End) changed.range.start = AddWorkbenchTime(changed.range.start, delta);
              if (edge != TrimEdge::Start) changed.range.end = AddWorkbenchTime(changed.range.end, delta);
              if (phase == TrimPhase::Move) {
                trim_preview = TrimPreview{{}, {}, changed.range, overlay.id};
              } else {
                trim_preview = std::nullopt;
                if (delta.ticks) edit([&](Project& value) {
                  value.Edit([&](ProjectEdit& draft) { draft.SetOverlay(changed); });
                  return true;
                });
              }
            }, true};
        break;
      }
      top += height;
    }
  }
  View tracks = Canvas([project, visible, samples, lanes, scrolled, scroll_limit, selection = selected.Get(),
                        overlay = selected_overlay.Get(),
                        trimmable, trim = trim_preview.Get(), position,
                        colors = theme.colors](PaintContext& paint, Size size) {
    PaintEditorTimeline(paint, size, project.get(), visible, samples.get(), lanes, scrolled, scroll_limit, selection, overlay,
                        trimmable, trim, position, colors);
  }).With(Frame{.height = pinned_height + auxiliary_visible}, Semantics{.label = "可拖动的多轨时间线"});
  if (has_media) tracks = std::move(tracks)
      .With(TimelinePointer{visible, !wide, duration, transport, timeline_zoom, track_scroll, scroll_limit, select,
                            handles, window_handle, tasks, zoom_by}, Enabled(allowed))
      .On<ViewEvents::ScrollInput>([move_to, transport, span, track_scroll, scroll_limit, zoom_by](const ScrollInputEvent& event) {
        if (event.modifiers.control) {
          const auto amount = std::abs(event.delta_y) >= std::abs(event.delta_x) ? event.delta_y : event.delta_x;
          if (amount == 0.0F) return false;
          if (!transport.scrubbing.Get()) zoom_by(-amount / 72.0);
          return true;
        }
        if (std::abs(event.delta_y) > std::abs(event.delta_x) && scroll_limit > 0.0F) {
          track_scroll = std::clamp(track_scroll.Get() + event.delta_y, 0.0F, scroll_limit);
          return true;
        }
        if (event.delta_x == 0) return false;
        move_to(transport.position.Get() - event.delta_x * span / 350.0);
        return true;
      });
  const bool strip_failed = samples && !samples->error.empty();
  View timeline_panel = Stack{
    std::move(tracks),
    strip_failed ? View(Text(samples->error).LineLimit(1).Overflow(TextOverflow::Ellipsis)
        .With(Padding(EdgeInsets::Symmetric(8.0F, 2.0F)), Background(theme.colors.surface_container_low))) : View{},
  }.With(Frame{.height = pinned_height + auxiliary_visible}, Background(theme.colors.surface_container_low),
         Align(HorizontalAlignment::Stretch, VerticalAlignment::End));

  std::string clip_name;
  if (selection) {
    const auto source = std::ranges::find(project->assets, selection->placement.source.asset, &AssetInfo::id);
    if (source != project->assets.end()) clip_name = source->source.file.Name();
  }
  View inspector = WorkbenchPanel(Column{
    Text("片段属性", TextRole::Label),
    Divider(),
    selection ? View(Column{
      Text(clip_name, TextRole::Title).LineLimit(1).Overflow(TextOverflow::Ellipsis),
      Text(WorkbenchTimelineTime(Seconds(selection->placement.timeline.start), true) + " – " +
           WorkbenchTimelineTime(Seconds(selection->placement.timeline.end), true))
          .Style(TextStyle{.font = Font::Monospace(11.0F), .foreground = theme.colors.on_surface_variant}),
      WorkbenchControl(Button("画面布局").OnClick(layout)
          .With(Enabled(allowed && can_layout)), theme, WorkbenchActionTone::Surface, true),
      WorkbenchControl(Button(selection->placement.source.reverse ? "恢复正放" : "倒放").OnClick(reverse)
          .With(Enabled(allowed)), theme, WorkbenchActionTone::Surface, true),
      audio_clip ? WorkbenchControl(Button(linked ? "原声" : "音量").OnClick(adjust_audio)
          .With(Enabled(allowed)), theme, WorkbenchActionTone::Surface, true) : View{},
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch))) :
      View(Text("选中时间线上的片段以裁切或调整画面。").With(Foreground(theme.colors.on_surface_variant))),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme);

  const auto import_captions = [context] {
    RunWorkbenchAction(context, [context]() -> Task<void> {
      const auto picked = co_await PickWorkbenchFile(context, {"SRT 字幕", {"srt"}, {}}, "Captions", "字幕.srt");
      if (!picked) co_return;
      auto captions = co_await RunWorker([file = *picked] { return ReadWorkbenchCaptions(file); });
      auto next = context.document.Get();
      if (!next) co_return;
      next->revision = next->project->Edit([&](ProjectEdit& edit) {
        for (const auto& cue : captions) {
          const auto timestamp = [](double seconds) { return media::MediaTimestamp{std::llround(seconds * 1000), 1000}; };
          const auto start = timestamp(cue.begin);
          const auto end = std::min(timestamp(cue.end), next->revision->Range().end, Before);
          if (!Before(start, end)) continue;
          edit.AddOverlay({{}, CanvasOverlayKind::Subtitle, {start, end},
                           {0.06F, 0.78F, 0.88F, 0.17F}, cue.text});
        }
      });
      next->dirty = true;
      context.document = std::move(next);
    });
  };
  const auto open_overlay = [=](const CanvasOverlay& overlay, Rect artwork) {
    overlay_draft = overlay;
    text_draft = TextEditingValue::FromText(overlay.text);
    overlay_page = overlay.kind == CanvasOverlayKind::Sticker || overlay.kind == CanvasOverlayKind::PagText
        ? OverlayPage::Position : OverlayPage::Style;
    overlay_artwork_draft = artwork;
    geometry = GeometryValues(ArtworkBounds(overlay.destination, artwork), project->canvas);
    panel = EditorPanel::Overlay;
  };
  const auto add_overlay = [=](const WorkbenchSticker* sticker) {
    if (!has_media) return;
    const auto end = project->Range().end;
    const auto last = std::max<std::int64_t>(0, std::floor(Seconds(end) * 12.0) - 1);
    const auto start = media::MediaTimestamp{std::clamp(now.ticks, std::int64_t{}, last), 12};
    const auto stop = std::min(media::MediaTimestamp{start.ticks + 24, 12}, end, Before);
    const auto sticker_width = sticker ? std::clamp(0.24F * project->canvas.height * sticker->aspect /
                                                   project->canvas.width, 0.06F, 0.65F) : 0.0F;
    const Rect bounds = sticker ? Rect{0.92F - sticker_width, 0.08F, sticker_width, 0.24F}
                                : Rect{0.08F, 0.73F, 0.84F, 0.19F};
    const auto kind = sticker ? CanvasOverlayKind::Sticker : CanvasOverlayKind::Text;
    const ResourceRef resource{"avkit.preview", "1", sticker ? sticker->key : "pag-point-text"};
    CanvasOverlay overlay{{}, kind, {start, stop}, bounds, sticker ? "" : "标题", resource};
    edit([&](Project& value) {
      value.Edit([&](ProjectEdit& draft) {
        overlay.id = draft.AddOverlay(overlay);
      });
      return true;
    });
    if (!overlay.id.value) return;
    choose_canvas(overlay.id);
    move_to(Seconds(start));
    if (!sticker && !wide) {
      Rect artwork{0, 0, 1, 1};
      if (resource_pack) {
        try { artwork = MeasureWorkbenchOverlay(overlay, project->canvas, *resource_pack); }
        catch (const std::exception&) {}
      }
      open_overlay(overlay, artwork);
    }
  };
  const auto add_text = [add_overlay] { add_overlay(nullptr); };
  const auto sticker_catalog = [panel] { panel = EditorPanel::Stickers; };
  const auto adjust_overlay = [=] {
    if (overlay_selection) open_overlay(*overlay_selection, overlay_artwork(overlay_selection->id));
  };
  const auto overlay_editor = [&]() -> View {
    if (!overlay_selection) return {};
    const auto original = *overlay_selection;
    const bool text = original.kind != CanvasOverlayKind::Sticker;
    const bool styled_text = text && original.kind != CanvasOverlayKind::PagText;
    const auto overlay = overlay_draft.Get();
    const auto artwork = overlay_artwork_draft.Get();
    const auto basis = ArtworkBounds(overlay.destination, artwork);
    const auto bounds = ReadGeometry(geometry.Get(), basis, project->canvas);
    const auto apply = [=] {
      if (!bounds) return;
      auto changed = overlay_draft.Get();
      if (*bounds != basis) changed.destination = FrameFromArtwork(*bounds, artwork);
      if (text) changed.text = text_draft->text;
      edit([&](Project& value) {
        if (changed == original) return false;
        value.Edit([&](ProjectEdit& draft) { draft.SetOverlay(changed); });
        return true;
      });
      close_panel();
    };
    const auto tab = [&](const char* label, OverlayPage page) {
      return WorkbenchTab(label, theme, [=] {
        overlay_page = page;
        if (!wide) window_handle.HideKeyboard();
      }, overlay_page.Get() == page)
          .With(Frame{.min_width = 36.0F}, Grow());
    };
    const auto slider = [&](const char* label, float value, float low, float high, std::function<void(float)> changed) {
      return EditorSlider(theme, label, WorkbenchSeconds(value, 2), value, low, high, std::move(changed));
    };
    if (!text) {
      return EditorDock(theme, EditorRow(theme, GeometryFields(geometry, basis, project->canvas, theme), apply, bounds.has_value()),
          slider("透明度", overlay.opacity, 0, 1, [overlay_draft](float alpha) {
            overlay_draft.Update([&](CanvasOverlay& draft) { draft.opacity = alpha; });
          }));
    }
    const bool valid = bounds.has_value() && !text_draft->text.empty();
    const auto page = overlay_page.Get();
    auto tabs = Row{
      styled_text ? tab("字体", OverlayPage::Font) : View{},
      styled_text ? tab("样式", OverlayPage::Style) : View{},
      styled_text ? tab("背景", OverlayPage::Background) : View{},
      tab("位置", OverlayPage::Position),
    }.With(Spacing(2.0F));
    auto field = TextField(text_draft).Placeholder("输入文字").LineLimits(TextFieldLineLimits::MultiLine(1, 3))
        .OnChanged([text_draft](TextEditingValue value) { text_draft = std::move(value); })
        .With(text_input).On<ViewEvents::FocusChanged>([text_focused](bool focused) { text_focused = focused; });

    const auto palette = [&](bool background) {
      const auto current = background ? overlay.style.background : overlay.style.foreground;
      std::vector<View> colors;
      for (const auto& [name, color] : std::array{
               std::pair{"白色", Color::White()}, std::pair{"黑色", Color::Rgb(0, 0, 0)},
               std::pair{"珊瑚红", Color::Rgb(255, 101, 119)}, std::pair{"暖黄", Color::Rgb(255, 211, 82)},
               std::pair{"淡绿", Color::Rgb(192, 250, 92)}, std::pair{"天蓝", Color::Rgb(126, 191, 255)}}) {
        const bool selected = current.red == color.red && current.green == color.green && current.blue == color.blue;
        colors.push_back(View(Column{}.With(Frame{.width = 34.0F, .height = 34.0F}, Background(color),
            CornerRadius(17.0F), Border{selected ? theme.colors.primary : theme.colors.outline, selected ? 3.0F : 1.0F},
            Semantics{.role = SemanticRole::Button, .label = name})).OnClick([overlay_draft, color, background] {
              overlay_draft.Update([&](CanvasOverlay& draft) {
                auto& paint = background ? draft.style.background : draft.style.foreground;
                const auto alpha = background && paint.alpha == 0 ? 0.65F : paint.alpha;
                paint = {color.red, color.green, color.blue, alpha};
              });
            }));
      }
      return Row(std::move(colors)).With(Spacing(8.0F));
    };
    View content;
    switch (page) {
      case OverlayPage::Font:
        content = Row{
          WorkbenchTab("系统字体", theme, [overlay_draft] {
            overlay_draft.Update([](CanvasOverlay& draft) { draft.style.font = {}; });
          }, overlay.style.font.key.empty()),
          WorkbenchTab("Preview Sans", theme, [overlay_draft] {
            overlay_draft.Update([](CanvasOverlay& draft) { draft.style.font = {"avkit.preview", "1", "preview-sans"}; });
          }, !overlay.style.font.key.empty()),
        }.With(Spacing(12.0F));
        break;
      case OverlayPage::Style:
        content = Column{
          palette(false),
          slider("字号", overlay.style.size, 0.4F, 2.4F, [overlay_draft](float size) {
            overlay_draft.Update([&](CanvasOverlay& draft) { draft.style.size = size; });
          }),
          slider("透明度", overlay.style.foreground.alpha, 0, 1, [overlay_draft](float alpha) {
            overlay_draft.Update([&](CanvasOverlay& draft) { draft.style.foreground.alpha = alpha; });
          }),
        }.With(Spacing(8.0F));
        break;
      case OverlayPage::Background:
        content = Column{
          palette(true),
          slider("透明度", overlay.style.background.alpha, 0, 1, [overlay_draft](float alpha) {
            overlay_draft.Update([&](CanvasOverlay& draft) { draft.style.background.alpha = alpha; });
          }),
        }.With(Spacing(8.0F));
        break;
      case OverlayPage::Position:
        content = GeometryFields(geometry, basis, project->canvas, theme);
        break;
    }
    const bool typing = editing_text && text_focused.Get() && window_handle.KeyboardBounds();
    constexpr float text_details_height = 114.0F;
    return EditorDock(theme, Column{
      EditorRow(theme, EditorField(std::move(field), theme, true).With(Frame{.height = 40.0F}), apply, valid),
      std::move(tabs),
    }.With(CrossAlign(CrossAxisAlignment::Stretch)),
        typing ? View{} : std::move(content), typing ? std::nullopt : std::optional{text_details_height});
  };
  const auto remove_overlay = [=] {
    if (!overlay_selection) return;
    edit([&](Project& value) {
      value.Edit([&](ProjectEdit& draft) { draft.RemoveOverlay(overlay_selection->id); });
      return true;
    });
    choose_canvas({});
  };
  const auto save = [context] {
    RunWorkbenchAction(context, [context]() -> Task<void> {
      co_await SaveWorkbenchDocument(context);
      context.message = std::string("工程已保存");
    });
  };
  const auto create = [context, enter, transport, timeline_left, timeline_zoom, track_scroll] {
    StartWorkbenchProject(context, [=] {
      enter(EditorTools::Main);
      transport.position = 0.0;
      transport.seek = 0.0;
      timeline_left = 0.0;
      timeline_zoom = 0.0;
      track_scroll = 0.0F;
    });
  };
  const auto utility = [&](ImageResource icon, const char* label, std::function<void()> action, bool enabled) {
    return WorkbenchTool(icon, label, theme, std::move(action), allowed && enabled)
        .With(Frame{.min_width = 60.0F, .min_height = 53.0F});
  };
  const auto open_project = [context, enter, transport, timeline_left, timeline_zoom, track_scroll] {
    RunWorkbenchAction(context, [=]() -> Task<void> {
      const auto file = co_await PickWorkbenchFile(context, WorkbenchProjectFilter(), "Projects", "project.avkit");
      if (!file) co_return;
      auto opened = co_await RunWorker([file = *file] { return OpenWorkbenchProject(file); });
      if (const auto current = context.document.Get(); current && current->dirty)
        co_await SaveWorkbenchDocument(context);
      context.document = std::move(opened);
      enter(EditorTools::Main);
      timeline_left = 0.0;
      timeline_zoom = 0.0;
      track_scroll = 0.0F;
      transport.position = 0.0;
      transport.seek = 0.0;
    });
  };
  const auto project_menu = [=] {
    sheets.Show([=](BottomSheetContext sheet) {
      return Column{
        Text("工程", TextRole::Title),
        WorkbenchControl(Button("新建工程").OnClick([=] { sheet.Dismiss(); create(); })
            .With(Enabled(HasWorkbenchMedia() && CanSelectWorkbenchVideo(context))), theme),
        WorkbenchControl(Button("打开工程").OnClick([=] { sheet.Dismiss(); open_project(); })
            .With(Enabled(context.picker->CanOpenFiles())), theme),
        WorkbenchControl(Button("保存工程").OnClick([=] { sheet.Dismiss(); save(); })
            .With(Enabled(document.has_value())), theme),
      }.With(Padding(16.0F), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
    });
  };
  const auto canvas_menu = [enter] { enter(EditorTools::Canvas); };
  const auto canvas_options = [&]() -> View {
    if (!project) return {};
    auto original = project->canvas;
    for (const auto& asset : project->assets) {
      const auto track = std::ranges::find_if(asset.source.tracks, [](const AssetTrack& item) { return item.display_size.has_value(); });
      if (track != asset.source.tracks.end()) { original = *track->display_size; break; }
    }
    const auto short_edge = std::clamp(std::min(original.width, original.height), 2U, 1080U) & ~1U;
    bool has_active_ratio = false;
    const auto choose = [&](ImageResource icon, const char* label, unsigned w, unsigned h) {
      const auto unit = double(short_edge) / std::min(w, h);
      const auto even = [unit](unsigned n) { return static_cast<unsigned>(std::clamp(std::round(n * unit / 2), 1.0, 4096.0)) * 2; };
      const PixelSize size{even(w), even(h)};
      const bool active = project->canvas == size && !std::exchange(has_active_ratio, true);
      return WorkbenchTool(icon, label, theme, [=] {
        edit([&](Project& value) {
          if (value.Snapshot()->canvas == size) return false;
          value.Edit([&](ProjectEdit& draft) { draft.SetCanvas(size); });
          return true;
        });
      }, allowed, true, active).With(Grow());
    };
    return Row{
      choose(app::images::icons_canvas, "原比例", original.width, original.height),
      choose(app::images::icons_portrait, "9:16", 9, 16),
      choose(app::images::icons_landscape, "16:9", 16, 9),
      choose(app::images::icons_square, "1:1", 1, 1),
    }.With(CrossAlign(CrossAxisAlignment::Center));
  };
  const auto show_library = [sheets, library] {
    sheets.Show([library] { return ScrollView(library).With(Frame{.max_height = 310.0F}); });
  };
  const auto can_pick = has_file_media && CanSelectWorkbenchVideo(context);
  const auto entry = [enter](EditorTools page) {
    return [enter, page] { enter(page); };
  };
  const auto canvas_action = [=](CanvasCorner corner) {
    if (!canvas_item) return;
    if (corner == CanvasCorner::Edit) {
      if (overlay_selection) adjust_overlay();
      else if (can_layout) layout();
      return;
    }
    if (corner == CanvasCorner::Delete) {
      if (overlay_selection) remove_overlay();
      else if (selection) {
        edit([&](Project& value) {
          value.Edit([&](ProjectEdit& draft) { draft.RemoveClip(selection->id); });
          return true;
        });
        choose_canvas({});
      }
      return;
    }
    if (corner != CanvasCorner::Duplicate) return;
    const auto item = *canvas_item;
    const auto visible = item.Visible();
    const auto bounds = FrameFromArtwork(PlaceCanvasItem(visible, visible.x + 0.035F, visible.y + 0.035F), item.artwork);
    std::optional<CanvasTarget> duplicate;
    edit([&](Project& value) {
      value.Edit([&](ProjectEdit& draft) {
        if (overlay_selection) {
          auto copied = *overlay_selection;
          copied.destination = bounds;
          duplicate = draft.AddOverlay(std::move(copied));
        } else if (selection) {
          const auto lane = draft.AddTrack(media::TrackKind::Video);
          const auto id = draft.InsertClip(lane, selection->placement);
          draft.SetLayout(id, bounds);
          draft.SetClipFill(id, true);
          draft.SetClipRotation(id, selection->rotation_degrees);
          if (selection->background) draft.SetBackground(id, selection->background);
          for (const auto& sticker : selection->stickers) draft.AddSticker(id, sticker.spec);
          duplicate = id;
        }
      });
      return true;
    });
    if (duplicate) {
      if (const auto* id = std::get_if<InstanceId>(&*duplicate)) {
        text_bounds.Update([&](MeasuredBounds& bounds) { bounds.emplace_back(*id, item.artwork); });
      }
      choose_canvas(duplicate);
    }
  };
  View dock;
  switch (panel.Get()) {
    case EditorPanel::None: break;
    case EditorPanel::Overlay: dock = overlay_editor(); break;
    case EditorPanel::Picture: dock = picture_editor(); break;
    case EditorPanel::Audio: dock = audio_editor(); break;
    case EditorPanel::Stickers:
      dock = EditorDock(theme, EditorRow(theme, StickerCatalog([add_overlay](const WorkbenchSticker& sticker) {
        add_overlay(&sticker);
      }), close_panel));
      break;
  }
  const bool dock_open = bool(dock);
  const auto up = [current, dock_open, back, allowed, close_panel, choose_canvas, enter] {
    if (!allowed) return;
    if (dock_open) close_panel();
    else if (current == EditorTools::Main) back();
    else if (current == EditorTools::Clip || current == EditorTools::Overlay) choose_canvas({});
    else enter(EditorTools::Main);
  };
  View canvas_controls;
  if (has_media) {
    const auto plate_icon = UseVectorImage(app::images::icons_corner_plate);
    const auto pressed_plate_icon = UseVectorImage(app::images::icons_corner_plate_pressed);
    const std::array action_icons{
        UseVectorImage(app::images::icons_corner_delete), UseVectorImage(app::images::icons_corner_edit),
        UseVectorImage(app::images::icons_corner_transform), UseVectorImage(app::images::icons_corner_duplicate)};
    canvas_controls = Canvas([canvas = project->canvas, item = drag ? drag : canvas_item, pressed = pressed_corner.Get(),
                               dock_open, plate_icon, pressed_plate_icon, action_icons](PaintContext& paint, Size size) {
      if (item) PaintCanvasSelection(paint, CanvasArea(size, canvas), *item, canvas, !dock_open, pressed,
                                     plate_icon, pressed_plate_icon, action_icons);
    }).With(CanvasPointer{project->canvas, std::move(canvas_items), canvas_item, pressed_corner, choose_canvas, change_canvas, canvas_action},
            Enabled(allowed && !dock_open), Semantics{.label = "画布对象编辑区"});
  }
  EditorMonitorViews monitor_views{std::move(canvas_controls), {}, {}, resource_pack};
  if (wide) {
    if (overlay_selection) {
      inspector = WorkbenchPanel(Column {
        Text(OverlayLabel(overlay_selection->kind), TextRole::Title),
        WorkbenchControl(Button("编辑属性").OnClick(adjust_overlay), theme),
        WorkbenchControl(Button("删除").OnClick(remove_overlay), theme),
      }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)), theme);
    }
    monitor_views.media = ScrollView(library).With(Frame{.width = 216.0F}, Background(theme.colors.surface_container_low));
    monitor_views.inspector = (dock_open ? std::move(dock) : View(ScrollView(std::move(inspector))))
        .With(Frame{.width = 288.0F}, Background(theme.colors.surface_container_low));
  }
  auto monitor = EditorMonitor(context, project, transport, wide, back, project_menu, std::move(monitor_views), preview_layout);
  if (wide) {
    auto controls = current == EditorTools::Canvas ? View(Row{
      WorkbenchBack("返回上一级", theme, up, allowed), canvas_options().With(Grow()),
    }.With(Frame{.width = 344.0F}, CrossAlign(CrossAxisAlignment::Center))) : View(Row{
      tools("添加文字", add_text, has_media),
      tools("导入 SRT", import_captions, has_media && context.picker->CanOpenFiles()),
      tools("贴纸", sticker_catalog, has_media),
      tools("画布", canvas_menu, has_media),
    }.With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center)));
    return Column {
      std::move(monitor).With(Grow()),
      Canvas([ink = theme.colors.on_surface_variant](PaintContext& paint, Size size) {
        paint.DrawRect({(size.width - 34.0F) / 2, 4.0F, 34.0F, 2.0F}, ink, CornerRadii{1.0F});
      }).With(Frame{.height = 10.0F}, PointerCursor(PointerCursorKind::ResizeVertical),
              TimelineResize{desktop_tracks, auxiliary_visible}),
      Divider(),
      Row{Text("时间线", TextRole::Label), Spacer(), std::move(controls)}
          .With(Frame{.height = 46.0F}, Padding(EdgeInsets::Symmetric(14.0F, 0.0F)),
                CrossAlign(CrossAxisAlignment::Center)),
      std::move(timeline_panel),
    }.With(Background(theme.colors.surface), CrossAlign(CrossAxisAlignment::Stretch));
  }
  std::vector<View> actions;
  switch (current) {
    case EditorTools::Main:
      actions.push_back(utility(app::images::icons_video, "素材", entry(EditorTools::Media), can_pick));
      actions.push_back(utility(app::images::icons_audio, "音频", entry(EditorTools::Audio), has_media && can_pick));
      actions.push_back(utility(app::images::icons_captions, "文字", entry(EditorTools::Captions), has_media));
      actions.push_back(utility(app::images::icons_sticker, "贴纸", entry(EditorTools::Stickers), has_media));
      actions.push_back(utility(app::images::icons_layout, "画中画", entry(EditorTools::PictureInPicture), has_media));
      actions.push_back(utility(app::images::icons_canvas, "画布", canvas_menu, has_media));
      break;
    case EditorTools::Clip:
      actions.push_back(utility(app::images::icons_reverse, selection->placement.source.reverse ? "正放" : "倒放", reverse, true));
      if (selected_kind == media::TrackKind::Video)
        actions.push_back(utility(app::images::icons_layout, inset ? "位置" : "画面", layout, can_layout));
      if (audio_clip) actions.push_back(utility(app::images::icons_audio, linked ? "原声" : "音量", adjust_audio, true));
      break;
    case EditorTools::Media:
      actions.push_back(utility(app::images::icons_add, "导入主轨", add_movie, can_pick));
      if (project && !project->assets.empty())
        actions.push_back(utility(app::images::icons_video, "素材库", show_library, true));
      break;
    case EditorTools::Audio:
      actions.push_back(utility(app::images::icons_add, "添加音乐", add_music, context.picker->CanOpenFiles()));
      actions.push_back(utility(app::images::icons_audio, "从视频提取音频", add_audio, can_pick));
      break;
    case EditorTools::Captions:
      actions.push_back(utility(app::images::icons_captions, "添加文字", add_text, has_media));
      actions.push_back(utility(app::images::icons_captions, "导入 SRT", import_captions, context.picker->CanOpenFiles()));
      break;
    case EditorTools::Stickers:
      actions.push_back(utility(app::images::icons_sticker, "添加贴纸", sticker_catalog, has_media));
      break;
    case EditorTools::PictureInPicture:
      actions.push_back(utility(app::images::icons_add, "添加画中画", add_inset, has_media && can_pick));
      break;
    case EditorTools::Overlay:
      actions.push_back(utility(app::images::icons_duplicate, "复制", [canvas_action] {
        canvas_action(CanvasCorner::Duplicate);
      }, canvas_item.has_value()));
      actions.push_back(utility(app::images::icons_captions, "编辑", adjust_overlay, true));
      actions.push_back(utility(app::images::icons_delete, "删除", remove_overlay, true));
      break;
    case EditorTools::Canvas: break;
  }
  constexpr float toolbar_height = 58.0F;
  View tool_actions;
  if (current == EditorTools::Canvas) {
    tool_actions = canvas_options().With(Grow());
  } else {
    tool_actions = ScrollView(Row(std::move(actions)).With(Spacing(2.0F), CrossAlign(CrossAxisAlignment::Center)))
        .ScrollAxis(Axis::Horizontal).With(Grow()).Key(std::to_string(static_cast<int>(current)));
  }
  auto bottom = Column{
    std::move(timeline_panel), Row{
      current != EditorTools::Main ? WorkbenchBack("返回上一级", theme, up, allowed) : View{},
      std::move(tool_actions),
    }
        .With(Frame{.height = toolbar_height}, Padding(EdgeInsets::Symmetric(6.0F, 0.0F)),
              CrossAlign(CrossAxisAlignment::Center)),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Enabled(!dock_open), Semantics{.hidden = dock_open});
  return View(Stack{
    Column{
      std::move(monitor).With(Grow()),
      std::move(bottom).With(Frame{.height = pinned_height + auxiliary_visible + toolbar_height}, ClipChildren{}),
    }.With(Background(theme.colors.surface), CrossAlign(CrossAxisAlignment::Stretch)),
    dock_open ? View(Stack{
      std::move(dock).With(Frame{.min_height = toolbar_height}, AvoidKeyboard{}),
    }.With(Align(HorizontalAlignment::Stretch, VerticalAlignment::End))) : View{},
  }.With(Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch))).On<ViewEvents::BackRequested>(up);
}

} // namespace avkit_preview
