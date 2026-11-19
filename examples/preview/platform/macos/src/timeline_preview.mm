#include "timeline_preview.h"

#include "metal_preview.h"
#include "movie_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <huxerui/media_playback.h>

namespace avkit_preview {
namespace {

using namespace std::chrono_literals;
using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::media;
using huxerui::macos::MetalTexture;

constexpr auto kPreviewPcmCapacity = 250ms;
constexpr double kMaxPreviewLagMs = 100;

template<class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

MediaTimestamp ToGrid(MediaTimestamp value, std::uint32_t rate) {
  auto converted = value.Rescale(rate);
  if (!converted) throw std::invalid_argument("Timeline preview has an invalid playback time");
  return *converted;
}

struct PreviewOptions {
  std::shared_ptr<const ProjectSnapshot> project;
  MediaTimestamp start;
  std::shared_ptr<analysis::AnalysisProvider> analysis;
  CodecPolicy policy;
};

std::optional<PreviewOptions> preview;
std::mutex seek_mutex;
std::optional<bool> pending_seek;
std::shared_ptr<const ProjectSnapshot> pending_project;
std::jthread worker;

render::ColorSurface DrawTimelineSample(TimelineSession& session, MovieRenderer& renderer, MediaTimestamp at,
                                        std::stop_token stop) {
  const auto frame = session.SampleVideo(at, render::PixelFormat::Bgra8Unorm, stop,
                                         [&](const auto& video) { return renderer.ImportFrame(video); }, 12ms);
  return renderer.Draw(frame);
}

} // namespace

MovieCodecProviders TimelineProviders() { return {CreateLibMediaCodecProvider(), CreateFFmpegCodecProvider()}; }

void RenderTimelineSample(TimelineSession& session, MovieRenderer& renderer, MediaTimestamp at, MetalTexture& mailbox,
                          std::stop_token stop) {
  PublishPreviewSurface(mailbox, DrawTimelineSample(session, renderer, at, stop));
}

TimelinePlaybackStats PlayTimeline(TimelineSession& session, TimeRange range, MovieRenderer& renderer,
                                   MetalTexture& mailbox, MediaTimestamp at, bool muted, const TimelineSeek& seek,
                                   std::stop_token stop) {
  auto pcm = session.PcmFormat();
  auto end = ToGrid(range.end, pcm.sample_rate);
  auto next = ToGrid(at, pcm.sample_rate);
  if (range.end != end || at.Compare(range.start) == -1 || at.Compare(range.end) != -1 || next.ticks >= end.ticks)
    throw std::invalid_argument("Timeline preview needs an in-range start and a sample-aligned end");

  const auto open_playback = [&](const AudioFormat& format) {
    auto result = Require(DecodedPlayback::Create({AudioPlaybackConfig{.format = format, .buffer_capacity = kPreviewPcmCapacity}}));
    Require(result.SetMuted(muted));
    return result;
  };
  auto playback = open_playback(pcm);
  auto epoch = playback.Epoch();
  RenderTimelineSample(session, renderer, at, mailbox, stop);
  auto presented = at;
  std::optional<AudioFrame> sound;
  bool sealed = false;
  bool play_requested = false;
  auto last_progress = std::chrono::steady_clock::now();
  std::optional<MediaTimestamp> position;
  TimelinePlaybackStats stats;
  std::uint64_t retired_underruns = 0;

  const auto refill = [&](MediaTimestamp buffered) {
    const std::int64_t capacity = (kPreviewPcmCapacity.count() * pcm.sample_rate + 999) / 1000;
    const auto remaining = end.ticks - next.ticks;
    const auto count = std::min(remaining, capacity - buffered.ticks);
    // Fill the available window before video can hold the producer through another decode or render.
    if (!sound && remaining > 0 && count >= std::min<std::int64_t>({1024, capacity, remaining}))
      sound = session.SampleAudio(next, static_cast<std::uint32_t>(count));
    bool submitted = false;
    if (sound && Require(playback.TrySubmitAudio(epoch, *sound)) == SubmitStatus::Accepted) {
      next.ticks += sound->sample_count;
      sound.reset();
      submitted = true;
    }
    if (next == end) {
      Require(playback.FinishAudio(epoch));
      sealed = true;
    }
    if (submitted && !play_requested) {
      Require(playback.Play());
      play_requested = true;
    }
    return submitted || sealed;
  };

  while (!stop.stop_requested()) {
    @autoreleasepool {
      auto state = playback.Snapshot();
      if (state.error) throw std::runtime_error(state.error->message);
      stats.audio_underruns = retired_underruns + state.audio_underruns;
      bool progressed = position != state.position;
      position = state.position;
      if (state.state != PlaybackState::Starting && state.state != PlaybackState::Resetting) {
        if (const auto requested = seek(presented)) {
          const auto format = session.PcmFormat();
          const auto new_end = ToGrid(range.end, format.sample_rate);
          const auto anchor = ToGrid(*requested, format.sample_rate);
          if (new_end != range.end || requested->Compare(range.start) == -1 || requested->Compare(range.end) != -1 ||
              anchor.ticks >= new_end.ticks) throw std::out_of_range("Preview seek is outside the timeline or the PCM grid");
          retired_underruns += playback.Snapshot().audio_underruns;
          if (format != pcm) {
            pcm = format;
            end = new_end;
            playback = open_playback(pcm);
            epoch = playback.Epoch();
          } else {
            epoch = Require(playback.Reset());
          }
          session.Seek(*requested);
          sound.reset();
          sealed = false;
          play_requested = false;
          position.reset();
          next = anchor;
          RenderTimelineSample(session, renderer, *requested, mailbox, stop);
          presented = *requested;
          last_progress = std::chrono::steady_clock::now();
          continue;
        }
        if (stop.stop_requested()) break;
        if (!sealed && refill(state.audio_buffered_duration)) {
          progressed = true;
          state = playback.Snapshot();
        }
      }
      if (state.position && (state.state == PlaybackState::Playing || state.state == PlaybackState::Ended)) {
        auto draw_at = ToGrid(*state.position, kTimelinePreviewRate);
        if (draw_at.Compare(*state.position) == 1 || draw_at.Compare(range.end) >= 0) --draw_at.ticks;
        if (draw_at.Compare(presented) == 1 && draw_at.Compare(range.end) == -1) {
          const auto surface = DrawTimelineSample(session, renderer, draw_at, stop);
          const auto clock = playback.Snapshot().position.value_or(*state.position);
          const auto frame_on_pcm_clock = ToGrid(draw_at, clock.timescale);
          const auto lag = 1000.0 * (clock.ticks - frame_on_pcm_clock.ticks) / clock.timescale;
          if (lag < kMaxPreviewLagMs) {
            PublishPreviewSurface(mailbox, surface);
            stats.maximum_video_lag_ms = std::max(stats.maximum_video_lag_ms, lag);
            ++stats.clocked_frames;
            presented = draw_at;
          } else {
            // Keep the last publication until a current frame can catch up with the PCM clock.
            ++stats.late_video_frames;
          }
          progressed = true;
        }
      }
      if (progressed || state.state == PlaybackState::Ended) last_progress = std::chrono::steady_clock::now();
      else if (std::chrono::steady_clock::now() - last_progress > 15s) throw std::runtime_error("Timeline playback stalled");
      if (!progressed) std::this_thread::sleep_for(2ms);
    }
  }
  stats.audio_underruns = retired_underruns + playback.Snapshot().audio_underruns;
  mailbox.Finish();
  return stats;
}

void ConfigureTimelinePreview(std::string_view path, std::string_view seconds,
                              std::shared_ptr<analysis::AnalysisProvider> analysis, bool software_only) {
  auto project = Project::Open(File(path)).Snapshot();
  std::size_t length = 0;
  const auto value = std::stod(std::string(seconds), &length);
  const auto end = project->Range().end;
  if (length != seconds.size() || !std::isfinite(value) || value < 0 ||
      value >= double(end.ticks) / end.timescale || value >= double(std::numeric_limits<std::int64_t>::max()) / 1000000)
    throw std::invalid_argument("Preview start is outside the timeline");
  preview = PreviewOptions{std::move(project), {std::llround(value * 1000000), 1000000}, std::move(analysis),
                           software_only ? CodecPolicy::SoftwareOnly : CodecPolicy::Auto};
}

bool HasTimelinePreview() { return preview.has_value(); }

std::shared_ptr<ExternalTexture> CreateTimelinePreview() {
  if (!preview) return {};
  const auto options = *preview;
  const auto canvas = options.project->canvas;
  auto mailbox = std::make_shared<MetalTexture>(huxerui::Size{float(canvas.width), float(canvas.height)});
  worker = std::jthread([mailbox, options](std::stop_token stop) {
    @autoreleasepool {
      try {
        MovieRenderer renderer(options.project->canvas);
        TimelineSession session(options.project, TimelineProviders(), options.policy, {}, options.analysis);
        const auto range = options.project->Range();
        PlayTimeline(session, range, renderer, *mailbox, options.start, false, [&](MediaTimestamp presented) -> std::optional<MediaTimestamp> {
          std::optional<bool> command;
          std::shared_ptr<const ProjectSnapshot> replacement;
          {
            std::scoped_lock lock(seek_mutex);
            command = std::exchange(pending_seek, std::nullopt);
            replacement = std::exchange(pending_project, {});
          }
          if (replacement) session.ApplyProject(std::move(replacement));
          else if (!command) return std::nullopt;
          if (!command) return presented;
          const auto end = ToGrid(range.end, session.PcmFormat().sample_rate);
          return MediaTimestamp{*command ? end.ticks / 2 : 0, end.timescale};
        }, stop);
      } catch (const std::exception& error) {
        if (!stop.stop_requested()) std::cerr << "Timeline preview: " << error.what() << '\n';
        mailbox->Finish();
      }
    }
  });
  return mailbox;
}

void SeekTimelinePreview(bool midpoint) {
  std::scoped_lock lock(seek_mutex);
  pending_seek = midpoint;
}

void ReplaceTimelinePreviewProject(std::shared_ptr<const ProjectSnapshot> project) {
  std::scoped_lock lock(seek_mutex);
  pending_project = std::move(project);
}

void StopTimelinePreview() {
  worker.request_stop();
  if (worker.joinable()) worker.join();
}

void ExportComposedTimeline(std::shared_ptr<const ProjectSnapshot> project, const File& file,
                            const MovieCodecProviders& providers, MovieRenderer& renderer, CodecPolicy policy,
                            OnCodecFallback on_fallback, std::shared_ptr<analysis::AnalysisProvider> analyzer) {
  FileExportSettings settings;
  settings.policy = policy;
  settings.video = PreviewVideoConfig(project->canvas);
  settings.video.frame_rate_numerator = kTimelinePreviewRate;
  ExportTimeline(std::move(project), file, providers, settings,
      [&](const TimelineFrame& frame, const VideoEncoderConfig& bound, MediaTimestamp duration) {
        @autoreleasepool {
          return renderer.EncoderFrame(renderer.Draw(frame), bound, frame.pts, duration);
        }
      }, std::move(on_fallback), std::move(analyzer), [&](const auto& video) { return renderer.ImportFrame(video); });
}

void ExportTimelinePreview(std::string_view path, std::string_view destination, bool software_only,
                           std::shared_ptr<analysis::AnalysisProvider> analyzer) {
  @autoreleasepool {
    auto project = Project::Open(File(path)).Snapshot();
    MovieRenderer renderer(project->canvas);
    ExportComposedTimeline(std::move(project), File(destination), TimelineProviders(), renderer,
                           software_only ? CodecPolicy::SoftwareOnly : CodecPolicy::Auto, {}, std::move(analyzer));
  }
}

} // namespace avkit_preview
