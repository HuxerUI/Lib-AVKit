#include "timeline_tracking_verify.h"

#include "bgfx_metal_texture.h"
#include "metal_preview.h"
#include "movie_renderer.h"
#include "timeline_preview.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <huxerui/avkit/ncnn.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::analysis;
using namespace huxerui::media;
using huxerui::Point;
using huxerui::macos::MetalTexture;

constexpr PixelSize kCanvas{320, 240};
constexpr int kFrames = 16;
constexpr auto kPolicy = CodecPolicy::SoftwareOnly;

MediaTimestamp Tick(int frame) { return {frame, kTimelinePreviewRate}; }
TimeRange Range(int first, int end) { return {Tick(first), Tick(end)}; }
int SourceFrame(int frame) { return frame < kFrames ? frame : 2 * kFrames - 1 - frame; }
bool HasAnchor(int source) { return source < 5 || source >= 10; }

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

RawAsset ReadAsset(const File& file) {
  auto bytes = file.ReadBytes();
  if (!bytes.Succeeded()) throw std::runtime_error("Cannot read ncnn model or verification asset: " + file.Path());
  return RawAsset::FromBytes(std::move(bytes).Value());
}

std::optional<Point> StickerCenter(const Bytes& pixels) {
  std::uint64_t x_sum = 0, y_sum = 0, count = 0;
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    if (std::to_integer<int>(pixels[i]) <= 207 || std::to_integer<int>(pixels[i + 1]) <= 184 ||
        std::to_integer<int>(pixels[i + 2]) >= 91) continue;
    x_sum += i / 4 % kCanvas.width;
    y_sum += i / 4 / kCanvas.width;
    ++count;
  }
  if (count < 20) return std::nullopt;
  return Point{float(x_sum) / count, float(y_sum) / count};
}

AnalysisCompletion Submit(AnalysisSession& session, const VideoFrame& frame, std::uint64_t sequence,
                          std::stop_token stop = {}) {
  const FrameKey key{1, 1, sequence, frame.pts};
  std::optional<AnalysisCompletion> result;
  session.Submit({sequence, key, {{key, frame}}, {}}, stop, [&](auto completion) {
    Check(!result, "YuNet finished the same submission twice");
    result.emplace(std::move(completion));
  });
  Check(result && result->job_id == sequence && result->target == key, "YuNet completed the wrong frame");
  return std::move(*result);
}

void VerifyProvider(AnalysisProvider& provider, const File& fixture, const MovieCodecProviders& codecs,
                    const File& pack) {
  Check(!CreateNcnnFaceAnalysisProvider({}, {}).Succeeded(), "YuNet loaded an empty model pack");
  const auto weights = ReadAsset(pack.Child("yunet.ncnn.bin"));
  Check(!CreateNcnnFaceAnalysisProvider(RawAsset::FromBytes(Bytes{std::byte{1}}), weights).Succeeded(),
        "YuNet loaded a malformed graph");
  const auto graph = ReadAsset(pack.Child("yunet.ncnn.param"));
  auto partial = weights.ReadBytes();
  partial.pop_back();
  Check(!CreateNcnnFaceAnalysisProvider(graph, RawAsset::FromBytes(std::move(partial))).Succeeded(),
        "YuNet loaded truncated weights");

  MovieReader reader(ImportMovie(fixture), codecs, TrackKind::Video, Tick(0), kPolicy);
  const auto first = reader.ReadVideo();
  Check(first && first->pts == Tick(0), "Missing YuNet reference picture");
  AnalysisConfig config{AnalysisCapability::FaceLandmarks, first->format, first->Storage()};
  const auto contract = provider.Probe(config);
  Check(contract.Succeeded() && !contract.Value().stateful && !contract.Value().past_frames &&
        !contract.Value().future_frames, "YuNet requires unexpected temporal frames");
  auto session = provider.Open(config);
  Check(bool(session), "Cannot open YuNet's supported configuration");
  config.capability = AnalysisCapability::HandLandmarks;
  Check(!provider.Probe(config).Succeeded() && !provider.Open(config), "YuNet advertised hand tracking");
  config.capability = AnalysisCapability::FaceLandmarks;
  config.storage = VideoStorage::Platform;
  Check(!provider.Probe(config).Succeeded(), "YuNet accepted platform video storage");
  config.storage = VideoStorage::Cpu;

  std::stop_source cancel;
  cancel.request_stop();
  const auto cancelled = Submit(*session, *first, 1, cancel.get_token());
  Check(cancelled.result.Succeeded() && !cancelled.result.Value(), "Cancelled YuNet job produced detections");
  const auto initial = Submit(*session, *first, 2);
  Check(initial.result.Succeeded() && initial.result.Value() && initial.result.Value()->size() == 1,
        "YuNet did not deliver landmarks after cancellation");

  reader.Seek(Tick(7));
  const auto different = reader.ReadVideo();
  Check(different && different->pts == Tick(7), "Missing second face picture");
  auto independent = provider.Open(config);
  Check(bool(independent), "Cannot open an independent YuNet session");
  const auto other = Submit(*independent, *different, 1);
  const auto rejected = Submit(*session, *different, 3);
  Check(other.result.Succeeded() && other.result.Value() && other.result.Value()->size() == 1 &&
        rejected.result.Succeeded() && rejected.result.Value() && rejected.result.Value()->empty(),
        "A second face was undetected or replaced the original session's subject");
}

std::shared_ptr<const ProjectSnapshot> MakeProject(const File& fixture, const File& document) {
  const auto asset = InspectTimelineMovie(fixture);
  const auto& video = asset.tracks[0];
  const auto& audio = asset.tracks[1];
  Check(video.kind == TrackKind::Video && audio.kind == TrackKind::Audio && video.presentation == Range(0, kFrames),
        "YuNet verification needs the 16-frame portrait fixture");
  Project project(kCanvas);
  project.Edit([&](ProjectEdit& edit) {
    const auto id = edit.Import(asset);
    const auto v = edit.AddTrack(TrackKind::Video), a = edit.AddTrack(TrackKind::Audio);
    for (int i = 0; i < 2; ++i) {
      const auto clip = edit.InsertClip(v, {{id, video.id, Range(0, kFrames), i == 1}, Range(i * kFrames, (i + 1) * kFrames)});
      edit.InsertClip(a, {{id, audio.id, Range(0, kFrames)}, Range(i * kFrames, (i + 1) * kFrames)});
      edit.AddSticker(clip, {TrackingStickerResource(TrackedPart::Face), {-0.5F, -0.45F, 2, 0.9F},
                             TrackingAnchor{TrackedPart::Face, "right_eye", "left_eye"}});
    }
  });
  project.Save(document);
  return Project::Open(document).Snapshot();
}

Bytes ReadPicture(MovieRenderer& renderer, const TimelineFrame& frame) {
  return ReadPreviewPixels(huxerui::avkit::macos::GetMetalTexture(renderer.Draw(frame)));
}

using Pictures = std::array<Bytes, kFrames>;

Pictures VerifySampling(TimelineSession& session, MovieRenderer& renderer) {
  Pictures pictures;
  std::array<std::vector<render::Draw>, kFrames> draws;
  for (int i = 0; i < 2 * kFrames; ++i) {
    @autoreleasepool {
      const auto frame = session.SampleVideo(Tick(i));
      const auto source = SourceFrame(i);
      Check(std::get<VideoFrame>(frame.inputs.front()).pts == Tick(source), "YuNet sampled the wrong source PTS");
      auto pixels = ReadPicture(renderer, frame);
      if (bool(StickerCenter(pixels)) != HasAnchor(source))
        throw std::runtime_error("Unexpected YuNet glasses visibility at source frame " + std::to_string(source));
      if (i < kFrames) {
        Check(frame.plan->draws.size() == (HasAnchor(source) ? 2 : 1), "Unexpected YuNet sticker draw");
        if (HasAnchor(source)) Check(frame.plan->draws.back().transform.has_value(), "Missing two-point eye transform");
        pictures[source] = std::move(pixels);
        draws[source] = frame.plan->draws;
      } else {
        Check(pictures[source] == pixels && draws[source] == frame.plan->draws, "Reverse changed the YuNet composition");
      }
    }
  }
  for (const int i : {4, 13, 7, 2, 15, 6, 9, 5}) {
    for (const auto at : {i, 2 * kFrames - 1 - i}) {
      session.Reset();
      Check(ReadPicture(renderer, session.SampleVideo(Tick(at))) == pictures[i], "Seeking changed the YuNet composition");
    }
  }
  return pictures;
}

void VerifyClock(TimelineSession& session, TimeRange range, MovieRenderer& renderer, const Pictures& pictures) {
  MetalTexture mailbox({float(kCanvas.width), float(kCanvas.height)});
  std::stop_source stop;
  std::optional<MediaTimestamp> last;
  int stage = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  session.Reset();
  const auto stats = PlayTimeline(session, range, renderer, mailbox, Tick(0), true,
      [&](MediaTimestamp presented) -> std::optional<MediaTimestamp> {
        Check(std::chrono::steady_clock::now() < deadline, "Clocked YuNet preview timed out");
        const auto frame = presented.Rescale(kTimelinePreviewRate)->ticks;
        if (last != presented) {
          MetalTexture::Origin origin;
          const auto texture = mailbox.AcquireFrame(origin);
          Check(texture && origin == MetalTexture::Origin::TopLeft &&
                ReadPreviewPixels(texture) == pictures.at(SourceFrame(frame)),
                "Clocked YuNet preview changed the sampled image");
          last = presented;
        }
        if (stage == 0 && frame >= 4) { ++stage; return Tick(20); }
        if (stage == 1 && frame == 20) ++stage;
        if (stage == 2 && frame >= 23) { ++stage; return Tick(3); }
        if (stage == 3 && frame == 3) ++stage;
        if (stage == 4 && frame >= 8) stop.request_stop();
        return std::nullopt;
      }, stop.get_token());
  Check(stage == 4 && stats.clocked_frames > 0, "Clocked YuNet playback did not resume both seeks");
  std::cout << "YuNet clocked preview: " << stats.clocked_frames << " clocked frames, " << stats.audio_underruns
            << " audio underruns\n";
}

void VerifyExport(const std::shared_ptr<const ProjectSnapshot>& project, const File& destination,
                  const MovieCodecProviders& codecs, const std::shared_ptr<AnalysisProvider>& provider,
                  MovieRenderer& renderer, const Pictures& pictures) {
  FileExportSettings settings;
  settings.video = PreviewVideoConfig(kCanvas);
  settings.video.frame_rate_numerator = kTimelinePreviewRate;
  settings.policy = kPolicy;
  int written = 0;
  ExportTimeline(project, destination, codecs, settings, [&](const TimelineFrame& frame, const VideoEncoderConfig& config,
                                                              MediaTimestamp duration) {
    @autoreleasepool {
      auto surface = renderer.Draw(frame);
      Check(frame.pts == Tick(written) && ReadPreviewPixels(huxerui::avkit::macos::GetMetalTexture(surface)) ==
            pictures.at(SourceFrame(written)), "YuNet export changed PTS or pixels before encoding");
      ++written;
      return renderer.EncoderFrame(surface, config, frame.pts, duration);
    }
  }, {}, provider);
  Check(written == 2 * kFrames, "YuNet export wrote the wrong number of frames");
  MovieReader reader(ImportMovie(destination), codecs, TrackKind::Video, Tick(0), kPolicy);
  const auto plan = std::make_shared<render::RenderPlan>(render::RenderPlan{{kCanvas, render::PixelFormat::Bgra8Unorm},
                                                                          {{0, render::Blend::Replace}}});
  double drift = 0;
  for (int i = 0; i < written; ++i) {
    @autoreleasepool {
      const auto frame = reader.ReadVideo();
      Check(frame && frame->pts == Tick(i) && frame->duration == Tick(1), "Reimport changed YuNet video timing");
      const auto expected = StickerCenter(pictures[SourceFrame(i)]);
      const auto actual = StickerCenter(ReadPicture(renderer, {frame->pts, plan, {*frame}, {}}));
      Check(bool(actual) == bool(expected), "Reimport changed YuNet sticker visibility");
      if (actual) drift = std::max(drift, double(std::hypot(actual->x - expected->x, actual->y - expected->y)));
    }
  }
  Check(!reader.ReadVideo() && drift <= 2, "Reimport changed frame count or shifted the glasses");
  std::cout << "YuNet export: " << written << " exact pre-encode frames and MP4 PTS, max glasses shift " << drift << " px\n";
}

} // namespace

std::shared_ptr<huxerui::avkit::analysis::AnalysisProvider> CreateTimelineNcnnProvider(std::string_view model_directory) {
  const File pack(model_directory);
  const auto model = [&](std::string_view stem) {
    return NcnnModelAssets{ReadAsset(pack.Child(std::string(stem) + ".ncnn.param")),
                           ReadAsset(pack.Child(std::string(stem) + ".ncnn.bin"))};
  };
  NcnnAnalysisModels models;
  if (pack.Child("yunet.ncnn.param").Exists()) models.face = model("yunet");
  if (pack.Child("mp_persondet.ncnn.param").Exists())
    models.body_hand = NcnnBodyHandModels{model("mp_persondet"), model("mp_pose"), model("mp_palmdet"), model("mp_handpose")};
  auto provider = CreateNcnnAnalysisProvider(models);
  if (!provider.Succeeded()) throw std::runtime_error(provider.Error());
  return std::move(provider).Value();
}

void VerifyTimelineNcnn(std::string_view source, std::string_view model_directory, std::string_view directory) {
  @autoreleasepool {
    const File fixture(source), output(directory);
    const auto document = output.Child("yunet.avkit"), video = output.Child("yunet.mp4");
    Check(output.CreateDirectories() && !document.Exists() && !video.Exists(), "YuNet verification needs a fresh directory");
    const auto provider = CreateTimelineNcnnProvider(model_directory);
    const auto codecs = TimelineProviders();
    VerifyProvider(*provider, fixture, codecs, File(model_directory));
    const auto project = MakeProject(fixture, document);
    TimelineSession session(project, codecs, kPolicy, {}, provider);
    MovieRenderer renderer(kCanvas);
    const auto pictures = VerifySampling(session, renderer);
    VerifyClock(session, project->Range(), renderer, pictures);
    VerifyExport(project, video, codecs, provider, renderer, pictures);
    std::cout << "YuNet preview: 32 playback frames, 16 seeks and 2 clocked seeks, no switch to the second face\n";
  }
}


namespace {

constexpr PixelSize kBodyCanvas{640, 480};
constexpr std::array kHands{TrackedPart::LeftHand, TrackedPart::RightHand};
using WristFrame = std::array<std::array<Point, 2>, 2>;

class CountedBodyProvider final : public AnalysisProvider {
public:
  explicit CountedBodyProvider(std::shared_ptr<AnalysisProvider> provider) : provider_(std::move(provider)) {}

  Result<AnalysisWindow, std::string> Probe(const AnalysisConfig& config) const override {
    return provider_->Probe(config);
  }

  std::unique_ptr<AnalysisSession> Open(const AnalysisConfig& config) override {
    auto session = provider_->Open(config);
    if (session && config.capability == AnalysisCapability::BodyLandmarks)
      return std::make_unique<CountedSession>(std::move(session), count_);
    return session;
  }

  int Count() const { return count_; }

private:
  class CountedSession final : public AnalysisSession {
  public:
    CountedSession(std::unique_ptr<AnalysisSession> session, int& count) : session_(std::move(session)), count_(count) {}

    void Submit(AnalysisJob job, std::stop_token stop, Completion complete) override {
      ++count_;
      session_->Submit(std::move(job), stop, std::move(complete));
    }

  private:
    std::unique_ptr<AnalysisSession> session_;
    int& count_;
  };

  std::shared_ptr<AnalysisProvider> provider_;
  int count_{};
};

std::vector<WristFrame> ReadWrists(const File& path) {
  std::istringstream input(ReadAsset(path).ReadString());
  std::vector<WristFrame> frames;
  input >> std::ws;
  while (!input.eof()) {
    WristFrame frame{};
    for (auto& person : frame) {
      for (auto& wrist : person) {
        Check(bool(input >> wrist.x >> wrist.y), "Incomplete verification wrist coordinates");
        Check(wrist.x >= 0 && wrist.x < kBodyCanvas.width && wrist.y >= 0 && wrist.y < kBodyCanvas.height,
              "Verification wrist is outside the 640 by 480 fixture");
      }
    }
    frames.push_back(frame);
    input >> std::ws;
  }
  Check(frames.size() >= 5 && frames.size() <= 120, "Expected five to 120 verification frames");
  return frames;
}

int ClipStart(int clip, int frames) { return clip * frames; }
int ClipLength(int clip, int frames) { return frames - clip; }

int BodyFrame(int clip, int offset, int frames, bool opposite) {
  const bool reversed = (clip != 0) != opposite;
  return reversed ? frames - 1 - offset : clip + offset;
}

std::shared_ptr<const ProjectSnapshot> MakeBodyProject(const File& fixture, int frames, bool opposite,
                                                        const File* document = nullptr) {
  const auto asset = InspectTimelineMovie(fixture);
  const auto& video = asset.tracks.at(0);
  const auto& audio = asset.tracks.at(1);
  Check(video.presentation == Range(0, frames), "Body fixture must be a 24 fps movie starting at zero");
  Project project(kBodyCanvas);
  project.Edit([&](ProjectEdit& edit) {
    const auto id = edit.Import(asset);
    const auto v = edit.AddTrack(TrackKind::Video), a = edit.AddTrack(TrackKind::Audio);
    for (int i = 0; i < 2; ++i) {
      const auto interval = Range(ClipStart(i, frames), ClipStart(i, frames) + ClipLength(i, frames));
      const auto clip = edit.InsertClip(v, {{id, video.id, Range(i, frames), (i != 0) != opposite}, interval});
      edit.InsertClip(a, {{id, audio.id, Range(i, frames)}, interval});
      for (const auto hand : kHands)
        edit.AddSticker(clip, {TrackingStickerResource(hand), {-0.026F, -0.03F, 0.052F, 0.06F},
                               TrackingAnchor{hand, "wrist", {}}});
    }
  });
  if (!document) return project.Snapshot();
  project.Save(*document);
  return Project::Open(*document).Snapshot();
}

std::optional<Point> WristSticker(const Bytes& pixels, TrackedPart part) {
  std::uint64_t x = 0, y = 0, count = 0;
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    const auto r = std::to_integer<int>(pixels[i]);
    const auto g = std::to_integer<int>(pixels[i + 1]);
    const auto b = std::to_integer<int>(pixels[i + 2]);
    const bool matches = part == TrackedPart::LeftHand ? r < 88 && g > 204 && b > 210 : r > 210 && g < 116 && b > 145;
    if (!matches) continue;
    x += i / 4 % kBodyCanvas.width;
    y += i / 4 / kBodyCanvas.width;
    ++count;
  }
  if (count < 30) return std::nullopt;
  return Point{float(x) / count, float(y) / count};
}

void CheckWrist(const std::optional<Point>& marker, Point expected, int clip, int frame, int side) {
  if (marker && (expected == Point{} || std::hypot(marker->x - expected.x, marker->y - expected.y) > 24.F)) {
    throw std::runtime_error("Unexpected hand sticker in clip " + std::to_string(clip + 1) + " at source frame " +
                             std::to_string(frame) + (side ? " (right)" : " (left)"));
  }
}

using ClipPictures = std::array<std::vector<Bytes>, 2>;

ClipPictures VerifyBodySampling(TimelineSession& session, MovieRenderer& renderer,
                                const std::vector<WristFrame>& wrists) {
  const int frames = int(wrists.size());
  ClipPictures reference;
  std::array<std::array<int, 2>, 2> counts{};
  int shared = 0;
  bool partial_occlusion = false;
  for (int clip = 0; clip < 2; ++clip) {
    reference[clip].resize(frames);
    for (int offset = 0; offset < ClipLength(clip, frames); ++offset) {
      @autoreleasepool {
        const auto sample = session.SampleVideo(Tick(ClipStart(clip, frames) + offset));
        const int source = BodyFrame(clip, offset, frames, false);
        const auto& video = std::get<VideoFrame>(sample.inputs.front());
        Check(video.format.visible.width == kBodyCanvas.width && video.format.visible.height == kBodyCanvas.height &&
              video.pts == Tick(source), "Body preview sampled the wrong source frame");
        auto pixels = ReadPicture(renderer, sample);
        for (int side = 0; side < 2; ++side) {
          const auto marker = WristSticker(pixels, kHands[side]);
          CheckWrist(marker, wrists[source][clip][side], clip, source, side);
          counts[clip][side] += bool(marker);
        }
        if (clip == 0 && wrists[source][0][0] != Point{} && wrists[source][0][1] == Point{} &&
            WristSticker(pixels, TrackedPart::LeftHand)) partial_occlusion = true;
        if (clip) {
          const auto a = WristSticker(reference[0][source], TrackedPart::RightHand);
          const auto b = WristSticker(pixels, TrackedPart::RightHand);
          const auto expected_a = wrists[source][0][1], expected_b = wrists[source][1][1];
          if (a != b && expected_a != Point{} && expected_b != Point{} &&
              std::hypot(expected_a.x - expected_b.x, expected_a.y - expected_b.y) > 48.F) ++shared;
        }
        reference[clip][source] = std::move(pixels);
      }
    }
  }
  Check(counts[0][0] > 1 && counts[0][1] > 1 && counts[1][0] + counts[1][1] > 1 && shared > 0,
        "The fixture did not exercise both hand sides and both clip references at shared PTS");
  Check(partial_occlusion, "The fixture did not retain the visible wrist during partial occlusion");
  Check(WristSticker(reference[0].front(), TrackedPart::LeftHand) &&
        WristSticker(reference[0].front(), TrackedPart::RightHand) &&
        WristSticker(reference[0].back(), TrackedPart::LeftHand) &&
        WristSticker(reference[0].back(), TrackedPart::RightHand), "Reference hand stickers did not recover after crossing");
  std::cout << "ncnn body clips: left/right anchors " << counts[0][0] << '/' << counts[0][1] << " and "
            << counts[1][0] << '/' << counts[1][1] << ", " << shared << " shared-PTS frames with distinct clip results\n";
  return reference;
}

void VerifyBodyDirection(TimelineSession& reverse, MovieRenderer& renderer, const ClipPictures& reference, int frames) {
  for (int clip = 0; clip < 2; ++clip) {
    for (int offset = 0; offset < ClipLength(clip, frames); ++offset) {
      @autoreleasepool {
        const auto at = Tick(ClipStart(clip, frames) + offset);
        const auto source = BodyFrame(clip, offset, frames, true);
        Check(ReadPicture(renderer, reverse.SampleVideo(at)) == reference[clip][source],
              "Changing the playback direction changed body tracking pixels");
      }
    }
  }
}

void VerifyBodySeeks(TimelineSession& session, MovieRenderer& renderer, const ClipPictures& reference,
                     int frames, bool opposite) {
  for (int clip = 0; clip < 2; ++clip) {
    for (const int offset : {2, 0, ClipLength(clip, frames) - 1, ClipLength(clip, frames) / 2}) {
      @autoreleasepool {
        session.Reset();
        const auto source = BodyFrame(clip, offset, frames, opposite);
        const auto at = Tick(ClipStart(clip, frames) + offset);
        Check(ReadPicture(renderer, session.SampleVideo(at)) == reference[clip][source],
              "Seeking changed a body track or wrist anchor");
      }
    }
  }
}

void VerifyBodyClock(TimelineSession& session, TimeRange range, MovieRenderer& renderer,
                     const ClipPictures& reference, int frames) {
  MetalTexture mailbox({float(kBodyCanvas.width), float(kBodyCanvas.height)});
  std::stop_source stop;
  std::optional<MediaTimestamp> last;
  int stage = 0;
  const int second = 2 * frames - 3;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  session.Reset();
  const auto stats = PlayTimeline(session, range, renderer, mailbox, Tick(0), true,
      [&](MediaTimestamp presented) -> std::optional<MediaTimestamp> {
        Check(std::chrono::steady_clock::now() < deadline, "Clocked body preview timed out");
        const auto frame = presented.Rescale(kTimelinePreviewRate)->ticks;
        if (last != presented) {
          const int clip = frame >= frames;
          const auto source = BodyFrame(clip, int(frame) - ClipStart(clip, frames), frames, false);
          MetalTexture::Origin origin;
          const auto texture = mailbox.AcquireFrame(origin);
          Check(texture && origin == MetalTexture::Origin::TopLeft &&
                ReadPreviewPixels(texture) == reference[clip][source], "Clocked preview used the wrong body reference");
          last = presented;
        }
        if (stage == 0 && frame >= 2) { ++stage; return Tick(second); }
        if (stage == 1 && frame == second) { ++stage; return Tick(2); }
        if (stage == 2 && frame >= 2) stop.request_stop();
        return std::nullopt;
      }, stop.get_token());
  Check(stage == 2 && stats.clocked_frames > 0, "Clocked body preview did not seek between both clips");
  std::cout << "ncnn body clocked preview: two clip seeks, " << stats.audio_underruns << " audio underruns\n";
}

void VerifyBodyExport(const std::shared_ptr<const ProjectSnapshot>& project, const File& destination,
                      const MovieCodecProviders& codecs, const std::shared_ptr<AnalysisProvider>& provider,
                      MovieRenderer& renderer, const ClipPictures& reference, int frames) {
  FileExportSettings settings;
  settings.video = PreviewVideoConfig(kBodyCanvas);
  settings.video.frame_rate_numerator = kTimelinePreviewRate;
  settings.policy = kPolicy;
  const auto expected = [&](int at) -> const Bytes& {
    const int clip = at >= frames;
    return reference[clip].at(BodyFrame(clip, at - ClipStart(clip, frames), frames, false));
  };
  int written = 0;
  ExportTimeline(project, destination, codecs, settings, [&](const TimelineFrame& frame, const VideoEncoderConfig& config,
                                                              MediaTimestamp duration) {
    @autoreleasepool {
      auto surface = renderer.Draw(frame);
      Check(frame.pts == Tick(written) && ReadPreviewPixels(huxerui::avkit::macos::GetMetalTexture(surface)) == expected(written),
            "Body export changed the source time or pre-encode pixels");
      ++written;
      return renderer.EncoderFrame(surface, config, frame.pts, duration);
    }
  }, {}, provider);
  Check(written == 2 * frames - 1, "Body export frame count changed");
  MovieReader reader(ImportMovie(destination), codecs, TrackKind::Video, Tick(0), kPolicy);
  const auto plan = std::make_shared<render::RenderPlan>(render::RenderPlan{{kBodyCanvas, render::PixelFormat::Bgra8Unorm},
                                                                          {{0, render::Blend::Replace}}});
  float drift = 0;
  for (int i = 0; i < written; ++i) {
    @autoreleasepool {
      auto frame = reader.ReadVideo();
      Check(frame && frame->pts == Tick(i) && frame->duration == Tick(1), "Body reimport timing changed");
      const auto pixels = ReadPicture(renderer, {frame->pts, plan, {*frame}, {}});
      for (const auto hand : kHands) {
        const auto before = WristSticker(expected(i), hand);
        const auto after = WristSticker(pixels, hand);
        Check(bool(before) == bool(after), "Body reimport changed wrist sticker visibility");
        if (after) drift = std::max(drift, std::hypot(after->x - before->x, after->y - before->y));
      }
    }
  }
  Check(!reader.ReadVideo() && drift <= 2.F, "Body reimport changed frame count or shifted a wrist sticker");
  std::cout << "ncnn body export: " << written << " exact pre-encode frames and MP4 PTS, max wrist sticker shift "
            << drift << " px\n";
}

} // namespace

void VerifyTimelineNcnnBody(std::string_view source, std::string_view model_directory, std::string_view landmarks,
                            std::string_view directory) {
  @autoreleasepool {
    const File fixture(source), output(directory);
    const auto document = output.Child("body-hand.avkit"), video = output.Child("body-hand.mp4");
    Check(output.CreateDirectories() && !document.Exists() && !video.Exists(), "Body verification needs a fresh directory");
    const auto wrists = ReadWrists(File(landmarks));
    const auto frames = int(wrists.size());
    const auto provider = std::make_shared<CountedBodyProvider>(CreateTimelineNcnnProvider(model_directory));
    const auto codecs = TimelineProviders();
    const auto project = MakeBodyProject(fixture, frames, false, &document);
    TimelineSession session(project, codecs, kPolicy, {}, provider);
    MovieRenderer renderer(kBodyCanvas);
    const auto reference = VerifyBodySampling(session, renderer, wrists);
    TimelineSession reversed(MakeBodyProject(fixture, frames, true), codecs, kPolicy, {}, provider);
    VerifyBodyDirection(reversed, renderer, reference, frames);
    const int samples = 2 * frames - 1;
    Check(provider->Count() == 2 * samples, "Ordered body sampling did not evaluate each source PTS once per clip");
    VerifyBodySeeks(session, renderer, reference, frames, false);
    VerifyBodySeeks(reversed, renderer, reference, frames, true);
    VerifyBodyClock(session, project->Range(), renderer, reference, frames);
    Check(provider->Count() == 2 * samples, "Seeking resubmitted cached body analysis");
    VerifyBodyExport(project, video, codecs, provider, renderer, reference, frames);
    Check(provider->Count() == 3 * samples, "Export did not build its own ordered body results once per clip");
    std::cout << "ncnn body preview: " << samples << " source samples match in both directions and at 16 cached seeks\n";
  }
}

} // namespace avkit_preview
