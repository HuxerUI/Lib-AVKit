#include "timeline_mask_verify.h"

#include "bgfx_metal_texture.h"
#include "metal_verify.h"
#include "movie_renderer.h"
#include "timeline_verify.h"
#include "tracking_fixture.h"

#include "timeline_tracking_verify.h"
#include "codec_verification.h"

#include <future>
#include <map>
#include <huxerui/avkit/ncnn.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>

#include <huxerui/avkit/bgfx.h>
#include <huxerui/macos/external_texture.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::analysis;
using namespace huxerui::avkit::render;
using namespace huxerui::media;
using huxerui::Point;
using huxerui::Rect;
using huxerui::macos::MetalTexture;
using verification::Interval;
using verification::Tick;
using verification::kCanvas;

constexpr int kFrames = 16, kZero = 2, kSkip = 4, kLate = 6;
constexpr Rect kPip{0.5F, 0.25F, 0.5F, 0.5F};
constexpr BackgroundReplacement kMain{Color::Rgb(180, 55, 120), {0.125F, 0.125F, 0.75F, 0.75F}};
constexpr BackgroundReplacement kInset{Color::Rgb(40, 105, 214), {0.25F, 0.25F, 0.5F, 0.5F}, true};
constexpr Rect kValid{0.125F, 0.125F, 0.75F, 0.75F};

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int Index(MediaTimestamp at) {
  const auto scaled = at.Rescale(24);
  Check(scaled && *scaled == at, "Expected an exact fixture frame");
  return static_cast<int>(scaled->ticks);
}

bool Alternate(ClipId clip) { return (clip.value & 2) == 0; }

int Coverage(int x, int y, bool alternate, int frame) {
  if (frame == kZero) return 0;
  const int column = alternate ? 7 - x : x;
  const int row = alternate ? 7 - y : y;
  if (column <= 3 && row >= 2 && row <= 5) return 255;
  if (column == 4 && row == 2 + frame % 2) return 96;
  return 0;
}

AnalysisTexture MaskFor(FrameKey key, bool poison = false) {
  constexpr std::size_t stride = 12, extent = 8;
  const auto alternate = Alternate({key.source});
  const auto index = Index(key.pts);
  auto bytes = std::make_shared<Bytes>(stride * extent, std::byte{255});
  for (int y = 0; y < int(extent); ++y) {
    for (int x = 0; x < int(extent); ++x) {
      const auto row = alternate ? extent - 1 - y : y;
      // Padding and the invalid border stay white to expose clamping or stride mistakes.
      if (x >= 1 && x <= 6 && y >= 1 && y <= 6)
        (*bytes)[row * stride + x] = static_cast<std::byte>(poison ? 255 : Coverage(x, y, alternate, index));
    }
  }
  const auto surface = RetainBgfxPixels({{extent, extent}, PixelFormat::R8Unorm}, BufferReference(*bytes, bytes), stride,
                                       alternate ? RowOrigin::BottomLeft : RowOrigin::TopLeft);
  return {surface, {2, 0, 0, 2, -0.5F, -0.5F}, kValid};
}

template<class Read>
float Bilinear(float x, float y, int first, int last, const Read& read) {
  x = std::clamp(x, float(first), float(last));
  y = std::clamp(y, float(first), float(last));
  const int x0 = int(x), y0 = int(y);
  const int x1 = std::min(x0 + 1, last), y1 = std::min(y0 + 1, last);
  return std::lerp(std::lerp(float(read(x0, y0)), float(read(x1, y0)), x - x0),
                   std::lerp(float(read(x0, y1)), float(read(x1, y1)), x - x0), y - y0);
}

struct HeldMask {
  MaskCompletion result;
  AnalysisSession::MaskCallback callback;
  std::stop_token stop;
};

struct MaskState {
  std::optional<ClipId> hold;
  std::optional<HeldMask> late;
  std::size_t late_deliveries{};
  std::vector<FrameKey> requests;
};

class MockMaskSession final : public AnalysisSession {
public:
  explicit MockMaskSession(std::shared_ptr<MaskState> state) : state_(std::move(state)) {}

  void SubmitMask(AnalysisJob job, std::stop_token stop, MaskCallback complete) override {
    using R = decltype(MaskCompletion::result);
    Check(job.frames.size() == 1 && job.frames.front().key == job.target &&
              job.frames.front().image.pts == job.target.pts && job.regions.empty(),
          "A mask job lost the source frame or its identity");
    state_->requests.push_back(job.target);
    if (state_->hold == ClipId{job.target.source} && Index(job.target.pts) == kLate) {
      state_->hold.reset();
      state_->late = HeldMask{{job.id, job.target, R::Success(MaskFor(job.target, true))}, std::move(complete), stop};
      return;
    }
    if (state_->late && state_->late->stop.stop_requested() &&
        state_->late->result.target.source == job.target.source && state_->late->result.target.pts == job.target.pts) {
      auto held = std::exchange(state_->late, std::nullopt);
      held->callback(held->result);
      // Even with the current job ID, an old generation must never complete this job.
      held->result.job_id = job.id;
      complete(std::move(held->result));
      ++state_->late_deliveries;
    }
    auto wrong = job.target;
    ++wrong.source;
    complete({job.id, wrong, R::Success(MaskFor(job.target, true))});
    auto mask = Index(job.target.pts) == kSkip ? std::nullopt : std::optional{MaskFor(job.target)};
    complete({job.id, job.target, R::Success(std::move(mask))});
  }

private:
  std::shared_ptr<MaskState> state_;
};

class MockMaskProvider final : public AnalysisProvider {
public:
  explicit MockMaskProvider(std::shared_ptr<MaskState> state) : state_(std::move(state)) {}

  Result<AnalysisWindow, std::string> Probe(const AnalysisConfig& config) const override {
    using R = Result<AnalysisWindow, std::string>;
    if (config.capability != AnalysisCapability::PortraitMask || config.uses_roi || !config.format.Validate())
      return R::Failure("Unsupported mock portrait mask input");
    return R::Success({});
  }

  std::unique_ptr<AnalysisSession> Open(const AnalysisConfig&) override {
    return std::make_unique<MockMaskSession>(state_);
  }

private:
  std::shared_ptr<MaskState> state_;
};

Bytes Pixels(const ColorSurface& surface) {
  return ReadPreviewPixels(avkit::macos::GetMetalTexture(surface));
}

using Pictures = std::array<Bytes, kFrames>;

void CheckPixels(const Bytes& actual, const Pictures& source, int main, int inset, ClipId first, ClipId second,
                 bool timeout = false, PixelSize canvas = kCanvas) {
  std::size_t checked = 0, mixed = 0;
  for (unsigned y = 0; y < canvas.height; ++y) {
    for (unsigned x = 0; x < canvas.width; ++x) {
      const Point point{(x + 0.5F) / canvas.width, (y + 0.5F) / canvas.height};
      const bool pip = kPip.Contains(point);
      const auto& effect = pip ? kInset : kMain;
      const auto frame = pip ? inset : main;
      const auto u = pip ? (point.x - kPip.x) / kPip.width : point.x;
      const auto v = pip ? (point.y - kPip.y) / kPip.height : point.y;
      const auto& crop = effect.source_crop;
      const auto sx = crop.x + (effect.mirrored ? 1 - u : u) * crop.width;
      const auto sy = crop.y + v * crop.height;
      const auto mx = (2 * sx - 0.5F) * 8, my = (2 * sy - 0.5F) * 8;
      // Texel-center probes avoid exact sampler boundaries.
      if (std::abs(mx - std::round(mx)) < 0.0001F || std::abs(my - std::round(my)) < 0.0001F) continue;
      const float weight = frame == kSkip || (timeout && !pip) ? 255 :
          mx >= 1 && mx < 7 && my >= 1 && my < 7 ? Bilinear(mx - 0.5F, my - 0.5F, 1, 6, [&](int col, int row) {
            return Coverage(col, row, Alternate(pip ? second : first), frame);
          }) : 0;
      const auto start = 4 * (unsigned(sy * kCanvas.height) * kCanvas.width + unsigned(sx * kCanvas.width));
      const auto dest = 4 * (y * canvas.width + x);
      const std::array background{effect.color.red, effect.color.green, effect.color.blue};
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto foreground = std::to_integer<int>(source[frame][start + channel]);
        const auto expected = std::lround((foreground * weight + background[channel] * 255 * (255 - weight)) / 255);
        Check(std::abs(std::to_integer<int>(actual[dest + channel]) - expected) <= 2,
              "Source pixels or R8 coverage moved away from the cropped and mirrored layer");
      }
      Check(actual[dest + 3] == std::byte{255}, "The result lost opaque alpha");
      ++checked;
      mixed += weight > 0 && weight < 255;
    }
  }
  Check(checked > 50000 && (mixed || timeout || main == kZero || inset == kZero || main == kSkip || inset == kSkip),
        "The pixel oracle did not cover partial foreground coverage");
}

void VerifyImportedMaskAlignment(const ProjectSnapshot& source, const MovieCodecProviders& codecs,
                                 const std::shared_ptr<AnalysisProvider>& provider, const Pictures& raw,
                                 ClipId first, ClipId second) {
  constexpr PixelSize canvas{320, 240};
  auto project = std::make_shared<ProjectSnapshot>(source);
  project->canvas = canvas;
  TimelineSession session(project, codecs, CodecPolicy::SoftwareOnly, {}, provider);
  MovieRenderer renderer(canvas);
  constexpr int sample = 0;
  const auto check = [&](const ImportVideoFrame& import) {
    const auto frame = session.SampleVideo(Tick(sample), PixelFormat::Bgra8Unorm, {}, import);
    CheckPixels(Pixels(renderer.Draw(frame)), raw, sample, kFrames - 1 - sample, first, second, false, canvas);
  };
  check({});
  check([&](const VideoFrame& frame) { return renderer.ImportFrame(frame); });
  for (const unsigned padding : {0U, 16U}) {
    check([&](const VideoFrame& frame) -> ImportedVideoFrame {
      Check(frame.format.visible.width == kCanvas.width && frame.format.visible.height == kCanvas.height,
            "Unexpected decoded extent in the import fixture");
      const PixelSize extent{kCanvas.width + 2 * padding, kCanvas.height + 2 * padding};
      const auto stride = std::size_t(extent.width) * 4;
      const auto pixels = std::make_shared<Bytes>(stride * extent.height);
      const auto& source_pixels = raw.at(Index(frame.pts));
      constexpr auto source_stride = std::size_t(kCanvas.width) * 4;
      for (unsigned row = 0; row < kCanvas.height; ++row) {
        const auto offset = (std::size_t(row) + padding) * stride + 4 * padding;
        std::copy_n(source_pixels.data() + row * source_stride, source_stride, pixels->data() + offset);
      }
      return {{RetainImagePixels({extent, PixelFormat::Rgba8Unorm}, {BufferReference(*pixels, pixels), stride}),
               AlphaMode::Opaque},
              {float(kCanvas.width) / extent.width, 0, 0, float(kCanvas.height) / extent.height,
               float(padding) / extent.width, float(padding) / extent.height}};
    });
  }
  std::cout << "Imported portrait masks aligned for a 320x180 source on a 320x240 canvas, including fitted, full-source, and padded textures.\n";
}

TimelineFrame FlatFrame(const VideoFrame& picture, PixelSize canvas = kCanvas) {
  auto plan = std::make_shared<RenderPlan>();
  plan->target = {canvas, PixelFormat::Bgra8Unorm};
  plan->draws = {{0, Blend::Replace}};
  return {picture.pts, std::move(plan), {picture}};
}

} // namespace

std::shared_ptr<AnalysisProvider> CreateMockMaskProvider() {
  return std::make_shared<MockMaskProvider>(std::make_shared<MaskState>());
}

void VerifyTimelineMasks(std::string_view directory) {
  @autoreleasepool {
    const File output(directory);
    const auto file = output.Child("mask-source.mp4");
    const auto document = output.Child("portrait-mask.avkit");
    const auto exported = output.Child("portrait-mask.mp4");
    Check(output.CreateDirectories() && !file.Exists() && !document.Exists() && !exported.Exists(),
          "Mask verification needs a fresh directory");
    const auto codecs = TimelineProviders();
    verification::WriteFixture(file, codecs, PreviewVideoConfig(kCanvas));
    const auto asset = ImportMovie(file);
    Project project(kCanvas);
    ClipId first, second;
    project.Edit([&](ProjectEdit& edit) {
      const auto id = edit.Import(InspectTimelineMovie(file));
      first = edit.InsertClip(edit.AddTrack(TrackKind::Video), {{id, asset.video.id, Interval(0, kFrames)}, Interval(0, kFrames)});
      second = edit.InsertClip(edit.AddTrack(TrackKind::Video), {{id, asset.video.id, Interval(0, kFrames), true}, Interval(0, kFrames)});
      edit.SetLayout(second, kPip);
      edit.SetBackground(first, kMain);
      edit.SetBackground(second, kInset);
      edit.InsertClip(edit.AddTrack(TrackKind::Audio), {{id, asset.audio.id, Interval(0, kFrames)}, Interval(0, kFrames)});
    });
    project.Save(document);
    const auto opened = Project::Open(document).Snapshot();
    Check(opened->tracks == project.Snapshot()->tracks && Alternate(first) != Alternate(second),
          "Fixture clip masks or source identities did not round-trip");

    Pictures raw, preview;
    {
      MovieRenderer renderer(kCanvas);
      MovieReader reader(asset, codecs, TrackKind::Video, {}, CodecPolicy::SoftwareOnly);
      for (int index = 0; index < kFrames; ++index) {
        const auto picture = reader.ReadVideo();
        Check(picture && picture->pts == Tick(index), "Fixture video PTS changed");
        raw[index] = Pixels(renderer.Draw(FlatFrame(*picture)));
      }
    }
    const auto state = std::make_shared<MaskState>();
    const auto provider = std::make_shared<MockMaskProvider>(state);
    VerifyImportedMaskAlignment(*opened, codecs, provider, raw, first, second);
    MovieRenderer renderer(kCanvas);
    TimelineSession session(opened, codecs, CodecPolicy::SoftwareOnly, {}, provider);
    for (int index = 0; index < kFrames; ++index) {
      const auto sample = session.SampleVideo(Tick(index));
      Check(std::get<VideoFrame>(sample.inputs.at(0)).pts == Tick(index) &&
                std::get<VideoFrame>(sample.inputs.at(1)).pts == Tick(kFrames - 1 - index),
            "Forward and reversed clips did not sample the expected source PTS");
      preview[index] = Pixels(renderer.Draw(sample));
      CheckPixels(preview[index], raw, index, kFrames - 1 - index, first, second);
    }
    auto reversed = std::make_shared<ProjectSnapshot>(*opened);
    for (auto& track : reversed->tracks) {
      if (track.kind == TrackKind::Video)
        track.clips.front().placement.source.reverse = !track.clips.front().placement.source.reverse;
    }
    TimelineSession reverse(reversed, codecs, CodecPolicy::SoftwareOnly, {}, provider);
    for (int index = 0; index < kFrames; ++index)
      Check(Pixels(renderer.Draw(reverse.SampleVideo(Tick(index)))) == preview[kFrames - 1 - index],
            "Playing backward changed the composed pixels at the same source PTS");
    for (const int index : {3, 12, 2, 10, 4, 0, 15}) {
      session.Reset();
      Check(Pixels(renderer.Draw(session.SampleVideo(Tick(index)))) == preview[index],
            "Seeking changed the mask or pixels for either clip");
    }
    Check(std::ranges::any_of(state->requests, [&](const auto& key) {
      return key.source == first.value && key.pts == Tick(3);
    }) && std::ranges::any_of(state->requests, [&](const auto& key) {
      return key.source == second.value && key.pts == Tick(3);
    }), "The same source PTS never reached both independent mask sessions");

    session.Reset();
    state->hold = first;
    const auto missed = session.SampleVideo(Tick(kLate), PixelFormat::Bgra8Unorm, {}, {}, std::chrono::milliseconds{5});
    Check(state->late && state->late->stop.stop_requested() && missed.analysis_masks.size() == 1,
          "Mask timeout did not cancel the job and keep the other clip's mask");
    CheckPixels(Pixels(renderer.Draw(missed)), raw, kLate, kFrames - 1 - kLate, first, second, true);
    const auto old = state->late->result.target;
    session.Reset();
    Check(Pixels(renderer.Draw(session.SampleVideo(Tick(kLate)))) == preview[kLate] &&
              state->late_deliveries == 1 && state->requests.at(state->requests.size() - 2).generation != old.generation,
          "A late mask from an older generation changed the same source PTS after a seek");

    MetalTexture mailbox({320, 180});
    std::stop_source stop;
    unsigned seeks = 0;
    MetalTexture::Origin origin;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    const auto stats = PlayTimeline(session, opened->Range(), renderer, mailbox, {}, true,
        [&](MediaTimestamp presented) -> std::optional<MediaTimestamp> {
          Check(std::chrono::steady_clock::now() < deadline, "Clocked mask preview did not finish");
          if (seeks == 0 && presented.Compare(Tick(3)) >= 0) {
            ++seeks;
            return Tick(9);
          }
          if (seeks == 1 && presented == Tick(9)) {
            Check(ReadPreviewPixels(mailbox.AcquireFrame(origin)) == preview[9],
                  "Clocked forward seek published a different mask");
            ++seeks;
            return Tick(1);
          }
          if (seeks == 2 && presented == Tick(1)) {
            Check(ReadPreviewPixels(mailbox.AcquireFrame(origin)) == preview[1],
                  "Clocked backward seek published a different mask");
            ++seeks;
          }
          if (presented == Tick(kFrames - 1)) stop.request_stop();
          return std::nullopt;
        }, stop.get_token());
    Check(seeks == 3 && stats.clocked_frames > 1 && !stats.audio_underruns &&
              ReadPreviewPixels(mailbox.AcquireFrame(origin)) == preview.back(),
          "Clocked preview did not reach the expected final masked frame");

    FileExportSettings settings;
    settings.policy = CodecPolicy::SoftwareOnly;
    settings.video = PreviewVideoConfig(kCanvas);
    settings.video.frame_rate_numerator = kTimelinePreviewRate;
    // Compare the decoded SDR samples without applying an output transfer conversion in the fixture.
    settings.video.input.transfer = ColorTransfer::Unknown;
    std::size_t frames = 0;
    ExportTimeline(opened, exported, codecs, settings, [&](const auto& sample, const auto& config, auto duration) {
      const auto composed = renderer.Draw(sample);
      Check(sample.pts == Tick(int(frames)), "Export sampled a different project PTS");
      Check(Pixels(composed) == preview.at(frames),
            "Export submitted pixels that differ from preview at the same source PTS");
      ++frames;
      return renderer.EncoderFrame(composed, config, sample.pts, duration);
    }, {}, provider);
    Check(frames == kFrames, "Export omitted a masked frame");

    MovieReader decoded(ImportMovie(exported), codecs, CodecPolicy::SoftwareOnly);
    double worst = 0;
    for (int index = 0; index < kFrames; ++index) {
      const auto picture = decoded.ReadVideo();
      Check(picture && picture->pts == Tick(index) && picture->duration == Tick(1), "Reimport changed the masked video timing");
      const auto pixels = Pixels(renderer.Draw(FlatFrame(*picture)));
      double delta = 0;
      for (std::size_t offset = 0; offset < pixels.size(); ++offset)
        delta += std::abs(std::to_integer<int>(pixels[offset]) - std::to_integer<int>(preview[index][offset]));
      worst = std::max(worst, delta / pixels.size());
    }
    Check(!decoded.ReadVideo(), "Reimport added an unexpected video frame");
    if (worst >= 4)
      throw std::runtime_error("Reimported video diverged from the rendered mask. Mean error " + std::to_string(worst));
    std::int64_t samples = 0;
    while (const auto pcm = decoded.ReadAudio()) {
      Check(pcm->pts == MediaTimestamp{samples, pcm->format.sample_rate}, "Masked export lost audio sample continuity");
      samples += pcm->sample_count;
    }
    Check(samples == opened->Range().end.Rescale(asset.pcm.sample_rate)->ticks,
          "Masked export changed the presented audio duration");
    std::cout << "Portrait masks: " << frames << " frames matched preview and pre-encode pixels, both seek directions, and reverse playback\n"
              << "Crop, mirror, picture-in-picture, partial coverage, row origins, and clip isolation passed\n"
              << "Zero coverage showed the replacement; skipped and timed-out masks kept the original; late results were rejected\n"
              << "Reimport preserved exact PTS and " << samples << " PCM samples. Worst mean channel error " << worst << " / 255\n";
  }
}

namespace {

constexpr int kRealFrames = 41, kMainStart = 2, kInsetStart = 8;
constexpr PixelSize kRealCanvas{640, 480};
constexpr Rect kRealPip{0.58F, 0.58F, 0.40F, 0.40F};
constexpr BackgroundReplacement kRealMain{Color::Rgb(19, 129, 96), {0.03F, 0.04F, 0.90F, 0.90F}};
constexpr BackgroundReplacement kRealInset{Color::Rgb(75, 123, 218), {0.05F, 0.10F, 0.51F, 0.80F}, true};
using RealMasks = std::map<std::pair<std::uint64_t, int>, ImageSurface>;

struct MaskProbe {
  enum class Mode { Measure, Skip, Hold, ReplayLate };
  struct ReadyMask {
    ClipId clip;
    MediaTimestamp pts;
    ImageSurface surface;
  };
  ClipId clip{};
  Mode mode{};
  std::optional<ReadyMask> ready;
  std::optional<std::stop_token> held_stop;
  bool overlap_seen{};
  std::promise<HeldMask> pending;
  std::optional<HeldMask> late;
  std::optional<MaskCompletion> foreign;
  std::mutex mutex;
  std::vector<double> latency_ms;
};

class MeasuredMaskSession final : public AnalysisSession {
public:
  MeasuredMaskSession(std::unique_ptr<AnalysisSession> session, std::shared_ptr<MaskProbe> probe)
      : session_(std::move(session)), probe_(std::move(probe)) {}

  void Submit(AnalysisJob job, std::stop_token stop, Completion complete) override {
    session_->Submit(std::move(job), stop, std::move(complete));
  }

  void SubmitMask(AnalysisJob job, std::stop_token stop, MaskCallback complete) override {
    if (probe_->ready && probe_->ready->clip.value == job.target.source && probe_->ready->pts == job.target.pts) {
      probe_->overlap_seen = probe_->held_stop && !probe_->held_stop->stop_requested();
      complete({job.id, job.target, decltype(MaskCompletion::result)::Success(AnalysisTexture{probe_->ready->surface, {}})});
      return;
    }
    if (probe_->foreign && probe_->foreign->target.source != job.target.source) {
      auto foreign = std::exchange(probe_->foreign, std::nullopt);
      Check(foreign->target.pts == job.target.pts, "The late result must target the same PTS as the other clip");
      foreign->job_id = job.id;
      foreign->target.generation = job.target.generation;
      complete(std::move(*foreign));
    }
    const auto mode = job.target.source == probe_->clip.value ? probe_->mode : MaskProbe::Mode::Measure;
    if (mode == MaskProbe::Mode::Skip) {
      complete({job.id, job.target, decltype(MaskCompletion::result)::Success(std::nullopt)});
      return;
    }
    if (mode == MaskProbe::Mode::Hold) {
      probe_->held_stop = stop;
      session_->SubmitMask(std::move(job), {}, [probe = probe_, stop, complete = std::move(complete)](auto result) mutable {
        probe->pending.set_value({std::move(result), std::move(complete), stop});
      });
      return;
    }
    if (mode == MaskProbe::Mode::ReplayLate) {
      auto held = std::exchange(probe_->late, std::nullopt);
      Check(held && held->result.target.pts == job.target.pts && held->result.target.generation != job.target.generation,
            "Expected a delayed ncnn result for an older job at the same source PTS");
      held->callback(held->result);
      held->result.job_id = job.id;
      complete(std::move(held->result));
    }
    const auto start = std::chrono::steady_clock::now();
    session_->SubmitMask(std::move(job), stop, [probe = probe_, start, complete = std::move(complete)](auto result) {
      if (result.result.Succeeded() && result.result.Value()) {
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
        const std::lock_guard lock(probe->mutex);
        probe->latency_ms.push_back(elapsed.count());
      }
      complete(std::move(result));
    });
  }

private:
  std::unique_ptr<AnalysisSession> session_;
  std::shared_ptr<MaskProbe> probe_;
};

class MeasuredMaskProvider final : public AnalysisProvider {
public:
  MeasuredMaskProvider(std::shared_ptr<AnalysisProvider> provider, std::shared_ptr<MaskProbe> probe)
      : provider_(std::move(provider)), probe_(std::move(probe)) {}

  Result<AnalysisWindow, std::string> Probe(const AnalysisConfig& config) const override { return provider_->Probe(config); }

  std::unique_ptr<AnalysisSession> Open(const AnalysisConfig& config) override {
    auto session = provider_->Open(config);
    return session ? std::make_unique<MeasuredMaskSession>(std::move(session), probe_) : nullptr;
  }

private:
  std::shared_ptr<AnalysisProvider> provider_;
  std::shared_ptr<MaskProbe> probe_;
};

std::span<const std::byte> MaskBytes(const ImageSurface& mask) {
  const auto pixels = GetImagePixels(mask);
  Check(mask.image == ImageDesc{{256, 256}, PixelFormat::R8Unorm} && mask.origin == RowOrigin::TopLeft &&
            pixels && pixels->row_bytes == 256, "ncnn did not return a top-left R8 coverage image");
  return pixels->bytes.AsBytes();
}

ImageSurface ReferenceMask(AnalysisProvider& provider, const VideoFrame& picture, std::string_view model_id) {
  const AnalysisConfig config{AnalysisCapability::PortraitMask, picture.format, picture.Storage(), false,
                              std::string(model_id)};
  const auto session = provider.Open(config);
  Check(bool(session), "Cannot open the reference portrait model");
  auto completion = std::make_shared<std::promise<MaskCompletion>>();
  auto pending = completion->get_future();
  const FrameKey key{1, 1, 0, picture.pts};
  session->SubmitMask({1, key, {{key, picture}}, {}}, {}, [completion](auto result) {
    completion->set_value(std::move(result));
  });
  Check(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready, "Reference portrait inference timed out");
  const auto result = pending.get();
  Check(result.job_id == 1 && result.target == key && result.result.Succeeded() && result.result.Value().has_value(),
        "Reference portrait inference failed");
  return result.result.Value()->surface;
}

std::optional<std::array<std::size_t, 2>> RealEdgeSamples(const Bytes& actual, const Bytes& main, const Bytes& inset,
                                                         const ImageSurface* main_mask, const ImageSurface* inset_mask) {
  const auto main_bytes = main_mask ? MaskBytes(*main_mask) : std::span<const std::byte>{};
  const auto inset_bytes = inset_mask ? MaskBytes(*inset_mask) : std::span<const std::byte>{};
  std::array<std::size_t, 2> edges{};
  for (unsigned y = 1; y < kRealCanvas.height; y += 3) {
    for (unsigned x = 1; x < kRealCanvas.width; x += 3) {
      const Point at{(x + 0.5F) / kRealCanvas.width, (y + 0.5F) / kRealCanvas.height};
      const bool pip = kRealPip.Contains(at);
      const auto& effect = pip ? kRealInset : kRealMain;
      const auto& crop = effect.source_crop;
      const auto& src = pip ? inset : main;
      const auto coverage = pip ? inset_bytes : main_bytes;
      const auto u = pip ? (at.x - kRealPip.x) / kRealPip.width : at.x;
      const auto v = pip ? (at.y - kRealPip.y) / kRealPip.height : at.y;
      const auto sx = crop.x + (pip ? 1 - u : u) * crop.width;
      const auto sy = crop.y + v * crop.height;
      const auto px = sx * kRealCanvas.width, py = sy * kRealCanvas.height;
      if (std::abs(px - std::round(px)) < 0.001F || std::abs(py - std::round(py)) < 0.001F) continue;
      const auto alpha = coverage.empty() ? 1.F : Bilinear(sx * 256 - 0.5F, sy * 256 - 0.5F, 0, 255, [&](int col, int row) {
        return std::to_integer<int>(coverage[row * 256 + col]);
      }) / 255.F;
      edges[pip] += alpha > 0.05F && alpha < 0.95F;
      const auto from = 4 * (unsigned(py) * kRealCanvas.width + unsigned(px));
      const auto to = 4 * (y * kRealCanvas.width + x);
      const std::array background{effect.color.red, effect.color.green, effect.color.blue};
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto expected = std::lerp(background[channel] * 255, float(std::to_integer<int>(src[from + channel])), alpha);
        if (std::abs(std::to_integer<int>(actual[to + channel]) - expected) > 3) return std::nullopt;
      }
    }
  }
  return edges;
}

} // namespace

void VerifyTimelineNcnnMasks(std::string_view source, std::string_view model_directory, std::string_view directory,
                             bool mixed_models) {
  @autoreleasepool {
    const File output(directory), input(source);
    const auto document = output.Child("ncnn-portrait.avkit"), exported = output.Child("ncnn-portrait.mp4");
    Check(output.CreateDirectories() && !document.Exists() && !exported.Exists(), "Use a fresh verification directory");
    const auto codecs = TimelineProviders();
    const auto asset = ImportMovie(input);
    const auto probe = std::make_shared<MaskProbe>();
    const auto ncnn = CreateTimelineNcnnProvider(model_directory);
    auto provider = std::make_shared<MeasuredMaskProvider>(ncnn, probe);
    std::vector<Bytes> source_pixels;
    constexpr int reference_frame = 14;
    ImageSurface selfie_reference{}, wide_reference{};
    MovieRenderer renderer(kRealCanvas);
    {
      MovieReader reader(asset, codecs, TrackKind::Video, {}, CodecPolicy::SoftwareOnly);
      for (int index = 0; index < kInsetStart + kRealFrames; ++index) {
        const auto picture = reader.ReadVideo();
        Check(picture && picture->pts == Tick(index) && picture->duration == Tick(1) &&
                  picture->format.visible.width == kRealCanvas.width && picture->format.visible.height == kRealCanvas.height,
              "Expected a 640x480, 24 fps SDR fixture with an audio track and at least 49 frames");
        if (mixed_models && index == reference_frame) {
          selfie_reference = ReferenceMask(*ncnn, *picture, {});
          wide_reference = ReferenceMask(*ncnn, *picture, kPPHumanSegV2LiteModelId);
          Check(std::ranges::equal(MaskBytes(selfie_reference),
                    MaskBytes(ReferenceMask(*ncnn, *picture, kMediaPipeSelfieModelId))) &&
                    !std::ranges::equal(MaskBytes(selfie_reference), MaskBytes(wide_reference)),
                "The default model did not match MediaPipe or the wide model returned the same coverage");
        }
        source_pixels.push_back(Pixels(renderer.Draw(FlatFrame(*picture, kRealCanvas))));
      }
    }
    Project project(kRealCanvas);
    ClipId first, second;
    project.Edit([&](ProjectEdit& edit) {
      const auto id = edit.Import(InspectTimelineMovie(input));
      const auto interval = Interval(0, kRealFrames);
      first = edit.InsertClip(edit.AddTrack(TrackKind::Video), {{id, asset.video.id, Interval(kMainStart, kMainStart + kRealFrames)}, interval});
      second = edit.InsertClip(edit.AddTrack(TrackKind::Video), {{id, asset.video.id, Interval(kInsetStart, kInsetStart + kRealFrames), true}, interval});
      edit.SetLayout(second, kRealPip);
      auto main = kRealMain, inset = kRealInset;
      if (mixed_models) {
        main.model_id = kPPHumanSegV2LiteModelId;
        inset.model_id = kMediaPipeSelfieModelId;
      }
      edit.SetBackground(first, std::move(main));
      edit.SetBackground(second, std::move(inset));
      edit.InsertClip(edit.AddTrack(TrackKind::Audio), {{id, asset.audio.id, interval}, interval});
    });
    project.Save(document);
    const auto opened = Project::Open(document).Snapshot();
    Check(opened->tracks == project.Snapshot()->tracks, "The background and model choices did not survive reopening");
    verification::DecoderUsage decode_usage;
    std::size_t decoded_pictures = 0;
    const MovieCodecProviders observed{codecs.platform, std::make_shared<verification::ObservedCodecProvider>(
        codecs.software, &decode_usage, verification::VideoFaults{}, [&](VideoFrame&) { ++decoded_pictures; })};
    TimelineSession session(opened, observed, CodecPolicy::SoftwareOnly, {}, provider);
    const auto sample = [&](int frame, std::optional<std::chrono::milliseconds> budget = {}) {
      return session.SampleVideo(Tick(frame), PixelFormat::Bgra8Unorm, {}, {}, budget);
    };
    const auto inset_at = [](int frame) { return kInsetStart + kRealFrames - 1 - frame; };
    RealMasks masks;
    std::vector<Bytes> previews;
    std::array<std::size_t, 2> edges{};
    for (int frame = 0; frame < kRealFrames; ++frame) {
      const int main = kMainStart + frame, inset = inset_at(frame);
      const auto composed = sample(frame);
      Check(composed.analysis_masks.size() == 2 && std::get<VideoFrame>(composed.inputs[0]).pts == Tick(main) &&
                std::get<VideoFrame>(composed.inputs[1]).pts == Tick(inset), "Wrong source PTS or missing per-clip mask");
      previews.push_back(Pixels(renderer.Draw(composed)));
      const auto counts = RealEdgeSamples(previews.back(), source_pixels[main], source_pixels[inset],
                                          &composed.analysis_masks[0], &composed.analysis_masks[1]);
      Check(counts.has_value(), "Real mask boundary diverged from the cropped and mirrored source pixels");
      for (std::size_t i = 0; i < edges.size(); ++i) edges[i] += (*counts)[i];
      masks.emplace(std::pair{first.value, main}, composed.analysis_masks[0]);
      masks.emplace(std::pair{second.value, inset}, composed.analysis_masks[1]);
    }
    Check(edges[0] > 100 && edges[1] > 100, "Real fixture did not exercise fractional coverage in both clips");
    auto stream = input.OpenRandomAccess();
    Check(stream.Succeeded(), "Cannot open the reverse cache verification source");
    auto packets = MediaDemuxer::Open(std::move(stream).Value());
    Check(packets.Succeeded(), "Cannot inspect the reverse clip's seek points");
    auto& demuxer = packets.Value();
    const auto first_anchor = demuxer.FindSeekPoint(asset.video.id, Tick(kInsetStart));
    const auto last_anchor = demuxer.FindSeekPoint(asset.video.id, Tick(kInsetStart + kRealFrames - 1));
    Check(first_anchor.Succeeded() && last_anchor.Succeeded(), "Cannot resolve the reverse clip's seek points");
    const bool one_reverse_gop = first_anchor.Value() && last_anchor.Value() &&
        first_anchor.Value()->decode_start_pts == last_anchor.Value()->decode_start_pts;
    if (one_reverse_gop) {
      Check(decode_usage.video.opened == 2, "Reverse playback reopened a decoder for a cached part of the same GOP");
    }
    std::cout << "Sequential mixed-direction sampling: " << decode_usage.video.opened << " video decoder opens, "
              << decoded_pictures << " decoded pictures for " << 2 * kRealFrames << " requested pictures\n";
    std::size_t shared = 0;
    for (int frame = kInsetStart; frame < kMainStart + kRealFrames; ++frame) {
      const auto& a = masks.at({first.value, frame});
      const auto& b = masks.at({second.value, frame});
      Check(a.storage != b.storage && std::ranges::equal(MaskBytes(a), MaskBytes(b)) == !mixed_models,
            "The two clips did not receive the selected masks at a shared source PTS");
      ++shared;
    }
    if (mixed_models) {
      Check(std::ranges::equal(MaskBytes(masks.at({first.value, reference_frame})), MaskBytes(wide_reference)) &&
                std::ranges::equal(MaskBytes(masks.at({second.value, reference_frame})), MaskBytes(selfie_reference)),
            "The reopened clips swapped the selected models at the same source PTS");
      const int frame = reference_frame - kMainStart;
      Check(Pixels(renderer.Draw(sample(frame))) == previews[frame], "Model edit did not start on the expected frame");
      project.Edit([&](ProjectEdit& edit) { edit.SetBackground(first, kRealMain); });
      session.ApplyProject(project.Snapshot());
      const auto changed = sample(frame);
      Check(changed.analysis_masks.size() == 2 &&
                std::ranges::equal(MaskBytes(changed.analysis_masks[0]), MaskBytes(selfie_reference)) &&
                std::ranges::equal(MaskBytes(changed.analysis_masks[1]), MaskBytes(masks.at({second.value, inset_at(frame)}))),
            "Changing the main clip's model reused stale coverage or affected the inset");
      session.ApplyProject(opened);
    }
    auto flipped = std::make_shared<ProjectSnapshot>(*opened);
    for (auto& track : flipped->tracks) {
      if (track.kind == TrackKind::Video)
        track.clips.front().placement.source.reverse = !track.clips.front().placement.source.reverse;
    }
    TimelineSession reverse(flipped, codecs, CodecPolicy::SoftwareOnly, {}, provider);
    for (int frame = 0; frame < kRealFrames; ++frame)
      Check(Pixels(renderer.Draw(reverse.SampleVideo(Tick(frame)))) == previews[kRealFrames - 1 - frame],
            "Reverse playback changed the masked composition at identical source PTS");
    session.Reset();
    (void)sample(0);
    const auto before_seeks = decode_usage.video.opened;
    constexpr std::array seek_frames{3, 32, 7, 21, 12, 36, 0, kRealFrames - 1};
    for (const int frame : seek_frames) {
      session.Seek(Tick(frame));
      Check(Pixels(renderer.Draw(sample(frame))) == previews[frame], "Seek changed a real mask or sampled the other clip");
    }
    const auto seek_video_opens = decode_usage.video.opened - before_seeks;
    if (one_reverse_gop)
      Check(seek_video_opens == seek_frames.size(), "Seek reopened the warmed reverse decoder");
    std::cout << "Bidirectional seeks: " << seek_frames.size() << " seeks, " << seek_video_opens << " video decoder opens\n";

    constexpr int kDelayed = 23;
    const auto check_fallback = [&](const TimelineFrame& frame, ClipId missing) {
      const int inset = inset_at(kDelayed);
      const bool main_missing = missing == first;
      const auto& expected = main_missing ? masks.at({second.value, inset}) : masks.at({first.value, kMainStart + kDelayed});
      Check(frame.analysis_masks.size() == 1 && std::ranges::equal(MaskBytes(frame.analysis_masks.front()),
                                                                  MaskBytes(expected)),
            "A clip's fallback affected the other model's mask");
      Check(RealEdgeSamples(Pixels(renderer.Draw(frame)), source_pixels[kMainStart + kDelayed], source_pixels[inset],
                            main_missing ? nullptr : &frame.analysis_masks.front(),
                            main_missing ? &frame.analysis_masks.front() : nullptr).has_value(),
            "The skipped or delayed clip did not show its original cropped picture");
    };
    session.Reset();
    probe->clip = first;
    probe->mode = MaskProbe::Mode::Skip;
    check_fallback(sample(kDelayed), first);
    for (const auto clip : {first, second}) {
      session.Reset();
      probe->clip = clip;
      probe->mode = MaskProbe::Mode::Hold;
      probe->held_stop.reset();
      probe->overlap_seen = false;
      probe->pending = {};
      auto delayed = probe->pending.get_future();
      // Use the other clip's measured coverage so this test injects exactly one missed deadline.
      const auto other = clip == first ? second : first;
      const auto other_frame = clip == first ? inset_at(kDelayed) : kMainStart + kDelayed;
      const auto& other_mask = masks.at({other.value, other_frame});
      probe->ready = MaskProbe::ReadyMask{other, Tick(other_frame), other_mask};
      const auto late = sample(kDelayed, std::chrono::milliseconds{12});
      probe->ready.reset();
      if (clip == first) Check(probe->overlap_seen, "The second mask was submitted after waiting for the first");
      check_fallback(late, clip);
      Check(delayed.wait_for(std::chrono::seconds{2}) == std::future_status::ready, "Delayed ncnn test inference did not finish");
      probe->late = delayed.get();
      Check(probe->late->stop.stop_requested() && probe->late->result.result.Succeeded() && probe->late->result.result.Value(),
            "The late-result fixture did not hold a real mask from a cancelled job");
      // Make acceptance observable even where both clips share a source PTS and model.
      auto poison = std::make_shared<Bytes>(256 * 256, std::byte{0});
      probe->late->result.result.Value()->surface = RetainImagePixels({{256, 256}, PixelFormat::R8Unorm},
                                                                      {BufferReference(*poison, poison), 256});
      probe->foreign = probe->late->result;
      session.Seek(Tick(kDelayed));
      probe->mode = MaskProbe::Mode::ReplayLate;
      Check(Pixels(renderer.Draw(sample(kDelayed))) == previews[kDelayed] && !probe->late && !probe->foreign,
            "A real mask from an old generation or another clip changed the composition after seek");
    }
    probe->mode = MaskProbe::Mode::Measure;

    const auto verify_clocked = [&](TimelineSession& playback, bool reverse_video) {
      playback.Reset();
      MetalTexture mailbox({kRealCanvas.width, kRealCanvas.height});
      MetalTexture::Origin origin;
      std::stop_source stop;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
      int last_clocked = -1, seeks = 0, masked = 0, fallbacks = 0;
      int skipped = 0;
      const auto stats = PlayTimeline(playback, opened->Range(), renderer, mailbox, Tick(0), true,
          [&](MediaTimestamp presented) -> std::optional<MediaTimestamp> {
            Check(std::chrono::steady_clock::now() < deadline, "Clocked real mask preview did not finish");
            const auto frame = Index(presented);
            if (frame == last_clocked) return std::nullopt;
            if (last_clocked >= 0 && frame > last_clocked) skipped += frame - last_clocked - 1;
            last_clocked = frame;
            const int reference = reverse_video ? kRealFrames - 1 - frame : frame;
            const auto pixels = ReadPreviewPixels(mailbox.AcquireFrame(origin));
            if (pixels == previews[reference]) {
              ++masked;
            } else {
              const auto main = kMainStart + reference, inset = inset_at(reference);
              const auto& a = masks.at({first.value, main});
              const auto& b = masks.at({second.value, inset});
              const auto matches = [&](const ImageSurface* ma, const ImageSurface* mb) {
                return RealEdgeSamples(pixels, source_pixels[main], source_pixels[inset], ma, mb).has_value();
              };
              Check(matches(nullptr, &b) || matches(&a, nullptr) || matches(nullptr, nullptr),
                    "Clocked preview showed pixels other than the expected masks or source fallback");
              ++fallbacks;
            }
            if (seeks == 0 && frame >= 5) {
              ++seeks;
              last_clocked = -1;
              return Tick(kRealFrames - 5);
            }
            if (seeks == 1 && frame >= kRealFrames - 2) {
              ++seeks;
              last_clocked = -1;
              return Tick(2);
            }
            if (frame == kRealFrames - 1) stop.request_stop();
            return std::nullopt;
          }, stop.get_token());
      std::cout << (reverse_video ? "Clocked reversed video: " : "Clocked preview: ") << masked
                << " exact masked frames, " << fallbacks << " source fallbacks, " << seeks << " seeks and "
                << stats.audio_underruns << " audio underruns, maximum video lag " << stats.maximum_video_lag_ms
                << " ms, " << skipped << " video grid positions missed, " << stats.late_video_frames
                << " late video frames discarded\n";
      Check(seeks == 2 && stats.clocked_frames > 1 && masked > 1 && stats.audio_underruns == 0 &&
                stats.maximum_video_lag_ms < 100,
            "Clocked preview lost PCM or did not resume both seeks with the expected masks");
    };
    verify_clocked(session, false);
    if (mixed_models) verify_clocked(reverse, true);

    FileExportSettings settings;
    settings.policy = CodecPolicy::SoftwareOnly;
    settings.video = PreviewVideoConfig(kRealCanvas);
    settings.video.frame_rate_numerator = kTimelinePreviewRate;
    settings.video.input.transfer = ColorTransfer::Unknown;
    std::size_t written = 0;
    ExportTimeline(opened, exported, codecs, settings, [&](const auto& frame, const auto& config, auto duration) {
      const auto surface = renderer.Draw(frame);
      Check(frame.pts == Tick(int(written)) && Pixels(surface) == previews.at(written),
            "Export diverged from the preview pixels before encoding");
      ++written;
      return renderer.EncoderFrame(surface, config, frame.pts, duration);
    }, {}, provider);
    Check(written == kRealFrames, "Export omitted a masked frame");

    MovieReader reimported(ImportMovie(exported), codecs, CodecPolicy::SoftwareOnly);
    double worst = 0;
    for (int frame = 0; frame < kRealFrames; ++frame) {
      const auto picture = reimported.ReadVideo();
      Check(picture && picture->pts == Tick(frame) && picture->duration == Tick(1), "Reimport changed masked video PTS");
      const auto pixels = Pixels(renderer.Draw(FlatFrame(*picture, kRealCanvas)));
      double error = 0;
      for (std::size_t i = 0; i < pixels.size(); ++i)
        error += std::abs(std::to_integer<int>(pixels[i]) - std::to_integer<int>(previews[frame][i]));
      worst = std::max(worst, error / pixels.size());
    }
    std::int64_t pcm = 0;
    while (const auto audio = reimported.ReadAudio()) {
      Check(audio->pts == MediaTimestamp{pcm, audio->format.sample_rate}, "Masked export lost PCM continuity");
      pcm += audio->sample_count;
    }
    Check(!reimported.ReadVideo() && worst < 4 && pcm == Tick(kRealFrames).Rescale(asset.pcm.sample_rate)->ticks,
          "Unexpected reimported color samples or audio duration");

    const auto mask = MaskBytes(masks.at({first.value, kMainStart + kDelayed}));
    constexpr std::string_view pgm_header = "P5\n256 256\n255\n";
    const auto header = std::as_bytes(std::span(pgm_header));
    Bytes pgm(header.begin(), header.end());
    pgm.insert(pgm.end(), mask.begin(), mask.end());
    Check(output.Child("coverage.pgm").WriteBytes(pgm), "Cannot write inferred mask for edge inspection");

    const std::lock_guard lock(probe->mutex);
    auto& latency = probe->latency_ms;
    std::ranges::sort(latency);
    if (mixed_models) std::cout << "Reopened PP-HumanSegV2-Lite on the main clip and MediaPipe on the inset; changing one model kept the other clip's mask\n";
    std::cout << "ncnn portrait: " << written << " exact pre-encode frames, " << shared << " shared source PTS, "
              << edges[0] << '/' << edges[1] << " fractional-coverage probes (main/inset)\n"
              << "Both masks submitted before waiting. Skip and independent 12 ms timeouts preserved the other clip's mask.\n"
              << "Rejected late results across generations and clips at the same source PTS.\n"
              << "ncnn mask jobs: " << latency.size() << ", median " << latency[latency.size() / 2] << " ms, p95 "
              << latency[(latency.size() - 1) * 95 / 100] << " ms, max " << latency.back() << " ms\n"
              << "Reimport: exact PTS, " << pcm << " PCM samples, worst mean channel error " << worst << " / 255\n";
  }
}

} // namespace avkit_preview
