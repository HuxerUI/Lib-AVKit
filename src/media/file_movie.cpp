#include <huxerui/avkit/file_movie.h>
#include <huxerui/avkit/timeline.h>

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
Selection<Session> OpenSelected(const MovieCodecProviders& providers, CodecOperation operation, bool software_only,
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

std::optional<EncodedPacket> ReadPacket(MediaDemuxer& demuxer, std::optional<MediaTimestamp>* first_audible = nullptr) {
  while (auto item = Require(demuxer.Read())) {
    if (auto* start = std::get_if<TrackDecodeStart>(&*item)) {
      if (start->sample_description_index != 1) throw std::runtime_error("Sample description changed during file playback");
      if (first_audible) *first_audible = start->first_audible_pts;
      continue;
    }
    auto& packet = std::get<ContainerPacket>(*item);
    if (!packet.pts || packet.sample_description_index != 1)
      throw std::runtime_error("A file packet has no supported timing or sample description");
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

MovieAsset InspectMovie(const File& file, MediaDemuxer& demuxer) {
  if (demuxer.Format() != ContainerFormat::Mp4 && demuxer.Format() != ContainerFormat::Mov)
    throw std::runtime_error("The video container must be MP4 or MOV");
  std::optional<MediaTrack> video, audio;
  bool has_audio = false;
  for (const auto& track : demuxer.Tracks()) {
    has_audio |= track.kind == TrackKind::Audio;
    if (track.descriptions.size() != 1) continue;
    const auto& description = track.descriptions.front();
    if (!video && track.kind == TrackKind::Video &&
        (description.codec == CodecId::H264 || description.codec == CodecId::Hevc)) video = track;
    if (!audio && track.kind == TrackKind::Audio && description.codec == CodecId::Aac &&
        (description.sample_rate == 44100 || description.sample_rate == 48000) &&
        (description.channel_count == 1 || description.channel_count == 2) &&
        description.initialization_data.size() == 2 &&
        (std::to_integer<unsigned>(description.initialization_data[0]) >> 3) == 2) audio = track;
  }
  if (!video) throw std::runtime_error("The file needs a video track with one H.264 or HEVC configuration");
  const auto vrange = demuxer.PresentationRange(video->id);
  const auto& picture = video->descriptions.front();
  if (!vrange || !Before(vrange->start, vrange->end) || Before(vrange->start, {}) ||
      !picture.coded_width || !picture.coded_height || *picture.coded_width > 8192 || *picture.coded_height > 8192)
    throw std::runtime_error("The video presentation range or frame size is unsupported");
  if (picture.codec == CodecId::Hevc) {
    const auto& hvcc = picture.initialization_data;
    // hvcC stores chroma_format and both bit depths before the parameter-set arrays.
    if (hvcc.size() < 23 || (std::to_integer<unsigned>(hvcc[16]) & 3U) != 1U ||
        ((std::to_integer<unsigned>(hvcc[17]) | std::to_integer<unsigned>(hvcc[18])) & 7U))
      throw std::runtime_error("HEVC import requires 8-bit 4:2:0 video");
  }
  if (!has_audio) {
    const AudioFormat pcm{48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Mono};
    return {file, std::move(*video), {}, *vrange, {}, vrange->end, pcm};
  }
  if (!audio) throw std::runtime_error("The audio track must be mono or stereo AAC-LC at 44.1 or 48 kHz");
  const auto arange = demuxer.PresentationRange(audio->id);
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
  if (demuxer.Format() != ContainerFormat::Wave) {
    auto movie = InspectMovie(file, demuxer);
    if (!movie.HasAudio()) throw std::runtime_error("The selected video has no audio track");
    return movie;
  }
  const auto tracks = demuxer.Tracks();
  if (tracks.size() != 1 || tracks[0].kind != TrackKind::Audio || tracks[0].descriptions.size() != 1)
    throw std::runtime_error("Expected one prepared audio track");
  const auto& description = tracks[0].descriptions.front();
  const auto range = demuxer.PresentationRange(tracks[0].id);
  if (description.codec != CodecId::PcmS16Le || !description.sample_rate ||
      (*description.sample_rate != 44100 && *description.sample_rate != 48000) ||
      (description.channel_count != 1 && description.channel_count != 2) ||
      !range || !Before(range->start, range->end)) throw std::runtime_error("Invalid prepared PCM WAVE audio");
  const AudioFormat pcm{*description.sample_rate, AudioSampleFormat::S16, AudioPacking::Interleaved,
      description.channel_count == 1 ? AudioChannelLayout::Mono : AudioChannelLayout::Stereo};
  return {file, {}, tracks[0], {}, *range, range->end, pcm};
}

struct MovieReader::Impl {
  Impl(MovieAsset movie, MovieCodecProviders codec_providers, CodecPolicy selection_policy, OnCodecFallback fallback,
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
    config.output = kPlatformVideoLayout;
    config.output_storage = VideoStorage::Platform;
    auto cpu = config;
    cpu.output_storage = VideoStorage::Cpu;
    cpu.output = VideoPixelFormat::I420;
    video = std::make_unique<Pump<VideoDecoderSession>>(
        OpenSelected<VideoDecoderSession>(providers, CodecOperation::VideoDecode, software, config, cpu, report),
        CodecOperation::VideoDecode, [this] { return ReadPacket(*video_packets); });
  }

  void OpenAudio(MediaTimestamp at, bool software) {
    audio.reset();
    pending_audio.clear();
    audio_end = !asset.HasAudio() || !Before(at, asset.audio_range.end);
    if (audio_end) {
      audio_packets.reset();
      return;
    }
    audio_packets.emplace(OpenDemuxer(asset.file));
    if (asset.audio.descriptions.front().codec == CodecId::PcmS16Le) {
      Select(*audio_packets, asset.audio.id, at);
      return;
    }
    const auto seek = AtScale(at, asset.pcm.sample_rate);
    const MediaTimestamp preroll{std::max<std::int64_t>(0, seek.ticks - 4096), seek.timescale};
    Select(*audio_packets, asset.audio.id, preroll);
    AudioDecoderConfig config;
    config.output = asset.pcm;
    config.initialization_data = asset.audio.descriptions.front().initialization_data;
    audio_audible.reset();
    audio = std::make_unique<Pump<AudioDecoderSession>>(
        OpenSelected<AudioDecoderSession>(providers, CodecOperation::AudioDecode, software, config, config, report),
        CodecOperation::AudioDecode, [this] { return ReadPacket(*audio_packets, &audio_audible); });
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

  std::optional<AudioFrame> ReadAudio(std::stop_token stop) {
    if (selected == TrackKind::Video) throw std::logic_error("The movie reader selected only the video track");
    if (!ready) throw std::runtime_error("Seek has not opened the selected movie tracks");
    const auto rate = asset.pcm.sample_rate;
    for (;;) {
      CheckCancelled(stop);
      if (!pending_audio.empty()) {
        const auto& frame = pending_audio.front();
        if (next_audio.ticks < frame.pts.ticks) {
          const auto count = static_cast<std::uint32_t>(std::min<std::int64_t>(1024, frame.pts.ticks - next_audio.ticks));
          auto bytes = std::make_shared<Bytes>(std::size_t(count) * asset.pcm.ChannelCount() * sizeof(std::int16_t));
          auto pts = next_audio;
          next_audio.ticks += count;
          return AudioFrame{asset.pcm, pts, count, {BufferReference(*bytes, bytes)}};
        }
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
        const auto packet = ReadPacket(*audio_packets);
        if (!packet) { audio_end = true; continue; }
        const auto bytes_per_sample = asset.pcm.ChannelCount() * sizeof(std::int16_t);
        const auto size = packet->data.AsBytes().size();
        if (!size || size % bytes_per_sample || size / bytes_per_sample > std::numeric_limits<std::uint32_t>::max())
          throw std::runtime_error("Invalid PCM packet size");
        pending_audio.push_back({asset.pcm, AtScale(packet->pts, rate),
                                 static_cast<std::uint32_t>(size / bytes_per_sample), {packet->data}});
        continue;
      }
      if (!audio) throw std::runtime_error("Seek to reopen the audio decoder");
      try {
        auto output = audio->Pull(stop);
        if (std::holds_alternative<EndOfStream>(output)) audio_end = true;
        auto* frame = std::get_if<AudioFrame>(&output);
        if (!frame) continue;
        if (frame->format != asset.pcm || !frame->Validate()) throw std::runtime_error("Audio decoder changed the negotiated PCM");
        const auto media_pts = AtScale(frame->pts, rate);
        const auto media_end = Add(media_pts, frame->Duration());
        auto media_start = media_pts;
        if (audio_audible && Before(media_start, *audio_audible)) media_start = *audio_audible;
        if (!Before(media_start, media_end)) continue;
        std::vector<MediaPresentationSlice> slices;
        Require(MapToPresentation(asset.audio.edits, media_start, media_end, slices));
        for (const auto& slice : slices) {
          const auto pts = AtScale(slice.presentation.start, rate);
          const auto end = AtScale(slice.presentation.end, rate);
          const auto offset = AtScale(slice.media_start, rate).ticks - media_pts.ticks;
          if (offset < 0 || offset > frame->sample_count || end.ticks < pts.ticks ||
              end.ticks - pts.ticks > frame->sample_count - offset)
            throw std::runtime_error("Invalid PCM edit interval");
          if (end.ticks != pts.ticks)
            pending_audio.push_back(SliceAudio(*frame, offset, end.ticks - pts.ticks, pts));
        }
      } catch (const CodecFailure& failure) {
        if (failure.software || !Recoverable(failure.error)) throw;
        const auto from = audio->Provider();
        OpenAudio(next_audio, true);
        if (report && audio) report({failure.operation, from, audio->Provider(), failure.error, next_audio});
      }
    }
  }

  MovieAsset asset;
  MovieCodecProviders providers;
  CodecPolicy policy;
  OnCodecFallback report;
  const std::optional<TrackKind> selected;
  std::optional<MediaDemuxer> video_packets, audio_packets;
  std::unique_ptr<Pump<VideoDecoderSession>> video;
  std::unique_ptr<Pump<AudioDecoderSession>> audio;
  std::optional<MediaTimestamp> audio_audible, last_video;
  std::deque<VideoFrame> pending_video;
  std::deque<AudioFrame> pending_audio;
  MediaTimestamp target, next_audio;
  bool ready{}, video_end{}, audio_end{}, force_software_video{};
};

MovieReader::MovieReader(MovieAsset asset, MovieCodecProviders providers, CodecPolicy policy, OnCodecFallback report)
    : impl_(std::make_unique<Impl>(std::move(asset), std::move(providers), policy, std::move(report))) {
  Seek({});
}

MovieReader::MovieReader(MovieAsset asset, MovieCodecProviders providers, TrackKind track, MediaTimestamp start,
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
VideoFrame MovieReader::RetryDetachedVideoFrame(const MovieAsset& movie, const MovieCodecProviders& providers,
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

template<class Format, class Session> Format ReadFormat(Pump<Session>& pump) {
  auto event = pump.Pull();
  if (auto* format = std::get_if<Format>(&event)) return std::move(*format);
  throw std::runtime_error("Encoder did not announce the output format before its packets");
}

struct MuxLane {
  std::uint32_t id;
  std::optional<ContainerPacket> packet;
};

void EncodeMp4(const File& destination, const MovieCodecProviders& providers, const FileExportSettings& settings,
               const AudioFormat& pcm, std::uint32_t video_timescale, const ReadRecordingVideo& read_video,
               const ReadRecordingAudio& read_audio, const OnCodecFallback& report, bool software_only) {
  auto platform = settings.video;
  platform.input_storage = VideoStorage::Platform;
  platform.input.pixel_format = kPlatformVideoLayout;
  auto software = settings.video;
  software.input_storage = VideoStorage::Cpu;
  software.input.pixel_format = VideoPixelFormat::I420;
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
    } catch (const PlatformVideoTransferError& error) {
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

  const auto picture = ReadFormat<VideoEncodeFormat>(video);
  std::vector tracks{VideoTrack(picture, bound, video_timescale)};
  if (audio) tracks.push_back(AudioTrack(ReadFormat<AudioEncodeFormat>(*audio)));
  Require(MediaMuxer::ValidateOutput(ContainerFormat::Mp4, tracks));
  StagedOutput stage(destination);
  auto muxer = Require(MediaMuxer::Open(stage.TakeStream(), ContainerFormat::Mp4, tracks));
  std::optional<AudioEncodeSummary> summary;
  const auto next_video = [&]() -> std::optional<ContainerPacket> {
    auto event = video.Pull();
    if (auto* packet = std::get_if<EncodedPacket>(&event)) return ToContainer(*packet, 1);
    if (!std::holds_alternative<EndOfStream>(event)) throw std::runtime_error("Unexpected video encode event");
    Require(muxer.EndTrack(1));
    return std::nullopt;
  };
  const auto next_audio = [&]() -> std::optional<ContainerPacket> {
    if (!audio) return std::nullopt;
    for (;;) {
      auto event = audio->Pull();
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
  Require(muxer.Finish());
  stage.Publish(destination);
}

void ExportAttempt(const MovieAsset& asset, const File& destination, const MovieCodecProviders& providers,
                   const FileExportSettings& settings, const ComposeMovieFrame& compose,
                   const OnCodecFallback& report, bool software_only) {
  MovieReader source(asset, providers, settings.policy, report);
  ReadRecordingAudio read_audio;
  if (asset.HasAudio()) read_audio = [&] { return source.ReadAudio(); };
  EncodeMp4(destination, providers, settings, asset.pcm, asset.video.timescale.value_or(90000),
      [&](const VideoEncoderConfig& bound) -> std::optional<VideoFrame> {
        auto input = source.ReadVideo();
        if (!input) return std::nullopt;
        auto output = [&] {
          try {
            return compose(*input, bound);
          } catch (const PlatformVideoImportError& error) {
            input = source.RetryVideoFrame(*input, error);
            return compose(*input, bound);
          }
        }();
        if (output.pts != input->pts || output.duration != input->duration)
          throw std::runtime_error("Compositor changed source video timing");
        return output;
      }, read_audio, report, software_only);
}

template<class Attempt>
void ExportWithRestart(const MovieCodecProviders& providers, CodecPolicy policy, const OnCodecFallback& report,
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

void ExportMovie(const MovieAsset& asset, const File& destination, const MovieCodecProviders& providers,
                 const FileExportSettings& settings, const ComposeMovieFrame& compose, OnCodecFallback report) {
  if (!compose || destination == asset.file) throw std::invalid_argument("Export requires a compositor and a new destination");
  ExportWithRestart(providers, settings.policy, report, [&](bool software_only) {
    ExportAttempt(asset, destination, providers, settings, compose, report, software_only);
  });
}

void WriteLiveMovie(const File& destination, const MovieCodecProviders& providers, const FileExportSettings& settings,
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

void ExportTimeline(std::shared_ptr<const ProjectSnapshot> project, const File& destination,
                    const MovieCodecProviders& providers, const FileExportSettings& settings,
                    const ComposeTimelineFrame& compose, OnCodecFallback report,
                    std::shared_ptr<analysis::AnalysisProvider> analysis, const ImportVideoFrame& import,
                    render::PixelFormat target_format) {
  if (!project || !compose) throw std::invalid_argument("Export requires a project and a compositor");
  const auto numerator = settings.video.frame_rate_numerator;
  const auto denominator = settings.video.frame_rate_denominator;
  const auto end = project->Range().end;
  if (!numerator || !denominator || !Before({}, end))
    throw std::invalid_argument("Timeline export requires a positive duration and frame rate");
  if (std::ranges::any_of(project->assets, [&](const auto& asset) { return asset.source.file == destination; }))
    throw std::invalid_argument("Timeline export requires a destination separate from its source files");

  ExportWithRestart(providers, settings.policy, report, [&](bool software_only) {
    TimelineSession session(project, providers, settings.policy, report, analysis);
    const auto pcm = session.PcmFormat();
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
    EncodeMp4(destination, providers, settings, pcm, video_scale,
        [&](const VideoEncoderConfig& bound) -> std::optional<VideoFrame> {
          if (video_at == video_end) return std::nullopt;
          const MediaTimestamp pts{video_at, video_scale};
          const MediaTimestamp duration{std::min(std::int64_t(step), video_end - video_at), video_scale};
          auto output = compose(session.SampleVideo(pts, target_format, {}, import), bound, duration);
          if (output.pts != pts || output.duration != duration)
            throw std::runtime_error("Compositor changed the timeline video timing");
          video_at += duration.ticks;
          return output;
        }, [&]() -> std::optional<AudioFrame> {
          if (audio_at == audio_end) return std::nullopt;
          const auto count = static_cast<std::uint32_t>(std::min<std::int64_t>(1024, audio_end - audio_at));
          auto output = session.SampleAudio({audio_at, pcm.sample_rate}, count);
          audio_at += count;
          return output;
        }, report, software_only);
  });
}

} // namespace huxerui::avkit
