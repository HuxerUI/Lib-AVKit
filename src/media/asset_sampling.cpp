#include <huxerui/avkit/asset_sampling.h>

#include <algorithm>
#include <cstring>
#include <list>
#include <numeric>
#include <stdexcept>

#include "project/project_internal.h"
#include "support/cancellation.h"
#include "support/media_time.h"

namespace huxerui::avkit {
namespace {

using namespace media;
using detail::Add;
using detail::AtScale;
using detail::Before;
using detail::CheckCancelled;
using detail::Difference;
using detail::ExactTicks;

FileInfo InspectFile(const File& file) {
  const auto info = file.Stat();
  if (!info.Succeeded() || info.Value().type != FileType::File)
    throw std::runtime_error("Cannot sample unavailable timeline asset: " + file.Path());
  return info.Value();
}

struct AssetVersion {
  File file;
  FileInfo info;
  std::uint64_t revision;
  bool operator==(const AssetVersion&) const = default;
};

AssetVersion Resolve(const ProjectSnapshot& project, AssetId id, std::uint32_t track, TrackKind kind) {
  const auto& asset = detail::FindAsset(project, id);
  if (detail::FindSourceTrack(asset.source, track).kind != kind) throw std::invalid_argument("Incorrect sampling track kind");
  return {asset.source.file, InspectFile(asset.source.file), asset.content_revision};
}

void CheckVersion(const AssetVersion& version, std::stop_token stop) {
  CheckCancelled(stop);
  if (InspectFile(version.file) != version.info) throw std::runtime_error("Asset changed during timeline sampling");
}

struct ThumbnailKey {
  AssetVersion version;
  std::uint32_t track;
  PixelSize size;
  MediaTimestamp time;
  bool operator==(const ThumbnailKey&) const = default;
};

struct BucketKey {
  AssetVersion version;
  std::uint32_t track;
  std::int64_t start, samples;
  bool operator==(const BucketKey&) const = default;
};

template<class Key, class Value, std::size_t Limit>
class Cache {
public:
  std::optional<Value> Find(const Key& key) {
    const auto entry = std::ranges::find_if(entries_, [&](const auto& item) { return item.first == key; });
    if (entry == entries_.end()) return std::nullopt;
    entries_.splice(entries_.begin(), entries_, entry);
    return entries_.front().second;
  }

  void Put(Key key, Value value) {
    entries_.emplace_front(std::move(key), std::move(value));
    if (entries_.size() > Limit) entries_.pop_back();
  }

private:
  std::list<std::pair<Key, Value>> entries_;
};

void Include(WaveformPeak& peak, float sample) {
  peak.minimum = std::min(peak.minimum, sample);
  peak.maximum = std::max(peak.maximum, sample);
}

} // namespace

struct AssetSampler::Impl {
  ThumbnailStrip ReadThumbnails(const ProjectSnapshot& project, const ThumbnailRequest& request, std::stop_token stop) {
    CheckCancelled(stop);
    if (!encode || !request.size.width || !request.size.height) throw std::invalid_argument("Thumbnail needs an image encoder and size");
    const auto version = Resolve(project, request.asset, request.track, TrackKind::Video);
    const auto movie = ImportMovie(version.file);
    if (movie.video.id != request.track) throw std::invalid_argument("Video track changed after import");
    for (const auto at : request.source_times) {
      if (Before(at, movie.video_range.start) || !Before(at, movie.video_range.end))
        throw std::out_of_range("Thumbnail source time is outside the video track");
    }

    std::vector<std::size_t> order(request.source_times.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return Before(request.source_times[a], request.source_times[b]); });
    ThumbnailStrip result(order.size());
    std::vector<std::size_t> decoded;
    std::unique_ptr<MovieReader> reader;
    std::optional<VideoFrame> picture;
    std::optional<MediaTimestamp> previous;
    for (const auto index : order) {
      CheckCancelled(stop);
      const auto time = request.source_times[index];
      if (const auto hit = thumbnails.Find({version, request.track, request.size, time})) {
        result[index] = *hit;
        continue;
      }
      if (previous && *previous == time) {
        result[index] = result[decoded.back()];
        continue;
      }
      if (!reader || Before(Add(*previous, {1, 1}), time)) {
        picture.reset();
        reader.reset();
        reader = std::make_unique<MovieReader>(movie, providers, TrackKind::Video, time, policy);
        picture = reader->ReadVideo(stop);
      }
      while (picture && !Before(time, Add(picture->pts, *picture->duration))) picture = reader->ReadVideo(stop);
      if (picture && !Before(time, picture->pts)) {
        auto image = encode(*picture, request.size);
        if (image.PixelWidth() != request.size.width || image.PixelHeight() != request.size.height)
          throw std::runtime_error("Thumbnail adapter returned an unexpected image size");
        result[index] = Thumbnail{picture->pts, std::move(image)};
      }
      previous = time;
      decoded.push_back(index);
    }
    CheckVersion(version, stop);
    for (const auto index : decoded) thumbnails.Put({version, request.track, request.size, request.source_times[index]}, result[index]);
    return result;
  }

  Waveform ReadWaveform(const ProjectSnapshot& project, const WaveformRequest& request, std::stop_token stop) {
    CheckCancelled(stop);
    const auto version = Resolve(project, request.asset, request.track, TrackKind::Audio);
    const auto movie = ImportMovie(version.file);
    const auto rate = movie.pcm.sample_rate, channels = movie.pcm.ChannelCount();
    if (movie.audio.id != request.track || Before(request.source_range.start, movie.audio_range.start) ||
        Before(movie.audio_range.end, request.source_range.end) || !Before(request.source_range.start, request.source_range.end))
      throw std::invalid_argument("Waveform interval or audio track is unavailable");
    const auto bucket = ExactTicks(request.bucket_interval, rate);
    if (bucket <= 0) throw std::invalid_argument("Waveform needs a positive bucket interval");
    const auto origin = ExactTicks(movie.audio_range.start, rate);
    const auto start = Difference(request.source_range.start, movie.audio_range.start);
    const auto stop_at = Difference(request.source_range.end, movie.audio_range.start);
    const auto start_tick = AtScale(start, rate), stop_tick = AtScale(stop_at, rate);
    const auto floor_start = start_tick.ticks - Before(start, start_tick);
    const auto ceil_end = stop_tick.ticks + Before(stop_tick, stop_at);
    const auto first = floor_start / bucket, last = ceil_end / bucket + (ceil_end % bucket != 0);
    if (last > (std::numeric_limits<std::int64_t>::max() - origin) / bucket)
      throw std::overflow_error("Waveform bucket boundary is outside the sample clock");

    Waveform waveform{{origin + first * bucket, rate}, {bucket, rate}, channels, {}};
    std::vector<BucketKey> computed;
    std::unique_ptr<MovieReader> reader;
    std::optional<AudioFrame> frame;
    std::int64_t position = -1;
    bool exhausted = false;
    for (auto index = first; index < last; ++index) {
      CheckCancelled(stop);
      const auto at = origin + index * bucket;
      BucketKey key{version, request.track, at, bucket};
      auto peaks = CachedBucket(key);
      if (!peaks) {
        if (position != at) {
          frame.reset();
          reader.reset();
          reader = std::make_unique<MovieReader>(movie, providers, TrackKind::Audio, MediaTimestamp{at, rate}, policy);
          position = at;
          exhausted = false;
        }
        peaks.emplace(channels, WaveformPeak{1, -1});
        while (position < at + bucket) {
          if (!exhausted && (!frame || frame->pts.ticks + frame->sample_count <= position)) {
            frame = reader->ReadAudio(stop);
            exhausted = !frame;
          }
          if (!frame || frame->pts.ticks > position) {
            for (auto& peak : *peaks) Include(peak, 0);
            position = frame ? std::min(at + bucket, frame->pts.ticks) : at + bucket;
            continue;
          }
          const auto bytes = frame->planes.front().AsBytes();
          const auto end = std::min(at + bucket, frame->pts.ticks + frame->sample_count);
          for (; position < end; ++position) {
            const auto sample = std::size_t(position - frame->pts.ticks) * channels;
            for (std::size_t channel = 0; channel < channels; ++channel) {
              std::int16_t value;
              std::memcpy(&value, bytes.data() + (sample + channel) * sizeof(value), sizeof(value));
              Include((*peaks)[channel], value / 32768.0F);
            }
          }
        }
        computed.push_back(key);
      }
      waveform.peaks.insert(waveform.peaks.end(), peaks->begin(), peaks->end());
    }
    CheckVersion(version, stop);
    for (auto& key : computed) {
      const auto offset = std::size_t((key.start - waveform.start.ticks) / bucket) * channels;
      buckets.Put(std::move(key), {waveform.peaks.begin() + offset, waveform.peaks.begin() + offset + channels});
    }
    return waveform;
  }

  std::optional<std::vector<WaveformPeak>> CachedBucket(const BucketKey& key) {
    if (auto hit = buckets.Find(key)) return hit;
    if (key.samples % 2) return std::nullopt;
    auto half = key;
    half.samples /= 2;
    auto left = buckets.Find(half);
    if (!left) return std::nullopt;
    half.start += half.samples;
    const auto right = buckets.Find(half);
    if (!right) return std::nullopt;
    for (std::size_t channel = 0; channel < left->size(); ++channel) {
      Include((*left)[channel], (*right)[channel].minimum);
      Include((*left)[channel], (*right)[channel].maximum);
    }
    buckets.Put(key, *left);
    return left;
  }

  MovieCodecProviders providers;
  EncodeThumbnail encode;
  CodecPolicy policy;
  Cache<ThumbnailKey, std::optional<Thumbnail>, 96> thumbnails;
  Cache<BucketKey, std::vector<WaveformPeak>, 4096> buckets;
};

AssetSampler::AssetSampler(MovieCodecProviders providers, EncodeThumbnail encode, CodecPolicy policy)
    : impl_(std::make_unique<Impl>(std::move(providers), std::move(encode), policy)) {}

AssetSampler::~AssetSampler() = default;

ThumbnailStrip AssetSampler::ReadThumbnails(const ProjectSnapshot& project, const ThumbnailRequest& request, std::stop_token stop) {
  return impl_->ReadThumbnails(project, request, stop);
}

Waveform AssetSampler::ReadWaveform(const ProjectSnapshot& project, const WaveformRequest& request, std::stop_token stop) {
  return impl_->ReadWaveform(project, request, stop);
}

} // namespace huxerui::avkit
