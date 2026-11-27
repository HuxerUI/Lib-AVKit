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
#include <variant>
#include <vector>

#include <huxerui/avkit/file_movie.h>
#include <huxerui/avkit/types.h>
#include <huxerui/resource.h>

namespace huxerui::avkit {

struct ProjectSnapshot;

/// @brief Thumbnail grid requested from one video track.
struct ThumbnailRequest {
  /// Imported asset to sample in the supplied snapshot.
  ResourceRef asset;
  /// Video track identifier within that asset.
  std::uint32_t track;
  std::vector<media::MediaTimestamp> source_times; ///< Requested presentation times on the source clock.
  /// Requested width and height in physical pixels.
  PixelSize size;
};

/// @brief Retained thumbnail image suitable for presentation on the UI sequence.
using ThumbnailImage = std::variant<ImageAsset, Bitmap, std::shared_ptr<ExternalTexture>>;

/// @brief Thumbnail image and the actual decoded source presentation time.
struct Thumbnail {
  /// Actual source PTS of the retained picture.
  media::MediaTimestamp pts;
  /// Encoded image, bitmap, or platform texture accepted by HuxerUI.
  ThumbnailImage image;
  /// Physical extent of the produced thumbnail.
  PixelSize size;
};

/// @brief Thumbnails in request order, with an empty entry for an unavailable picture.
using ThumbnailStrip = std::vector<std::optional<Thumbnail>>;
/// @brief Receives each resolved request slot on the sampling sequence, including cache hits.
using OnThumbnail = std::function<void(std::size_t, const std::optional<Thumbnail>&)>;
/// @brief Produces an oriented thumbnail at the requested physical size on the sampling sequence.
using MakeThumbnail = std::function<ThumbnailImage(const media::VideoFrame&, PixelSize)>;

/// @brief Opens a thumbnail adapter for images drawn in a HuxerUI Canvas. Link AVKit::Render.
/// Open from the application sequence after AVKit installation on Android. Call the returned adapter on
/// the sampling sequence. A platform transfer failure can be retried through the software decoder at the same PTS.
/// @return An adapter that supplies HuxerUI images at the requested extent.
MakeThumbnail OpenCanvasThumbnailSource();

/// @brief Requested source interval and bucket resolution for one audio track.
struct WaveformRequest {
  /// Imported asset to sample in the supplied snapshot.
  ResourceRef asset;
  /// Audio track identifier within that asset.
  std::uint32_t track;
  /// Half-open source interval for the requested waveform.
  TimeRange source_range;
  media::MediaTimestamp bucket_interval; ///< Positive duration aligned to the track's PCM sample grid.
};

/// @brief Minimum and maximum decoded PCM values for one channel in one bucket.
struct WaveformPeak {
  /// Smallest and largest normalized sample values in the channel and bucket.
  float minimum, maximum;

  bool operator==(const WaveformPeak&) const = default;
};

/// @brief Whole PCM buckets in source order, with channels interleaved per bucket.
struct Waveform {
  /// Exact source time of the first bucket and the duration of each bucket.
  media::MediaTimestamp start, bucket_interval;
  /// Number of channels represented in each bucket.
  std::uint32_t channels;
  /// Peaks in bucket order, with channels consecutive in each bucket.
  std::vector<WaveformPeak> peaks;
};

/// @brief Samples an immutable project revision on a worker sequence.
/// Source caches are scoped by the resolved media record, file metadata, track, and resolution.
class AssetSampler {
public:
  /// @brief Creates a sampler using the supplied codec providers and thumbnail adapter.
  /// @param providers Platform and software codec candidates for independent read cursors.
  /// @param make_thumbnail Adapter for materializing one sampled picture at the requested size.
  /// @param policy Selects the allowed codec providers.
  AssetSampler(CodecProviders providers, MakeThumbnail make_thumbnail, CodecPolicy policy = CodecPolicy::Auto);

  ~AssetSampler();

  AssetSampler(const AssetSampler&) = delete;

  AssetSampler& operator=(const AssetSampler&) = delete;

  /// @brief Decodes the video track and returns actual PTS in request order.
  /// @param project Immutable revision containing the named asset and track.
  /// @param request Source positions and requested physical thumbnail extent.
  /// @param stop Cooperatively cancels background sampling before publishing cache entries.
  /// @param ready Optional callback for each request slot as it resolves, including cache hits.
  /// @return One entry per requested time. An empty entry denotes an unavailable picture.
  /// @throws std::system_error With std::errc::operation_canceled when stopped.
  ThumbnailStrip ReadThumbnails(const ProjectSnapshot& project, const ThumbnailRequest& request,
                                std::stop_token stop = {}, const OnThumbnail& ready = {});

  /// @brief Decodes the audio track and returns whole buckets aligned to the track start.
  /// @param project Immutable revision containing the named asset and track.
  /// @param request Source interval and bucket duration, aligned to the PCM grid.
  /// @param stop Cancels pending decode and bucket computation.
  /// @return Whole buckets with interleaved channel peaks.
  /// @throws std::system_error With std::errc::operation_canceled when stopped.
  Waveform ReadWaveform(const ProjectSnapshot& project, const WaveformRequest& request, std::stop_token stop = {});

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::avkit
