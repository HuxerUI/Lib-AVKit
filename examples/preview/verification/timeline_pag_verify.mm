#include "timeline_pag_verify.h"

#include "bgfx_metal_texture.h"
#include "metal_verify.h"
#include "movie_renderer.h"
#include "timeline_verify.h"
#include "tracking_fixture.h"
#include "workbench_player.h"

#include <huxerui/avkit/bgfx.h>
#include <huxerui/avkit/pag.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using namespace huxerui::media;
using huxerui::macos::MetalTexture;
using verification::Tick;

constexpr PixelSize kStickerSize{244, 244};
constexpr int kStickerFrames = 28;
constexpr int kProjectFrames = 41;
constexpr PixelSize kTransitionCanvas{320, 180};
constexpr int kTransitionStart = 14, kTransitionEnd = 26;

void Check(bool passed, const char* message) {
  if (!passed) throw std::runtime_error(message);
}

int Index(MediaTimestamp time) {
  const auto value = time.Rescale(kTimelinePreviewRate);
  Check(value && *value == time && value->ticks >= 0, "Expected an exact project frame time");
  return static_cast<int>(value->ticks);
}

Bytes Pixels(const ColorSurface& surface) {
  return ReadPreviewPixels(avkit::macos::GetMetalTexture(surface));
}

void VerifyWorkbenchPreview(const std::shared_ptr<const ProjectSnapshot>& project) {
  auto* package = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"HuxerUI" isDirectory:YES];
  auto assets = LoadOverlayAssets(Resources::OpenPackage(File(package.fileSystemRepresentation)));
  struct Frame {
    WorkbenchFrame state;
    std::uint64_t revision{};
    Bytes pixels;
  };
  struct Pending {
    WorkbenchFrame state;
    PreviewFrameCommit commit;
    std::uint64_t previous_revision;
  };
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<Pending> pending;
  WorkbenchPlayer player(project->canvas, std::move(assets), [&](WorkbenchFrame frame, std::stop_token token,
                                                               PreviewFrameCommit commit) {
    const auto revision = frame.texture->Revision();
    {
      const std::lock_guard lock(mutex);
      if (token.stop_requested()) return;
      pending.emplace(std::move(frame), std::move(commit), revision);
    }
    ready.notify_one();
  });
  const auto prepare = [&](int at, std::optional<WorkbenchDrag> drag) {
    const auto seconds = double(at) / kTimelinePreviewRate;
    std::unique_lock lock(mutex);
    player.Show(project, seconds, false, drag);
    Check(ready.wait_for(lock, std::chrono::seconds(5), [&] { return pending.has_value(); }),
          "The workbench did not prepare a paused frame");
    auto frame = std::move(*std::exchange(pending, std::nullopt));
    Check(frame.state.seconds == seconds && frame.state.drag == drag && !frame.state.playing,
          "The prepared frame reported the wrong seek or drag state");
    return frame;
  };
  const auto show = [&](int at, std::optional<WorkbenchDrag> drag = {}) {
    auto frame = prepare(at, drag);
    const auto texture = std::dynamic_pointer_cast<MetalTexture>(frame.state.texture);
    Check(texture && frame.commit && texture->Revision() == frame.previous_revision,
          "The workbench changed the texture before the application accepted the frame");
    frame.commit();
    Check(texture->Revision() == frame.previous_revision + 1, "The application commit did not publish the prepared frame");
    MetalTexture::Origin origin{};
    auto image = texture->AcquireFrame(origin);
    Check(image != nil && origin == MetalTexture::Origin::TopLeft, "Cannot acquire the published workbench frame");
    return Frame{std::move(frame.state), texture->Revision(), ReadPreviewPixels(image)};
  };
  const auto first = show(8);
  {
    const auto skipped = prepare(12, {});
    Check(skipped.commit && skipped.previous_revision == first.revision, "Preparing a skipped frame changed the preview");
  }
  const auto forward = show(21);
  Check(forward.revision == first.revision + 1, "A skipped frame reached the preview texture");
  const auto backward = show(8);
  const auto& sticker = project->overlays.at(1);
  auto destination = sticker.destination;
  destination.x -= 0.20F;
  const auto moved = show(8, WorkbenchDrag{{sticker.id, {.translate_x = -0.20F}}, destination, sticker.rotation_degrees});
  const auto restored = show(8);
  std::uint64_t revision = first.revision;
  for (const auto* frame : {&forward, &backward, &moved, &restored}) {
    Check(frame->state.texture == first.state.texture && frame->revision > revision,
          "The workbench replaced its texture or skipped a paused publication");
    revision = frame->revision;
  }
  Check(first.pixels != forward.pixels && first.pixels == backward.pixels && first.pixels != moved.pixels &&
            first.pixels == restored.pixels, "The published frame did not match a paused seek or sticker move");
  std::cout << "Workbench preview kept one texture through forward and backward seek and paused PAG sticker movement\n";
}

std::shared_ptr<RenderPlan> FlatPlan(PixelSize size) {
  return std::make_shared<RenderPlan>(RenderPlan{{size, PixelFormat::Bgra8Unorm}, {{0, Blend::Replace}}});
}

void CheckPagTransitionFormats() {
  const auto sample = [](PixelFormat format) {
    const ImageDesc target{kTransitionCanvas, format};
    TimelineRenderer renderer(target, CreateBgfxBackend(target));
    auto plan = FlatPlan(target.size);
    plan->target = target;
    const auto picture = [&](bool incoming) {
      // Distinct rows and unequal channels reveal a flipped image or an incorrect channel order.
      auto pixels = std::make_shared<Bytes>();
      for (std::uint32_t y = 0; y < target.size.height; ++y) {
        for (std::uint32_t x = 0; x < target.size.width; ++x) {
          pixels->push_back(std::byte(incoming ? 29 + x % 69 : 112 + x % 91));
          pixels->push_back(std::byte(35 + y % 66));
          pixels->push_back(std::byte(incoming ? 101 + y % 71 : 22 + y % 31));
          pixels->push_back(std::byte{227});
        }
      }
      const ColorSurface texture{RetainImagePixels({target.size, PixelFormat::Rgba8Unorm},
          {BufferReference(*pixels, pixels), target.size.width * 4})};
      return renderer.Render({plan, {texture}});
    };
    const auto from = picture(false), to = picture(true);
    auto transition = OpenPreviewPagTransition(target.size);
    const auto duration = Tick(kTransitionEnd - kTransitionStart);
    return std::array{Pixels(transition->Sample(Tick(0), duration, from, to)),
                      Pixels(transition->Sample(Tick(5), duration, from, to)),
                      Pixels(transition->Sample(Tick(duration.ticks - 1), duration, from, to))};
  };
  Check(sample(PixelFormat::Bgra8Unorm) == sample(PixelFormat::Rgba8Unorm),
        "The RGBA8 PAG transition changed the Metal input colors, alpha, or row order");
  std::cout << "PAG transitions match for BGRA8 and RGBA8 inputs\n";
}

void CheckPagAlpha() {
  auto animation = OpenPreviewPagSticker();
  const auto sample = [&](int frame, AnimationPlayback mode = AnimationPlayback::HoldLast) {
    return animation->Sample(Tick(frame), mode);
  };
  const auto first = sample(0);
  const auto held = GetImagePixels(first.pixels).value();
  const auto source = held.bytes.AsBytes();
  const Bytes original(source.begin(), source.end());
  const auto one = GetImagePixels(sample(1).pixels).value();
  const auto last = GetImagePixels(sample(kStickerFrames - 1).pixels).value();
  Check(first.alpha == AlphaMode::Premultiplied && first.pixels.image == ImageDesc{kStickerSize, PixelFormat::Rgba8Unorm} &&
            first.pixels.origin == RowOrigin::TopLeft && held.row_bytes == kStickerSize.width * 4 &&
            !std::ranges::equal(source, one.bytes.AsBytes()) &&
            !std::ranges::equal(source, last.bytes.AsBytes()), "Expected distinct retained frames of the PAG fixture");
  const auto looping = sample(kStickerFrames, AnimationPlayback::Loop);
  const auto holding = sample(kStickerFrames + 1);
  Check(std::ranges::equal(source, GetImagePixels(looping.pixels)->bytes.AsBytes()) &&
            std::ranges::equal(last.bytes.AsBytes(), GetImagePixels(holding.pixels)->bytes.AsBytes()) &&
            std::ranges::equal(source, original), "PAG boundary sampling changed the frame or an earlier lease");

  constexpr std::array<std::byte, 4> background{std::byte{40}, std::byte{90}, std::byte{170}, std::byte{255}};
  auto backing = std::make_shared<decltype(background)>(background);
  const ColorSurface solid{RetainImagePixels({{1, 1}, PixelFormat::Rgba8Unorm}, {BufferReference(*backing, backing), 4}),
                           AlphaMode::Opaque};
  auto plan = FlatPlan(kStickerSize);
  plan->draws.push_back({1, Blend::PremultipliedOver});
  MovieRenderer renderer(kStickerSize);
  const auto actual = Pixels(renderer.Draw(TimelineFrame{Tick(0), plan, {solid, first}}));
  std::size_t clear = 0, translucent = 0;
  for (std::size_t i = 0; i < source.size(); i += 4) {
    const auto alpha = std::to_integer<int>(source[i + 3]);
    clear += alpha == 0;
    translucent += alpha > 0 && alpha < 255;
    for (std::size_t channel = 0; channel < 3; ++channel) {
      const auto color = std::to_integer<int>(source[i + channel]);
      const auto base = std::to_integer<int>(background[channel]);
      const auto expected = color + (base * (255 - alpha) + 127) / 255;
      Check(color <= alpha && std::abs(std::to_integer<int>(actual[i + channel]) - expected) <= 1,
            "PAG alpha or RGB changed during bgfx source-over");
    }
    Check(actual[i + 3] == std::byte{255}, "PAG compositing changed the opaque backdrop");
  }
  Check(clear && translucent, "Expected fully transparent and semitransparent PAG pixels");
  std::cout << "PAG alpha: " << clear << " transparent and " << translucent
            << " semitransparent pixels match bgfx source-over\n";
}

void CheckPointTextRendering() {
  constexpr PixelSize size{960, 540};
  constexpr std::array ink{96, 176, 240};
  constexpr std::array<std::byte, 4> background{std::byte{40}, std::byte{90}, std::byte{170}, std::byte{255}};
  auto backing = std::make_shared<decltype(background)>(background);
  const ColorSurface solid{RetainImagePixels({{1, 1}, PixelFormat::Rgba8Unorm}, {BufferReference(*backing, backing), 4}),
                           AlphaMode::Opaque};
  const CanvasOverlay title{.id = {1}, .kind = CanvasOverlayKind::Text, .range = {Tick(0), Tick(40)},
      .destination = {0.083F, 0.723F, 0.813F, 0.197F}, .text = "PAG overlay",
      .style = {.foreground = Color::Rgb(ink[0], ink[1], ink[2], 0.64F)}};
  const Draw text{.input_slot = 0, .blend = Blend::PremultipliedOver, .destination = title.destination, .opacity = 0.73F};
  auto plan = std::make_shared<RenderPlan>(RenderPlan{{size, PixelFormat::Bgra8Unorm}, {text}});
  MovieRenderer renderer(size);
  PagOverlayStats stats;
  const TimelineInput input = CanvasOverlaySample{title, Tick(0)};
  const auto source = Pixels(renderer.Draw(TimelineFrame{Tick(0), plan, {input}}, stats, true));
  Check(stats.batched_flushes == 1, "A single title did not use the canvas-sized PAG target");
  auto backdrop = std::make_shared<RenderPlan>(*plan);
  backdrop->draws.insert(backdrop->draws.begin(), {1, Blend::Replace});
  const auto composed = Pixels(renderer.Draw(TimelineFrame{Tick(0), backdrop, {input, solid}}, stats, true));
  Check(stats.batched_flushes == 1, "The unchanged title lost its canvas-sized cache");
  unsigned edges = 0, fill = 0;
  for (std::size_t i = 0; i < source.size(); i += 4) {
    const int alpha = std::to_integer<int>(source[i + 3]);
    edges += alpha > 12 && alpha < 108;
    fill += alpha >= 108;
    for (std::size_t channel = 0; channel < ink.size(); ++channel) {
      const int color = std::to_integer<int>(source[i + channel]);
      Check(std::abs(color - (ink[channel] * alpha + 127) / 255) <= 2, "A point text pixel has a dark fringe or incorrect alpha");
      const auto expected = color + (std::to_integer<int>(background[channel]) * (255 - alpha) + 127) / 255;
      Check(std::abs(std::to_integer<int>(composed[i + channel]) - expected) <= 1,
            "The translucent title is not composited with premultiplied source-over");
    }
  }
  Check(edges > 100 && fill > 100, "Expected antialiased edges and filled glyphs in the title");
  std::cout << "Canvas-sized point text: " << edges << " edge pixels and " << fill << " fill pixels without a dark fringe\n";
}

void VerifyClocked(TimelineSession& session, const ProjectSnapshot& project, MovieRenderer& renderer,
                   const std::vector<Bytes>& expected) {
  session.Reset();
  MetalTexture mailbox({float(project.canvas.width), float(project.canvas.height)});
  std::stop_source stop;
  std::vector<std::pair<int, id<MTLTexture>>> frames;
  frames.reserve(2 * kProjectFrames);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  int epoch = 0, previous = -1;
  const auto stats = PlayTimeline(session, project.Range(), renderer, mailbox, Tick(0), true,
      [&](MediaTimestamp presented) -> std::optional<MediaTimestamp> {
        Check(std::chrono::steady_clock::now() < deadline, "PAG playback did not finish");
        const auto index = Index(presented);
        if (index == previous) return std::nullopt;
        Check(previous < 0 || index == previous + 1, "Clocked PAG playback skipped a frame");
        previous = index;
        MetalTexture::Origin origin;
        frames.emplace_back(index, mailbox.AcquireFrame(origin));
        if ((epoch == 0 && index == 16) || (epoch == 1 && index == 39)) {
          previous = -1;
          return Tick(++epoch == 1 ? 36 : 4);
        }
        if (index == kProjectFrames - 1) stop.request_stop();
        return std::nullopt;
      }, stop.get_token());
  Check(epoch == 2 && stats.audio_underruns == 0 && stats.late_video_frames == 0,
        "PAG preview did not resume both seeks with continuous audio");
  for (const auto& [index, texture] : frames)
    Check(ReadPreviewPixels(texture) == expected.at(index), "Clocked PAG preview sampled a different animation frame");
  std::cout << stats.clocked_frames << " clocked frames and both seeks. No missed frames or PCM underruns\n";
}

void VerifyExport(const std::shared_ptr<const ProjectSnapshot>& project, const MovieCodecProviders& codecs,
                  const File& file, const std::vector<Bytes>& expected, PagOverlayStats* stats = nullptr,
                  bool batch_overlays = true) {
  MovieRenderer renderer(project->canvas);
  FileExportSettings settings;
  settings.policy = CodecPolicy::SoftwareOnly;
  settings.video = PreviewVideoConfig(project->canvas);
  settings.video.frame_rate_numerator = kTimelinePreviewRate;
  int written = 0;
  const auto import = [&](const VideoFrame& picture) { return renderer.ImportFrame(picture); };
  ExportTimeline(project, file, codecs, settings, [&](const TimelineFrame& frame, const auto& config, auto duration) {
    const auto surface = stats ? renderer.Draw(frame, *stats, batch_overlays) : renderer.Draw(frame);
    Check(frame.pts == Tick(written) && Pixels(surface) == expected.at(written),
          "PAG export changed sampled pixels before encoding");
    ++written;
    return renderer.EncoderFrame(surface, config, frame.pts, duration);
  }, {}, {}, import);
  Check(written == kProjectFrames, "PAG export did not write every project frame");
  MovieReader movie(ImportMovie(file), codecs, CodecPolicy::SoftwareOnly);
  auto plan = FlatPlan(project->canvas);
  double worst = 0;
  for (int frame = 0; frame < kProjectFrames; ++frame) {
    const auto picture = movie.ReadVideo();
    Check(picture && picture->pts == Tick(frame) && picture->duration == Tick(1), "PAG export changed video timing");
    const auto pixels = Pixels(renderer.Draw(TimelineFrame{picture->pts, plan, {*picture}}));
    std::uint64_t error = 0;
    for (std::size_t i = 0; i < pixels.size(); ++i)
      error += std::abs(std::to_integer<int>(pixels[i]) - std::to_integer<int>(expected.at(frame)[i]));
    worst = std::max(worst, double(error) / pixels.size());
  }
  std::int64_t pcm = 0;
  std::uint32_t rate = 0;
  while (const auto sound = movie.ReadAudio()) {
    rate = sound->format.sample_rate;
    Check(sound->pts == MediaTimestamp{pcm, rate}, "PAG export changed PCM continuity");
    pcm += sound->sample_count;
  }
  Check(!movie.ReadVideo() && rate && MediaTimestamp{pcm, rate} == project->Range().end && worst < 3.0,
        "PAG reimport changed the pixels or duration");
  std::cout << written << " exact pre-encode frames and reimported PTS, " << pcm
            << " continuous PCM samples, worst mean reimport error " << worst << " / 255\n";
}

template<class Resolve>
std::vector<Bytes> VerifyPreview(const std::shared_ptr<const ProjectSnapshot>& project,
                                 const MovieCodecProviders& codecs, Resolve&& resolve) {
  MovieRenderer renderer(project->canvas);
  TimelineSession session(project, codecs, CodecPolicy::SoftwareOnly);
  const auto import = [&](const VideoFrame& picture) { return renderer.ImportFrame(picture); };
  const auto sample = [&](int frame) { return session.SampleVideo(Tick(frame), PixelFormat::Bgra8Unorm, {}, import); };
  std::vector<Bytes> expected;
  expected.reserve(kProjectFrames);
  for (int index = 0; index < kProjectFrames; ++index) {
    auto frame = sample(index);
    const auto actual = Pixels(renderer.Draw(frame));
    resolve(frame, index);
    Check(actual == Pixels(renderer.Draw(frame)), "PAG composition disagrees with explicit asset frame selection");
    expected.push_back(actual);
  }
  for (const auto index : {5, 28, 13, 40, 12, 0}) {
    session.Seek(Tick(index));
    Check(Pixels(renderer.Draw(sample(index))) == expected.at(index), "PAG seek changed the sampled pixels");
  }
  for (int index = kProjectFrames - 1; index >= 0; --index)
    Check(Pixels(renderer.Draw(sample(index))) == expected.at(index), "Descending playback changed PAG pixels");
  VerifyClocked(session, *project, renderer, expected);
  return expected;
}

void FlipVideoSources(Project& project) {
  const auto snapshot = project.Snapshot();
  project.Edit([&](ProjectEdit& edit) {
    for (const auto& track : snapshot->tracks) {
      if (track.kind != TrackKind::Video) continue;
      for (const auto& clip : track.clips) {
        auto placement = clip.placement;
        placement.source.reverse = !placement.source.reverse;
        edit.SetClip(clip.id, placement);
      }
    }
  });
}

Bytes SamplePagPixels(PagAnimation& animation, MediaTimestamp at) {
  const auto surface = animation.Sample(at, AnimationPlayback::HoldLast);
  const auto pixels = GetImagePixels(surface.pixels)->bytes.AsBytes();
  return {pixels.begin(), pixels.end()};
}

std::size_t CheckInsetChange(const Bytes& before, const Bytes& after, PixelSize size, huxerui::Rect inset) {
  Check(before.size() == after.size() && before.size() == std::size_t(size.width) * size.height * 4,
        "Unexpected PAG composite dimensions");
  std::size_t changed = 0;
  for (std::size_t i = 0; i < before.size(); i += 4) {
    if (std::equal(before.begin() + i, before.begin() + i + 4, after.begin() + i)) continue;
    const auto x = (float(i / 4 % size.width) + 0.5F) / size.width;
    const auto y = (float(i / 4 / size.width) + 0.5F) / size.height;
    Check(x >= inset.x && x <= inset.x + inset.width && y >= inset.y && y <= inset.y + inset.height,
          "An inset slot replacement changed pixels outside the inset");
    ++changed;
  }
  Check(changed, "A PAG replacement did not change any composited pixel");
  return changed;
}

int ExpectedSourceFrame(const ClipPlacement& clip, int frame) {
  const auto local = frame - Index(clip.timeline.start);
  return clip.source.reverse ? Index(clip.source.range.end) - local - 1 : Index(clip.source.range.start) + local;
}

void CheckCrossfade(const Bytes& actual, const Bytes& outgoing, const Bytes& incoming, int offset) {
  constexpr auto last = kTransitionEnd - kTransitionStart - 1;
  const auto alpha = (offset * 255 + last / 2) / last;
  Check(actual.size() == outgoing.size() && actual.size() == incoming.size(), "Unexpected PAG transition image size");
  if (alpha == 0 || alpha == 255) {
    Check(actual == (alpha == 0 ? outgoing : incoming), "PAG transition cut changed its endpoint pixels");
    return;
  }
  for (std::size_t i = 0; i < actual.size(); ++i) {
    const auto from = std::to_integer<int>(outgoing[i]), to = std::to_integer<int>(incoming[i]);
    const auto expected = (from * (255 - alpha) + to * alpha + 127) / 255;
    Check(std::abs(expected - std::to_integer<int>(actual[i])) <= 1, "PAG transition pixels disagree with the authored dissolve");
  }
}

std::vector<Bytes> VerifyTransitionPreview(const std::shared_ptr<const ProjectSnapshot>& project,
                                           const MovieCodecProviders& codecs) {
  MovieRenderer renderer(project->canvas);
  TimelineSession session(project, codecs, CodecPolicy::SoftwareOnly);
  const auto& transition = project->transitions.at(0);
  const auto& clips = project->tracks.front().clips;
  const auto flat = FlatPlan(project->canvas);
  const auto sample = [&](int frame) { return session.SampleVideo(Tick(frame)); };
  const auto flat_pixels = [&](const TimelineInput& picture) {
    return Pixels(renderer.Draw(TimelineFrame{{}, flat, {picture}}));
  };
  const auto video_slot = [](const TimelineFrame& frame, std::uint32_t pass) {
    const auto& draws = frame.plan->subcompositions.at(pass).draws;
    Check(draws.size() == 1, "Expected one picture in each PAG source pass");
    return draws.front().input_slot;
  };
  const auto picture = [](const TimelineFrame& frame, std::uint32_t slot) {
    const auto* result = std::get_if<VideoFrame>(&frame.inputs.at(slot));
    Check(result != nullptr, "Expected a retained decoded input with source PTS");
    return result;
  };

  std::vector<Bytes> expected;
  for (int index = 0; index < kProjectFrames; ++index) {
    const auto frame = sample(index);
    const auto actual = Pixels(renderer.Draw(frame));
    const auto active = index >= kTransitionStart && index < kTransitionEnd;
    Check(frame.pts == Tick(index) && frame.masks.empty() && frame.transition_animations.size() == std::size_t(active),
          "PAG transition must stay within its half-open overlap");
    if (active) {
      const auto& stamp = frame.transition_animations.front();
      const auto out = video_slot(frame, stamp.outgoing_pass), in = video_slot(frame, stamp.incoming_pass);
      Check(out != in && stamp.instance == transition.id && stamp.resource == PreviewPagTransitionResource() &&
                stamp.offset == Tick(index - kTransitionStart) && stamp.duration == Tick(kTransitionEnd - kTransitionStart) &&
                picture(frame, out)->pts == Tick(ExpectedSourceFrame(clips[0].placement, index)) &&
                picture(frame, in)->pts == Tick(ExpectedSourceFrame(clips[1].placement, index)),
            "PAG layers did not receive the outgoing and incoming PTS for the same project time");
      CheckCrossfade(actual, flat_pixels(frame.inputs[out]), flat_pixels(frame.inputs[in]), index - kTransitionStart);
    } else {
      const auto slot = frame.plan->draws.at(0).input_slot;
      Check(frame.plan->subcompositions.empty() &&
                picture(frame, slot)->pts == Tick(ExpectedSourceFrame(clips[index < kTransitionStart ? 0 : 1].placement, index)) &&
                actual == flat_pixels(frame.inputs[slot]), "PAG changed the clip just outside the overlap");
    }
    expected.push_back(actual);
  }

  constexpr auto middle = (kTransitionStart + kTransitionEnd) / 2;
  const auto original = sample(middle);
  const auto fractional = session.SampleVideo({middle * 2 + 1, 48});
  Check(Pixels(renderer.Draw(fractional)) == expected.at(middle), "A half-frame sample crossed a PAG frame boundary");
  const auto following = sample(middle + 1);
  const auto& first = original.transition_animations.front();
  const auto& next = following.transition_animations.front();
  const auto out = video_slot(original, first.outgoing_pass), in = video_slot(original, first.incoming_pass);
  const auto reference_out = flat_pixels(original.inputs[out]), reference_in = flat_pixels(original.inputs[in]);
  const auto next_out = flat_pixels(following.inputs[video_slot(following, next.outgoing_pass)]);
  const auto next_in = flat_pixels(following.inputs[video_slot(following, next.incoming_pass)]);
  {
    auto preview = OpenBgfxPreviewTexture(project->canvas);
    Check(preview.Succeeded(), "Could not open the PAG transition preview texture");
    const auto texture = std::dynamic_pointer_cast<MetalTexture>(preview.Value()->Texture());
    Check(texture != nullptr, "Expected a Metal preview texture");
    const auto revision = texture->Revision();
    auto pending = preview.Value()->Prepare(renderer.Timeline().Backend(), renderer.Draw(original));
    Check(pending.Succeeded() && pending.Value(), "Could not defer presentation of the PAG frame");
    Check(Pixels(renderer.Draw(following)) == expected.at(middle + 1) && texture->Revision() == revision,
          "A pending preview starved the next PAG frame or was published early");
    pending.Value()();
    MetalTexture::Origin origin{};
    const auto image = texture->AcquireFrame(origin);
    Check(image != nil && origin == MetalTexture::Origin::TopLeft && texture->Revision() == revision + 1 &&
              ReadPreviewPixels(image) == expected.at(middle), "The pending PAG frame changed before publication");
  }
  {
    auto pag = OpenPreviewPagTransition(project->canvas);
    const auto from = renderer.Draw(TimelineFrame{{}, flat, {original.inputs[out]}});
    const auto to = renderer.Draw(TimelineFrame{{}, flat, {original.inputs[in]}});
    const auto duration = first.duration;
    auto held = pag->Sample(Tick(0), duration, from, to);
    const auto texture = avkit::macos::GetMetalTexture(held);
    Check(texture && !GetImagePixels(held.pixels) && Pixels(held) == reference_out, "PAG output is not a Metal surface");
    const auto incoming = pag->Sample(Tick(kTransitionEnd - kTransitionStart - 1), duration, from, to);
    Check(avkit::macos::GetMetalTexture(incoming) != texture && Pixels(incoming) == reference_in &&
              Pixels(held) == reference_out, "Sampling PAG overwrote a leased IOSurface");
    held = {};
    const auto reused = pag->Sample(Tick(0), duration, from, to);
    Check(avkit::macos::GetMetalTexture(reused) == texture && Pixels(reused) == reference_out,
          "The released PAG IOSurface was not reused");
  }
  for (const bool replace_outgoing : {true, false}) {
    auto changed = original;
    changed.inputs[replace_outgoing ? out : in] =
        following.inputs[video_slot(following, replace_outgoing ? next.outgoing_pass : next.incoming_pass)];
    const auto actual = Pixels(renderer.Draw(changed));
    Check(actual != expected.at(middle), "PAG retained an old replacement at the same progress");
    CheckCrossfade(actual, replace_outgoing ? next_out : reference_out, replace_outgoing ? reference_in : next_in,
                   middle - kTransitionStart);
    Check(Pixels(renderer.Draw(original)) == expected.at(middle), "PAG retained the previous clip input after rebinding");
  }
  for (const auto index : {kTransitionEnd, middle, kTransitionStart - 1, kTransitionEnd - 1, kTransitionStart, 0}) {
    session.Seek(Tick(index));
    Check(Pixels(renderer.Draw(sample(index))) == expected.at(index), "A seek changed the PAG transition pixels");
  }
  for (int index = kProjectFrames - 1; index >= 0; --index)
    Check(Pixels(renderer.Draw(sample(index))) == expected.at(index), "Descending playback changed the PAG transition pixels");
  VerifyClocked(session, *project, renderer, expected);
  return expected;
}

void ReportTransitionTiming(const std::shared_ptr<const ProjectSnapshot>& project, const MovieCodecProviders& codecs,
                            CodecPolicy policy = CodecPolicy::SoftwareOnly) {
  MovieRenderer renderer(project->canvas);
  TimelineSession session(project, codecs, policy);
  using Milliseconds = std::chrono::duration<double, std::milli>;
  std::array<std::vector<double>, 9> times;
  int decoded = 0, platform_frames = 0;
  for (int repeat = 0; repeat < 5; ++repeat) {
    session.Seek(Tick(kTransitionStart));
    for (int index = kTransitionStart; index < kTransitionEnd; ++index) {
      const auto started = std::chrono::steady_clock::now();
      const auto frame = session.SampleVideo(Tick(index), PixelFormat::Bgra8Unorm, {},
          [&](const VideoFrame& picture) {
            ++decoded;
            platform_frames += picture.Storage() == VideoStorage::Platform;
            return renderer.ImportFrame(picture);
          });
      PagTransitionRenderTiming timing;
      const auto start = std::chrono::steady_clock::now();
      renderer.Draw(frame, &timing);
      const auto finished = std::chrono::steady_clock::now();
      Check(timing.enqueue_upload.count() == 0, "The PAG transition submitted a CPU texture upload");
      const std::array stages{timing.prepare_sides, timing.pag.stage_inputs, timing.pag.bind_images, timing.pag.flush,
                              timing.pag.wait_for_output, timing.enqueue_upload, timing.complete_output, finished - start,
                              finished - started};
      for (std::size_t stage = 0; auto& samples : times) samples.push_back(Milliseconds(stages[stage++]).count());
    }
  }
  constexpr std::array labels{"bgfx side composition", "Metal input blits and wait", "PAG image binding", "PAG flush",
                              "PAG GPU completion wait", "bgfx CPU upload enqueue", "bgfx final pass",
                              "total composition", "timeline preparation and composition"};
  std::cout << "PAG transition timing " << project->canvas.width << 'x' << project->canvas.height << ", "
            << platform_frames << '/' << decoded << " imported platform frames\n";
  for (std::size_t stage = 0; auto& samples : times) {
    std::ranges::sort(samples);
    const auto p95 = (samples.size() * 95 + 99) / 100 - 1;
    std::cout << labels[stage++] << " ms, p95 " << samples[p95] << ", max " << samples.back() << '\n';
  }
}

} // namespace

void VerifyTimelinePagStickers(std::string_view document, std::string_view directory) {
  @autoreleasepool {
    CheckPagAlpha();
    auto project = Project::Open(File(document));
    const auto fixture = project.Snapshot();
    Check(fixture->tracks.size() == 4 && fixture->transitions.empty() &&
              fixture->Range() == TimeRange{Tick(0), Tick(kProjectFrames)},
          "Use the DanceTrack animated template mask project");
    const auto main = fixture->tracks.at(0).clips.at(0);
    const auto inset = fixture->tracks.at(1).clips.at(0);
    const auto inset_start = Index(inset.placement.timeline.start);
    Check(main.placement.timeline.start == Tick(0) && inset_start == kProjectFrames - 1 - kStickerFrames,
          "The project must expose the inset animation's exact loop boundary");
    const File output(directory), saved(output, "pag-stickers.avkit");
    Check(output.CreateDirectories() && !saved.Exists(), "Use a fresh PAG verification directory");
    project.Edit([&](ProjectEdit& edit) {
      edit.AddSticker(main.id, {PreviewPagStickerResource(), {0.43F, 0.24F, 0.23F, 0.31F}, {}, {}, AnimationPlayback::HoldLast});
      edit.AddSticker(inset.id, {PreviewPagStickerResource(), {0.18F, 0.12F, 0.61F, 0.74F}, {}, {}, AnimationPlayback::Loop});
    });
    project.Save(saved);
    auto reopened = Project::Open(saved);
    Check(reopened.Snapshot()->tracks == project.Snapshot()->tracks, "Reopening changed a PAG resource or playback mode");

    const auto codecs = TimelineProviders();
    const auto verify = [&](const auto& snapshot) {
      auto reference = OpenPreviewPagSticker();
      return VerifyPreview(snapshot, codecs, [&](TimelineFrame& frame, int index) {
        int instances = 0;
        for (auto& input : frame.inputs) {
          const auto* sticker = std::get_if<AnimatedStickerSample>(&input);
          if (!sticker) continue;
          Check(sticker->resource == PreviewPagStickerResource(), "The animation resource was rebound");
          const bool holds = instances++ == 0;
          const auto local = holds ? index : index - inset_start;
          Check(local >= 0 && sticker->elapsed == Tick(local) &&
                    sticker->playback == (holds ? AnimationPlayback::HoldLast : AnimationPlayback::Loop),
                "Shared animation instances did not use their own clip-local clocks");
          const auto asset_frame = holds ? std::min(local, kStickerFrames - 1) : local % kStickerFrames;
          input = reference->Sample(Tick(asset_frame), AnimationPlayback::HoldLast);
        }
        Check(instances == (index >= inset_start ? 2 : 1), "Expected independent color inputs for the shared sticker");
      });
    };
    auto snapshot = reopened.Snapshot();
    auto expected = verify(snapshot);
    VerifyExport(snapshot, codecs, File(output, "pag-stickers.mp4"), expected);

    FlipVideoSources(reopened);
    snapshot = reopened.Snapshot();
    expected = verify(snapshot);
    VerifyExport(snapshot, codecs, File(output, "flipped-pag-stickers.mp4"), expected);
    std::cout << "PAG: shared resource, independent hold and loop, reopening, and both source directions verified\n";
  }
}

void VerifyTimelinePagReplacements(std::string_view document, std::string_view directory) {
  @autoreleasepool {
    auto project = Project::Open(File(document));
    const auto fixture = project.Snapshot();
    Check(fixture->tracks.size() == 4 && fixture->transitions.empty() &&
              fixture->Range() == TimeRange{Tick(0), Tick(kProjectFrames)},
          "Use the DanceTrack animated template mask project");
    const auto& main = fixture->tracks.at(0).clips.at(0);
    const auto& inset = fixture->tracks.at(1).clips.at(0);
    const int inset_start = Index(inset.placement.timeline.start);
    constexpr int edit_frame = 32;
    const auto local = Tick(edit_frame - inset_start);
    const File output(directory), saved(output, "pag-replacements.avkit");
    Check(output.CreateDirectories() && !saved.Exists(), "Use a fresh PAG replacement verification directory");
    const auto& resource = PreviewEditablePagResource();
    const StickerReplacements title{{{"title", "舞蹈 42"}}, {}};
    const StickerReplacements icon{{}, {{"icon", PreviewPagImageResource()}}};
    const StickerReplacements both{title.texts, icon.images};
    const StickerReplacements blank{{{"title", ""}}, {}};
    auto authored = OpenPreviewPagSticker(resource);
    const auto original = SamplePagPixels(*authored, local);
    const auto title_pixels = SamplePagPixels(*OpenPreviewPagSticker(resource, title), local);
    const auto icon_pixels = SamplePagPixels(*OpenPreviewPagSticker(resource, icon), local);
    const auto blank_pixels = SamplePagPixels(*OpenPreviewPagSticker(resource, blank), local);
    Check(original != title_pixels && original != icon_pixels && original != blank_pixels && title_pixels != blank_pixels &&
              original == SamplePagPixels(*authored, local), "Text, image or empty text did not replace its own layer");

    InstanceId main_id, inset_id;
    project.Edit([&](ProjectEdit& edit) {
      main_id = edit.AddSticker(main.id, {resource, {0.45F, 0.04F, 0.31F, 0.48F}, {}, {}, AnimationPlayback::HoldLast});
      inset_id = edit.AddSticker(inset.id, {resource, {0.05F, 0.03F, 0.92F, 0.94F}, {}, {}, AnimationPlayback::Loop});
    });
    const auto codecs = TimelineProviders();
    std::size_t title_change = 0, icon_change = 0;
    Bytes edited;
    {
      MovieRenderer renderer(fixture->canvas);
      TimelineSession session(project.Snapshot(), codecs, CodecPolicy::SoftwareOnly);
      const auto sample = [&] {
        session.ApplyProject(project.Snapshot());
        const auto frame = session.SampleVideo(Tick(edit_frame), PixelFormat::Bgra8Unorm, {},
            [&](const VideoFrame& picture) { return renderer.ImportFrame(picture); });
        return Pixels(renderer.Draw(frame));
      };
      const auto set = [&](const StickerReplacements& value) {
        project.Edit([&](ProjectEdit& edit) { edit.SetStickerReplacements(inset_id, value); });
      };
      const auto baseline = sample();
      set(title);
      const auto with_title = sample();
      title_change = CheckInsetChange(baseline, with_title, fixture->canvas, inset.destination);
      set(both);
      edited = sample();
      icon_change = CheckInsetChange(with_title, edited, fixture->canvas, inset.destination);
      Check(project.Undo() && sample() == with_title && project.Redo() && sample() == edited,
            "Undo or redo used a stale PAG sample at the edited timestamp");
      set({});
      Check(sample() == baseline && project.Undo() && sample() == edited,
            "Restoring authored values affected the main instance or retained an override");
    }
    project.Save(saved);
    auto reopened = Project::Open(saved);
    Check(reopened.Snapshot()->tracks == project.Snapshot()->tracks, "Reopening changed the sticker IDs or slot values");
    const auto verify = [&](const auto& snapshot) {
      auto replacement = OpenPreviewPagSticker(resource, both);
      return VerifyPreview(snapshot, codecs, [&](TimelineFrame& frame, int index) {
        int instances = 0;
        for (auto& input : frame.inputs) {
          const auto* sticker = std::get_if<AnimatedStickerSample>(&input);
          if (!sticker) continue;
          ++instances;
          const bool is_inset = sticker->instance == inset_id;
          Check((is_inset || sticker->instance == main_id) && sticker->resource == resource &&
                    sticker->elapsed == Tick(index - (is_inset ? inset_start : 0)) &&
                    (is_inset ? sticker->replacements == both : sticker->replacements.Empty()),
                "A PAG sample lost its instance ID, local time, or slot values");
          input = (is_inset ? *replacement : *authored).Sample(sticker->elapsed, sticker->playback);
        }
        Check(instances == (index >= inset_start ? 2 : 1), "Expected both editable PAG instances");
      });
    };
    auto snapshot = reopened.Snapshot();
    auto expected = verify(snapshot);
    Check(expected.at(edit_frame) == edited, "Reopening changed the replacement at the edited timestamp");
    VerifyExport(snapshot, codecs, File(output, "pag-replacements.mp4"), expected);
    FlipVideoSources(reopened);
    snapshot = reopened.Snapshot();
    expected = verify(snapshot);
    VerifyExport(snapshot, codecs, File(output, "flipped-pag-replacements.mp4"), expected);
    std::cout << "PAG replacements changed " << title_change << " text and " << icon_change
              << " image pixels inside the inset. Same-time editing, undo, redo, defaults, and reopening agree\n";
  }
}

void VerifyTimelinePagTransition(std::string_view document, std::string_view directory) {
  @autoreleasepool {
    const auto fixture = Project::Open(File(document)).Snapshot();
    const auto& video = fixture->tracks.at(0).clips.at(0);
    const auto& audio = fixture->tracks.at(3).clips.at(0);
    const auto source = fixture->assets.at(0);
    Check(video.placement.source.asset == source.id && audio.placement.source.asset == source.id &&
              audio.placement.timeline == TimeRange{Tick(0), Tick(kProjectFrames)},
          "Use the DanceTrack animated template mask project");
    const File output(directory), saved(output, "pag-transition.avkit");
    Check(output.CreateDirectories() && !saved.Exists(), "Use a fresh PAG transition verification directory");
    Project project(kTransitionCanvas);
    const auto created = project.Edit([&](ProjectEdit& edit) {
      const auto asset = edit.Import(source.source);
      const auto track = edit.AddTrack(TrackKind::Video);
      const auto media_track = video.placement.source.track;
      const auto from = edit.InsertClip(track, {{asset, media_track, {Tick(2), Tick(28)}}, {Tick(0), Tick(kTransitionEnd)}});
      const auto to = edit.InsertClip(track, {{asset, media_track, {Tick(26), Tick(53)}, true},
                                             {Tick(kTransitionStart), Tick(kProjectFrames)}});
      auto pcm = audio.placement;
      pcm.source.asset = asset;
      edit.InsertClip(edit.AddTrack(TrackKind::Audio), pcm);
      edit.SetTransition(from, to, {.range = {Tick(kTransitionStart), Tick(kTransitionEnd)},
                                    .animation = PreviewPagTransitionResource()});
    });
    project.Save(saved);
    auto reopened = Project::Open(saved);
    auto snapshot = reopened.Snapshot();
    Check(snapshot->transitions == created->transitions && snapshot->tracks == created->tracks,
          "PAG transition document did not retain the overlap, resource or source directions");
    const auto codecs = TimelineProviders();
    auto expected = VerifyTransitionPreview(snapshot, codecs);
    VerifyExport(snapshot, codecs, File(output, "pag-transition.mp4"), expected);
    FlipVideoSources(reopened);
    const auto flipped = reopened.Snapshot();
    expected = VerifyTransitionPreview(flipped, codecs);
    VerifyExport(flipped, codecs, File(output, "flipped-pag-transition.mp4"), expected);
    std::cout << "PAG transition: both source PTS, endpoints, same-progress rebinding, seeks and exports agree\n";
    ReportTransitionTiming(snapshot, codecs);
    auto hd = std::make_shared<ProjectSnapshot>(*snapshot);
    hd->canvas = {1920, 1080};
    ReportTransitionTiming(hd, codecs);
    ReportTransitionTiming(hd, codecs, CodecPolicy::Auto);
  }
}

void VerifyTimelinePagOverlayBatch(std::string_view first, std::string_view second, std::string_view directory) {
  @autoreleasepool {
    CheckPagTransitionFormats();
    CheckPointTextRendering();
    const File output(directory);
    Check(output.CreateDirectories(), "Cannot create the PAG overlay verification directory");
    Project project(kTransitionCanvas);
    const auto read = [](std::string_view path) { return InspectTimelineMovie(File(path)); };
    project.Edit([&](ProjectEdit& edit) {
      const auto insert = [&](const auto& source, TrackKind kind, TimeRange time, MediaTimestamp offset) {
        const auto track = std::ranges::find(source.tracks, kind, &AssetTrack::kind);
        Check(track != source.tracks.end(), "Missing required fixture track");
        const auto length = time.end.ticks - time.start.ticks;
        return edit.InsertClip(edit.AddTrack(kind), {{edit.Import(source), track->id,
            {offset, Tick(offset.ticks + length)}}, time});
      };
      const auto main = read(first), inset = read(second);
      const TimeRange range{Tick(0), Tick(kProjectFrames)};
      insert(main, TrackKind::Video, range, Tick(0));
      insert(main, TrackKind::Audio, range, Tick(0));
      const auto pip = insert(inset, TrackKind::Video, range, Tick(6));
      edit.SetLayout(pip, {0.42F, 0.1F, 0.5F, 0.5F});
      edit.AddOverlay({{}, CanvasOverlayKind::Text, range, {0.08F, 0.72F, 0.84F, 0.21F}, "PAG overlay"});
      edit.AddOverlay({{}, CanvasOverlayKind::Sticker, range, {0.48F, 0.17F, 0.31F, 0.46F}, "",
          {"avkit.preview", "1", "pag-spark"}});
      edit.AddOverlay({{}, CanvasOverlayKind::Sticker, {Tick(5), range.end}, {0.58F, 0.2F, 0.3F, 0.41F}, "",
          {"avkit.preview", "1", "pag-arrow"}, {}, 0.72F, 13.0F});
    });
    const File saved(output, "overlay-batch.avkit");
    project.Save(saved);
    const auto snapshot = Project::Open(saved).Snapshot();
    VerifyWorkbenchPreview(snapshot);
    const auto codecs = TimelineProviders();
    const auto sample = [](TimelineSession& session, MovieRenderer& renderer, int index) {
      return session.SampleVideo(Tick(index), PixelFormat::Bgra8Unorm, {},
          [&](const auto& frame) { return renderer.ImportFrame(frame); });
    };
    std::vector<Bytes> expected, batched_pixels;
    const auto preview = [&](bool batch) {
      MovieRenderer renderer(snapshot->canvas);
      TimelineSession session(snapshot, codecs, CodecPolicy::SoftwareOnly);
      PagOverlayStats stats;
      for (int frame = 0; frame < kProjectFrames; ++frame) {
        const auto pixels = Pixels(renderer.Draw(sample(session, renderer, frame), stats, batch));
        if (!batch) expected.push_back(pixels);
        else batched_pixels.push_back(pixels);
      }
      if (batch) {
        for (const auto frame : {30, 2, 38, 6, 0}) {
          session.Seek(Tick(frame));
          Check(Pixels(renderer.Draw(sample(session, renderer, frame))) == batched_pixels[frame],
                "PAG batching changed the forward or backward seek pixels");
        }
        VerifyClocked(session, *snapshot, renderer, batched_pixels);
      }
      return stats;
    };
    const auto compare = [](const char* name, const PagOverlayStats& single, const PagOverlayStats& batch) {
      Check(single.text_layers == 1 && batch.text_layers == 1 && single.text_layer_cache_hits == kProjectFrames - 1 &&
                batch.text_layer_cache_hits == single.text_layer_cache_hits, "PAG batching changed the static text cache");
      Check(batch.batched_flushes == kProjectFrames && batch.flushes == kProjectFrames && single.flushes > batch.flushes,
            "PAG batching did not render the title and active stickers together");
      using Milliseconds = std::chrono::duration<double, std::milli>;
      std::cout << name << ": " << single.flushes << " -> " << batch.flushes << " PAG flushes, "
                << Milliseconds(single.gpu_sync).count() << " -> " << Milliseconds(batch.gpu_sync).count()
                << " ms total PAG GPU wait, " << batch.text_layer_cache_hits << " text layer cache hits\n";
    };
    const auto single = preview(false);
    const auto batch = preview(true);
    Check(File(output, "reference.rgba").WriteBytes(expected.at(22)) &&
              File(output, "batched.rgba").WriteBytes(batched_pixels.at(22)),
          "Cannot save the overlay comparison frames");
    std::size_t different = 0;
    std::uint64_t total = 0;
    int maximum = 0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
      const auto& a = expected[frame];
      const auto& b = batched_pixels[frame];
      Check(a.size() == b.size(), "PAG batching changed the output dimensions");
      constexpr auto row = kTransitionCanvas.width * 4;
      Check(std::equal(a.begin(), a.begin() + row * 20, b.begin()) &&
                std::equal(a.begin() + row * 168, a.end(), b.begin() + row * 168),
            "PAG batching changed pixels outside the overlay rows");
      for (std::size_t i = 0; i < a.size(); ++i) {
        const auto delta = std::abs(std::to_integer<int>(a[i]) - std::to_integer<int>(b[i]));
        different += delta > 0;
        total += delta;
        maximum = std::max(maximum, delta);
      }
    }
    const auto mean = double(total) / (expected.size() * expected.front().size());
    std::cout << "A/B pixel comparison: " << different << " differing channels, mean " << mean
              << " / 255, maximum " << maximum << " / 255\n";
    Check(mean < 1.0, "PAG batching changed the preview pixels beyond expected edge rasterization differences");
    constexpr auto overlap = (73 * kTransitionCanvas.width + 214) * 4;
    const std::array blended{210, 239, 89, 255};
    for (std::size_t channel = 0; channel < blended.size(); ++channel) {
      const auto value = std::to_integer<int>(batched_pixels.at(22)[overlap + channel]);
      Check(std::abs(value - blended[channel]) <= 1, "The translucent arrow must draw over the opaque spark");
    }
    compare("Preview", single, batch);
    PagOverlayStats single_export, batch_export;
    VerifyExport(snapshot, codecs, File(output, "overlay-reference.mp4"), expected, &single_export, false);
    VerifyExport(snapshot, codecs, File(output, "overlay-batch.mp4"), batched_pixels, &batch_export);
    compare("Export", single_export, batch_export);
  }
}

} // namespace avkit_preview
