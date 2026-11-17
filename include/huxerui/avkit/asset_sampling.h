#pragma once

/// @file
/// @brief Source-frame thumbnails and PCM peak buckets for timeline displays.
///
/// Link AVKit::Media. Each AssetSampler owns bounded source caches and opens independent track
/// cursors on its worker sequence.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

#include <huxerui/avkit/file_movie.h>
#include <huxerui/avkit/project.h>
#include <huxerui/resource.h>

namespace huxerui::avkit {

/// @brief Thumbnail grid requested from one video track.
struct ThumbnailRequest {
  AssetId asset;
  std::uint32_t track;
  std::vector<media::MediaTimestamp> source_times; ///< Requested presentation times on the source clock.
  PixelSize size;
};

/// @brief Encoded thumbnail and the actual decoded source presentation time.
struct Thumbnail {
  media::MediaTimestamp pts;
  ImageAsset image;
};

/// @brief Thumbnails in request order, with an empty entry for an unavailable picture.
using ThumbnailStrip = std::vector<std::optional<Thumbnail>>;
/// @brief Encodes an oriented picture at the requested physical size on the sampling sequence.
using EncodeThumbnail = std::function<ImageAsset(const media::VideoFrame&, PixelSize)>;

/// @brief Requested source interval and bucket resolution for one audio track.
struct WaveformRequest {
  AssetId asset;
  std::uint32_t track;
  TimeRange source_range;
  media::MediaTimestamp bucket_interval; ///< Positive duration aligned to the track's PCM sample grid.
};

/// @brief Minimum and maximum decoded PCM values for one channel in one bucket.
struct WaveformPeak {
  float minimum, maximum;
  bool operator==(const WaveformPeak&) const = default;
};

/// @brief Whole PCM buckets in source order, with channels interleaved per bucket.
struct Waveform {
  media::MediaTimestamp start, bucket_interval;
  std::uint32_t channels;
  std::vector<WaveformPeak> peaks;
};

/// @brief Samples an immutable project revision on a worker sequence.
/// Source caches are scoped by asset revision, file metadata, track, and resolution.
class AssetSampler {
public:
  /// @brief Creates a sampler using the supplied codec providers and thumbnail encoder.
  AssetSampler(MovieCodecProviders providers, EncodeThumbnail encode, CodecPolicy policy = CodecPolicy::Auto);
  ~AssetSampler();
  AssetSampler(const AssetSampler&) = delete;
  AssetSampler& operator=(const AssetSampler&) = delete;

  /// @brief Decodes the video track and returns actual PTS in request order.
  /// @param stop Cooperatively cancels background sampling before publishing cache entries.
  /// @throws std::system_error With std::errc::operation_canceled when stopped.
  ThumbnailStrip ReadThumbnails(const ProjectSnapshot& project, const ThumbnailRequest& request, std::stop_token stop = {});
  /// @brief Decodes the audio track and returns whole buckets aligned to the track start.
  /// @throws std::system_error With std::errc::operation_canceled when stopped.
  Waveform ReadWaveform(const ProjectSnapshot& project, const WaveformRequest& request, std::stop_token stop = {});

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::avkit
