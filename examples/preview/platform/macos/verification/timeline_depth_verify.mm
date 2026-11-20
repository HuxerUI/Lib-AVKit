#include "timeline_depth_verify.h"

#include "bgfx_metal_texture.h"
#include "metal_preview.h"
#include "movie_renderer.h"
#include "timeline_preview.h"
#include "verification/tracking_fixture.h"

#include <huxerui/avkit_ecs.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

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
using verification::Tick;

using Pixel = std::array<std::uint8_t, 4>;
constexpr int kFrames = 41;
constexpr int kDepthSize = 8;
constexpr int kDepthStride = 11;
constexpr float kNear = 1.0F;
constexpr float kFar = 2.5F;
constexpr Rect kValidDepth{0.125F, 0.125F, 0.75F, 0.75F};
constexpr Transform2D kSourceToDepth{0.91F, 0, 0, 1.07F, 0.063F, -0.081F};

void Check(bool passed, const char* message) {
  if (!passed) throw std::runtime_error(message);
}

int Index(MediaTimestamp pts) {
  const auto frame = pts.Rescale(kTimelinePreviewRate);
  Check(frame && *frame == pts, "Expected an exact video frame time");
  return static_cast<int>(frame->ticks);
}

Bytes Pixels(const ColorSurface& surface) { return ReadPreviewPixels(avkit::macos::GetMetalTexture(surface)); }

template<class Value>
ImageSurface Image(ImageDesc desc, std::shared_ptr<std::vector<Value>> values, std::size_t stride,
                   RowOrigin origin = RowOrigin::TopLeft) {
  return RetainImagePixels(desc, {BufferReference(std::as_bytes(std::span(*values)), values), stride * sizeof(Value)}, origin);
}

ColorSurface Solid(Pixel color) {
  return {Image({{1, 1}, PixelFormat::Rgba8Unorm}, std::make_shared<std::vector<Pixel>>(1, color), 1), AlphaMode::Opaque};
}

bool Contains(Rect region, Point p) {
  return p.x >= region.x && p.y >= region.y && p.x < region.x + region.width && p.y < region.y + region.height;
}

void CheckOffscreen() {
  constexpr PixelSize size{16, 8};
  constexpr Pixel base{21, 42, 83, 255}, near_color{190, 41, 25, 255}, far_color{29, 193, 67, 255};
  constexpr float inf = std::numeric_limits<float>::infinity();
  constexpr float nan = std::numeric_limits<float>::quiet_NaN();
  constexpr std::array logical{0.F, 1.5F, 3.F, 3.F, 3.F, inf, 3.F, 1.5F, 1.5F, 3.F, nan, 3.F};
  constexpr PixelSize depth_size{4, 3};
  constexpr Rect valid{0.25F, 0, 0.75F, 1};
  auto rows = std::make_shared<std::vector<float>>(6 * depth_size.height, 15.F);
  for (std::uint32_t y = 0; y < depth_size.height; ++y)
    std::ranges::copy(std::span(logical).subspan(y * depth_size.width, depth_size.width),
                      rows->begin() + (depth_size.height - 1 - y) * 6);

  SceneEvaluator scene;
  scene.World().entity().set<CompositeLayer>({0, 0, Blend::Replace});
  const auto sticker = [&](std::uint32_t slot, float top, float depth) {
    scene.World().entity()
        .set<LocalTransform2D>({{0, top}, {1, 0.5F}})
        .set<CompositeLayer>({slot, static_cast<std::int32_t>(slot)})
        .set<DepthOcclusion>({0, {}, valid, {0, 0, 1, 1}, depth});
  };
  sticker(1, 0.F, kNear);
  sticker(2, 0.5F, kFar);
  auto plan = scene.Evaluate(Tick(0), {size, PixelFormat::Bgra8Unorm});
  auto backend = avkit::macos::CreateBgfxBackend(MTLCreateSystemDefaultDevice(), plan->target);
  RenderRequest request{plan, {Solid(base), Solid(near_color), Solid(far_color)},
                        {Image({depth_size, PixelFormat::R32Float}, rows, 6, RowOrigin::BottomLeft)}};
  const auto submitted = backend->TrySubmit(request);
  if (!submitted.Succeeded()) throw std::runtime_error(submitted.Error().message);
  Check(submitted.Value().has_value(), "Depth render had no available target");
  backend->Pump();
  const auto complete = backend->TryTake(*submitted.Value());
  if (!complete.Succeeded()) throw std::runtime_error(complete.Error().message);
  Check(complete.Value().has_value(), "Depth render did not complete");
  const auto pixels = Pixels(*complete.Value());
  std::size_t near_visible = 0, far_visible = 0;
  for (std::uint32_t y = 0; y < size.height; ++y) {
    for (std::uint32_t x = 0; x < size.width; ++x) {
      const Point center{(x + 0.5F) / size.width, (y + 0.5F) / size.height};
      const auto sample = logical.at(std::size_t(center.y * depth_size.height) * depth_size.width +
                                     std::size_t(center.x * depth_size.width));
      const bool near = y < size.height / 2;
      const bool visible = Contains(valid, center) && std::isfinite(sample) && sample >= (near ? kNear : kFar);
      if (visible) ++(near ? near_visible : far_visible);
      const auto expected = visible ? (near ? near_color : far_color) : base;
      Check(std::ranges::equal(std::span(pixels).subspan((y * size.width + x) * 4, 4),
                               std::as_bytes(std::span(expected))), "Offscreen metric depth pixel mismatch");
    }
  }
  Check(near_visible && far_visible && near_visible < size.width * size.height / 2 && far_visible < near_visible,
        "Expected both visible and hidden pixels for each ECS layer");
  request.data_inputs[0] = Image({{1, 1}, PixelFormat::R8Unorm}, std::make_shared<std::vector<std::uint8_t>>(1, 255), 1);
  const auto wrong_format = backend->TrySubmit(request);
  Check(!wrong_format.Succeeded() && wrong_format.Error().code == RenderErrorCode::InvalidPlan,
        "An R8Unorm texture was accepted as metric depth");
  std::cout << "Offscreen depth: " << near_visible << " near and " << far_visible
            << " far pixels visible. Padded rows, bottom origin, and invalid samples verified.\n";
}

void CheckSdrColorTransfer() {
  constexpr PixelSize size{4, 4};
  constexpr Pixel sample{200, 118, 51, 255};
  struct Case {
    ColorMatrix matrix;
    ColorRange range;
    std::array<int, 3> yuv;
  };
  constexpr std::array cases{
      Case{ColorMatrix::BT709, ColorRange::Limited, {134, 92, 165}},
      Case{ColorMatrix::BT709, ColorRange::Full, {137, 87, 170}},
      Case{ColorMatrix::BT601, ColorRange::Limited, {137, 89, 167}},
  };
  MovieRenderer renderer(size);
  auto plan = std::make_shared<RenderPlan>(RenderPlan{{size, PixelFormat::Bgra8Unorm}, {{0, Blend::Replace}}});
  const auto surface = renderer.Draw(TimelineFrame{Tick(0), plan, {Solid(sample)}});
  for (const auto& test : cases) {
    auto config = PreviewVideoConfig(size);
    config.input.pixel_format = VideoPixelFormat::I420;
    config.input.matrix = test.matrix;
    config.input.range = test.range;
    const auto encoded = renderer.EncoderFrame(surface, config, Tick(0), Tick(1));
    Check(encoded.planes.size() == test.yuv.size(), "Color transfer returned the wrong CPU plane layout");
    for (std::size_t channel = 0; channel < test.yuv.size(); ++channel) {
      const auto code = std::to_integer<int>(encoded.planes[channel].data.AsBytes().front());
      Check(std::abs(code - test.yuv[channel]) <= 1, "Unexpected YUV code for the selected matrix and range");
    }
    const auto pixels = Pixels(renderer.Draw(TimelineFrame{Tick(0), plan, {encoded}}));
    for (std::size_t index = 0; index < pixels.size(); ++index) {
      Check(std::abs(std::to_integer<int>(pixels[index]) - sample[index % sample.size()]) <= 2,
            "CPU RGB/YUV round trip exceeded the SDR quantization tolerance");
    }
  }
  std::cout << "SDR color round trip: limited BT.709, full BT.709, and limited BT.601 matrix verified\n";
}

struct DepthControl {
  enum class Mode { Current, Skipped, Delayed };

  struct DelayedJob {
    FrameKey target;
    std::uint64_t id;
    std::stop_token token;
    AnalysisSession::DepthCallback complete;
  };

  std::uint64_t primary;
  std::uint64_t inset;
  Mode mode{Mode::Current};
  std::optional<DelayedJob> delayed;
};

float DepthValue(int x, int y, const FrameKey& key, std::uint64_t primary) {
  constexpr std::array values{0.F, 0.75F, 1.5F, 3.5F, 3.5F};
  return values.at((x + 2 * y + Index(key.pts) % 3 + (key.source == primary ? 0 : 2)) % values.size());
}

AnalysisTexture DepthTexture(const FrameKey& key, std::uint64_t primary) {
  auto depth = std::make_shared<std::vector<float>>(kDepthSize * kDepthStride, 11.F);
  const bool bottom_up = key.source != primary;
  for (int y = 0; y < kDepthSize; ++y) {
    const auto row = bottom_up ? kDepthSize - y - 1 : y;
    for (int x = 0; x < kDepthSize; ++x)
      (*depth)[row * kDepthStride + x] = DepthValue(x, y, key, primary);
  }
  return {Image({{kDepthSize, kDepthSize}, PixelFormat::R32Float}, depth, kDepthStride,
                bottom_up ? RowOrigin::BottomLeft : RowOrigin::TopLeft), kSourceToDepth, kValidDepth};
}

class MockDepthSession final : public AnalysisSession {
public:
  explicit MockDepthSession(std::shared_ptr<DepthControl> control) : control_(std::move(control)) {}

  void SubmitDepth(AnalysisJob job, std::stop_token token, DepthCallback complete) override {
    using Reply = decltype(DepthCompletion::result);
    Check(job.frames.size() == 1 && job.target == job.frames.front().key && job.target.pts == job.frames.front().image.pts,
          "Depth job targeted the wrong decoded frame");
    if (job.target.source == control_->inset) {
      if (control_->mode == DepthControl::Mode::Skipped) {
        complete({job.id, job.target, Reply::Success(std::nullopt)});
        return;
      }
      if (control_->mode == DepthControl::Mode::Delayed) {
        control_->delayed = DepthControl::DelayedJob{job.target, job.id, token, std::move(complete)};
        return;
      }
      auto wrong = job.target;
      wrong.source = control_->primary;
      complete({job.id, wrong, Reply::Success(DepthTexture(wrong, control_->primary))});
    }
    complete({job.id, job.target, Reply::Success(DepthTexture(job.target, control_->primary))});
  }

private:
  std::shared_ptr<DepthControl> control_;
};

class MockDepthProvider final : public AnalysisProvider {
public:
  explicit MockDepthProvider(std::shared_ptr<DepthControl> control) : control_(std::move(control)) {}

  Result<AnalysisWindow, std::string> Probe(const AnalysisConfig& config) const override {
    using Reply = Result<AnalysisWindow, std::string>;
    return config.capability == AnalysisCapability::DepthMap && !config.uses_roi
        ? Reply::Success({}) : Reply::Failure("The fixture provides full-frame metric depth");
  }

  std::unique_ptr<AnalysisSession> Open(const AnalysisConfig&) override {
    return std::make_unique<MockDepthSession>(control_);
  }

private:
  std::shared_ptr<DepthControl> control_;
};

bool ExpectedVisible(const ClipInfo& clip, Point target, int project_frame, std::uint64_t primary) {
  const auto& rect = clip.destination;
  if (!Contains(rect, target)) return false;
  const auto& background = *clip.background;
  const auto& crop = background.source_crop;
  const auto x = (target.x - rect.x) / rect.width;
  const auto y = (target.y - rect.y) / rect.height;
  const auto uv = kSourceToDepth.Apply({crop.x + (background.mirrored ? 1.F - x : x) * crop.width,
                                        crop.y + y * crop.height});
  if (!Contains(kValidDepth, uv)) return false;
  const auto& placement = clip.placement;
  const auto offset = project_frame - Index(placement.timeline.start);
  const auto pts = placement.source.reverse ? Index(placement.source.range.end) - 1 - offset
                                            : Index(placement.source.range.start) + offset;
  const auto depth = DepthValue(int(uv.x * kDepthSize), int(uv.y * kDepthSize), {clip.id.value, 0, 0, Tick(pts)}, primary);
  return depth >= *clip.stickers.back().spec.depth_meters;
}

void CheckDancePixels(MovieRenderer& renderer, TimelineFrame frame, const std::array<const ClipInfo*, 2>& clips,
                      std::span<const std::byte> actual) {
  const auto index = Index(frame.pts);
  const auto size = frame.plan->target.size;
  auto reference = std::make_shared<RenderPlan>(*frame.plan);
  frame.plan = reference;
  for (auto& draw : reference->draws) draw.depth_occlusion.reset();
  const auto full = Pixels(renderer.Draw(frame));
  const auto full_count = reference->draws.size();
  const auto first = std::remove_if(reference->draws.begin(), reference->draws.end(), [&](const auto& draw) {
    return std::holds_alternative<ResourceRef>(frame.inputs.at(draw.input_slot));
  });
  reference->draws.erase(first, reference->draws.end());
  const std::size_t active = Index(clips.back()->placement.timeline.start) <= index ? 2 : 1;
  Check(full_count - reference->draws.size() == active && frame.depth_maps.size() == active,
        "Expected one independently sampled depth map and sticker per active clip");
  const auto base = Pixels(renderer.Draw(frame));

  for (std::uint32_t y = 0; y < size.height; ++y) {
    for (std::uint32_t x = 0; x < size.width; ++x) {
      const auto offset = (std::size_t(y) * size.width + x) * 4;
      const auto all_stickers = std::span(full).subspan(offset, 4);
      const auto none = std::span(base).subspan(offset, 4);
      if (std::ranges::equal(all_stickers, none)) continue;
      const Point target{(x + 0.5F) / size.width, (y + 0.5F) / size.height};
      const auto& clip = Contains(clips.back()->destination, target) ? *clips.back() : *clips.front();
      const auto expected = ExpectedVisible(clip, target, index, clips.front()->id.value) ? all_stickers : none;
      Check(std::ranges::equal(actual.subspan(offset, 4), expected),
            "DanceTrack depth pixels disagree with the source-space reference");
    }
  }
}

void CheckFallback(const std::shared_ptr<const ProjectSnapshot>& project, const MovieCodecProviders& codecs,
                   MovieRenderer& renderer, const std::shared_ptr<DepthControl>& control,
                   const std::shared_ptr<AnalysisProvider>& provider, const std::vector<Bytes>& expected) {
  TimelineSession timeline(project, codecs, CodecPolicy::SoftwareOnly, {}, provider);
  constexpr int frame = 16;
  const auto import = [&](const auto& picture) { return renderer.ImportFrame(picture); };
  const auto sample = [&] { return timeline.SampleVideo(Tick(frame), PixelFormat::Bgra8Unorm, {}, import,
                                                        std::chrono::milliseconds(4)); };
  control->mode = DepthControl::Mode::Skipped;
  const auto skipped = sample();
  Check(skipped.depth_maps.size() == 1 && std::ranges::count_if(skipped.plan->draws, [](const auto& draw) {
          return draw.depth_occlusion && draw.depth_occlusion->layer_depth_meters == kNear;
        }) == 1 &&
        std::ranges::none_of(skipped.plan->draws, [](const auto& draw) {
          return draw.depth_occlusion && draw.depth_occlusion->layer_depth_meters == kFar;
        }), "Skipped inset depth hid the wrong layers");
  const auto fallback = Pixels(renderer.Draw(skipped));
  Check(fallback != expected[frame], "Fallback fixture did not hide any far sticker pixels");
  timeline.Seek(Tick(frame));
  control->mode = DepthControl::Mode::Delayed;
  const auto missed = sample();
  Check(missed.depth_maps.size() == 1 && Pixels(renderer.Draw(missed)) == fallback &&
            control->delayed && control->delayed->token.stop_requested(), "Late inset depth did not apply the skip fallback");
  timeline.Seek(Tick(frame));
  control->mode = DepthControl::Mode::Current;
  auto late = std::exchange(control->delayed, std::nullopt);
  const auto new_frame = sample();
  late->complete({late->id, late->target, decltype(DepthCompletion::result)::Success(std::nullopt)});
  Check(new_frame.depth_maps.size() == 2 && Pixels(renderer.Draw(new_frame)) == expected[frame] &&
            Pixels(renderer.Draw(sample())) == expected[frame],
        "Old-generation depth contaminated the same PTS after seek");
}

void CheckClocked(TimelineSession& session, const ProjectSnapshot& project, MovieRenderer& renderer,
                  const std::vector<Bytes>& expected) {
  session.Reset();
  MetalTexture mailbox({float(project.canvas.width), float(project.canvas.height)});
  std::stop_source stop;
  std::vector<std::pair<int, id<MTLTexture>>> snapshots;
  snapshots.reserve(2 * kFrames);
  int seeks = 0, previous = -1;
  const auto stats = PlayTimeline(session, project.Range(), renderer, mailbox, Tick(0), true,
      [&](MediaTimestamp at) -> std::optional<MediaTimestamp> {
        const auto frame = Index(at);
        if (previous == frame) return std::nullopt;
        Check(previous < 0 || frame == previous + 1, "Clocked depth preview skipped a frame position");
        previous = frame;
        MetalTexture::Origin origin;
        snapshots.emplace_back(frame, mailbox.AcquireFrame(origin));
        if (seeks == 0 && frame == 16) {
          ++seeks;
          previous = -1;
          return Tick(36);
        }
        if (seeks == 1 && frame == 39) {
          ++seeks;
          previous = -1;
          return Tick(4);
        }
        if (seeks == 2 && frame == kFrames - 1) stop.request_stop();
        return std::nullopt;
      }, stop.get_token());
  Check(seeks == 2 && stats.audio_underruns == 0 && stats.late_video_frames == 0,
        "Clocked depth preview reported audio underrun or late composition");
  for (const auto& [index, texture] : snapshots)
    Check(ReadPreviewPixels(texture) == expected.at(index), "Clocked depth preview pixel mismatch");
}

void CheckExport(const std::shared_ptr<const ProjectSnapshot>& project, const File& destination,
                 const MovieCodecProviders& codecs, MovieRenderer& renderer,
                 const std::shared_ptr<AnalysisProvider>& provider, const std::vector<Bytes>& expected) {
  FileExportSettings settings;
  settings.policy = CodecPolicy::SoftwareOnly;
  settings.video = PreviewVideoConfig(project->canvas);
  settings.video.frame_rate_numerator = kTimelinePreviewRate;
  int written = 0;
  const auto import = [&](const auto& picture) { return renderer.ImportFrame(picture); };
  ExportTimeline(project, destination, codecs, settings, [&](const TimelineFrame& frame, const auto& config, auto duration) {
    const auto surface = renderer.Draw(frame);
    Check(frame.pts == Tick(written) && Pixels(surface) == expected.at(written), "Export differs before encoding");
    ++written;
    return renderer.EncoderFrame(surface, config, frame.pts, duration);
  }, {}, provider, import);
  Check(written == kFrames, "Export omitted an occlusion frame");

  MovieReader movie(ImportMovie(destination), codecs, CodecPolicy::SoftwareOnly);
  std::array<std::uint64_t, 3> channel_error{};
  double worst = 0;
  std::uint64_t pixels_counted = 0;
  for (int index = 0; index < written; ++index) {
    const auto picture = movie.ReadVideo();
    Check(picture && picture->pts == Tick(index) && picture->duration == Tick(1), "Reimport changed occlusion frame timing");
    Check(picture->format.primaries == ColorPrimaries::BT709 && picture->format.transfer == ColorTransfer::BT709 &&
              picture->format.matrix == ColorMatrix::BT709 && picture->format.range == ColorRange::Limited,
          "Reimport changed the default limited-range BT.709 color metadata");
    auto plan = std::make_shared<RenderPlan>(RenderPlan{{project->canvas, PixelFormat::Bgra8Unorm}, {{0, Blend::Replace}}});
    const auto pixels = Pixels(renderer.Draw(TimelineFrame{picture->pts, std::move(plan), {*picture}}));
    std::uint64_t error = 0;
    const auto& reference = expected[index];
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
      for (std::size_t channel = 0; channel < channel_error.size(); ++channel) {
        const auto delta = std::abs(std::to_integer<int>(pixels[i + channel]) - std::to_integer<int>(reference[i + channel]));
        channel_error[channel] += delta;
        error += delta;
      }
    }
    pixels_counted += pixels.size() / 4;
    worst = std::max(worst, double(error) / (3 * (pixels.size() / 4)));
  }
  const auto mean = (channel_error[0] + channel_error[1] + channel_error[2]) / (3.0 * pixels_counted);
  std::cout << "Default BT.709 reimport RGB MAE " << mean << " / 255, worst frame " << worst << " / 255, channels "
            << double(channel_error[0]) / pixels_counted << ", " << double(channel_error[1]) / pixels_counted << ", "
            << double(channel_error[2]) / pixels_counted << '\n';
  Check(!movie.ReadVideo() && worst < 3.0, "Reimport changed occlusion pixels beyond the compression tolerance");
  std::int64_t pcm = 0;
  std::uint32_t rate = 0;
  while (const auto audio = movie.ReadAudio()) {
    rate = audio->format.sample_rate;
    Check(audio->pts == MediaTimestamp{pcm, rate}, "Occlusion export contains discontinuous audio");
    pcm += audio->sample_count;
  }
  Check(rate && MediaTimestamp{pcm, rate} == project->Range().end, "Occlusion export audio ended at the wrong PTS");
  std::cout << written << " exact pre-encode frames and reimported PTS, " << pcm << " continuous PCM samples\n";
}

} // namespace

void VerifyTimelineDepth(std::string_view document, std::string_view directory) {
  @autoreleasepool {
    CheckOffscreen();
    CheckSdrColorTransfer();
    auto project = Project::Open(File(document));
    const auto source = project.Snapshot();
    Check(source->assets.size() == 1 && source->tracks.size() == 4 && source->transitions.empty() &&
          source->Range() == TimeRange{Tick(0), Tick(kFrames)}, "Expected the DanceTrack animated mask fixture");
    for (int index = 0; index < 3; ++index) {
      const auto& track = source->tracks.at(index);
      Check(track.kind == TrackKind::Video && track.clips.size() == 1 && track.clips.front().stickers.empty() &&
              track.clips.front().background && track.clips.front().background->animated_mask,
            "Expected the three DanceTrack video clips and their animated masks");
    }

    const File output(directory), saved(output, "depth-occlusion.avkit");
    Check(output.CreateDirectories() && !saved.Exists(), "Use a fresh depth verification directory");
    const auto main = source->tracks[0].clips.front().id;
    const auto inset = source->tracks[1].clips.front().id;
    project.Edit([&](ProjectEdit& edit) {
      edit.AddSticker(main, {PreviewStickerResource(), {0.40F, 0.22F, 0.22F, 0.29F}, {}, kNear});
      edit.AddSticker(inset, {PreviewStickerResource(), {0.05F, 0.10F, 0.83F, 0.78F}, {}, kFar});
    });
    project.Save(saved);
    auto reopened = Project::Open(saved);
    Check(reopened.Snapshot()->tracks == project.Snapshot()->tracks, "Reopening lost either sticker depth");

    auto control = std::make_shared<DepthControl>(DepthControl{main.value, inset.value});
    auto provider = std::make_shared<MockDepthProvider>(control);
    const auto codecs = TimelineProviders();
    MovieRenderer renderer(source->canvas);
    const auto import = [&](const auto& picture) { return renderer.ImportFrame(picture); };
    const auto verify = [&](const std::shared_ptr<const ProjectSnapshot>& snapshot, const File& export_file) {
      TimelineSession session(snapshot, codecs, CodecPolicy::SoftwareOnly, {}, provider);
      const std::array clips{&snapshot->tracks[0].clips.front(), &snapshot->tracks[1].clips.front()};
      const auto sample = [&](int index) { return session.SampleVideo(Tick(index), PixelFormat::Bgra8Unorm, {}, import); };
      std::vector<Bytes> expected;
      expected.reserve(kFrames);
      for (int index = 0; index < kFrames; ++index) {
        const auto frame = sample(index);
        expected.push_back(Pixels(renderer.Draw(frame)));
        CheckDancePixels(renderer, frame, clips, expected.back());
      }
      for (const auto index : {9, 18, 12, 38, 16, 40, 0}) {
        session.Seek(Tick(index));
        Check(Pixels(renderer.Draw(sample(index))) == expected.at(index), "Depth pixels changed after seeking");
      }
      for (int index = kFrames - 1; index >= 0; --index)
        Check(Pixels(renderer.Draw(sample(index))) == expected.at(index), "Descending sampling changed depth pixels");
      CheckFallback(snapshot, codecs, renderer, control, provider, expected);
      CheckClocked(session, *snapshot, renderer, expected);
      CheckExport(snapshot, export_file, codecs, renderer, provider, expected);
    };
    verify(reopened.Snapshot(), File(output, "depth-occlusion.mp4"));
    reopened.Edit([&](ProjectEdit& edit) {
      for (const auto& track : source->tracks) {
        if (track.kind != TrackKind::Video) continue;
        const auto& clip = track.clips.front();
        auto placement = clip.placement;
        placement.source.reverse = !placement.source.reverse;
        edit.SetClip(clip.id, placement);
      }
    });
    verify(reopened.Snapshot(), File(output, "flipped-depth-occlusion.mp4"));
    std::cout << "DanceTrack: crop, mirror, insets, both seeks, skipped and late depth, and both source directions verified.\n";
  }
}

} // namespace avkit_preview
