#include "timeline_audio_source.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <map>
#include <stdexcept>

namespace huxerui::avkit::detail {
namespace {

using namespace media;

struct AudioCursor {
  std::unique_ptr<MovieReader> reader;
  std::optional<AudioFrame> frame;
  std::int64_t next{};
};

struct AudioPlayback {
  AudioSource binding;
  AudioCursor cursor;
  std::int64_t reverse_start{};
  std::vector<std::int16_t> reverse_pcm;
  bool used{};
  std::int64_t conversion_start{};
  std::vector<std::int16_t> conversion_pcm;
};

} // namespace

struct TimelineAudioSource::Impl {
  const CodecProviders& providers;
  CodecPolicy policy;
  const OnCodecFallback& report;
  std::map<entityx::Entity, AudioPlayback> clips;

  void ReadPcm(const MovieAsset& movie, AudioCursor& cursor, std::int64_t source, std::int64_t count,
               std::stop_token stop, const auto& consume) {
    const auto stride = movie.pcm.ChannelCount() * sizeof(std::int16_t);
    if (!cursor.reader)
      cursor.reader = std::make_unique<MovieReader>(movie, providers, TrackKind::Audio,
          MediaTimestamp{source, movie.pcm.sample_rate}, policy, report);
    for (auto remaining = count; remaining > 0;) {
      if (!cursor.frame || source - cursor.frame->pts.ticks >= cursor.frame->sample_count)
        cursor.frame = cursor.reader->ReadAudio(stop);
      if (!cursor.frame) throw std::runtime_error("Timeline audio ended before its selected source range");
      const auto& frame = *cursor.frame;
      const auto offset = source - frame.pts.ticks;
      if (offset < 0 || offset >= frame.sample_count) throw std::runtime_error("Noncontiguous timeline audio");
      const auto take = std::min<std::int64_t>(remaining, frame.sample_count - offset);
      consume(frame.planes.front().AsBytes().subspan(std::size_t(offset) * stride, std::size_t(take) * stride));
      source += take;
      remaining -= take;
    }
    cursor.next = source;
  }

  void ReadReverse(const MovieAsset& movie, AudioPlayback& playback, std::int64_t trim, std::int64_t length,
                   std::int64_t offset, std::int64_t count, std::stop_token stop, const ConsumePcm& consume) {
    const auto channels = movie.pcm.ChannelCount();
    const std::int64_t window = movie.pcm.sample_rate / 4;
    auto source = trim + length - offset - 1;
    for (auto remaining = count; remaining > 0;) {
      // Anchor decode windows to the trim so a seek uses the same compressed-audio preroll as playback.
      const auto begin = trim + ((source - trim) / window) * window;
      const auto window_length = std::min(window, trim + length - begin);
      if (begin != playback.reverse_start || std::size_t(window_length) * channels != playback.reverse_pcm.size()) {
        std::vector<std::int16_t> pcm(std::size_t(window_length) * channels);
        AudioCursor cursor;
        auto* destination = pcm.data();
        ReadPcm(movie, cursor, begin, window_length, stop, [&](auto bytes) {
          std::memcpy(destination, bytes.data(), bytes.size());
          destination += bytes.size() / sizeof(std::int16_t);
        });
        playback.reverse_start = begin;
        playback.reverse_pcm = std::move(pcm);
      }
      const auto last = source - playback.reverse_start;
      const auto take = std::min(remaining, last + 1);
      const auto samples = std::span(playback.reverse_pcm).subspan(std::size_t(last + 1 - take) * channels,
                                                                   std::size_t(take) * channels);
      consume(std::as_bytes(samples), true);
      source -= take;
      remaining -= take;
    }
  }

  void ReadConverted(const MovieAsset& movie, AudioPlayback& playback, const AudioFormat& output,
                     std::int64_t trim, std::int64_t length, bool reverse, std::int64_t offset,
                     std::int64_t count, std::stop_token stop, const ConsumePcm& consume) {
    const auto input_rate = std::int64_t(movie.pcm.sample_rate);
    const auto output_rate = std::int64_t(output.sample_rate);
    const auto channels = movie.pcm.ChannelCount();
    const auto output_channels = output.ChannelCount();
    const auto point = [&](std::int64_t sample) {
      const auto phase = (sample % output_rate) * input_rate;
      return std::pair{std::min(length - 1, sample / output_rate * input_rate + phase / output_rate),
                       double(phase % output_rate) / output_rate};
    };
    const auto first = point(offset).first;
    const auto last = std::min(length - 1, point(offset + count - 1).first + 1);
    std::vector<std::int16_t> input;
    input.reserve(std::size_t(last - first + 1) * channels);
    std::int64_t next = first;
    if (!reverse && first >= playback.conversion_start) {
      const auto cached_end = playback.conversion_start + std::int64_t(playback.conversion_pcm.size() / channels);
      next = std::clamp(cached_end, first, last + 1);
      if (next > first) {
        const auto begin = playback.conversion_pcm.begin() + (first - playback.conversion_start) * channels;
        input.insert(input.end(), begin, begin + (next - first) * channels);
      }
    }
    const auto append = [&](std::span<const std::byte> bytes, bool reversed) {
      const auto start = input.size();
      const auto amount = bytes.size() / sizeof(std::int16_t);
      input.resize(start + amount);
      std::memcpy(input.data() + start, bytes.data(), bytes.size());
      if (reversed) {
        const auto frames = amount / channels;
        for (std::size_t frame = 0; frame < frames / 2; ++frame)
          for (std::uint32_t channel = 0; channel < channels; ++channel)
            std::swap(input[start + frame * channels + channel], input[start + (frames - 1 - frame) * channels + channel]);
      }
    };
    if (next <= last) {
      if (reverse) {
        ReadReverse(movie, playback, trim, length, next, last - next + 1, stop, append);
      } else {
        const auto at = trim + next;
        if (playback.cursor.reader && playback.cursor.next != at) playback.cursor = {};
        ReadPcm(movie, playback.cursor, at, last - next + 1, stop, [&](auto bytes) { append(bytes, false); });
      }
    }

    std::vector<std::int16_t> samples(std::size_t(count) * output_channels);
    const auto value = [&](std::int64_t at, std::uint32_t channel) -> double {
      const auto index = std::size_t(at - first) * channels;
      if (output_channels == 1 && channels == 2) return (double(input[index]) + input[index + 1]) / 2;
      return input[index + (channels == 1 ? 0 : channel)];
    };
    for (std::int64_t index = 0; index < count; ++index) {
      const auto [left, fraction] = point(offset + index);
      const auto right = std::min(left + 1, last);
      for (std::uint32_t channel = 0; channel < output_channels; ++channel)
        samples[std::size_t(index) * output_channels + channel] =
            static_cast<std::int16_t>(std::lround(std::lerp(value(left, channel), value(right, channel), fraction)));
    }
    playback.conversion_start = first;
    playback.conversion_pcm = std::move(input);
    consume(std::as_bytes(std::span(samples)), false);
  }

};

TimelineAudioSource::TimelineAudioSource(const CodecProviders& providers, CodecPolicy policy, const OnCodecFallback& report)
    : impl_(std::make_unique<Impl>(Impl{providers, policy, report})) {}

TimelineAudioSource::~TimelineAudioSource() = default;

void TimelineAudioSource::Read(entityx::Entity clip, const AudioSource& binding, const MovieAsset& movie, const AudioFormat& output,
                               std::int64_t trim, std::int64_t length, std::int64_t output_length, bool reverse,
                               std::int64_t offset,
                               std::int64_t count, std::stop_token stop, const ConsumePcm& consume) {
  auto [entry, inserted] = impl_->clips.try_emplace(clip, AudioPlayback{binding});
  auto& playback = entry->second;
  if (!inserted && playback.binding != binding) playback = AudioPlayback{binding};
  if (movie.pcm != output || length != output_length) {
    impl_->ReadConverted(movie, playback, output, trim, length, reverse, offset, count, stop, consume);
  } else if (reverse) {
    impl_->ReadReverse(movie, playback, trim, length, offset, count, stop, consume);
  } else {
    const auto source = trim + offset;
    if (playback.cursor.reader && playback.cursor.next != source) playback.cursor = {};
    impl_->ReadPcm(movie, playback.cursor, source, count, stop, [&](auto bytes) { consume(bytes, false); });
  }
  if (offset + count == output_length) impl_->clips.erase(entry);
  else playback.used = true;
}

void TimelineAudioSource::FinishBlock() {
  for (auto entry = impl_->clips.begin(); entry != impl_->clips.end();) {
    if (std::exchange(entry->second.used, false)) ++entry;
    else entry = impl_->clips.erase(entry);
  }
}

void TimelineAudioSource::Reset() { impl_->clips.clear(); }

} // namespace huxerui::avkit::detail
