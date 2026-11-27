#include <huxerui/avkit/timeline.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <huxerui/media_playback.h>

#include "support/media_time.h"

namespace huxerui::avkit {
namespace {

using namespace std::chrono_literals;
using namespace huxerui::media;

constexpr auto kPreviewPcmCapacity = 250ms;
constexpr auto kVideoPrerollLookahead = 400ms;
constexpr auto kMaxPcmClockLead = 150ms;
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

double VideoLagMs(MediaTimestamp clock, MediaTimestamp video) {
  return 1000.0 * (clock.ticks - ToGrid(video, clock.timescale).ticks) / clock.timescale;
}

class PreviewAudioClock {
public:
  explicit PreviewAudioClock(MediaTimestamp start) : start_(start) {}

  void Reset(MediaTimestamp at) {
    start_ = at;
    anchor_.reset();
  }

  std::optional<MediaTimestamp> Read(const PlaybackSnapshot& state) {
    if (!state.position) return {};
    const auto confirmed = *state.position;
    if (state.state != PlaybackState::Playing || state.audio_buffered_duration.ticks <= 0) {
      Reset(confirmed);
      return confirmed;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!anchor_) {
      if (confirmed == start_) return confirmed;
      anchor_ = Anchor{confirmed.ticks, now};
    }
    // Device completions can arrive in batches. Pace video between them within the accepted PCM.
    const auto lead = std::min<std::int64_t>(state.audio_buffered_duration.ticks,
                                            kMaxPcmClockLead.count() * confirmed.timescale / 1000);
    const auto elapsed = std::chrono::duration<double>(now - anchor_->wall).count();
    const auto advance = std::min(elapsed * confirmed.timescale, double(confirmed.ticks + lead - anchor_->ticks));
    const auto estimate = anchor_->ticks + static_cast<std::int64_t>(advance);
    if (estimate < confirmed.ticks) {
      anchor_ = Anchor{confirmed.ticks, now};
      return confirmed;
    }
    return MediaTimestamp{estimate, confirmed.timescale};
  }

private:
  struct Anchor {
    std::int64_t ticks;
    std::chrono::steady_clock::time_point wall;
  };

  MediaTimestamp start_;
  std::optional<Anchor> anchor_;
};

} // namespace

TimelinePlaybackStats PlayClockedTimeline(TimelineSession& session, TimeRange range, MediaTimestamp at, bool muted,
    const TimelineSeek& seek, std::stop_token stop, const DrawTimelinePicture& draw,
    const PublishTimelinePicture& publish, TimelineIterationScope scope, std::uint32_t frame_rate, bool keep_open_at_end) {
  if (!frame_rate) throw std::invalid_argument("Timeline playback needs a positive video rate");
  auto pcm = session.PcmFormat();
  auto end = huxerui::avkit::detail::FloorOnGrid(range.end, pcm.sample_rate);
  auto next = ToGrid(at, pcm.sample_rate);
  if (at.Compare(range.start) == -1 || at.Compare(range.end) != -1 || next.ticks >= end.ticks)
    throw std::invalid_argument("Timeline preview needs an in-range start and a positive PCM duration");

  const auto open_playback = [&](const AudioFormat& format) {
    auto result = Require(DecodedPlayback::Create({AudioPlaybackConfig{.format = format, .buffer_capacity = kPreviewPcmCapacity}}));
    Require(result.SetMuted(muted));
    return result;
  };
  auto playback = open_playback(pcm);
  auto epoch = playback.Epoch();
  auto presented = at;
  PreviewAudioClock audio_clock(next);
  const auto samples_for = [&](std::chrono::milliseconds duration) -> std::int64_t {
    return (duration.count() * pcm.sample_rate + 999) / 1000;
  };
  const auto preroll_end = [&](MediaTimestamp anchor) {
    return MediaTimestamp{anchor.ticks + std::min<std::int64_t>(samples_for(kVideoPrerollLookahead), end.ticks - anchor.ticks),
                          pcm.sample_rate};
  };
  const auto preroll = [&](MediaTimestamp clock) {
    const auto until = preroll_end(ToGrid(clock, pcm.sample_rate));
    if (presented.Compare(until) == -1) session.PrerollVideoEntries({presented, until}, stop);
  };
  publish(draw(at, stop), at);
  std::optional<AudioFrame> sound;
  bool sealed = false;
  bool play_requested = false;
  auto last_progress = std::chrono::steady_clock::now();
  std::optional<MediaTimestamp> position;
  TimelinePlaybackStats stats;
  std::uint64_t retired_underruns = 0;

  const auto refill = [&](MediaTimestamp buffered) {
    const auto queue_capacity = samples_for(kPreviewPcmCapacity);
    const auto remaining = end.ticks - next.ticks;
    const auto count = std::min(remaining, queue_capacity - buffered.ticks);
    // Fill the available window before video can hold the producer through another decode or render.
    if (!sound && remaining > 0 && count >= std::min<std::int64_t>({1024, queue_capacity, remaining}))
      sound = session.SampleAudio(next, static_cast<std::uint32_t>(count), stop);
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
      preroll(presented);
    }
    return submitted || sealed;
  };

  const std::function<bool()> iteration = [&] {
    auto state = playback.Snapshot();
    if (state.error) throw std::runtime_error(state.error->message);
    stats.audio_underruns = retired_underruns + state.audio_underruns;
    bool progressed = position != state.position;
    position = state.position;
    if (state.state != PlaybackState::Starting && state.state != PlaybackState::Resetting) {
      if (const auto requested = seek ? seek(presented) : std::nullopt) {
        const auto format = session.PcmFormat();
        const auto new_end = huxerui::avkit::detail::FloorOnGrid(range.end, format.sample_rate);
        const auto anchor = ToGrid(*requested, format.sample_rate);
        if (requested->Compare(range.start) == -1 || requested->Compare(range.end) != -1 ||
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
        session.Seek(*requested, preroll_end(anchor));
        sound.reset();
        sealed = false;
        play_requested = false;
        position.reset();
        next = anchor;
        audio_clock.Reset(anchor);
        publish(draw(*requested, stop), *requested);
        presented = *requested;
        last_progress = std::chrono::steady_clock::now();
        return true;
      }
      if (stop.stop_requested()) return false;
      if (!sealed && refill(state.audio_buffered_duration)) {
        progressed = true;
        state = playback.Snapshot();
      }
    }
    const auto clock = audio_clock.Read(state);
    if (clock && (state.state == PlaybackState::Playing || state.state == PlaybackState::Ended)) {
      auto draw_at = ToGrid(*clock, frame_rate);
      if (draw_at.Compare(*clock) == 1 || draw_at.Compare(range.end) >= 0) --draw_at.ticks;
      if (progressed || draw_at.Compare(presented) == 1) preroll(*clock);
      if (draw_at.Compare(presented) == 1 && draw_at.Compare(range.end) == -1) {
        auto following = ToGrid(presented, frame_rate);
        if (following.Compare(presented) != 1) ++following.ticks;
        // Keep the next due frame when it still has time to render after a clock update.
        if (following.Compare(draw_at) == -1 &&
            VideoLagMs(*clock, following) < kMaxPreviewLagMs - 1000.0 / frame_rate) draw_at = following;
        const auto surface = draw(draw_at, stop);
        const auto after = playback.Snapshot();
        const auto current = audio_clock.Read(after).value_or(*clock);
        const auto lag = VideoLagMs(current, draw_at);
        if (lag >= kMaxPreviewLagMs) {
          // Keep the last publication until a current frame can catch up with the PCM clock.
          ++stats.late_video_frames;
        } else if (lag >= 0 || after.state == PlaybackState::Playing) {
          publish(surface, draw_at);
          stats.maximum_video_lag_ms = std::max(stats.maximum_video_lag_ms, lag);
          ++stats.clocked_frames;
          presented = draw_at;
        }
        progressed = true;
      }
    }
    if ((!seek || !keep_open_at_end) && state.state == PlaybackState::Ended) return false;
    if (progressed || state.state == PlaybackState::Ended) last_progress = std::chrono::steady_clock::now();
    else if (std::chrono::steady_clock::now() - last_progress > 15s) throw std::runtime_error("Timeline playback stalled");
    if (!progressed) std::this_thread::sleep_for(2ms);
    return true;
  };
  while (!stop.stop_requested() && (scope ? scope(iteration) : iteration())) {}
  stats.audio_underruns = retired_underruns + playback.Snapshot().audio_underruns;
  return stats;
}

} // namespace huxerui::avkit
