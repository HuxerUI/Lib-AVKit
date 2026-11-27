#include <huxerui/avkit/file_movie.h>
#include <huxerui/avkit/resource_catalog.h>
#include "media/timeline_internal.h"

#include "support/cancellation.h"
#include "support/media_time.h"
#include "support/staged_output.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace huxerui::avkit {
namespace {

using namespace media;
using detail::Add;
using detail::AtScale;
using detail::Before;
using detail::CheckCancelled;
using detail::Difference;
using detail::StagedOutput;

#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
constexpr auto kPlatformVideoLayout = VideoPixelFormat::Opaque;
#else
constexpr auto kPlatformVideoLayout = VideoPixelFormat::NV12;
#endif

template<class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

MediaDemuxer OpenDemuxer(const File& file) {
  return Require(MediaDemuxer::Open(Require(file.OpenRandomAccess())));
}

bool Recoverable(const CodecError& error) {
  return error.code == CodecErrorCode::Unsupported || error.code == CodecErrorCode::ResourceExhausted ||
         error.code == CodecErrorCode::PlatformFailure;
}

VideoFrame CheckVideoReplacement(std::optional<VideoFrame> replacement, const VideoFrame& rejected) {
  if (!replacement || replacement->Storage() != VideoStorage::Cpu || replacement->pts != rejected.pts ||
      replacement->duration != rejected.duration)
    throw std::runtime_error("Software decode did not reproduce the rejected frame's exact presentation interval");
  return std::move(*replacement);
}

struct CodecFailure {
  CodecOperation operation;
  bool software;
  CodecError error;
};

class WakeSignal {
public:
  auto Version() {
    std::lock_guard lock(mutex_);
    return version_;
  }

  void Wake() {
    {
      std::lock_guard lock(mutex_);
      ++version_;
    }
    changed_.notify_one();
  }

  bool Wait(std::uint64_t version) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(10), [&] { return version_ != version; });
  }
private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::uint64_t version_{};
};

template<class Session> struct Selection {
  std::unique_ptr<Session> session;
  std::shared_ptr<WakeSignal> signal;
  std::string provider;
  bool software{};
};

template<class Session, class Config>
Selection<Session> OpenSelected(const CodecProviders& providers, CodecOperation operation, bool software_only,
                                Config platform, Config software, const OnCodecFallback& report) {
  Selection<Session> selection;
  selection.signal = std::make_shared<WakeSignal>();
  using OpenResult = CodecResult<std::unique_ptr<Session>>;
  const auto open = [&](const std::shared_ptr<CodecProvider>& provider, Config& config) -> OpenResult {
    if (!provider) return OpenResult::Failure({CodecErrorCode::Unsupported, "Codec provider is unavailable", {}});
    auto support = provider->Probe(config);
    if (!support.Succeeded()) return OpenResult::Failure(support.Error());
    config.wakeup = [signal = selection.signal] { signal->Wake(); };
    auto opened = provider->Open(config);
    if (!opened.Succeeded()) return opened;
    if (!opened.Value() || opened.Value()->Negotiated().video != support.Value().video) {
      if (opened.Value()) opened.Value()->Close();
      return OpenResult::Failure({CodecErrorCode::Unsupported, "Codec session changed its probed frame storage or layout", {}});
    }
    return opened;
  };
  std::optional<CodecError> cause;
  if (!software_only && providers.platform) {
    auto result = open(providers.platform, platform);
    if (result.Succeeded()) {
      selection.session = std::move(result).Value();
      selection.provider = providers.platform->Name();
      return selection;
    }
    if (!Recoverable(result.Error())) throw CodecFailure{operation, false, result.Error()};
    cause = result.Error();
  }
  selection.software = true;
  auto result = open(providers.software, software);
  if (!result.Succeeded()) {
    auto failure = result.Error();
    if (cause) failure.message = cause->message + ". Software fallback: " + failure.message;
    throw CodecFailure{operation, true, std::move(failure)};
  }
  selection.session = std::move(result).Value();
  selection.provider = providers.software->Name();
  if (cause && report) report({operation, std::string(providers.platform->Name()), selection.provider, *cause, {}});
  return selection;
}

template<class Session> class Pump {
public:
  using Input = typename Session::Input;
  using Output = typename Session::Output;

  Pump(Selection<Session> selection, CodecOperation operation, std::function<std::optional<Input>()> next)
      : selection_(std::move(selection)), operation_(operation), next_(std::move(next)) {}

  ~Pump() { selection_.session->Close(); }

  bool Software() const { return selection_.software; }

  const std::string& Provider() const { return selection_.provider; }

  bool AllowsSequenceHandoff() const noexcept { return selection_.session->Negotiated().allow_sequence_handoff; }

  Output Pull(std::stop_token stop = {}) {
    auto& session = *selection_.session;
    const std::stop_callback wake(stop, [&] { selection_.signal->Wake(); });
    for (;;) {
      const auto version = selection_.signal->Version();
      CheckCancelled(stop);
      auto event = Call(session.TryRead());
      if (event) return std::move(*event);
      if (!finished_) {
        if (!pending_) pending_ = next_();
        if (!pending_) {
          Call(session.FinishInput());
          finished_ = true;
          continue;
        }
        if (Call(session.TrySubmit(*pending_)) == SubmitStatus::Accepted) {
          pending_.reset();
          continue;
        }
      }
      if (!selection_.signal->Wait(version))
        throw CodecFailure{operation_, selection_.software, {CodecErrorCode::PlatformFailure, "Codec stopped making progress", {}}};
    }
  }

private:
  template<class T> T Call(CodecResult<T> result) {
    if (!result.Succeeded()) throw CodecFailure{operation_, selection_.software, result.Error()};
    if constexpr (!std::is_void_v<T>) return std::move(result).Value();
  }
  Selection<Session> selection_;
  CodecOperation operation_;
  std::function<std::optional<Input>()> next_;
  std::optional<Input> pending_;
  bool finished_{};
};

void Select(MediaDemuxer& demuxer, std::uint32_t id, MediaTimestamp time) {
  Require(demuxer.SelectTracks(std::array{id}));
  if (Before({}, time)) Require(demuxer.Seek(id, time));
}

struct AudioPacketTiming {
  std::optional<MediaTimestamp> first_audible;
  std::optional<MediaTimestamp> next_sample;
  std::deque<std::pair<MediaTimestamp, MediaTimestamp>> discards;
};

std::optional<EncodedPacket> ReadPacket(MediaDemuxer& demuxer, AudioPacketTiming* audio = nullptr) {
  while (auto item = Require(demuxer.Read())) {
    if (auto* start = std::get_if<TrackDecodeStart>(&*item)) {
      if (start->sample_description_index != 1) throw std::runtime_error("Sample description changed during file playback");
      if (audio) audio->first_audible = start->first_audible_pts;
      continue;
    }
    auto& packet = std::get<ContainerPacket>(*item);
    if (!packet.pts || packet.sample_description_index != 1)
      throw std::runtime_error("A file packet has no supported timing or sample description");
    if (audio && packet.discard_padding) audio->discards.emplace_back(*packet.pts, *packet.discard_padding);
    return EncodedPacket{std::move(packet.data), *packet.pts, packet.dts, packet.duration, packet.sync_sample.value_or(false)};
  }
  return std::nullopt;
}

AudioFrame SliceAudio(const AudioFrame& frame, std::uint32_t offset, std::uint32_t count, MediaTimestamp pts) {
  const auto stride = frame.format.ChannelCount() * frame.format.BytesPerSample();
  return {frame.format, pts, count, {frame.planes.front().Slice(std::size_t(offset) * stride, std::size_t(count) * stride)}};
}

MediaTrack VideoTrack(const VideoEncodeFormat& format, const VideoEncoderConfig& config, std::uint32_t timescale) {
  MediaTrack track;
  track.id = 1;
  track.kind = TrackKind::Video;
  track.timescale = timescale;
  MediaSampleDescription description;
  description.codec = format.codec;
  description.framing = PacketFraming::LengthPrefixedNals;
  description.initialization_data = format.initialization_data;
  description.coded_width = format.width;
  description.coded_height = format.height;
  description.visible = format.visible;
  description.pixel_aspect_ratio = MediaSampleDescription::PixelAspectRatio{config.input.aspect_width, config.input.aspect_height};
  track.descriptions.push_back(std::move(description));
  return track;
}

MediaTrack AudioTrack(const AudioEncodeFormat& format) {
  MediaTrack track;
  track.id = 2;
  track.kind = TrackKind::Audio;
  track.timescale = format.sample_rate;
  MediaSampleDescription description;
  description.codec = format.codec;
  description.initialization_data = format.initialization_data;
  description.sample_rate = format.sample_rate;
  AudioFormat layout;
  layout.channel_layout = format.channel_layout;
  description.channel_count = layout.ChannelCount();
  track.descriptions.push_back(std::move(description));
  return track;
}

ContainerPacket ToContainer(const EncodedPacket& packet, std::uint32_t track) {
  ContainerPacket result;
  result.track_id = track;
  result.data = packet.data;
  result.pts = packet.pts;
  result.dts = packet.dts.value_or(packet.pts);
  result.duration = packet.duration;
  result.sync_sample = packet.key_frame;
  return result;
}

bool IsPcm(CodecId codec) {
  using enum CodecId;
  switch (codec) {
    case PcmU8: case PcmS16Le: case PcmS24Le: case PcmS32Le: case PcmF32Le:
    case PcmS16Be: case PcmS24Be: case PcmS32Be: case PcmF32Be:
      return true;
    default: return false;
  }
}

bool MovieVideo(const MediaSampleDescription& description) {
  using enum CodecId;
  switch (description.codec) {
    case H264: case Hevc: case Vp8: case Vp9: case Av1: return true;
    case ProRes:
      return description.sample_entry == "apco" || description.sample_entry == "apcs" ||
             description.sample_entry == "apcn" || description.sample_entry == "apch";
    default: return false;
  }
}

bool MovieAudio(const MediaSampleDescription& description) {
  if ((description.sample_rate != 44100 && description.sample_rate != 48000) ||
      (description.channel_count != 1 && description.channel_count != 2)) return false;
  using enum CodecId;
  switch (description.codec) {
    case Aac:
      return description.initialization_data.size() == 2 &&
             (std::to_integer<unsigned>(description.initialization_data.front()) >> 3) == 2;
    case Alac: case Opus: case Vorbis: case Mp3: return true;
    default: return IsPcm(description.codec);
  }
}

bool HasSegmentClock(const MediaDemuxer& demuxer) {
  return demuxer.Format() == ContainerFormat::Matroska || demuxer.Format() == ContainerFormat::WebM;
}

std::optional<MediaPresentationRange> MovieTrackRange(const MediaDemuxer& demuxer, const MediaTrack& track) {
  if (const auto range = demuxer.PresentationRange(track.id)) return range;
  // Ordinary Matroska tracks share the segment clock and may only declare a segment duration.
  if (HasSegmentClock(demuxer)) {
    if (const auto duration = demuxer.Duration()) return MediaPresentationRange{{}, *duration};
  }
  return std::nullopt;
}

bool SizeVideoQueues(VideoDecoderConfig& config) {
  // Match the decoder's coded-frame charge, including the minimum output reservations for reorder.
  constexpr std::uint64_t minimum_output = 34;
  const std::uint64_t luma = std::uint64_t(config.width) * config.height;
  const std::uint64_t chroma = std::uint64_t((config.width + 1) / 2) * ((config.height + 1) / 2);
  const auto bytes = config.output == VideoPixelFormat::Opaque ? 4 * luma : luma + 2 * chroma;
  if (bytes > std::numeric_limits<std::size_t>::max() / minimum_output) return false;
  config.limits.input_bytes = std::max(config.limits.input_bytes,
      static_cast<std::size_t>(std::min<std::uint64_t>(bytes, 64 * 1024 * 1024)));
  config.limits.output_bytes = std::max(config.limits.output_bytes, static_cast<std::size_t>(bytes * minimum_output));
  return true;
}

MovieAsset InspectMovie(const File& file, MediaDemuxer& demuxer) {
  using enum ContainerFormat;
  switch (demuxer.Format()) {
    case Mp4: case Mov: case Matroska: case WebM: case MpegTs: case Avi: break;
    default: throw std::runtime_error("The video container must be MP4, MOV, MKV, WebM, MPEG-TS or AVI");
  }
  std::optional<MediaTrack> video, audio;
  bool has_audio = false;
  for (const auto& track : demuxer.Tracks()) {
    has_audio |= track.kind == TrackKind::Audio;
    if (track.descriptions.size() != 1) continue;
    const auto& description = track.descriptions.front();
    if (!video && track.kind == TrackKind::Video && MovieVideo(description)) video = track;
    if (!audio && track.kind == TrackKind::Audio && MovieAudio(description)) audio = track;
  }
  if (!video) throw std::runtime_error("No supported H.264, HEVC, VP8, VP9, AV1 or ProRes 422 video track");
  const auto vrange = MovieTrackRange(demuxer, *video);
  const auto& picture = video->descriptions.front();
  if (!vrange || !Before(vrange->start, vrange->end) || Before(vrange->start, {}) ||
      !picture.coded_width.value_or(0) || !picture.coded_height.value_or(0) ||
      *picture.coded_width > 8192 || *picture.coded_height > 8192)
    throw std::runtime_error("The video presentation range or frame size is unsupported");
  if (!has_audio) {
    const AudioFormat pcm{48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Mono};
    return {file, std::move(*video), {}, *vrange, {}, vrange->end, pcm};
  }
  if (!audio) throw std::runtime_error("No supported mono or stereo audio track at 44.1 or 48 kHz");
  const auto arange = MovieTrackRange(demuxer, *audio);
  if (!arange || !Before(arange->start, arange->end) || Before(arange->start, {}))
    throw std::runtime_error("The audio presentation range is unsupported");
  const auto& sound = audio->descriptions.front();
  AudioFormat pcm{*sound.sample_rate, AudioSampleFormat::S16, AudioPacking::Interleaved,
                  *sound.channel_count == 1 ? AudioChannelLayout::Mono : AudioChannelLayout::Stereo};
  return {file, std::move(*video), std::move(*audio), *vrange, *arange,
          Before(vrange->end, arange->end) ? arange->end : vrange->end, pcm};
}

} // namespace

MovieAsset ImportMovie(const File& file) {
  auto demuxer = OpenDemuxer(file);
  return InspectMovie(file, demuxer);
}

MovieAsset ImportMovieAudio(const File& file) {
  auto demuxer = OpenDemuxer(file);
  for (const auto& track : demuxer.Tracks()) {
    if (track.kind != TrackKind::Audio || track.descriptions.size() != 1 ||
        !MovieAudio(track.descriptions.front())) continue;
    const auto range = MovieTrackRange(demuxer, track);
    if (!range || !Before(range->start, range->end) || Before(range->start, {})) continue;
    const auto& description = track.descriptions.front();
    const AudioFormat pcm{*description.sample_rate, AudioSampleFormat::S16, AudioPacking::Interleaved,
        description.channel_count == 1 ? AudioChannelLayout::Mono : AudioChannelLayout::Stereo};
    return {file, {}, track, {}, *range, range->end, pcm};
  }
  throw std::runtime_error("No supported mono or stereo audio track at 44.1 or 48 kHz");
}

struct MovieReader::Impl {
  Impl(MovieAsset movie, CodecProviders codec_providers, CodecPolicy selection_policy, OnCodecFallback fallback,
       std::optional<TrackKind> track = {})
      : asset(std::move(movie)), providers(std::move(codec_providers)), policy(selection_policy),
        report(std::move(fallback)), selected(track) {
    if (selected && *selected != TrackKind::Video && *selected != TrackKind::Audio)
      throw std::invalid_argument("Movie decoding requires a video or audio track");
  }

  void OpenVideo(MediaTimestamp at, bool software) {
    video.reset();
    pending_video.clear();
    video_end = !Before(at, asset.video_range.end);
    if (video_end) {
      video_packets.reset();
      return;
    }
    video_packets.emplace(OpenDemuxer(asset.file));
    Select(*video_packets, asset.video.id, at);
    const auto& description = asset.video.descriptions.front();
    VideoDecoderConfig config;
    config.codec = description.codec;
    config.initialization_data = description.initialization_data;
    config.width = *description.coded_width;
    config.height = *description.coded_height;
    // An AVC sample entry can report the cropped size. The decoder bounds include whole macroblocks.
    if (config.codec == CodecId::H264) {
      config.width = (config.width + 15) & ~15u;
      config.height = (config.height + 15) & ~15u;
    }
    const bool platform_texture = config.codec != CodecId::Av1 && config.codec != CodecId::ProRes;
    config.output = platform_texture ? kPlatformVideoLayout : VideoPixelFormat::I420;
    config.output_storage = platform_texture ? VideoStorage::Platform : VideoStorage::Cpu;
    if (config.codec == CodecId::ProRes) config.prores_sample_entry = description.sample_entry;
    auto cpu = config;
    cpu.output_storage = VideoStorage::Cpu;
    cpu.output = VideoPixelFormat::I420;
    const auto platform_fits = SizeVideoQueues(config);
    if (!SizeVideoQueues(cpu)) throw std::runtime_error("Decoded video frames exceed the addressable queue size");
    video = std::make_unique<Pump<VideoDecoderSession>>(
        OpenSelected<VideoDecoderSession>(providers, CodecOperation::VideoDecode, software || !platform_fits, config, cpu, report),
        CodecOperation::VideoDecode, [this] { return ReadPacket(*video_packets); });
  }

  void OpenAudio(MediaTimestamp at, bool software) {
    audio.reset();
    pending_audio.clear();
    audio_timing = {};
    segment_audio_end.reset();
    audio_end = !asset.HasAudio() || !Before(at, asset.audio_range.end);
    if (audio_end) {
      audio_packets.reset();
      return;
    }
    audio_packets.emplace(OpenDemuxer(asset.file));
    if (HasSegmentClock(*audio_packets) && !audio_packets->PresentationRange(asset.audio.id))
      segment_audio_end = AtScale(asset.audio_range.end, asset.pcm.sample_rate);
    const auto& description = asset.audio.descriptions.front();
    if (description.codec == CodecId::PcmS16Le) {
      Select(*audio_packets, asset.audio.id, at);
      return;
    }
    const auto seek = AtScale(at, asset.pcm.sample_rate);
    const auto preroll_samples = asset.audio.seek_preroll ? AtScale(*asset.audio.seek_preroll, seek.timescale).ticks : 4096;
    const MediaTimestamp preroll{std::max<std::int64_t>(0, seek.ticks - preroll_samples), seek.timescale};
    Select(*audio_packets, asset.audio.id, preroll);
    AudioDecoderConfig config;
    config.output = asset.pcm;
    config.codec = description.codec;
    config.initialization_data = description.initialization_data;
    audio = std::make_unique<Pump<AudioDecoderSession>>(
        OpenSelected<AudioDecoderSession>(providers, CodecOperation::AudioDecode, software || IsPcm(config.codec),
                                         config, config, report),
        CodecOperation::AudioDecode, [this] { return ReadPacket(*audio_packets, &audio_timing); });
  }

  void Seek(MediaTimestamp at) {
    if (Before(at, {}) || !Before(at, asset.end)) throw std::runtime_error("Seek is outside the movie presentation interval");
    ready = false;
    const bool software = policy == CodecPolicy::SoftwareOnly;
    if (selected != TrackKind::Audio) {
      target = at;
      last_video.reset();
      OpenVideo(at, software || force_software_video);
    }
    if (selected != TrackKind::Video) {
      next_audio = AtScale(at, asset.pcm.sample_rate);
      OpenAudio(at, software);
    }
    ready = true;
  }

  std::optional<VideoFrame> ReadVideo(std::stop_token stop, std::optional<MediaTimestamp> cpu_preroll = {}) {
    if (selected == TrackKind::Audio) throw std::logic_error("The movie reader selected only the audio track");
    if (!ready) throw std::runtime_error("Seek has not opened the selected movie tracks");
    for (;;) {
      CheckCancelled(stop);
      if (!pending_video.empty()) {
        auto frame = std::move(pending_video.front());
        pending_video.pop_front();
        if (last_video && !Before(*last_video, frame.pts)) continue;
        last_video = frame.pts;
        return frame;
      }
      if (video_end) return std::nullopt;
      if (!video) throw std::runtime_error("Seek to reopen the video decoder");
      try {
        auto output = video->Pull(stop);
        if (std::holds_alternative<EndOfStream>(output)) video_end = true;
        auto* frame = std::get_if<VideoFrame>(&output);
        if (!frame) continue;
        if (!frame->duration || !Before({}, *frame->duration)) throw std::runtime_error("Video frame has no positive duration");
        frame->format.rotation = asset.video.rotation;
        std::vector<MediaPresentationSlice> slices;
        Require(MapToPresentation(asset.video.edits, frame->pts, Add(frame->pts, *frame->duration), slices));
        const auto cutoff = frame->Storage() == VideoStorage::Cpu ? cpu_preroll.value_or(target) : target;
        for (const auto& slice : slices) {
          if (!Before(cutoff, slice.presentation.end)) continue;
          auto copy = *frame;
          copy.pts = slice.presentation.start;
          copy.duration = Difference(slice.presentation.end, slice.presentation.start);
          pending_video.push_back(std::move(copy));
        }
      } catch (const CodecFailure& failure) {
        const bool invalid_output = failure.error.code == CodecErrorCode::InvalidData;
        if (failure.software || (!Recoverable(failure.error) && !invalid_output)) throw;
        const auto from = video->Provider();
        OpenVideo(last_video.value_or(target), true);
        force_software_video |= invalid_output;
        if (report) report({failure.operation, from, video->Provider(), failure.error, last_video.value_or(target)});
      }
    }
  }

  VideoFrame RetryVideoFrame(const VideoFrame& rejected, const PlatformVideoImportError& cause, std::stop_token stop) {
    CheckCancelled(stop);
    if (!ready || !video || !providers.platform || !providers.software ||
        rejected.Storage() != VideoStorage::Platform || !last_video || Before(*last_video, rejected.pts)) {
      throw cause;
    }
    const auto at = rejected.pts;
    const auto from = video->Software() ? std::string(providers.platform->Name()) : video->Provider();
    target = at;
    last_video.reset();
    OpenVideo(at, true);
    auto replacement = CheckVideoReplacement(ReadVideo(stop), rejected);
    force_software_video = true;
    if (report) report({CodecOperation::VideoDecode, from, video->Provider(),
                        {CodecErrorCode::Unsupported, cause.what(), {}}, at});
    return replacement;
  }

  void QueueAudio(const AudioFrame& frame) {
    if (frame.format != asset.pcm || !frame.Validate()) throw std::runtime_error("Audio decoder changed the negotiated PCM");
    const auto rate = asset.pcm.sample_rate;
    auto media_pts = AtScale(frame.pts, rate);
    // A Matroska segment may round packet timestamps to milliseconds while the decoded PCM is sample-exact.
    if (audio_packets && HasSegmentClock(*audio_packets) && audio_timing.next_sample) {
      const auto difference = Difference(media_pts, *audio_timing.next_sample).ticks;
      if (difference >= -std::int64_t(rate / 1000) && difference <= rate / 1000) media_pts = *audio_timing.next_sample;
    }
    auto start = media_pts;
    auto end = Add(media_pts, frame.Duration());
    audio_timing.next_sample = end;
    auto& discards = audio_timing.discards;
    while (!discards.empty() && Before(discards.front().first, frame.pts)) discards.pop_front();
    if (!discards.empty() && discards.front().first == frame.pts) {
      const auto trim = AtScale(discards.front().second, rate).ticks;
      discards.pop_front();
      if (trim < -std::int64_t(frame.sample_count) || trim > frame.sample_count)
        throw std::runtime_error("Packet trim exceeds its audio duration");
      start.ticks -= std::min<std::int64_t>(0, trim);
      end.ticks -= std::max<std::int64_t>(0, trim);
    }
    if (audio_timing.first_audible && Before(start, *audio_timing.first_audible)) start = *audio_timing.first_audible;
    if (!Before(start, end)) return;
    std::vector<MediaPresentationSlice> slices;
    Require(MapToPresentation(asset.audio.edits, start, end, slices));
    const auto range_start = AtScale(asset.audio_range.start, rate).ticks;
    const auto range_end = AtScale(asset.audio_range.end, rate).ticks;
    for (const auto& slice : slices) {
      auto pts = AtScale(slice.presentation.start, rate);
      const auto stop = std::min(AtScale(slice.presentation.end, rate).ticks, range_end);
      const auto begin = std::max(pts.ticks, range_start);
      if (stop <= begin) continue;
      const auto offset = AtScale(slice.media_start, rate).ticks - media_pts.ticks + (begin - pts.ticks);
      if (offset < 0 || offset > frame.sample_count || stop - begin > frame.sample_count - offset)
        throw std::runtime_error("Invalid PCM edit interval");
      pts.ticks = begin;
      pending_audio.push_back(SliceAudio(frame, offset, stop - begin, pts));
    }
  }

  std::optional<AudioFrame> ReadAudio(std::stop_token stop) {
    if (selected == TrackKind::Video) throw std::logic_error("The movie reader selected only the video track");
    if (!ready) throw std::runtime_error("Seek has not opened the selected movie tracks");
    const auto rate = asset.pcm.sample_rate;
    for (;;) {
      CheckCancelled(stop);
      const auto silence_until = !pending_audio.empty() ? std::optional{pending_audio.front().pts} :
                                 audio_end ? segment_audio_end : std::nullopt;
      // A segment duration can extend past the last packet of one track.
      if (silence_until && next_audio.ticks < silence_until->ticks) {
        const auto count = static_cast<std::uint32_t>(std::min<std::int64_t>(1024, silence_until->ticks - next_audio.ticks));
        auto bytes = std::make_shared<Bytes>(std::size_t(count) * asset.pcm.ChannelCount() * sizeof(std::int16_t));
        const auto pts = next_audio;
        next_audio.ticks += count;
        return AudioFrame{asset.pcm, pts, count, {BufferReference(*bytes, bytes)}};
      }
      if (!pending_audio.empty()) {
        auto frame_copy = std::move(pending_audio.front());
        pending_audio.pop_front();
        const auto skip = next_audio.ticks - frame_copy.pts.ticks;
        if (skip >= frame_copy.sample_count) continue;
        auto result = SliceAudio(frame_copy, static_cast<std::uint32_t>(skip),
                                 frame_copy.sample_count - static_cast<std::uint32_t>(skip), next_audio);
        next_audio.ticks += result.sample_count;
        return result;
      }
      if (audio_end) return std::nullopt;
      if (asset.audio.descriptions.front().codec == CodecId::PcmS16Le) {
        const auto packet = ReadPacket(*audio_packets, &audio_timing);
        if (!packet) { audio_end = true; continue; }
        const auto bytes_per_sample = asset.pcm.ChannelCount() * sizeof(std::int16_t);
        const auto size = packet->data.AsBytes().size();
        if (!size || size % bytes_per_sample || size / bytes_per_sample > std::numeric_limits<std::uint32_t>::max())
          throw std::runtime_error("Invalid PCM packet size");
        QueueAudio({asset.pcm, packet->pts, static_cast<std::uint32_t>(size / bytes_per_sample), {packet->data}});
        continue;
      }
      if (!audio) throw std::runtime_error("Seek to reopen the audio decoder");
      try {
        auto output = audio->Pull(stop);
        if (std::holds_alternative<EndOfStream>(output)) audio_end = true;
        if (const auto* frame = std::get_if<AudioFrame>(&output)) QueueAudio(*frame);
      } catch (const CodecFailure& failure) {
        if (failure.software || !Recoverable(failure.error)) throw;
        const auto from = audio->Provider();
        OpenAudio(next_audio, true);
        if (report && audio) report({failure.operation, from, audio->Provider(), failure.error, next_audio});
      }
    }
  }

  MovieAsset asset;
  CodecProviders providers;
  CodecPolicy policy;
  OnCodecFallback report;
  const std::optional<TrackKind> selected;
  std::optional<MediaDemuxer> video_packets, audio_packets;
  std::unique_ptr<Pump<VideoDecoderSession>> video;
  std::unique_ptr<Pump<AudioDecoderSession>> audio;
  AudioPacketTiming audio_timing;
  std::optional<MediaTimestamp> last_video, segment_audio_end;
  std::deque<VideoFrame> pending_video;
  std::deque<AudioFrame> pending_audio;
  MediaTimestamp target, next_audio;
  bool ready{}, video_end{}, audio_end{}, force_software_video{};
};

MovieReader::MovieReader(MovieAsset asset, CodecProviders providers, CodecPolicy policy, OnCodecFallback report)
    : impl_(std::make_unique<Impl>(std::move(asset), std::move(providers), policy, std::move(report))) {
  Seek({});
}

MovieReader::MovieReader(MovieAsset asset, CodecProviders providers, TrackKind track, MediaTimestamp start,
                         CodecPolicy policy, OnCodecFallback report)
    : impl_(std::make_unique<Impl>(std::move(asset), std::move(providers), policy, std::move(report), track)) {
  Seek(start);
}

MovieReader::~MovieReader() = default;

void MovieReader::Seek(MediaTimestamp at) {
  try { impl_->Seek(at); }
  catch (const CodecFailure& failure) { throw std::runtime_error(failure.error.message); }
}

std::optional<VideoFrame> MovieReader::ReadVideo(std::stop_token stop) {
  return ReadVideoInternal(stop, std::nullopt);
}

bool MovieReader::AllowsVideoSequenceHandoff() const noexcept {
  return impl_->selected == TrackKind::Video && impl_->video && impl_->video->AllowsSequenceHandoff();
}

void MovieReader::SetCodecFallbackHandler(OnCodecFallback report) { impl_->report = std::move(report); }

std::optional<VideoFrame> MovieReader::ReadVideoInternal(std::stop_token stop, std::optional<MediaTimestamp> cpu_preroll) {
  try { return impl_->ReadVideo(stop, cpu_preroll); }
  catch (const CodecFailure& failure) { throw std::runtime_error(failure.error.message); }
}

VideoFrame MovieReader::RetryVideoFrame(const VideoFrame& rejected, const PlatformVideoImportError& cause,
                                       std::stop_token stop) {
  try { return impl_->RetryVideoFrame(rejected, cause, stop); }
  catch (const CodecFailure& failure) { throw std::runtime_error(failure.error.message); }
}

VideoFrame MovieReader::RetryDetachedVideoFrame(const MovieAsset& movie, const CodecProviders& providers,
                                               const VideoFrame& rejected, const PlatformVideoImportError& cause,
                                               const OnCodecFallback& report, std::stop_token stop) {
  CheckCancelled(stop);
  if (!providers.platform || !providers.software || rejected.Storage() != VideoStorage::Platform) throw cause;
  MovieReader reader(movie, providers, TrackKind::Video, rejected.pts, CodecPolicy::SoftwareOnly);
  auto replacement = CheckVideoReplacement(reader.ReadVideo(stop), rejected);
  if (report) report({CodecOperation::VideoDecode, std::string(providers.platform->Name()),
                      std::string(providers.software->Name()), {CodecErrorCode::Unsupported, cause.what(), {}}, rejected.pts});
  return replacement;
}

std::optional<AudioFrame> MovieReader::ReadAudio(std::stop_token stop) {
  try { return impl_->ReadAudio(stop); }
  catch (const CodecFailure& failure) { throw std::runtime_error(failure.error.message); }
}

namespace {

template<class Format, class Session> Format ReadFormat(Pump<Session>& pump, std::stop_token stop) {
  auto event = pump.Pull(stop);
  if (auto* format = std::get_if<Format>(&event)) return std::move(*format);
  throw std::runtime_error("Encoder did not announce the output format before its packets");
}

struct MuxLane {
  std::uint32_t id;
  std::optional<ContainerPacket> packet;
};

std::pair<VideoEncoderConfig, VideoEncoderConfig> EncoderInputs(const FileExportSettings& settings) {
  auto platform = settings.video;
  platform.input_storage = VideoStorage::Platform;
  platform.input.pixel_format = kPlatformVideoLayout;
  auto software = settings.video;
  software.input_storage = VideoStorage::Cpu;
  software.input.pixel_format = VideoPixelFormat::I420;
  return {std::move(platform), std::move(software)};
}

void EncodeMp4(const File& destination, const CodecProviders& providers, const FileExportSettings& settings,
               const AudioFormat& pcm, std::uint32_t video_timescale, const ReadRecordingVideo& read_video,
               const ReadRecordingAudio& read_audio, const OnCodecFallback& report, bool software_only,
               std::stop_token stop = {}) {
  CheckCancelled(stop);
  const auto [platform, software] = EncoderInputs(settings);
  auto selected = OpenSelected<VideoEncoderSession>(providers, CodecOperation::VideoEncode, software_only,
                                                  platform, software, report);
  const auto contract = selected.session->Negotiated().video;
  if (!contract) throw std::runtime_error("Video encoder did not negotiate frame storage and layout");
  auto bound = settings.video;
  bound.input_storage = contract->storage;
  bound.input.pixel_format = contract->layout;
  const bool video_software = selected.software;
  Pump<VideoEncoderSession> video(std::move(selected), CodecOperation::VideoEncode, [&]() -> std::optional<VideoFrame> {
    try {
      auto output = read_video(bound);
      if (output && (output->format != bound.input || output->Storage() != bound.input_storage))
        throw std::runtime_error("The source returned a video frame outside the negotiated format");
      return output;
    } catch (const PlatformEncoderInputError& error) {
      if (bound.input_storage != VideoStorage::Platform) throw;
      throw CodecFailure{CodecOperation::VideoEncode, video_software,
                         {CodecErrorCode::Unsupported, error.what(), {}}};
    }
  });
  AudioEncoderConfig audio_config;
  audio_config.input = pcm;
  audio_config.bitrate = settings.audio_bitrate;
  std::optional<Pump<AudioEncoderSession>> audio;
  if (read_audio) {
    audio.emplace(OpenSelected<AudioEncoderSession>(providers, CodecOperation::AudioEncode,
                   software_only || settings.audio_policy == CodecPolicy::SoftwareOnly,
                   audio_config, audio_config, report), CodecOperation::AudioEncode, read_audio);
  }

  const auto picture = ReadFormat<VideoEncodeFormat>(video, stop);
  std::vector tracks{VideoTrack(picture, bound, video_timescale)};
  if (audio) tracks.push_back(AudioTrack(ReadFormat<AudioEncodeFormat>(*audio, stop)));
  Require(MediaMuxer::ValidateOutput(ContainerFormat::Mp4, tracks));
  StagedOutput stage(destination);
  auto muxer = Require(MediaMuxer::Open(stage.TakeStream(), ContainerFormat::Mp4, tracks));
  std::optional<AudioEncodeSummary> summary;
  const auto next_video = [&]() -> std::optional<ContainerPacket> {
    auto event = video.Pull(stop);
    if (auto* packet = std::get_if<EncodedPacket>(&event)) return ToContainer(*packet, 1);
    if (!std::holds_alternative<EndOfStream>(event)) throw std::runtime_error("Unexpected video encode event");
    Require(muxer.EndTrack(1));
    return std::nullopt;
  };
  const auto next_audio = [&]() -> std::optional<ContainerPacket> {
    if (!audio) return std::nullopt;
    for (;;) {
      auto event = audio->Pull(stop);
      if (auto* packet = std::get_if<EncodedPacket>(&event)) return ToContainer(*packet, 2);
      if (auto* counts = std::get_if<AudioEncodeSummary>(&event)) { summary = *counts; continue; }
      if (!std::holds_alternative<EndOfStream>(event) || !summary)
        throw std::runtime_error("AAC encoder ended without sample accounting");
      const auto& counts = *summary;
      if (!counts.priming_samples || !counts.padding_samples || *counts.priming_samples > counts.encoded_samples ||
          *counts.padding_samples > counts.encoded_samples - *counts.priming_samples ||
          counts.encoded_samples - *counts.priming_samples - *counts.padding_samples != counts.input_samples) {
        throw CodecFailure{CodecOperation::AudioEncode, audio->Software(),
                           {CodecErrorCode::PlatformFailure, "AAC encoder did not provide exact priming and padding", {}}};
      }
      Require(muxer.EndTrack(2, summary->priming_samples, summary->padding_samples));
      return std::nullopt;
    }
  };

  std::array<MuxLane, 2> lanes{{{1, next_video()}, {2, next_audio()}}};
  std::optional<std::uint32_t> required;
  while (lanes[0].packet || lanes[1].packet) {
    std::size_t index = 0;
    if (required) index = *required == 1 ? 0 : 1;
    else if (!lanes[0].packet || (lanes[1].packet &&
             Before(lanes[1].packet->dts.value(), lanes[0].packet->dts.value()))) index = 1;
    auto& lane = lanes[index];
    if (!lane.packet) throw std::runtime_error("MP4 muxer needs a packet from an exhausted track");
    const auto needed = Require(muxer.TryWrite(*lane.packet));
    if (needed) {
      if (*needed == lane.id || (required && *required != *needed))
        throw std::runtime_error("MP4 muxer made no interleaving progress");
      required = needed;
      continue;
    }
    required.reset();
    lane.packet.reset();
    lane.packet = index == 0 ? next_video() : next_audio();
  }
  CheckCancelled(stop);
  Require(muxer.Finish());
  CheckCancelled(stop);
  stage.Publish(destination);
}

template<class Attempt>
void ExportWithRestart(const CodecProviders& providers, CodecPolicy policy, const OnCodecFallback& report,
                       const Attempt& attempt) {
  try {
    try {
      attempt(policy == CodecPolicy::SoftwareOnly);
    } catch (const CodecFailure& failure) {
      if (failure.software || !Recoverable(failure.error) || !providers.software) throw;
      if (report) report({failure.operation, std::string(providers.platform->Name()),
                          std::string(providers.software->Name()), failure.error, {}});
      attempt(true);
    }
  } catch (const CodecFailure& failure) {
    throw std::runtime_error(failure.error.message);
  }
}

} // namespace

FileExportSettings FileExportSettings::Sdr(PixelSize size) {
  FileExportSettings settings;
  settings.video.input = {.width = size.width, .height = size.height, .visible = {0, 0, size.width, size.height},
      .primaries = ColorPrimaries::BT709, .transfer = ColorTransfer::BT709,
      .matrix = ColorMatrix::BT709, .range = ColorRange::Limited};
#if defined(__ANDROID__)
  // Android AAC encoders may omit the priming and padding required for an exact MP4 edit list.
  settings.audio_policy = CodecPolicy::SoftwareOnly;
#endif
  return settings;
}

media::CodecResult<CodecNegotiation> ProbeVideoExport(const CodecProviders& providers, const FileExportSettings& settings) {
  const auto [platform, software] = EncoderInputs(settings);
  using Probe = media::CodecResult<CodecNegotiation>;
  if (settings.policy != CodecPolicy::SoftwareOnly && providers.platform) {
    auto result = providers.platform->Probe(platform);
    if (result.Succeeded() || !providers.software || !Recoverable(result.Error())) return result;
  }
  if (providers.software) return providers.software->Probe(software);
  return Probe::Failure({CodecErrorCode::Unsupported, "No available video encoder", {}});
}

void WriteLiveMovie(const File& destination, const CodecProviders& providers, const FileExportSettings& settings,
                    const AudioFormat& pcm, const ReadRecordingVideo& video, const ReadRecordingAudio& audio,
                    OnCodecFallback on_fallback) {
  if (!video || !pcm.sample_rate) throw std::invalid_argument("Live MP4 requires a video source and a valid sample clock");
  try {
    EncodeMp4(destination, providers, settings, pcm, pcm.sample_rate, video, audio,
              on_fallback, settings.policy == CodecPolicy::SoftwareOnly);
  } catch (const CodecFailure& failure) {
    throw std::runtime_error(failure.error.message);
  }
}

void detail::ExportTimeline(std::shared_ptr<const ProjectSnapshot> project, const File& destination,
    const CodecProviders& providers, const FileExportSettings& settings,
    const std::shared_ptr<render::RenderBackend>& device, render::ImageInfo target,
    const ComposeTimelineFrame& compose, const render::TimelineExportOptions& options) {
  const auto stop = options.stop;
  const auto& report = options.on_fallback;
  CheckCancelled(stop);
  if (!project || !compose) throw std::invalid_argument("Export requires a project and a compositor");
  const auto& visible = settings.video.input.visible;
  const auto size = visible.width && visible.height ? PixelSize{visible.width, visible.height} : project->canvas_size;
  if (target.size != size) throw std::invalid_argument("The export renderer must match the output extent");
  const auto numerator = settings.video.frame_rate_numerator;
  const auto denominator = settings.video.frame_rate_denominator;
  const auto end = project->Range().end;
  if (!numerator || !denominator || !Before({}, end))
    throw std::invalid_argument("Timeline export requires a positive duration and frame rate");
  if (project->resources && std::ranges::any_of(project->resources->Media(), [&](const auto& entry) {
        return entry.second->file == destination;
      }))
    throw std::invalid_argument("Timeline export requires a destination separate from its source files");

  ExportWithRestart(providers, settings.policy, report, [&](bool software_only) {
    CheckCancelled(stop);
    auto session = TimelineSessionAccess::Open(project, providers, settings.policy,
        options.scene, report, device, target);
    const auto pcm = session->PcmFormat();
    const auto mux_end = detail::FloorOnGrid(end, pcm.sample_rate);
    const auto audio_end = mux_end.ticks;
    if (!audio_end) throw std::invalid_argument("Timeline is shorter than one audio sample");
    const auto scale = std::lcm<std::uint64_t>(pcm.sample_rate, numerator);
    if (scale > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("Incompatible export frame rate");
    const auto video_scale = static_cast<std::uint32_t>(scale);
    const auto step = (scale / numerator) * denominator;
    if (step > std::numeric_limits<std::int64_t>::max()) throw std::invalid_argument("Unsupported export frame rate");
    const auto video_end = detail::ExactTicks(mux_end, video_scale);
    std::int64_t video_at = 0, audio_at = 0;
    const std::function<void(const VideoFrame&)> on_decode = [&](const VideoFrame& picture) {
      if (options.on_video_transfer) options.on_video_transfer(CodecOperation::VideoDecode, picture);
    };
    EncodeMp4(destination, providers, settings, pcm, video_scale,
        [&](const VideoEncoderConfig& bound) -> std::optional<VideoFrame> {
          if (video_at == video_end) return std::nullopt;
          const MediaTimestamp pts{video_at, video_scale};
          const MediaTimestamp duration{std::min(std::int64_t(step), video_end - video_at), video_scale};
          const auto advance = video_at ? std::optional{MediaTimestamp{static_cast<std::int64_t>(step), video_scale}} : std::nullopt;
          auto output = compose(session->SampleVideo(pts, {}, stop, {}, advance, on_decode), bound, duration);
          if (output.pts != pts || output.duration != duration)
            throw std::runtime_error("Compositor changed the timeline video timing");
          video_at += duration.ticks;
          return output;
        }, [&]() -> std::optional<AudioFrame> {
          if (audio_at == audio_end) return std::nullopt;
          const auto count = static_cast<std::uint32_t>(std::min<std::int64_t>(1024, audio_end - audio_at));
          auto output = session->SampleAudio({audio_at, pcm.sample_rate}, count, stop);
          audio_at += count;
          return output;
        }, report, software_only, stop);
  });
}

} // namespace huxerui::avkit
