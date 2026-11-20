#include "timeline_pag_verify.h"

#include "bgfx_metal_texture.h"
#include "metal_preview.h"
#include "movie_renderer.h"
#include "timeline_preview.h"
#include "verification/tracking_fixture.h"

#include <huxerui/avkit/pag.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
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

std::shared_ptr<RenderPlan> FlatPlan(PixelSize size) {
  return std::make_shared<RenderPlan>(RenderPlan{{size, PixelFormat::Bgra8Unorm}, {{0, Blend::Replace}}});
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
                  const File& file, const std::vector<Bytes>& expected) {
  MovieRenderer renderer(project->canvas);
  FileExportSettings settings;
  settings.policy = CodecPolicy::SoftwareOnly;
  settings.video = PreviewVideoConfig(project->canvas);
  settings.video.frame_rate_numerator = kTimelinePreviewRate;
  int written = 0;
  const auto import = [&](const VideoFrame& picture) { return renderer.ImportFrame(picture); };
  ExportTimeline(project, file, codecs, settings, [&](const TimelineFrame& frame, const auto& config, auto duration) {
    const auto surface = renderer.Draw(frame);
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
    const auto held = renderer.Draw(original);
    Check(Pixels(renderer.Draw(following)) == expected.at(middle + 1) && Pixels(held) == expected.at(middle),
          "A retained composite starved or changed during the following PAG transition frame");
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
      PreviewPagTransitionTiming timing;
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

} // namespace avkit_preview
