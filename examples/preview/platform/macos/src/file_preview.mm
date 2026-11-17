#include "file_preview.h"
#include "verification/codec_verification.h"
#include "metal_preview.h"
#include "movie_renderer.h"
#include "sticker_scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <huxerui/avkit/file_movie.h>
#include <huxerui/macos/external_texture.h>
#include <huxerui/media_playback.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using namespace huxerui::media;
using huxerui::macos::MetalTexture;
namespace metal = huxerui::avkit::macos;
using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

PixelSize OutputSize(const MovieAsset& asset) {
  const auto& description = asset.video.descriptions.front();
  const auto crop = description.visible.value_or(VideoRectangle{0, 0, *description.coded_width, *description.coded_height});
  auto width = crop.width;
  auto height = crop.height;
  if (asset.video.rotation == VideoRotation::Clockwise90 || asset.video.rotation == VideoRotation::Clockwise270)
    std::swap(width, height);
  Check(width >= 2 && height >= 2 && width <= 8192 && height <= 8192, "Unsupported fitted video size");
  return {width + (width % 2), height + (height % 2)};
}

FileExportSettings ExportSettings(PixelSize size, bool software_only) {
  FileExportSettings result;
  result.video = PreviewVideoConfig(size);
  result.policy = software_only ? CodecPolicy::SoftwareOnly : CodecPolicy::Auto;
  return result;
}

void LogFallback(const CodecFallback& event) {
  using enum CodecOperation;
  const char* operation = "";
  switch (event.operation) {
    case VideoDecode: operation = "video decode"; break;
    case AudioDecode: operation = "audio decode"; break;
    case VideoEncode: operation = "video encode"; break;
    case AudioEncode: operation = "audio encode"; break;
  }
  std::cout << operation << " fallback " << event.from << " -> " << event.to << " (" << event.cause.message << ')';
  if (event.resume_at) std::cout << " at " << event.resume_at->ticks << '/' << event.resume_at->timescale;
  std::cout << std::endl;
}

MovieCodecProviders Providers() {
  return {CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()};
}

struct PlaybackStats {
  std::size_t published{}, dropped{};
  double maximum_lag_ms{};
  std::uint64_t underruns{};
  std::optional<MediaTimestamp> first_audio, first_video, last_video;
};

using TakeSeek = std::function<std::optional<MediaTimestamp>(std::size_t published)>;

PlaybackStats PlayMovie(const MovieAsset& asset, MediaTimestamp at, MetalTexture& mailbox, bool muted,
                        const MovieCodecProviders& providers, const OnCodecFallback& report, const TakeSeek& take_seek,
                        std::stop_token stop = {}, bool cue = false) {
  MovieRenderer renderer(OutputSize(asset));
  MovieReader reader(asset, providers, CodecPolicy::Auto, report);
  if (at.ticks) reader.Seek(at);
  auto playback = Require(DecodedPlayback::Create({AudioPlaybackConfig{.format = asset.pcm}, true}));
  auto epoch = playback.Epoch();
  Require(playback.SetMuted(muted));
  Require(playback.Play());
  std::optional<VideoFrame> picture;
  std::optional<AudioFrame> sound;
  bool video_finished = false, audio_finished = false;
  bool audio_eof = false, ended_reported = false;
  PlaybackStats stats;
  auto last_progress = std::chrono::steady_clock::now();
  std::optional<MediaTimestamp> position;
  while (!stop.stop_requested()) {
    @autoreleasepool {
      const auto snapshot = playback.Snapshot();
      if (snapshot.error) throw std::runtime_error(snapshot.error->message);
      if (take_seek && snapshot.state != PlaybackState::Starting && snapshot.state != PlaybackState::Resetting) {
        if (auto requested = take_seek(stats.published)) {
          epoch = Require(playback.Reset());
          picture.reset();
          sound.reset();
          reader.Seek(*requested);
          Require(playback.Play());
          video_finished = audio_finished = audio_eof = ended_reported = false;
          stats = {};
          position.reset();
          last_progress = std::chrono::steady_clock::now();
          continue;
        }
      }
      if (snapshot.state == PlaybackState::Ended) {
        stats.underruns = snapshot.audio_underruns;
        if (!stop.stop_possible()) break;
        if (!ended_reported) {
          std::cout << "Preview published " << stats.published << " frames, dropped " << stats.dropped
                    << ", maximum audio-clock lag " << stats.maximum_lag_ms << " ms, underruns " << stats.underruns << std::endl;
          ended_reported = true;
        }
        std::this_thread::sleep_for(15ms);
        continue;
      }
      bool progressed = position != snapshot.position;
      position = snapshot.position;
      if (!video_finished) {
        if (!picture) picture = reader.ReadVideo();
        if (!picture) {
          Require(playback.FinishVideo(epoch));
          video_finished = true;
        } else if (Require(playback.TrySubmitVideo(epoch, *picture)) == SubmitStatus::Accepted) {
          picture.reset();
          progressed = true;
        }
      }
      if (!audio_finished) {
        if (!sound && !audio_eof) {
          sound = reader.ReadAudio();
          audio_eof = !sound;
        }
        if (sound && (Require(playback.TrySubmitAudio(epoch, *sound)) == SubmitStatus::Accepted)) {
          if (!stats.first_audio) stats.first_audio = sound->pts;
          sound.reset();
          progressed = true;
        } else if (audio_eof && snapshot.state != PlaybackState::Starting && snapshot.state != PlaybackState::Resetting) {
          Require(playback.FinishAudio(epoch));
          audio_finished = true;
        }
      }
      const auto presented = Require(playback.PresentVideo(epoch, [&](const VideoFrame& frame) {
        const auto composed = renderer.Draw(frame, cue);
        mailbox.Publish({.texture = metal::GetMetalTexture(composed), .alpha = MetalTexture::Alpha::Opaque});
        if (!stats.first_video) stats.first_video = frame.pts;
        stats.last_video = frame.pts;
      }));
      stats.dropped += presented.dropped;
      if (presented.lag_ms) {
        ++stats.published;
        stats.maximum_lag_ms = std::max(stats.maximum_lag_ms, std::abs(*presented.lag_ms));
        progressed = true;
      }
      if (progressed) last_progress = std::chrono::steady_clock::now();
      else {
        Check(std::chrono::steady_clock::now() - last_progress < 15s, "Audio or video playback stalled");
        std::this_thread::sleep_for(2ms);
      }
    }
  }
  mailbox.Finish();
  return stats;
}

struct PreviewOptions {
  MovieAsset asset;
  MediaTimestamp start;
};
std::optional<PreviewOptions> preview;
std::mutex preview_seek_mutex;
std::optional<MediaTimestamp> preview_seek;
MediaTimestamp preview_midpoint;
std::jthread preview_worker;

huxerui::Size LogicalSize(PixelSize size) { return {float(size.width), float(size.height)}; }

void Export(const MovieAsset& asset, const File& output, const MovieCodecProviders& providers, bool software_only,
            const OnCodecFallback& report, bool cue = false) {
  MovieRenderer renderer(OutputSize(asset));
  ExportMovie(asset, output, providers, ExportSettings(OutputSize(asset), software_only),
              [&](const VideoFrame& input, const VideoEncoderConfig& config) { return renderer.EncodeFrame(input, config, cue); },
              report);
}

struct TimelineStats {
  std::vector<std::pair<MediaTimestamp, MediaTimestamp>> pictures;
  std::size_t platform_frames{}, cpu_frames{};
  std::int64_t samples{};
};

TimelineStats ReadTimeline(const MovieAsset& asset, const MovieCodecProviders& providers,
                           CodecPolicy policy, const OnCodecFallback& report = {}, bool check_cue = false) {
  MovieReader reader(asset, providers, policy, report);
  TimelineStats stats;
  while (auto picture = reader.ReadVideo()) {
    Check(picture->Validate() && picture->duration, "Decoded picture has invalid data or timing");
    if (check_cue) Check(HasStickerCue(*picture), "Reimported export misplaced its ECS sticker at the decoded video PTS");
    Check(stats.pictures.empty() || picture->pts.Compare(stats.pictures.back().first) == 1,
          "Video PTS repeated or moved backwards");
    stats.pictures.emplace_back(picture->pts, *picture->duration);
    if (picture->Storage() == VideoStorage::Platform) ++stats.platform_frames;
    else ++stats.cpu_frames;
  }
  while (auto audio = reader.ReadAudio()) {
    Check(audio->Validate() && audio->pts == MediaTimestamp{stats.samples, asset.pcm.sample_rate},
          "Decoded audio PTS is not contiguous");
    stats.samples += audio->sample_count;
  }
  Check(!stats.pictures.empty() && stats.samples > 0, "Expected both audio and video in the output");
  return stats;
}

} // namespace

void ConfigureFilePreview(std::string_view input, std::string_view start_seconds) {
  const std::string argument(start_seconds);
  char* end = nullptr;
  const double seconds = std::strtod(argument.c_str(), &end);
  Check(end != argument.c_str() && end == argument.c_str() + argument.size() &&
            std::isfinite(seconds) && seconds >= 0 &&
            seconds < double(std::numeric_limits<std::int64_t>::max() / 1000000),
        "Preview start must be a nonnegative number of seconds");
  const MediaTimestamp at{std::llround(seconds * 1000000), 1000000};
  auto asset = ImportMovie(File(input));
  Check(at.Compare(asset.end) == -1, "Preview start is outside the file");
  preview_midpoint = asset.end.Rescale(asset.pcm.sample_rate).value();
  preview_midpoint.ticks /= 2;
  preview.emplace(PreviewOptions{std::move(asset), at});
}

std::shared_ptr<ExternalTexture> CreateFilePreview() {
  if (!preview) return {};
  auto mailbox = std::make_shared<MetalTexture>(LogicalSize(OutputSize(preview->asset)));
  preview_worker = std::jthread([options = std::move(*preview), mailbox](std::stop_token stop) {
    @autoreleasepool {
      try {
        const auto take_seek = [](std::size_t) {
          std::lock_guard lock(preview_seek_mutex);
          return std::exchange(preview_seek, std::nullopt);
        };
        PlayMovie(options.asset, options.start, *mailbox, false, Providers(), LogFallback, take_seek, stop);
      } catch (const std::exception& error) {
        mailbox->Finish();
        std::cerr << "File preview failed: " << error.what() << std::endl;
      }
    }
  });
  preview.reset();
  return mailbox;
}

bool HasFilePreview() { return preview_worker.joinable(); }

void SeekFilePreview(bool midpoint) {
  std::lock_guard lock(preview_seek_mutex);
  preview_seek = midpoint ? preview_midpoint : MediaTimestamp{};
}

void StopFilePreview() {
  if (preview_worker.joinable()) {
    preview_worker.request_stop();
    preview_worker.join();
  }
}

void ExportFilePreview(std::string_view input, std::string_view output, bool software_only) {
  @autoreleasepool {
    Export(ImportMovie(File(input)), File(output), Providers(), software_only, LogFallback);
    std::cout << "Published MP4 " << output << std::endl;
  }
}

void VerifyFilePreview(std::string_view input, std::string_view output) {
  @autoreleasepool {
    const auto asset = ImportMovie(File(input));
    const auto providers = Providers();
    const auto baseline = ReadTimeline(asset, providers, CodecPolicy::Auto);
    Check(baseline.platform_frames == baseline.pictures.size() && baseline.pictures.size() > 16,
          "Verification needs a short clip accepted by the platform decoder");
    const auto software = ReadTimeline(asset, providers, CodecPolicy::SoftwareOnly);
    Check(software.cpu_frames == software.pictures.size() && software.pictures == baseline.pictures &&
              software.samples == baseline.samples,
          "Software and platform decoding disagree on the presentation interval");

    std::vector<CodecFallback> fallbacks;
    const auto report = [&](const CodecFallback& event) { fallbacks.push_back(event); LogFallback(event); };
    using verification::ObservedCodecProvider;
    using verification::VideoFaults;
    const MovieCodecProviders failing_decoder{
        std::make_shared<ObservedCodecProvider>(providers.platform, nullptr, VideoFaults{.decode_after = 8}), providers.software};
    const auto recovered = ReadTimeline(asset, failing_decoder, CodecPolicy::Auto, report);
    Check(recovered.platform_frames > 0 && recovered.cpu_frames > 0 && recovered.pictures == baseline.pictures &&
              recovered.samples == baseline.samples && fallbacks.size() == 1 &&
              fallbacks.back().operation == CodecOperation::VideoDecode && fallbacks.back().resume_at,
          "Decoder fallback did not preserve frame timing and audio continuity");

    const auto halfway = baseline.pictures[baseline.pictures.size() / 2].first.Rescale(asset.pcm.sample_rate).value();
    const MediaTimestamp seek{halfway.ticks + 1, halfway.timescale};
    MovieReader reader(asset, providers, CodecPolicy::Auto);
    (void)reader.ReadVideo();
    (void)reader.ReadAudio();
    reader.Seek(seek);
    const auto audio = reader.ReadAudio();
    const auto video = reader.ReadVideo();
    Check(audio && audio->pts == seek && video && video->pts == baseline.pictures[baseline.pictures.size() / 2].first,
          "Seek did not recover the video frame spanning its target and trim PCM to the requested sample");
    reader.Seek({});
    Check(reader.ReadVideo().value().pts == baseline.pictures.front().first && reader.ReadAudio().value().pts == MediaTimestamp{},
          "Backward seek did not reset decoder history");

    MetalTexture mailbox(LogicalSize(OutputSize(asset)));
    const MovieCodecProviders failing_preview{
        std::make_shared<ObservedCodecProvider>(providers.platform, nullptr, VideoFaults{.decode_after = 8}), providers.software};
    fallbacks.clear();
    bool requested = false;
    const auto stats = PlayMovie(asset, {}, mailbox, true, failing_preview, report, [&](std::size_t published)
        -> std::optional<MediaTimestamp> {
      if (published < 2 || requested) return std::nullopt;
      requested = true;
      return seek;
    }, {}, true);
    MetalTexture::Origin origin;
    auto snapshot = mailbox.AcquireFrame(origin);
    Check(snapshot != nil && origin == MetalTexture::Origin::TopLeft && requested && stats.published > 0 &&
              stats.first_audio == seek && stats.first_video && stats.first_video->Compare(video->pts) >= 0 &&
              stats.maximum_lag_ms < 100 && stats.underruns == 0 && fallbacks.size() == 1 &&
              fallbacks.back().operation == CodecOperation::VideoDecode,
          "Seeked preview did not publish synchronized Metal frames using the audio device clock");
    Check(stats.last_video && HasPreviewStickerCue(snapshot, *stats.last_video),
          "File preview misplaced its ECS sticker at the last published PTS");
    std::cout << "Seek " << seek.ticks << '/' << seek.timescale << ", preview " << stats.published << " frames, dropped "
              << stats.dropped << ", maximum audio-clock lag " << stats.maximum_lag_ms << " ms, underruns "
              << stats.underruns << std::endl;

    const File destination(output);
    const MovieCodecProviders failing_encoder{
        std::make_shared<ObservedCodecProvider>(providers.platform, nullptr, VideoFaults{.encode_after = 5}), providers.software};
    fallbacks.clear();
    Export(asset, destination, failing_encoder, false, report, true);
    Check(fallbacks.size() == 1 && fallbacks.back().operation == CodecOperation::VideoEncode &&
              fallbacks.back().cause.code == CodecErrorCode::ResourceExhausted && !fallbacks.back().resume_at,
          "Export did not restart after an active platform encoder failed");

    const auto reimported = ImportMovie(destination);
    const auto result = ReadTimeline(reimported, providers, CodecPolicy::SoftwareOnly, {}, true);
    Check(reimported.pcm == asset.pcm && result.pictures == baseline.pictures && result.samples == baseline.samples,
          "Reimported MP4 did not preserve exact frame PTS, durations and presented audio sample count");
    std::cout << "Reimport verified " << result.pictures.size() << " video PTS/durations and " << result.samples
              << " contiguous audio samples." << std::endl;
    std::cout << "ECS sticker position verified in the file preview and every reimported export frame." << std::endl;
  }
}

} // namespace avkit_preview
