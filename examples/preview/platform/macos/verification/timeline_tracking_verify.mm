#include "timeline_tracking_verify.h"

#include "bgfx_metal_texture.h"
#include "metal_preview.h"
#include "movie_renderer.h"
#include "timeline_preview.h"
#include "verification/tracking_fixture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>

#import <ImageIO/ImageIO.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::analysis;
using namespace huxerui::media;
using huxerui::Point;
using huxerui::Rect;
using enum AnalysisCapability;
using enum TrackedPart;
using namespace verification;

constexpr std::array kParts{Face, LeftHand, RightHand};
constexpr std::array<std::array<int, 4>, 3> kColors{{{252, 235, 30, 255}, {35, 244, 255, 255}, {254, 72, 192, 255}}};
constexpr Rect kGlasses{-0.5F, -0.45F, 2, 0.9F};
constexpr Rect kWrist{-0.025F, -0.04F, 0.05F, 0.08F};

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

Point OnDraw(const render::Draw& draw, Point uv) {
  const auto& rect = draw.destination;
  const Point point{rect.x + rect.width * uv.x, rect.y + rect.height * uv.y};
  return draw.transform ? draw.transform->Apply(point) : point;
}

void CheckPoint(Point actual, Point expected) {
  Check(std::abs(actual.x - expected.x) < 1e-5F && std::abs(actual.y - expected.y) < 1e-5F,
        "Sticker anchor moved to the wrong person, side, or frame");
}

bool HasColor(const Bytes& pixels, Point position, std::array<int, 4> color) {
  const auto x = int(position.x * kCanvas.width), y = int(position.y * kCanvas.height);
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      const auto offset = 4 * ((y + dy) * int(kCanvas.width) + x + dx);
      const auto pixel = std::span(pixels).subspan(offset, 4);
      if (std::equal(pixel.begin(), pixel.end(), color.begin(), [](std::byte a, int b) {
            return std::abs(std::to_integer<int>(a) - b) <= 2;
          })) return true;
    }
  }
  return false;
}

Bytes VerifyPicture(MovieRenderer& renderer, const TimelineFrame& sample, int expected) {
  Check(std::get<VideoFrame>(sample.inputs.front()).pts == Tick(expected), "Timeline sampled an unexpected source frame");
  const auto pixels = ReadPreviewPixels(avkit::macos::GetMetalTexture(renderer.Draw(sample)));
  const auto pose = Pose(kPrimary, expected), other = Pose(kSecondary, expected);
  std::size_t visible = 1;
  for (std::size_t i = 0; i < kParts.size(); ++i) {
    const auto part = kParts[i];
    const auto draw = std::ranges::find_if(sample.plan->draws, [&](const render::Draw& draw) {
      const auto* resource = std::get_if<ResourceRef>(&sample.inputs.at(draw.input_slot));
      return resource && *resource == TrackingStickerResource(part);
    });
    const bool edge = expected == 0 || expected >= kLength - 2;
    const bool hidden = expected == kMissingBody ||
        (part == Face ? expected == kWrongFace || expected == kCoincidentEyes
                      : edge || (part == LeftHand ? expected == kMissingLeft
                                                  : expected == kWeakRight || expected == kAmbiguousWrist));
    const auto probe = part == Face ? Point{0.25F, 0.21F} : Point{0.5F, 0.5F};
    if (hidden) {
      Check(draw == sample.plan->draws.end(), "Missing primary landmark reused old data or switched people");
      continue;
    }
    Check(draw != sample.plan->draws.end(), "Available sticker anchor is hidden");
    ++visible;
    if (part == Face) {
      Check(draw->transform.has_value(), "Two-point anchor did not rotate the glasses");
      CheckPoint(OnDraw(*draw, {0.25F, 0.5F}), pose.right_eye);
      CheckPoint(OnDraw(*draw, {0.75F, 0.5F}), pose.left_eye);
    } else {
      CheckPoint(OnDraw(*draw, probe), part == LeftHand ? pose.left_wrist : pose.right_wrist);
    }
    Check(HasColor(pixels, OnDraw(*draw, probe), kColors[i]), "GPU rendered a sticker away from its resolved anchor");
    const auto wrong = part == Face ? other.right_eye : part == LeftHand ? other.left_wrist : other.right_wrist;
    Check(!HasColor(pixels, wrong, kColors[i]), "A sticker also appeared on the other person");
  }
  Check(sample.plan->draws.size() == visible, "Unexpected tracked draw");
  return pixels;
}

void SavePicture(const File& path, Bytes pixels) {
  const auto color = CGColorSpaceCreateDeviceRGB();
  const auto context = CGBitmapContextCreate(pixels.data(), kCanvas.width, kCanvas.height, 8, kCanvas.width * 4,
                                             color, kCGImageAlphaPremultipliedLast);
  CGColorSpaceRelease(color);
  Check(context, "Cannot create tracking PNG bitmap");
  const auto image = CGBitmapContextCreateImage(context);
  CGContextRelease(context);
  Check(image, "Cannot create tracking PNG image");
  const auto url = [NSURL fileURLWithFileSystemRepresentation:path.Path().c_str() isDirectory:NO relativeToURL:nil];
  const auto png = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
  if (png) CGImageDestinationAddImage(png, image, nullptr);
  const bool written = png && CGImageDestinationFinalize(png);
  if (png) CFRelease(png);
  CGImageRelease(image);
  Check(written, "Cannot write tracking PNG");
}

void VerifyMissing(const std::shared_ptr<const ProjectSnapshot>& project, const MovieCodecProviders& codecs) {
  const auto state = std::make_shared<MockState>(false);
  TimelineSession session(project, codecs, CodecPolicy::SoftwareOnly, {}, MakeMockTrackingProvider(state));
  std::string diagnostic;
  try {
    (void)session.SampleVideo(Tick(24));
  } catch (const std::runtime_error& error) {
    diagnostic = error.what();
  }
  Check(diagnostic.find("hand landmarks") != std::string::npos && state->opens == 0 && state->trace.empty(),
        "Required capability failure was not detected before opening models");
  auto face_only = std::make_shared<ProjectSnapshot>(*project);
  for (auto& track : face_only->tracks) {
    for (auto& clip : track.clips) {
      std::erase_if(clip.stickers, [](const auto& sticker) { return sticker.spec.anchor->part != Face; });
    }
  }
  session.ApplyProject(std::move(face_only));
  const auto face = session.SampleVideo(Tick(0));
  Check(state->opens == 1 && state->trace.size() == 1 && state->trace.front().capability == FaceLandmarks &&
            face.plan->draws.size() == 2 && std::get<ResourceRef>(face.inputs.back()) == TrackingStickerResource(Face),
        "A face-only clip started unrelated analysis models");
}

void VerifyCancellation(const std::shared_ptr<const ProjectSnapshot>& project, const MovieCodecProviders& codecs,
                        MovieRenderer& renderer, ClipId first, ClipId second) {
  const auto state = std::make_shared<MockState>(true, 2);
  TimelineSession session(project, codecs, CodecPolicy::SoftwareOnly, {}, MakeMockTrackingProvider(state));
  std::stop_source cancel, recovery;
  std::exception_ptr error;
  std::jthread queues([&] {
    try {
      const auto old = state->TakeHeld();
      cancel.request_stop();
      const auto next = state->TakeHeld();
      Check(old.stop.stop_requested() && old.target.source == first.value && next.target.source == second.value &&
                old.target.pts == next.target.pts && old.target.generation != next.target.generation,
            "Cancellation failed to retire the old clip generation");
      const auto empty = DetectionResult::Success(std::vector<TrackingDetection>{});
      old.complete({old.id, old.target, empty});
      next.complete({next.id, old.target, empty});
      auto retired = next.target;
      retired.generation = old.target.generation;
      next.complete({next.id, retired, empty});
      next.complete({next.id + 1, next.target, empty});
      next.complete({next.id, next.target, DetectionResult::Success(next.detections)});
    } catch (...) {
      error = std::current_exception();
      cancel.request_stop();
      recovery.request_stop();
    }
  });
  bool cancelled = false;
  try {
    (void)session.SampleVideo(Tick(kCrossing), render::PixelFormat::Bgra8Unorm, cancel.get_token());
  } catch (const std::system_error& failure) {
    cancelled = failure.code() == std::errc::operation_canceled;
  }
  Check(cancelled, "An analysis result was published after cancellation");
  VerifyPicture(renderer, session.SampleVideo(Tick(kClipLength + kLength - 1 - kCrossing),
                                               render::PixelFormat::Bgra8Unorm, recovery.get_token()), kCrossing);
  queues.join();
  if (error) std::rethrow_exception(error);
}

void VerifyWristPrediction(const std::shared_ptr<const ProjectSnapshot>& project, const MovieCodecProviders& codecs) {
  const auto state = std::make_shared<MockState>();
  state->ordered_wrist_prediction = true;
  TimelineSession session(project, codecs, CodecPolicy::SoftwareOnly, {}, MakeMockTrackingProvider(state));
  const auto verify = [&](int frame, bool reverse) {
    const auto at = reverse ? kClipLength + kLength - 1 - frame : frame;
    const auto sample = session.SampleVideo(Tick(at));
    Check(std::get<VideoFrame>(sample.inputs.front()).pts == Tick(frame), "Wrist prediction changed the source PTS");
    for (const auto side : {LeftHand, RightHand}) {
      const auto draw = std::ranges::find_if(sample.plan->draws, [&](const auto& command) {
        const auto* resource = std::get_if<ResourceRef>(&sample.inputs.at(command.input_slot));
        return resource && *resource == TrackingStickerResource(side);
      });
      if (frame == kWristConfidenceDrop && side == LeftHand) {
        Check(draw != sample.plan->draws.end(), "A verified wrist did not recover inside the predicted region");
        CheckPoint(OnDraw(*draw, {0.5F, 0.5F}), Pose(kPrimary, frame).left_wrist - kObservedWristOffset);
      } else if (frame == kWristConfidenceDrop + 1 || frame == kForearmRetry) {
        Check(draw != sample.plan->draws.end(), "A verified wrist was lost during the second search");
        const auto pose = Pose(kPrimary, frame);
        auto expected = side == LeftHand ? pose.left_wrist : pose.right_wrist;
        if (frame == kWristConfidenceDrop + 1 && side == LeftHand) expected = expected + kObservedWristOffset;
        CheckPoint(OnDraw(*draw, {0.5F, 0.5F}), expected);
      } else {
        Check(draw == sample.plan->draws.end(), "A stale or unverified wrist was displayed");
      }
    }
  };
  for (const bool reverse : {false, true}) {
    const auto start = state->trace.size();
    verify(kWristConfidenceDrop + 1, reverse);
    Check(std::ranges::count_if(std::span(state->trace).subspan(start), [](const auto& job) {
            return job.capability == HandLandmarks && job.key.pts == Tick(kWristConfidenceDrop + 1);
          }) == 2, "The current wrist search must run before the predicted retry");
    const auto jobs = state->trace.size();
    session.Reset();
    verify(kWristConfidenceDrop, reverse);
    Check(state->trace.size() == jobs, "Seeking repeated ordered wrist analysis");
    verify(kForearmRetry, reverse);
  }
}

} // namespace

std::shared_ptr<AnalysisProvider> CreateMockTrackingProvider() {
  return MakeMockTrackingProvider(std::make_shared<MockState>());
}

void VerifyTimelineTracking(std::string_view directory) {
  @autoreleasepool {
    const File output(directory);
    const auto file = output.Child("two-people-crossing.mp4");
    const auto document = output.Child("tracking.avkit");
    Check(output.CreateDirectories() && !file.Exists() && !document.Exists(), "Tracking verification needs a fresh directory");
    const auto codecs = TimelineProviders();
    WriteFixture(file, codecs, PreviewVideoConfig(kCanvas));
    const auto asset = InspectTimelineMovie(file);
    const auto source = std::ranges::find(asset.tracks, TrackKind::Video, &AssetTrack::kind);
    Check(source != asset.tracks.end() && source->presentation == Interval(0, kLength), "Fixture video range changed");
    ClipId first, second;
    Project project(kCanvas);
    project.Edit([&](ProjectEdit& edit) {
      const auto id = edit.Import(asset);
      const auto video = edit.AddTrack(TrackKind::Video);
      first = edit.InsertClip(video, {{id, source->id, Interval(0, kClipLength)}, Interval(0, kClipLength)});
      second = edit.InsertClip(video, {{id, source->id, Interval(kLength - kClipLength, kLength), true},
                                       Interval(kClipLength, 2 * kClipLength)});
      for (const auto clip : {first, second}) {
        edit.AddSticker(clip, {TrackingStickerResource(Face), kGlasses, TrackingAnchor{Face, "right_eye", "left_eye"}});
        for (const auto part : {LeftHand, RightHand})
          edit.AddSticker(clip, {TrackingStickerResource(part), kWrist, TrackingAnchor{part, "wrist", {}}});
      }
    });
    project.Save(document);
    const auto opened = Project::Open(document).Snapshot();
    Check(opened->tracks == project.Snapshot()->tracks, "Tracking anchor or sticker instance did not round-trip");
    VerifyMissing(opened, codecs);
    std::cout << "Tracking capability probe rejects missing hands before opening models\n";
    VerifyWristPrediction(opened, codecs);
    std::cout << "Ordered wrist prediction retries only failed searches and publishes current-frame detections\n";

    MovieRenderer renderer(kCanvas);
    VerifyCancellation(opened, codecs, renderer, first, second);
    std::cout << "Tracking cancellation retires its generation and rejects late, wrong-source, wrong-generation, and wrong-job results\n";

    const auto state = std::make_shared<MockState>();
    TimelineSession session(opened, codecs, CodecPolicy::SoftwareOnly, {}, MakeMockTrackingProvider(state));
    std::array<Bytes, kClipLength> forward;
    std::array<std::vector<render::Draw>, kClipLength> plans;
    for (int frame = 0; frame < kClipLength; ++frame) {
      @autoreleasepool {
        const auto sample = session.SampleVideo(Tick(frame));
        forward[frame] = VerifyPicture(renderer, sample, frame);
        plans[frame] = sample.plan->draws;
      }
    }
    SavePicture(output.Child("before-crossing.png"), forward[kCrossing - 12]);
    SavePicture(output.Child("after-crossing.png"), forward[kCrossing + 12]);
    const auto after_forward = state->trace.size();
    for (int frame = kLength - 1; frame >= kLength - kClipLength; --frame) {
      @autoreleasepool {
        const auto sample = session.SampleVideo(Tick(kClipLength + kLength - 1 - frame));
        const auto pixels = VerifyPicture(renderer, sample, frame);
        if (frame < kClipLength) Check(pixels == forward[frame] && sample.plan->draws == plans[frame],
                                       "Reverse playback changed the pixels or anchors at the same source PTS");
      }
    }
    const auto after_reverse = state->trace.size();
    for (const auto& job : std::span(state->trace).first(after_forward))
      Check(job.key.source == first.value, "Forward analysis job belongs to the reverse clip");
    for (const auto& job : std::span(state->trace).subspan(after_forward))
      Check(job.key.source == second.value, "Reverse analysis job reused the forward clip identity");
    const auto first_generation = state->trace.front().key.generation;
    const auto second_generation = state->trace[after_forward].key.generation;
    const auto reverse_jobs = std::span(state->trace).subspan(after_forward);
    const auto reverse_boundary = std::ranges::find_if(reverse_jobs, [](const Trace& job) {
      return job.capability == BodyLandmarks && job.key.pts == Tick(kLength - 1);
    });
    Check(first_generation != second_generation &&
          state->trace.front().capability == BodyLandmarks && state->trace.front().window_size == 2 &&
          reverse_boundary != reverse_jobs.end() && reverse_boundary->window_size == 3,
          "Clip generations or truncated boundary windows are incorrect");
    constexpr std::array seek_frames{kAmbiguousWrist, kWeakRight, kMissingBody, kCoincidentEyes, kCrossing,
                                    kWrongFace, kMissingLeft, 36, kCrossing, 60};
    for (const int frame : seek_frames) {
      for (const bool reverse : {false, true}) {
        session.Reset();
        const auto start = state->trace.size();
        const auto at = reverse ? kClipLength + kLength - 1 - frame : frame;
        Check(VerifyPicture(renderer, session.SampleVideo(Tick(at)), frame) == forward[frame],
              "Seeking changed the composed pixels at the same source PTS");
        const auto key = state->trace.at(start).key;
        Check(key.source == (reverse ? second.value : first.value) &&
                  key.generation != (reverse ? second_generation : first_generation),
              "Seek did not retire the source generation");
      }
    }
    std::cout << "Tracking verified: " << 2 * kClipLength << " playback frames, " << kClipLength * 2 - kLength
              << " shared source frames pixel-identical in both directions, " << 2 * seek_frames.size() << " seeks, and "
              << state->trace.size() << " exact model jobs (" << after_reverse << " during playback)\n";
    std::cout << "Two people crossed with stable glasses and subject-left/right hand anchors; missing and invalid parts were hidden\n";
  }
}

} // namespace avkit_preview
