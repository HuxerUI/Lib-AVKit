#include "timeline_template_mask_verify.h"

#include "bgfx_metal_texture.h"
#include "metal_verify.h"
#include "movie_renderer.h"
#include "support/media_time.h"
#include "timeline_verify.h"
#include "codec_verification.h"
#include "tracking_fixture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>

#include <huxerui/avkit/edit_template.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using namespace huxerui::media;
using huxerui::Point;
using huxerui::Rect;
using huxerui::macos::MetalTexture;
using verification::Tick;

constexpr PixelSize kCanvas{640, 480};
constexpr int kFrames = 41;
constexpr int kNearSeekBack = 4;
constexpr int kMaskSize = 8;
constexpr int kInsetStart = 12;
constexpr int kSecondInsetStart = 13;
constexpr std::array kMaskStarts{0, kInsetStart, kSecondInsetStart};
constexpr MediaTimestamp kMaskInterval{5, 48};
constexpr std::uint32_t kMaskFrames = 3;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int Index(MediaTimestamp value) {
  const auto ticks = value.Rescale(24);
  Check(ticks && *ticks == value && ticks->ticks >= 0, "Expected an exact DanceTrack frame index");
  return static_cast<int>(ticks->ticks);
}

Bytes Pixels(const ColorSurface& surface) {
  return ReadPreviewPixels(avkit::macos::GetMetalTexture(surface));
}

TimelineFrame FlatFrame(const VideoFrame& picture) {
  auto plan = std::make_shared<RenderPlan>();
  plan->target = {kCanvas, PixelFormat::Bgra8Unorm};
  plan->draws = {{0, Blend::Replace}};
  return {picture.pts, std::move(plan), {picture}};
}

void CheckDanceTrackProject(const ProjectSnapshot& source) {
  Check(source.canvas == kCanvas && source.Range() == TimeRange{Tick(0), Tick(kFrames)} && source.assets.size() == 1 &&
        source.tracks.size() == 3 && source.tracks[0].kind == TrackKind::Video && source.tracks[1].kind == TrackKind::Video &&
        source.tracks[2].kind == TrackKind::Audio && source.transitions.empty(), "Expected the mixed DanceTrack project");
  for (const auto& track : source.tracks) {
    Check(track.clips.size() == 1 && track.clips.front().placement.timeline == TimeRange{Tick(0), Tick(kFrames)},
          "Expected one full-length clip on each DanceTrack fixture track");
    if (track.kind == TrackKind::Video)
      Check(track.clips.front().background.has_value(), "Expected the original DanceTrack crop and mirror settings");
  }
}

EditTemplateRecipe MakeRecipe(const ProjectSnapshot& source, std::string key) {
  const auto& asset = source.assets.front();
  EditTemplateRecipe recipe{{"avkit.preview", "1", std::move(key)}, source.canvas, {"dance"}};
  for (const auto& track : source.tracks) {
    const auto& clip = track.clips.front();
    const auto& placement = clip.placement;
    const auto meta = std::ranges::find(asset.source.tracks, placement.source.track, &AssetTrack::id);
    Check(placement.source.asset == asset.id && meta != asset.source.tracks.end(), "Wrong DanceTrack fixture source");
    const auto zero = meta->presentation.start;
    const auto range = placement.source.range;
    TemplateClip item{"dance", {avkit::detail::Difference(range.start, zero), avkit::detail::Difference(range.end, zero)},
                      placement.timeline, clip.destination, {}, clip.background, placement.source.reverse};
    for (const auto& sticker : clip.stickers) item.stickers.push_back(sticker.spec);
    recipe.tracks.push_back({track.kind, {std::move(item)}});
  }
  return recipe;
}

Project Assemble(const ProjectSnapshot& source, const EditTemplateRecipe& recipe) {
  Project project(source.canvas);
  AssetId asset_id;
  project.Edit([&](ProjectEdit& edit) { asset_id = edit.Import(source.assets.front().source); });
  const auto plan = PlanEditTemplate(project, recipe, {{asset_id}, {}});
  Check(project.Snapshot()->tracks.empty(), "Template planning edited the project");
  ApplyEditTemplate(project, plan);
  return project;
}

std::vector<Bytes> SourcePixels(const MovieAsset& asset, const MovieCodecProviders& codecs, MovieRenderer& renderer,
                                const ProjectSnapshot& project) {
  int end = 0;
  for (const auto& track : project.tracks) {
    if (track.kind == TrackKind::Video)
      end = std::max(end, Index(track.clips.front().placement.source.range.end));
  }
  MovieReader reader(asset, codecs, TrackKind::Video, {}, CodecPolicy::SoftwareOnly);
  std::vector<Bytes> raw;
  raw.reserve(end);
  for (int index = 0; index < end; ++index) {
    const auto picture = reader.ReadVideo();
    Check(picture && picture->pts == Tick(index) && picture->duration == Tick(1) &&
              picture->format.visible.width == kCanvas.width && picture->format.visible.height == kCanvas.height,
          "Expected the original 640x480, 24 fps DanceTrack source");
    raw.push_back(Pixels(renderer.Draw(FlatFrame(*picture))));
  }
  return raw;
}

int SourceFrame(const ClipInfo& clip, int index) {
  const auto& source = clip.placement.source;
  const auto local = index - Index(clip.placement.timeline.start);
  return source.reverse ? Index(source.range.end) - 1 - local : Index(source.range.start) + local;
}

float Coverage(float u, float v, int frame) {
  const auto sample = [&](int x, int y) {
    if (frame == 2) {
      if (x < 2 && y >= 3) return 255.F;
      return x >= 4 && y <= 3 ? 196.F : 0.F;
    }
    if (frame == 1) {
      if (x > 4 && y > 1) return 255.F;
      return x >= 1 && x < 5 && y > 4 ? 160.F : 0.F;
    }
    if (x < 3 && y >= 1 && y < 6) return 255.F;
    return x >= 3 && x < 6 && y < 3 ? 112.F : 0.F;
  };
  const auto x = std::clamp(u * kMaskSize - 0.5F, 0.F, float(kMaskSize - 1));
  const auto y = std::clamp(v * kMaskSize - 0.5F, 0.F, float(kMaskSize - 1));
  const int a = int(x), b = int(y);
  const int c = std::min(a + 1, kMaskSize - 1), d = std::min(b + 1, kMaskSize - 1);
  return std::lerp(std::lerp(sample(a, b), sample(c, b), x - a), std::lerp(sample(a, d), sample(c, d), x - a), y - b) / 255.F;
}

void CheckMaskPixels(const Bytes& image, const std::vector<Bytes>& raw, std::span<const ClipInfo* const> clips,
                     std::span<const int> masks, int index) {
  std::vector<std::size_t> edges(clips.size());
  for (unsigned y = 0; y < kCanvas.height; y += 3) {
    for (unsigned x = 0; x < kCanvas.width; x += 3) {
      const Point point{(x + 0.5F) / kCanvas.width, (y + 0.5F) / kCanvas.height};
      auto layer = clips.size() - 1;
      while (layer && (index < Index(clips[layer]->placement.timeline.start) || !clips[layer]->destination.Contains(point)))
        --layer;
      const auto& clip = *clips[layer];
      const auto& effect = *clip.background;
      const auto& crop = effect.source_crop;
      const auto& dest = clip.destination;
      const auto u = (point.x - dest.x) / dest.width, v = (point.y - dest.y) / dest.height;
      const auto sx = crop.x + crop.width * (effect.mirrored ? 1 - u : u);
      const auto sy = crop.y + crop.height * v;
      const auto px = sx * kCanvas.width, py = sy * kCanvas.height;
      // Ignore exact color texel boundaries where point filtering can round either way.
      if (std::abs(px - std::round(px)) < 0.001F || std::abs(py - std::round(py)) < 0.001F) continue;
      const auto coverage = Coverage(sx, sy, masks[layer]);
      const auto& picture = raw.at(SourceFrame(clip, index));
      const auto from = 4 * (unsigned(py) * kCanvas.width + unsigned(px));
      const auto to = 4 * (y * kCanvas.width + x);
      const std::array background{effect.color.red, effect.color.green, effect.color.blue};
      for (std::size_t channel = 0; channel < background.size(); ++channel) {
        const auto expected = std::lerp(background[channel] * 255, float(std::to_integer<unsigned>(picture[from + channel])), coverage);
        Check(std::abs(std::to_integer<unsigned>(image[to + channel]) - expected) <= 3,
              "Mask pixels moved away from the cropped or mirrored source");
      }
      Check(image[to + 3] == std::byte{255}, "An opaque mask effect lost alpha");
      edges[layer] += coverage > 0 && coverage < 1;
    }
  }
  for (std::size_t layer = 0; layer < clips.size(); ++layer) {
    if (Index(clips[layer]->placement.timeline.start) <= index)
      Check(edges[layer] > 100, "Mask fixture needs fractional coverage in each visible clip");
  }
}

void CheckReplacementPixels(const Bytes& before, const Bytes& after, Rect inset) {
  std::size_t changed = 0;
  for (unsigned y = 0; y < kCanvas.height; ++y) {
    for (unsigned x = 0; x < kCanvas.width; ++x) {
      const auto pixel = 4 * (y * kCanvas.width + x);
      if (std::equal(before.data() + pixel, before.data() + pixel + 4, after.data() + pixel)) continue;
      Check(inset.Contains({(x + 0.5F) / kCanvas.width, (y + 0.5F) / kCanvas.height}),
            "Replacing the inset mask changed pixels outside the inset");
      ++changed;
    }
  }
  Check(changed > 1000, "The inset mask replacement did not change enough pixels");
}

void CheckClocked(TimelineSession& session, MovieRenderer& renderer, const std::vector<Bytes>& preview,
                  std::string_view label, bool reverse_order = false, int seek_after = 5, int seek_back = 2) {
  session.Reset();
  MetalTexture mailbox({float(kCanvas.width), float(kCanvas.height)});
  std::vector<std::pair<int, id<MTLTexture>>> snapshots;
  snapshots.reserve(3 * kFrames);
  std::stop_source stop;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  std::optional<std::chrono::steady_clock::time_point> last_clocked_frame;
  double maximum_frame_gap_ms = 0;
  int last = -1, seeks = 0, missed = 0;
  const auto stats = PlayTimeline(session, {Tick(0), Tick(kFrames)}, renderer, mailbox, Tick(0), true,
      [&](MediaTimestamp presented) -> std::optional<MediaTimestamp> {
        const auto now = std::chrono::steady_clock::now();
        Check(now < deadline, "Clocked template mask preview did not finish");
        const auto frame = Index(presented);
        if (frame == last) return std::nullopt;
        if (last >= 0 && frame > last) {
          missed += frame - last - 1;
          if (last_clocked_frame) {
            const auto gap = std::chrono::duration<double, std::milli>(now - *last_clocked_frame).count();
            maximum_frame_gap_ms = std::max(maximum_frame_gap_ms, gap);
          }
          last_clocked_frame = now;
        }
        last = frame;
        MetalTexture::Origin origin;
        // Keep the published snapshot and verify its pixels after the clocked run.
        snapshots.emplace_back(frame, mailbox.AcquireFrame(origin));
        if (seeks == 0 && frame >= seek_after) {
          ++seeks;
          last = -1;
          last_clocked_frame.reset();
          return Tick(kFrames - 5);
        }
        if (seeks == 1 && frame >= kFrames - 2) {
          ++seeks;
          last = -1;
          last_clocked_frame.reset();
          return Tick(seek_back);
        }
        if (frame == kFrames - 1) stop.request_stop();
        return std::nullopt;
      }, stop.get_token());
  Check(seeks == 2 && stats.clocked_frames > 1 && stats.audio_underruns == 0,
        "Clocked template mask preview did not resume both seek directions");
  std::cout << label << ": " << stats.clocked_frames << " clocked frames, two seeks, no PCM underruns, " << missed
            << " missed frame positions, " << stats.late_video_frames << " late compositions, maximum lag "
            << stats.maximum_video_lag_ms << " ms, maximum frame interval " << maximum_frame_gap_ms << " ms\n";
  Check(missed == 0 && stats.late_video_frames == 0, "Clocked template mask preview skipped a frame");
  for (const auto& [frame, texture] : snapshots) {
    Check(ReadPreviewPixels(texture) == preview.at(reverse_order ? kFrames - 1 - frame : frame),
          "Clocked preview diverged from the template mask reference pixels");
  }
}

void CheckExport(const std::shared_ptr<const ProjectSnapshot>& project, const File& encoded,
                 const MovieCodecProviders& codecs, MovieRenderer& renderer, const MovieAsset& asset,
                 const std::vector<Bytes>& preview, const std::function<void(const TimelineFrame&, int)>& check_sample = {}) {
  FileExportSettings settings;
  settings.policy = CodecPolicy::SoftwareOnly;
  settings.video = PreviewVideoConfig(kCanvas);
  settings.video.frame_rate_numerator = kTimelinePreviewRate;
  settings.video.input.transfer = ColorTransfer::Unknown;
  int written = 0;
  ExportTimeline(project, encoded, codecs, settings, [&](const TimelineFrame& frame, const auto& config, auto duration) {
    if (check_sample) check_sample(frame, written);
    const auto surface = renderer.Draw(frame);
    Check(frame.pts == Tick(written) && Pixels(surface) == preview.at(written), "Mask export pixels diverged from preview");
    ++written;
    return renderer.EncoderFrame(surface, config, frame.pts, duration);
  });
  Check(written == kFrames, "Export omitted a template mask frame");
  MovieReader reimported(ImportMovie(encoded), codecs, CodecPolicy::SoftwareOnly);
  double worst = 0;
  for (int index = 0; index < kFrames; ++index) {
    const auto picture = reimported.ReadVideo();
    Check(picture && picture->pts == Tick(index) && picture->duration == Tick(1), "Reimport changed masked video timing");
    const auto pixels = Pixels(renderer.Draw(FlatFrame(*picture)));
    double error = 0;
    for (std::size_t byte = 0; byte < pixels.size(); ++byte)
      error += std::abs(std::to_integer<int>(pixels[byte]) - std::to_integer<int>(preview[index][byte]));
    worst = std::max(worst, error / pixels.size());
  }
  std::int64_t pcm = 0;
  while (const auto sound = reimported.ReadAudio()) {
    Check(sound->pts == MediaTimestamp{pcm, asset.pcm.sample_rate}, "Reimport lost PCM continuity");
    pcm += sound->sample_count;
  }
  Check(!reimported.ReadVideo() && worst < 4 && pcm == Tick(kFrames).Rescale(asset.pcm.sample_rate)->ticks,
        "Reimport changed masked pixels or the project duration");
  std::cout << written << " exact pre-encode frames, exact reimported PTS, " << pcm << " PCM samples, worst mean error "
            << worst << " / 255\n";
}

std::array<int, kMaskStarts.size()> ExpectedMaskFrames(int index) {
  return {std::min(2, index * 2 / 5), std::max(0, index - kInsetStart) * 2 / 5 % 3,
          std::max(0, index - kSecondInsetStart) * 2 / 5 % 3};
}

void CheckAnimatedSample(const TimelineFrame& frame, const ProjectSnapshot& project, int index) {
  const auto active = static_cast<std::size_t>(std::ranges::upper_bound(kMaskStarts, index) - kMaskStarts.begin());
  Check(frame.pts == Tick(index) && frame.animated_masks.size() == active && frame.plan->draws.size() == active &&
            frame.analysis_masks.empty() && frame.static_masks.empty(), "Expected one sample per active clip of the shared sequence");
  const auto expected = ExpectedMaskFrames(index);
  for (std::size_t layer = 0; layer < active; ++layer) {
    const auto& clip = project.tracks[layer].clips.front();
    const auto& draw = frame.plan->draws[layer];
    Check(std::get<VideoFrame>(frame.inputs.at(draw.input_slot)).pts == Tick(SourceFrame(clip, index)),
          "Wrong source video PTS for the clip mask sequence");
    Check(draw.source_sampling && draw.source_sampling->background, "Missing animated mask on the clip draw");
    const auto& sample = frame.animated_masks.at(draw.source_sampling->background->mask_slot);
    const auto local = index - kMaskStarts[layer];
    Check(sample.resource == PreviewAnimatedMaskResource() && sample.offset == Tick(local) &&
              sample.playback == (layer == 0 ? MaskPlayback::HoldLast : MaskPlayback::Loop) &&
              sample.FrameIndex(kMaskInterval, kMaskFrames) == static_cast<std::uint32_t>(expected[layer]),
          "Mask sequence did not select the expected frame from clip-local project time");
  }
}

void WaitForPreroll(TimelineSession& timeline, TimeRange lookahead) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!timeline.PrerollVideoEntries(lookahead)) {
    Check(std::chrono::steady_clock::now() < deadline, "Video entry preparation did not complete");
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

void CheckReverseEntry(const std::shared_ptr<const ProjectSnapshot>& project, const MovieCodecProviders& codecs,
                       MovieRenderer& renderer, CodecPolicy policy) {
  verification::DecoderUsage usage;
  const MovieCodecProviders observed{
      std::make_shared<verification::ObservedCodecProvider>(codecs.platform, &usage),
      std::make_shared<verification::ObservedCodecProvider>(codecs.software, &usage)};
  std::optional<CodecFallback> fallback;
  TimelineSession timeline(project, observed, policy, [&](const auto& event) { fallback = event; });
  const auto sample = [&](int index) {
    auto frame = timeline.SampleVideo(Tick(index));
    CheckAnimatedSample(frame, *project, index);
    return frame;
  };
  const auto warm = [&] {
    const TimeRange lookahead{Tick(0), Tick(kSecondInsetStart + 1)};
    Check(!timeline.PrerollVideoEntries(lookahead), "Preroll did not enqueue the cold inset");
    WaitForPreroll(timeline, lookahead);
  };
  Check(!timeline.PrerollVideoEntries({Tick(0), Tick(kInsetStart)}) && usage.video.opened == 0,
        "Preroll included the exclusive end of its lookahead");
  warm();
  const auto peak = usage.video.peak;
  Check(usage.video.opened == 2 && usage.video.active == 0 && peak <= 2 &&
            (policy != CodecPolicy::SoftwareOnly || peak == 2), "The two decoders did not stay within the preroll limit");
  sample(0);
  sample(kInsetStart - 1);
  const auto entry = Pixels(renderer.Draw(sample(kInsetStart)));
  const auto both = sample(kSecondInsetStart);
  const auto platform_entry = std::get<VideoFrame>(both.inputs.at(both.plan->draws[2].input_slot)).Storage() == VideoStorage::Platform;
  const auto second_entry = Pixels(renderer.Draw(both));
  Check(usage.video.opened == 3, "An inset reopened its decoder on entry");

  const TimeRange near_seek{Tick(kNearSeekBack), Tick(kSecondInsetStart + 1)};
  timeline.Seek(near_seek.start, near_seek.end);
  sample(kNearSeekBack);
  Check(timeline.PrerollVideoEntries(near_seek), "The seek did not retain both entry windows");
  sample(kInsetStart - 1);
  Check(Pixels(renderer.Draw(sample(kInsetStart))) == entry, "The inset pixels changed after seeking before entry");
  Check(Pixels(renderer.Draw(sample(kSecondInsetStart))) == second_entry, "The second inset pixels changed after seeking back");
  Check(usage.video.opened == 4 && usage.audio.opened == 0, "The backward seek did not reuse both inset windows");

  if (platform_entry) {
    const auto expected = Tick(SourceFrame(project->tracks[2].clips.front(), kSecondInsetStart));
    bool restored = false;
    timeline.SampleVideo(Tick(kSecondInsetStart), PixelFormat::Bgra8Unorm, {}, [&](const VideoFrame& picture) {
      if (picture.pts == expected) {
        if (picture.Storage() == VideoStorage::Platform) throw PlatformVideoImportError("Injected inset import failure");
        restored = true;
      }
      return renderer.ImportFrame(picture);
    });
    Check(restored && fallback && fallback->resume_at == expected && usage.video.opened == 5,
          "Detached platform picture did not recover in software at its exact source PTS");
  }

  timeline.ApplyProject(project);
  warm();
  Check(usage.video.opened == (platform_entry ? 7U : 6U), "Replacing the revision kept obsolete inset windows");
  std::cout << (platform_entry ? "Platform" : "CPU") << " reverse insets prepared with peak " << peak
            << " background decoders and reused on entry and after seeking back before entry\n";
}

void CheckForwardEntry(const std::shared_ptr<const ProjectSnapshot>& project, const MovieCodecProviders& codecs,
                       MovieRenderer& renderer) {
  verification::DecoderUsage usage;
  const MovieCodecProviders observed{std::make_shared<verification::ObservedCodecProvider>(codecs.platform, &usage),
                                     std::make_shared<verification::ObservedCodecProvider>(codecs.software, &usage)};
  std::optional<CodecFallback> fallback;
  TimelineSession timeline(project, observed, CodecPolicy::Auto, [&](const auto& event) { fallback = event; });
  const TimeRange upcoming{Tick(0), Tick(kSecondInsetStart + 1)};
  Check(!timeline.PrerollVideoEntries(upcoming), "Forward entry preparation should start asynchronously");
  WaitForPreroll(timeline, upcoming);
  Check(usage.video.opened == 2 && usage.video.active == 2, "Background preparation did not retain both forward cursors");
  const auto sample = [&](int index) {
    const auto frame = timeline.SampleVideo(Tick(index));
    CheckAnimatedSample(frame, *project, index);
    return frame;
  };
  const auto before = Pixels(renderer.Draw(sample(kSecondInsetStart - 1)));
  const auto both = sample(kSecondInsetStart);
  const auto& second = std::get<VideoFrame>(both.inputs.at(both.plan->draws[2].input_slot));
  const auto after = Pixels(renderer.Draw(both));
  Check(usage.video.opened == 3, "A forward inset reopened at entry");

  const TimeRange near_seek{Tick(kInsetStart - 1), upcoming.end};
  timeline.Seek(near_seek.start, near_seek.end);
  Check(timeline.PrerollVideoEntries(near_seek), "A seek before entry did not retain both forward cursors");
  Check(Pixels(renderer.Draw(sample(kSecondInsetStart - 1))) == before &&
            Pixels(renderer.Draw(sample(kSecondInsetStart))) == after, "Forward inset pixels changed after seeking back");
  const auto opened = usage.video.opened;
  Check(opened == 4, "Forward cache reuse reopened an inset decoder");

  if (second.Storage() == VideoStorage::Platform) {
    bool restored = false;
    timeline.SampleVideo(Tick(kSecondInsetStart), PixelFormat::Bgra8Unorm, {}, [&](const VideoFrame& picture) {
      if (picture.pts == second.pts) {
        if (picture.Storage() == VideoStorage::Platform) throw PlatformVideoImportError("Injected forward inset import failure");
        restored = true;
      }
      return renderer.ImportFrame(picture);
    });
    Check(restored && fallback && fallback->resume_at == second.pts && usage.video.opened == opened + 1,
          "Handed-off forward decoder did not recover at the exact source PTS");
  }
  std::cout << "Prepared forward insets reused both cursors on entry and after seeking back before entry\n";
}

void CheckFarSeekReleasesPreroll(const std::shared_ptr<const ProjectSnapshot>& project, const MovieCodecProviders& codecs) {
  verification::DecoderUsage usage;
  const MovieCodecProviders observed{std::make_shared<verification::ObservedCodecProvider>(codecs.platform, &usage),
                                     std::make_shared<verification::ObservedCodecProvider>(codecs.software, &usage)};
  auto shifted = std::make_shared<ProjectSnapshot>(*project);
  constexpr int shift = 240;
  for (auto& track : shifted->tracks) {
    for (auto& clip : track.clips) {
      auto& range = clip.placement.timeline;
      range.start = avkit::detail::Add(range.start, Tick(shift));
      range.end = avkit::detail::Add(range.end, Tick(shift));
    }
  }
  TimelineSession timeline(shifted, observed);
  const TimeRange upcoming{Tick(shift), Tick(shift + kSecondInsetStart + 1)};
  const TimeRange early{Tick(0), {2, 5}};
  WaitForPreroll(timeline, upcoming);
  Check(usage.video.active == 2, "Far seek fixture needs two live forward cursors");
  timeline.SampleVideo(Tick(shift + kSecondInsetStart));
  timeline.Seek(early.start, early.end);
  Check(usage.video.active == 0, "A far seek retained video decoders beyond its lookahead");

  WaitForPreroll(timeline, upcoming);
  Check(!timeline.PrerollVideoEntries(early) && usage.video.active == 0,
        "Shrinking the lookahead to an empty interval retained prepared decoders");
  std::cout << "Far seek and empty lookahead each released both forward cursors\n";
}

void CheckParallelPrerollFallback(const std::shared_ptr<const ProjectSnapshot>& project, const MovieCodecProviders& codecs) {
  verification::DecoderUsage usage;
  std::atomic_uint first_reads{};
  const MovieCodecProviders observed{std::make_shared<verification::ObservedCodecProvider>(codecs.platform, &usage,
      verification::VideoFaults{.decode_after = 1}, [&](auto&) {
        if (first_reads.fetch_add(1) >= 2) return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (first_reads.load() < 2) {
          Check(std::chrono::steady_clock::now() < deadline, "Both platform decode jobs must reach the injected fault");
          std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
      }), std::make_shared<verification::ObservedCodecProvider>(codecs.software, &usage)};
  const auto sampling_thread = std::this_thread::get_id();
  std::vector<CodecFallback> fallbacks;
  TimelineSession timeline(project, observed, CodecPolicy::Auto, [&](const auto& event) {
    Check(std::this_thread::get_id() == sampling_thread, "A worker delivered a codec fallback callback directly");
    fallbacks.push_back(event);
  });
  WaitForPreroll(timeline, {Tick(0), Tick(kSecondInsetStart + 1)});
  Check(usage.video.opened == 3 && usage.video.active == 2 && fallbacks.size() == 1,
        "Parallel preparation must inject exactly one fault and keep both forward cursors");
  const auto& event = fallbacks.front();
  Check(event.operation == CodecOperation::VideoDecode && event.cause.code == CodecErrorCode::ResourceExhausted &&
            event.from == codecs.platform->Name() && event.to == codecs.software->Name() && event.resume_at,
        "Unexpected parallel decode fallback");
  const auto frame = timeline.SampleVideo(Tick(kSecondInsetStart));
  CheckAnimatedSample(frame, *project, kSecondInsetStart);
  const auto storage = [&](std::size_t layer) {
    return std::get<VideoFrame>(frame.inputs.at(frame.plan->draws[layer].input_slot)).Storage();
  };
  Check(storage(1) != storage(2) && fallbacks.size() == 1 && usage.video.opened == 4,
        "The parallel fault changed the other inset or reopened a prepared cursor");
  std::cout << "Parallel forward preparation recovered one decoder and kept the other on the platform provider\n";
}

} // namespace

void VerifyTimelineTemplateMasks(std::string_view document, std::string_view directory) {
  @autoreleasepool {
    const auto source = Project::Open(File(document)).Snapshot();
    const File output(directory), saved(output, "static-masks.avkit"), encoded(output, "static-masks.mp4");
    Check(output.CreateDirectories() && !saved.Exists() && !encoded.Exists(), "Use a fresh verification directory");
    CheckDanceTrackProject(*source);

    const auto& [common, replacement] = PreviewStaticMaskResources();
    auto recipe = MakeRecipe(*source, "dance-static-masks");
    for (auto& track : recipe.tracks) {
      if (track.kind == TrackKind::Video) track.clips.front().background->static_mask = StaticMask{common};
    }
    auto project = Assemble(*source, recipe);
    const auto initial = project.Snapshot();
    const auto& main = initial->tracks[0].clips.front();
    const auto& inset = initial->tracks[1].clips.front();
    Check(main.id != inset.id && main.background->static_mask->resource == common &&
              inset.background->static_mask->resource == common, "Template did not bind both independent clips to one resource");
    const auto codecs = TimelineProviders();
    MovieRenderer renderer(kCanvas);
    const auto asset = ImportMovie(source->assets.front().source.file);
    const auto raw = SourcePixels(asset, codecs, renderer, *initial);

    TimelineSession timeline(initial, codecs, CodecPolicy::SoftwareOnly);
    constexpr int edit_frame = 14;
    const auto initial_frame = timeline.SampleVideo(Tick(edit_frame));
    Check(initial_frame.static_masks == std::vector{common} && initial_frame.analysis_masks.empty(),
          "Two clips sharing a static mask should bind one data slot");
    const auto before = Pixels(renderer.Draw(initial_frame));
    CheckMaskPixels(before, raw, std::array{&main, &inset}, std::array{0, 0}, edit_frame);
    const auto edited = project.Edit([&](ProjectEdit& edit) { edit.SetBackgroundMaskResource(inset.id, replacement); });
    auto expected = initial->tracks;
    expected[1].clips.front().background->static_mask->resource = replacement;
    Check(edited->tracks == expected, "Replacing the inset mask changed another project property");
    timeline.ApplyProject(edited);
    const auto edited_frame = timeline.SampleVideo(Tick(edit_frame));
    Check(edited_frame.static_masks == std::vector{common, replacement}, "Replacement did not get an independent data slot");
    const auto after = Pixels(renderer.Draw(edited_frame));
    CheckReplacementPixels(before, after, inset.destination);
    CheckMaskPixels(after, raw, std::array{&main, &inset}, std::array{0, 1}, edit_frame);

    Check(project.Undo() && project.Snapshot()->tracks == initial->tracks, "Undo did not restore both bindings");
    timeline.ApplyProject(project.Snapshot());
    Check(Pixels(renderer.Draw(timeline.SampleVideo(Tick(edit_frame)))) == before, "Undo did not restore the pixels");
    Check(project.Redo() && project.Snapshot()->tracks == expected, "Redo changed the replacement or clip IDs");
    timeline.ApplyProject(project.Snapshot());
    Check(Pixels(renderer.Draw(timeline.SampleVideo(Tick(edit_frame)))) == after, "Redo did not restore the replacement pixels");
    project.Save(saved);
    const auto reopened = Project::Open(saved).Snapshot();
    Check(reopened->tracks == expected, "The static mask bindings did not survive reopening");
    timeline.ApplyProject(reopened);

    std::vector<Bytes> preview;
    for (int index = 0; index < kFrames; ++index) {
      const auto frame = timeline.SampleVideo(Tick(index));
      Check(std::get<VideoFrame>(frame.inputs.at(0)).pts == Tick(SourceFrame(main, index)) &&
                std::get<VideoFrame>(frame.inputs.at(1)).pts == Tick(SourceFrame(inset, index)), "Wrong source frame for static masking");
      preview.push_back(Pixels(renderer.Draw(frame)));
      CheckMaskPixels(preview.back(), raw, std::array{&main, &inset}, std::array{0, 1}, index);
    }
    Check(preview.at(edit_frame) == after, "The reopened replacement changed the pixels");
    for (const int index : {3, 32, 7, 21, 12, 36, 0, kFrames - 1}) {
      timeline.Seek(Tick(index));
      Check(Pixels(renderer.Draw(timeline.SampleVideo(Tick(index)))) == preview[index], "Seek changed the sampled static mask");
    }
    auto flipped = std::make_shared<ProjectSnapshot>(*reopened);
    for (auto& track : flipped->tracks) {
      if (track.kind != TrackKind::Video) continue;
      auto& reverse = track.clips.front().placement.source.reverse;
      reverse = !reverse;
    }
    TimelineSession backwards(flipped, codecs, CodecPolicy::SoftwareOnly);
    for (int index = 0; index < kFrames; ++index)
      Check(Pixels(renderer.Draw(backwards.SampleVideo(Tick(index)))) == preview[kFrames - 1 - index],
            "Reversing the video tracks changed the static mask at the same source PTS");

    CheckClocked(timeline, renderer, preview, "Forward static masks");
    CheckClocked(backwards, renderer, preview, "Reversed static masks", true);
    CheckExport(reopened, encoded, codecs, renderer, asset, preview);
    std::cout << "Shared PNG, isolated inset replacement, undo, redo, and reopening verified\n";
  }
}

void VerifyTimelineAnimatedTemplateMasks(std::string_view document, std::string_view directory) {
  @autoreleasepool {
    const auto source = Project::Open(File(document)).Snapshot();
    const File output(directory), saved(output, "animated-masks.avkit"), encoded(output, "animated-masks.mp4"),
        flipped_encoded(output, "flipped-animated-masks.mp4");
    Check(output.CreateDirectories() && !saved.Exists() && !encoded.Exists() && !flipped_encoded.Exists(),
          "Use a fresh verification directory");
    CheckDanceTrackProject(*source);
    const auto& resource = PreviewAnimatedMaskResource();
    auto recipe = MakeRecipe(*source, "dance-animated-masks");
    auto& main_clip = recipe.tracks[0].clips.front();
    auto& inset_clip = recipe.tracks[1].clips.front();
    main_clip.background->static_mask.reset();
    inset_clip.background->static_mask.reset();
    main_clip.background->animated_mask = AnimatedMask{resource, MaskPlayback::HoldLast};
    inset_clip.background->animated_mask = AnimatedMask{resource, MaskPlayback::Loop};
    inset_clip.timeline.start = Tick(kInsetStart);
    inset_clip.source.start = avkit::detail::Add(inset_clip.source.start, Tick(kInsetStart));
    auto second_inset = inset_clip;
    second_inset.timeline.start = Tick(kSecondInsetStart);
    second_inset.source.start = avkit::detail::Add(second_inset.source.start, Tick(kSecondInsetStart - kInsetStart + 6));
    second_inset.source.end = avkit::detail::Add(second_inset.source.end, Tick(6));
    second_inset.destination = {0.03F, 0.08F, 0.36F, 0.38F};
    second_inset.background->color = {0.32F, 0.19F, 0.54F, 1.F};
    recipe.tracks.insert(recipe.tracks.begin() + 2, {TrackKind::Video, {std::move(second_inset)}});
    auto project = Assemble(*source, recipe);
    project.Save(saved);
    const auto reopened = Project::Open(saved).Snapshot();
    Check(reopened->tracks == project.Snapshot()->tracks, "The sequence reference or playback mode changed after reopening");

    AnimatedMaskSample boundary{resource, {4, 48}, MaskPlayback::HoldLast};
    Check(boundary.FrameIndex(kMaskInterval, kMaskFrames) == 0, "A fractional offset advanced the mask too early");
    boundary.offset = {5, 48};
    Check(boundary.FrameIndex(kMaskInterval, kMaskFrames) == 1, "Mask boundary did not select the next frame");
    boundary.offset = {15, 48};
    Check(boundary.FrameIndex(kMaskInterval, kMaskFrames) == 2, "HoldLast did not keep the final frame at sequence end");
    boundary.playback = MaskPlayback::Loop;
    Check(boundary.FrameIndex(kMaskInterval, kMaskFrames) == 0, "Loop did not wrap at the exact sequence end");

    const auto codecs = TimelineProviders();
    const auto asset = ImportMovie(source->assets.front().source.file);
    MovieRenderer renderer(kCanvas);
    const auto raw = SourcePixels(asset, codecs, renderer, *reopened);
    CheckReverseEntry(reopened, codecs, renderer, CodecPolicy::SoftwareOnly);
    CheckReverseEntry(reopened, codecs, renderer, CodecPolicy::Auto);
    TimelineSession session(reopened, codecs, CodecPolicy::SoftwareOnly);
    const auto sequential = [&](TimelineSession& timeline, const ProjectSnapshot& snapshot) {
      const std::array clips{&snapshot.tracks[0].clips.front(), &snapshot.tracks[1].clips.front(), &snapshot.tracks[2].clips.front()};
      std::vector<Bytes> pixels;
      pixels.reserve(kFrames);
      for (int index = 0; index < kFrames; ++index) {
        const auto frame = timeline.SampleVideo(Tick(index));
        CheckAnimatedSample(frame, snapshot, index);
        pixels.push_back(Pixels(renderer.Draw(frame)));
        CheckMaskPixels(pixels.back(), raw, clips, ExpectedMaskFrames(index), index);
      }
      return pixels;
    };
    const auto preview = sequential(session, *reopened);
    const auto compare_at = [&](int index) {
      const auto frame = session.SampleVideo(Tick(index));
      CheckAnimatedSample(frame, *reopened, index);
      Check(Pixels(renderer.Draw(frame)) == preview[index], "Sampling out of order changed the animated mask pixels");
    };
    for (const auto index : {4, 8, 5, 13, 12, 36, 7, kFrames - 1, 0}) {
      session.Seek(Tick(index));
      compare_at(index);
    }
    for (int index = kFrames - 1; index >= 0; --index) compare_at(index);

    auto flipped = std::make_shared<ProjectSnapshot>(*reopened);
    for (auto& track : flipped->tracks) {
      if (track.kind != TrackKind::Video) continue;
      auto& reverse = track.clips.front().placement.source.reverse;
      reverse = !reverse;
    }
    TimelineSession backwards(flipped, codecs, CodecPolicy::SoftwareOnly);
    const auto reversed_preview = sequential(backwards, *flipped);
    CheckForwardEntry(flipped, codecs, renderer);
    CheckFarSeekReleasesPreroll(flipped, codecs);
    CheckParallelPrerollFallback(flipped, codecs);
    const auto sampling_thread = std::this_thread::get_id();
    const auto first_entry_pts = Tick(SourceFrame(reopened->tracks[1].clips.front(), kInsetStart));
    const auto second_entry = SourceFrame(reopened->tracks[2].clips.front(), kSecondInsetStart);
    const auto second_entry_pts = Tick(second_entry);
    const auto stalled_picture_pts = Tick(second_entry - 2);
    verification::DecoderUsage playback_usage;
    std::atomic_uint first_entry_reads{}, second_entry_reads{}, stalled_reads{};
    const MovieCodecProviders clocked_codecs{codecs.platform,
        std::make_shared<verification::ObservedCodecProvider>(codecs.software, &playback_usage, verification::VideoFaults{},
            [&](const auto& picture) {
              if (std::this_thread::get_id() != sampling_thread) {
                if (picture.pts == first_entry_pts) ++first_entry_reads;
                if (picture.pts == second_entry_pts) ++second_entry_reads;
                if (picture.pts == stalled_picture_pts) {
                  ++stalled_reads;
                  std::this_thread::sleep_for(std::chrono::milliseconds{190});
                }
              }
            })};
    {
      TimelineSession clocked(reopened, clocked_codecs, CodecPolicy::SoftwareOnly);
      CheckClocked(clocked, renderer, preview, "Forward animated masks", false, kSecondInsetStart + 2, kNearSeekBack);
    }
    std::cout << "Clocked playback opened " << playback_usage.video.opened << " video decoders across three epochs\n";
    Check(stalled_reads.load() == 1, "Clocked playback did not exercise the second inset decoder stall once");
    Check(first_entry_reads.load() >= 1 && second_entry_reads.load() == 1 && playback_usage.video.opened == 5,
          "Clocked playback rebuilt an inset on the sampling sequence or after a seek");
    verification::DecoderUsage flipped_usage;
    const MovieCodecProviders flipped_codecs{codecs.platform,
        std::make_shared<verification::ObservedCodecProvider>(codecs.software, &flipped_usage)};
    {
      TimelineSession flipped_clocked(flipped, flipped_codecs, CodecPolicy::SoftwareOnly);
      CheckClocked(flipped_clocked, renderer, reversed_preview, "Reversed video with clip-local animated masks", false,
                   kSecondInsetStart + 2, kNearSeekBack);
    }
    std::cout << "Reversed-source preview opened " << flipped_usage.video.opened << " video decoders across three epochs\n";
    Check(flipped_usage.video.opened == 3 && flipped_usage.video.active == 0,
          "A reversed-source preview decoder reopened between entries and seeks");
    CheckExport(reopened, encoded, codecs, renderer, asset, preview,
                [&](const TimelineFrame& frame, int index) { CheckAnimatedSample(frame, *reopened, index); });
    CheckExport(flipped, flipped_encoded, codecs, renderer, asset, reversed_preview,
                [&](const TimelineFrame& frame, int index) { CheckAnimatedSample(frame, *flipped, index); });

    std::cout << "Shared three-frame PNG sequence, interval 5/48 s. Main holds, insets loop from project frames " << kInsetStart
              << " and " << kSecondInsetStart
              << ". Playback, both seek directions, descending timeline samples, and export matched the expected frame and pixels.\n";
  }
}

} // namespace avkit_preview
