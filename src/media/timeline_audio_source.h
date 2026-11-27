#pragma once

#include <huxerui/avkit/component.h>
#include <huxerui/avkit/file_movie.h>

namespace huxerui::avkit::detail {

/// Owns the decoder state for independent audio clips and linked original audio.
class TimelineAudioSource {
public:
  using ConsumePcm = std::function<void(std::span<const std::byte>, bool reversed)>;

  /// @brief Creates an audio-only cursor owner for the timeline mixer.
  /// @param providers Candidate audio decoders. Keep them alive through destruction.
  /// @param policy Selects the allowed providers.
  /// @param report Optional fallback callback. Keep it alive through destruction.
  TimelineAudioSource(const CodecProviders& providers, CodecPolicy policy, const OnCodecFallback& report);

  ~TimelineAudioSource();

  /// @brief Reads one intersecting clip interval into the current mix block.
  /// @param clip Entity identifying the clip cursor.
  /// @param binding Selected original audio track and its playback settings.
  /// @param movie Asset holding the selected track.
  /// @param output PCM format shared by the current mix block.
  /// @param trim Source start measured in native PCM samples.
  /// @param length Total trim length in native PCM samples.
  /// @param output_length Total clip length on the mixer sample grid.
  /// @param reverse Whether to traverse the source trim from end to start.
  /// @param offset Position within the trim in playback order, in samples.
  /// @param count Number of PCM samples per channel to read.
  /// @param stop Cancels decoding when the caller abandons the current block.
  /// @param consume Callback that consumes each borrowed PCM span and receives its reversal flag.
  void Read(entityx::Entity clip, const AudioSource& binding, const MovieAsset& movie, const media::AudioFormat& output,
            std::int64_t trim, std::int64_t length, std::int64_t output_length, bool reverse, std::int64_t offset,
            std::int64_t count, std::stop_token stop, const ConsumePcm& consume);

  /// @brief Evicts cursors that did not contribute to the completed mix block.
  void FinishBlock();

  /// @brief Releases audio cursors so the next block starts at its new source position.
  void Reset();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::avkit::detail
