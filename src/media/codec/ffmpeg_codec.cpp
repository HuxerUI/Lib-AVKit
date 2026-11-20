#include <huxerui/avkit/codec.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace huxerui::avkit {
namespace {

using namespace media;
template<class T, void (*Free)(T**)> struct AvDelete {
  void operator()(T* value) const noexcept { Free(&value); }
};
using Context = std::unique_ptr<AVCodecContext, AvDelete<AVCodecContext, avcodec_free_context>>;
using Packet = std::unique_ptr<AVPacket, AvDelete<AVPacket, av_packet_free>>;
using Frame = std::unique_ptr<AVFrame, AvDelete<AVFrame, av_frame_free>>;
using Resampler = std::unique_ptr<SwrContext, AvDelete<SwrContext, swr_free>>;

struct Failure { CodecError error; };
[[noreturn]] void Fail(CodecErrorCode code, std::string message) {
  throw Failure{{code, std::move(message), {}}};
}

void Check(int status, const char* operation, CodecErrorCode code = CodecErrorCode::PlatformFailure) {
  if (status >= 0) return;
  char text[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(status, text, sizeof(text));
  throw Failure{{status == AVERROR(ENOMEM) ? CodecErrorCode::ResourceExhausted : code,
                 std::string(operation) + ": " + text, std::to_string(status)}};
}

template<class T, class Work> CodecResult<T> Guard(Work work) {
  using R = CodecResult<T>;
  try {
    if constexpr (std::is_void_v<T>) { work(); return R::Success(); }
    else return R::Success(work());
  } catch (const Failure& failure) {
    return R::Failure(failure.error);
  } catch (const std::bad_alloc&) {
    return R::Failure({CodecErrorCode::ResourceExhausted, "Cannot allocate FFmpeg codec storage", {}});
  }
}

template<class T> T CheckedAllocation(T value) {
  if (!value) throw std::bad_alloc();
  return value;
}

Context Allocate(const AVCodec* codec) {
  if (!codec) Fail(CodecErrorCode::Unsupported, "The requested software codec is unavailable in this FFmpeg build");
  Context context(CheckedAllocation(avcodec_alloc_context3(codec)));
  context->thread_count = 1;
  return context;
}

BufferReference RetainBytes(std::shared_ptr<Bytes> bytes) { return BufferReference(*bytes, bytes); }

BufferReference CopyBytes(const void* data, std::size_t size) {
  auto bytes = std::make_shared<Bytes>(size);
  std::memcpy(bytes->data(), data, size);
  return RetainBytes(std::move(bytes));
}

void SetExtra(AVCodecContext& context, const Bytes& bytes) {
  if (bytes.empty() || bytes.size() > 64 * 1024) Fail(CodecErrorCode::InvalidArgument, "Invalid decoder configuration size");
  context.extradata = static_cast<std::uint8_t*>(CheckedAllocation(av_mallocz(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE)));
  std::memcpy(context.extradata, bytes.data(), bytes.size());
  context.extradata_size = static_cast<int>(bytes.size());
}

MediaTimestamp Ticks(MediaTimestamp time, std::uint32_t scale) {
  auto value = time.Rescale(scale);
  if (!value) Fail(CodecErrorCode::InvalidData, "Invalid or unrepresentable codec timestamp");
  return *value;
}

struct Timing {
  MediaTimestamp pts;
  std::optional<MediaTimestamp> duration;
};

AVBufferRef* MakeTiming(Timing timing) {
  auto owner = std::make_unique<Timing>(timing);
  auto* ref = av_buffer_create(reinterpret_cast<std::uint8_t*>(owner.get()), sizeof(Timing),
                              [](void*, std::uint8_t* p) { delete reinterpret_cast<Timing*>(p); }, nullptr, 0);
  CheckedAllocation(ref);
  owner.release();
  return ref;
}

const Timing& GetTiming(const AVBufferRef* ref) {
  if (!ref || ref->size != sizeof(Timing)) Fail(CodecErrorCode::InvalidData, "FFmpeg lost the input timestamp association");
  return *reinterpret_cast<const Timing*>(ref->data);
}

bool SupportedPcm(const AudioFormat& format) {
  return (format.sample_rate == 44100 || format.sample_rate == 48000) &&
         format.sample_format == AudioSampleFormat::S16 && format.packing == AudioPacking::Interleaved &&
         (format.channel_layout == AudioChannelLayout::Mono || format.channel_layout == AudioChannelLayout::Stereo);
}

bool MatchesAacLc(std::span<const std::byte> data, const AudioFormat& pcm) {
  if (data.size() != 2 || !SupportedPcm(pcm)) return false;
  const auto expected_rate_index = pcm.sample_rate == 48000 ? 3u : 4u;
  const auto expected = (2u << 11) | (expected_rate_index << 7) | (pcm.ChannelCount() << 3);
  const auto bits = (std::to_integer<unsigned>(data[0]) << 8) | std::to_integer<unsigned>(data[1]);
  return bits == expected;
}

CodecNegotiation Negotiation(const VideoDecoderConfig& config) {
  return {CodecImplementation::Software, VideoFrameContract{config.output_storage, config.output}, true};
}
CodecNegotiation Negotiation(const VideoEncoderConfig& config) {
  return {CodecImplementation::Software, VideoFrameContract{config.input_storage, config.input.pixel_format}};
}
CodecNegotiation Negotiation(const AudioDecoderConfig&) { return {CodecImplementation::Software, std::nullopt}; }
CodecNegotiation Negotiation(const AudioEncoderConfig&) { return {CodecImplementation::Software, std::nullopt}; }

Resampler MakeResampler(const AVChannelLayout& out, AVSampleFormat output, const AVChannelLayout& in,
                        AVSampleFormat input, int rate) {
  SwrContext* value = nullptr;
  const auto status = swr_alloc_set_opts2(&value, &out, output, rate, &in, input, rate, 0, nullptr);
  Resampler result(value);
  Check(status, "Configure PCM conversion");
  Check(swr_init(result.get()), "Initialize PCM conversion");
  return result;
}

VideoFrame ReadPicture(const AVFrame& decoded, const VideoDecoderConfig& config) {
  if ((decoded.format != AV_PIX_FMT_YUV420P && decoded.format != AV_PIX_FMT_YUVJ420P) ||
      decoded.width <= 0 || decoded.height <= 0 || static_cast<unsigned>(decoded.width) > config.width ||
      static_cast<unsigned>(decoded.height) > config.height) {
    Fail(CodecErrorCode::Unsupported, "Software H.264 output must be 8-bit 4:2:0 within the configured bounds");
  }
  const auto width = static_cast<std::uint32_t>(decoded.width);
  const auto height = static_cast<std::uint32_t>(decoded.height);
  VideoFormat format{width, height, {0, 0, width, height}, VideoPixelFormat::I420};
  if (decoded.sample_aspect_ratio.num > 0 && decoded.sample_aspect_ratio.den > 0) {
    format.aspect_width = decoded.sample_aspect_ratio.num;
    format.aspect_height = decoded.sample_aspect_ratio.den;
  }
  if (decoded.color_primaries == AVCOL_PRI_BT709) format.primaries = ColorPrimaries::BT709;
  if (decoded.color_primaries == AVCOL_PRI_SMPTE170M) format.primaries = ColorPrimaries::BT601_525;
  if (decoded.color_primaries == AVCOL_PRI_BT470BG) format.primaries = ColorPrimaries::BT601_625;
  if (decoded.color_primaries == AVCOL_PRI_BT2020) format.primaries = ColorPrimaries::BT2020;
  if (decoded.color_trc == AVCOL_TRC_BT709) format.transfer = ColorTransfer::BT709;
  if (decoded.color_trc == AVCOL_TRC_IEC61966_2_1) format.transfer = ColorTransfer::SRGB;
  if (decoded.color_trc == AVCOL_TRC_SMPTE2084) format.transfer = ColorTransfer::PQ;
  if (decoded.color_trc == AVCOL_TRC_ARIB_STD_B67) format.transfer = ColorTransfer::HLG;
  if (decoded.colorspace == AVCOL_SPC_BT709) format.matrix = ColorMatrix::BT709;
  if (decoded.colorspace == AVCOL_SPC_SMPTE170M || decoded.colorspace == AVCOL_SPC_BT470BG)
    format.matrix = ColorMatrix::BT601;
  if (decoded.colorspace == AVCOL_SPC_BT2020_NCL) format.matrix = ColorMatrix::BT2020;
  if (decoded.color_range == AVCOL_RANGE_JPEG || decoded.format == AV_PIX_FMT_YUVJ420P) format.range = ColorRange::Full;
  if (decoded.color_range == AVCOL_RANGE_MPEG) format.range = ColorRange::Limited;
  std::vector<VideoPlane> planes;
  for (int i = 0; i < 3; ++i) {
    const auto columns = i ? (width + 1) / 2 : width;
    const auto rows = i ? (height + 1) / 2 : height;
    auto bytes = std::make_shared<Bytes>(std::size_t(columns) * rows);
    for (std::uint32_t y = 0; y < rows; ++y)
      std::memcpy(bytes->data() + std::size_t(y) * columns,
                  decoded.data[i] + std::ptrdiff_t(y) * decoded.linesize[i], columns);
    planes.push_back({RetainBytes(std::move(bytes)), columns, 1});
  }
  const auto& timing = GetTiming(decoded.opaque_ref);
  return {format, timing.pts, timing.duration, std::move(planes)};
}

AudioFrame ReadPcm(const AVFrame& decoded, const AudioDecoderConfig& config) {
  const auto& format = config.output;
  if (decoded.sample_rate != static_cast<int>(format.sample_rate) ||
      decoded.ch_layout.nb_channels != static_cast<int>(format.ChannelCount()) || decoded.nb_samples <= 0) {
    Fail(CodecErrorCode::InvalidData, "AAC decoded PCM differs from the negotiated format");
  }
  auto resampler = MakeResampler(decoded.ch_layout, AV_SAMPLE_FMT_S16, decoded.ch_layout,
                                 static_cast<AVSampleFormat>(decoded.format), decoded.sample_rate);
  const auto samples = static_cast<std::uint32_t>(decoded.nb_samples);
  auto bytes = std::make_shared<Bytes>(std::size_t(samples) * format.ChannelCount() * sizeof(std::int16_t));
  auto* destination = reinterpret_cast<std::uint8_t*>(bytes->data());
  const auto converted = swr_convert(resampler.get(), &destination, decoded.nb_samples,
                                     const_cast<const std::uint8_t**>(decoded.extended_data), decoded.nb_samples);
  Check(converted, "Convert AAC PCM");
  if (converted != decoded.nb_samples) Fail(CodecErrorCode::PlatformFailure, "AAC PCM conversion changed the sample count");
  return {format, GetTiming(decoded.opaque_ref).pts, samples, {RetainBytes(std::move(bytes))}};
}

template<class Session, class Config, class MediaFrame, class Format>
class Decoder final : public Session {
public:
  Decoder(Context context, Config config) : context_(std::move(context)), config_(std::move(config)) {}

  CodecNegotiation Negotiated() const noexcept override { return Negotiation(config_); }
  CodecResult<SubmitStatus> TrySubmit(const EncodedPacket& input) override {
    return Guard<SubmitStatus>([&] {
      CheckOpen();
      if (finishing_) Fail(CodecErrorCode::InvalidState, "Decoder input is sealed");
      const auto data = input.data.AsBytes();
      if (data.empty() || data.size() > config_.limits.input_bytes || data.size() > std::numeric_limits<int>::max())
        Fail(CodecErrorCode::InvalidData, "Invalid compressed packet size");
      Packet packet(CheckedAllocation(av_packet_alloc()));
      Check(av_new_packet(packet.get(), static_cast<int>(data.size())), "Allocate compressed packet");
      std::memcpy(packet->data, data.data(), data.size());
      packet->pts = Ticks(input.pts, kClock).ticks;
      packet->dts = input.dts ? Ticks(*input.dts, kClock).ticks : AV_NOPTS_VALUE;
      if (input.duration) packet->duration = Ticks(*input.duration, kClock).ticks;
      if (input.key_frame) packet->flags |= AV_PKT_FLAG_KEY;
      packet->opaque_ref = MakeTiming({input.pts, input.duration});
      const int status = avcodec_send_packet(context_.get(), packet.get());
      if (status == AVERROR(EAGAIN)) return SubmitStatus::WouldBlock;
      Check(status, "Decode compressed packet", CodecErrorCode::InvalidData);
      return SubmitStatus::Accepted;
    });
  }
  CodecResult<std::optional<typename Session::Output>> TryRead() override {
    using Output = typename Session::Output;
    return Guard<std::optional<Output>>([&]() -> std::optional<Output> {
      CheckOpen();
      if (pending_) return Output(std::move(*std::exchange(pending_, std::nullopt)));
      if (ended_) return std::nullopt;
      Frame decoded(CheckedAllocation(av_frame_alloc()));
      int status = avcodec_receive_frame(context_.get(), decoded.get());
      if (status == AVERROR(EAGAIN) && finishing_ && !draining_) {
        Check(avcodec_send_packet(context_.get(), nullptr), "Drain decoder");
        draining_ = true;
        status = avcodec_receive_frame(context_.get(), decoded.get());
      }
      if (status == AVERROR(EAGAIN)) return std::nullopt;
      if (status == AVERROR_EOF) { ended_ = true; return Output(EndOfStream{}); }
      Check(status, "Read decoded frame", CodecErrorCode::InvalidData);
      MediaFrame frame;
      if constexpr (std::is_same_v<MediaFrame, VideoFrame>) frame = ReadPicture(*decoded, config_);
      else frame = ReadPcm(*decoded, config_);
      if (!format_ || *format_ != frame.format) {
        format_ = frame.format;
        pending_ = std::move(frame);
        return Output(*format_);
      }
      return Output(std::move(frame));
    });
  }
  CodecResult<void> FinishInput() override { return Guard<void>([&] { CheckOpen(); finishing_ = true; }); }
  void Close() noexcept override { pending_.reset(); context_.reset(); }

  static constexpr std::uint32_t kClock = 1'000'000;

private:
  void CheckOpen() const { if (!context_) Fail(CodecErrorCode::Closed, "Decoder is closed"); }
  Context context_;
  Config config_;
  std::optional<Format> format_;
  std::optional<MediaFrame> pending_;
  bool finishing_{}, draining_{}, ended_{};
};

Bytes AvcConfiguration(const AVCodecContext& context, H264Profile profile) {
  auto data = std::span<const std::uint8_t>(context.extradata, context.extradata_size);
  Bytes result;
  if (data.size() >= 7 && data[0] == 1) {
    result.assign(reinterpret_cast<const std::byte*>(data.data()),
                  reinterpret_cast<const std::byte*>(data.data() + data.size()));
  } else {
    std::span<const std::uint8_t> sps, pps;
    while (data.size() >= 4) {
      const std::size_t length = (std::size_t(data[0]) << 24) | (std::size_t(data[1]) << 16) |
                                 (std::size_t(data[2]) << 8) | data[3];
      data = data.subspan(4);
      if (!length || length > data.size()) break;
      if ((data[0] & 31) == 7) sps = data.first(length);
      if ((data[0] & 31) == 8) pps = data.first(length);
      data = data.subspan(length);
    }
    if (sps.size() < 4 || sps.size() > 65535 || pps.empty() || pps.size() > 65535)
      Fail(CodecErrorCode::PlatformFailure, "libx264 did not emit usable SPS and PPS");
    result = {std::byte{1}, std::byte{sps[1]}, std::byte{sps[2]}, std::byte{sps[3]}, std::byte{255}, std::byte{225}};
    const auto append = [&](std::span<const std::uint8_t> nal) {
      result.push_back(std::byte((nal.size() >> 8) & 255));
      result.push_back(std::byte(nal.size() & 255));
      auto bytes = std::as_bytes(nal);
      result.insert(result.end(), bytes.begin(), bytes.end());
    };
    append(sps);
    result.push_back(std::byte{1});
    append(pps);
  }
  const auto requested = profile == H264Profile::High ? 100 : profile == H264Profile::Main ? 77 : 66;
  if (std::to_integer<int>(result[1]) != requested)
    Fail(CodecErrorCode::Unsupported, "libx264 changed the requested H.264 profile");
  return result;
}

class VideoEncode final : public VideoEncoderSession {
public:
  VideoEncode(Context context, VideoEncoderConfig config, VideoEncodeFormat format)
      : context_(std::move(context)), config_(std::move(config)), format_(std::move(format)) {}

  CodecNegotiation Negotiated() const noexcept override { return Negotiation(config_); }
  CodecResult<SubmitStatus> TrySubmit(const VideoFrame& input) override {
    return Guard<SubmitStatus>([&] {
      CheckOpen();
      if (finishing_) Fail(CodecErrorCode::InvalidState, "Video encoder input is sealed");
      if (!input.Validate() || input.format != config_.input || input.Storage() != VideoStorage::Cpu ||
          (last_ && input.pts.Compare(*last_) != 1))
        Fail(CodecErrorCode::InvalidData, "Invalid software encoder frame format or PTS");
      Frame frame(CheckedAllocation(av_frame_alloc()));
      frame->format = context_->pix_fmt;
      frame->width = context_->width;
      frame->height = context_->height;
      frame->pts = Ticks(input.pts, kClock).ticks;
      if (input.duration) frame->duration = Ticks(*input.duration, kClock).ticks;
      frame->opaque_ref = MakeTiming({input.pts, input.duration});
      Check(av_frame_get_buffer(frame.get(), 32), "Allocate encoder picture");
      for (int i = 0; i < 3; ++i) {
        const auto& plane = input.planes[i];
        const int width = i ? frame->width / 2 : frame->width;
        const int height = i ? frame->height / 2 : frame->height;
        for (int y = 0; y < height; ++y) {
          const auto* source = plane.data.AsBytes().data() + std::size_t(y) * plane.row_stride;
          auto* destination = frame->data[i] + std::size_t(y) * frame->linesize[i];
          if (plane.pixel_stride == 1) std::memcpy(destination, source, width);
          else for (int x = 0; x < width; ++x) destination[x] = std::to_integer<std::uint8_t>(source[x * plane.pixel_stride]);
        }
      }
      const int status = avcodec_send_frame(context_.get(), frame.get());
      if (status == AVERROR(EAGAIN)) return SubmitStatus::WouldBlock;
      Check(status, "Encode H.264 picture");
      last_ = input.pts;
      return SubmitStatus::Accepted;
    });
  }
  CodecResult<std::optional<VideoEncodeOutput>> TryRead() override {
    return Guard<std::optional<VideoEncodeOutput>>([&]() -> std::optional<VideoEncodeOutput> {
      CheckOpen();
      if (!announced_) { announced_ = true; return format_; }
      if (ended_) return std::nullopt;
      Packet packet(CheckedAllocation(av_packet_alloc()));
      int status = avcodec_receive_packet(context_.get(), packet.get());
      if (status == AVERROR(EAGAIN) && finishing_ && !draining_) {
        Check(avcodec_send_frame(context_.get(), nullptr), "Drain video encoder");
        draining_ = true;
        status = avcodec_receive_packet(context_.get(), packet.get());
      }
      if (status == AVERROR(EAGAIN)) return std::nullopt;
      if (status == AVERROR_EOF) { ended_ = true; return EndOfStream{}; }
      Check(status, "Read H.264 packet");
      if (packet->size <= 0 || std::size_t(packet->size) > config_.max_packet_bytes)
        Fail(CodecErrorCode::ResourceExhausted, "Software video packet exceeds the requested bound");
      const auto& time = GetTiming(packet->opaque_ref);
      auto duration = time.duration.value_or(MediaTimestamp{config_.frame_rate_denominator, config_.frame_rate_numerator});
      return EncodedPacket{CopyBytes(packet->data, packet->size), time.pts, time.pts, duration,
                           (packet->flags & AV_PKT_FLAG_KEY) != 0};
    });
  }
  CodecResult<void> FinishInput() override { return Guard<void>([&] { CheckOpen(); finishing_ = true; }); }
  void Close() noexcept override { context_.reset(); }

  static constexpr std::uint32_t kClock = 90'000;

private:
  void CheckOpen() const { if (!context_) Fail(CodecErrorCode::Closed, "Video encoder is closed"); }
  Context context_;
  VideoEncoderConfig config_;
  VideoEncodeFormat format_;
  std::optional<MediaTimestamp> last_;
  bool announced_{}, finishing_{}, draining_{}, ended_{};
};

class AudioEncode final : public AudioEncoderSession {
public:
  AudioEncode(Context context, AudioEncoderConfig config)
      : context_(std::move(context)), config_(std::move(config)),
        resampler_(MakeResampler(context_->ch_layout, context_->sample_fmt, context_->ch_layout,
                                AV_SAMPLE_FMT_S16, context_->sample_rate)) {}

  CodecNegotiation Negotiated() const noexcept override { return Negotiation(config_); }
  CodecResult<SubmitStatus> TrySubmit(const AudioFrame& input) override {
    return Guard<SubmitStatus>([&] {
      CheckOpen();
      if (finishing_) Fail(CodecErrorCode::InvalidState, "Audio encoder input is sealed");
      const auto pts = Ticks(input.pts, config_.input.sample_rate);
      if (!input.Validate() || input.format != config_.input ||
          (start_ && pts.ticks != start_->ticks + static_cast<std::int64_t>(input_samples_)))
        Fail(CodecErrorCode::InvalidData, "Software audio encoder requires contiguous negotiated PCM");
      const std::size_t bytes = std::size_t(input.sample_count) * config_.input.ChannelCount() * sizeof(std::int16_t);
      if (bytes > config_.limits.input_bytes) Fail(CodecErrorCode::InvalidData, "PCM frame exceeds the input bound");
      if (pcm_.size() + bytes > config_.limits.input_bytes) return SubmitStatus::WouldBlock;
      auto data = input.planes[0].AsBytes().first(bytes);
      pcm_.insert(pcm_.end(), data.begin(), data.end());
      if (!start_) start_ = pts;
      input_samples_ += input.sample_count;
      return SubmitStatus::Accepted;
    });
  }
  CodecResult<std::optional<AudioEncodeOutput>> TryRead() override {
    return Guard<std::optional<AudioEncodeOutput>>([&]() -> std::optional<AudioEncodeOutput> {
      CheckOpen();
      const auto rate = config_.input.sample_rate;
      if (!announced_) {
        announced_ = true;
        return AudioEncodeFormat{rate, config_.input.channel_layout,
                                 {std::byte{context_->extradata[0]}, std::byte{context_->extradata[1]}}, CodecId::Aac};
      }
      if (ended_) return std::nullopt;
      if (summary_) { ended_ = true; return EndOfStream{}; }
      Packet packet(CheckedAllocation(av_packet_alloc()));
      int status;
      for (;;) {
        status = avcodec_receive_packet(context_.get(), packet.get());
        if (status != AVERROR(EAGAIN)) break;
        if (pcm_.size() >= BlockBytes() || (finishing_ && !pcm_.empty())) {
          SendBlock();
        } else if (finishing_ && !draining_) {
          Check(avcodec_send_frame(context_.get(), nullptr), "Drain AAC encoder");
          draining_ = true;
        } else {
          return std::nullopt;
        }
      }
      if (status == AVERROR_EOF) {
        summary_ = true;
        const auto priming = input_samples_ ? static_cast<std::uint64_t>(context_->initial_padding) : 0;
        if (encoded_samples_ < input_samples_ + priming)
          Fail(CodecErrorCode::PlatformFailure, "FFmpeg AAC sample accounting is inconsistent");
        return AudioEncodeSummary{start_, input_samples_, encoded_samples_, priming,
                                  encoded_samples_ - input_samples_ - priming};
      }
      Check(status, "Read AAC audio packet");
      if (!start_ || packet->size <= 0 || packet->pts == AV_NOPTS_VALUE || packet->duration != context_->frame_size)
        Fail(CodecErrorCode::InvalidData, "Invalid encoded AAC packet timing");
      const MediaTimestamp pts{packet->pts, rate};
      const MediaTimestamp duration{packet->duration, rate};
      encoded_samples_ += packet->duration;
      return EncodedPacket{CopyBytes(packet->data, packet->size), pts, pts, duration, true};
    });
  }
  CodecResult<void> FinishInput() override { return Guard<void>([&] { CheckOpen(); finishing_ = true; }); }
  void Close() noexcept override { pcm_.clear(); context_.reset(); }

private:
  void CheckOpen() const { if (!context_) Fail(CodecErrorCode::Closed, "Audio encoder is closed"); }
  std::size_t BlockBytes() const { return std::size_t(context_->frame_size) * config_.input.ChannelCount() * 2; }

  void SendBlock() {
    const auto bytes = std::min(pcm_.size(), BlockBytes());
    if (pcm_.size() < BlockBytes()) pcm_.resize(BlockBytes());
    Frame frame(CheckedAllocation(av_frame_alloc()));
    frame->format = context_->sample_fmt;
    frame->sample_rate = context_->sample_rate;
    Check(av_channel_layout_copy(&frame->ch_layout, &context_->ch_layout), "Copy AAC channel layout");
    frame->nb_samples = context_->frame_size;
    frame->pts = start_->ticks + sent_samples_;
    Check(av_frame_get_buffer(frame.get(), 0), "Allocate AAC input frame");
    const auto* input = reinterpret_cast<const std::uint8_t*>(pcm_.data());
    const auto converted = swr_convert(resampler_.get(), frame->extended_data, frame->nb_samples, &input, frame->nb_samples);
    Check(converted, "Convert AAC input samples");
    if (converted != frame->nb_samples) Fail(CodecErrorCode::PlatformFailure, "AAC input conversion changed the sample count");
    Check(avcodec_send_frame(context_.get(), frame.get()), "Encode AAC PCM");
    sent_samples_ += frame->nb_samples;
    pcm_.erase(pcm_.begin(), pcm_.begin() + (bytes < BlockBytes() ? pcm_.size() : bytes));
  }

  Context context_;
  AudioEncoderConfig config_;
  Resampler resampler_;
  Bytes pcm_;
  std::optional<MediaTimestamp> start_;
  std::uint64_t input_samples_{}, encoded_samples_{};
  std::int64_t sent_samples_{};
  bool announced_{}, finishing_{}, draining_{}, summary_{}, ended_{};
};

class FFmpegProvider final : public CodecProvider {
public:
  std::string_view Name() const noexcept override { return "FFmpeg"; }

  CodecResult<std::unique_ptr<VideoDecoderSession>> Open(const VideoDecoderConfig& config) const override {
    return Guard<std::unique_ptr<VideoDecoderSession>>([&]() -> std::unique_ptr<VideoDecoderSession> {
      if (config.codec != CodecId::H264 || config.output_storage != VideoStorage::Cpu ||
          config.output != VideoPixelFormat::I420 || !config.width || !config.height ||
          config.width > 8192 || config.height > 8192)
        Fail(CodecErrorCode::Unsupported, "FFmpeg file video decode accepts H.264 to CPU I420");
      using Session = Decoder<VideoDecoderSession, VideoDecoderConfig, VideoFrame, VideoFormat>;
      const auto* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
      auto context = Allocate(codec);
      context->pkt_timebase = {1, Session::kClock};
      context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
      SetExtra(*context, config.initialization_data);
      Check(avcodec_open2(context.get(), codec, nullptr), "Open FFmpeg H.264 decoder", CodecErrorCode::Unsupported);
      return std::make_unique<Session>(std::move(context), config);
    });
  }

  CodecResult<std::unique_ptr<AudioDecoderSession>> Open(const AudioDecoderConfig& config) const override {
    return Guard<std::unique_ptr<AudioDecoderSession>>([&]() -> std::unique_ptr<AudioDecoderSession> {
      if (config.codec != CodecId::Aac || !MatchesAacLc(config.initialization_data, config.output))
        Fail(CodecErrorCode::Unsupported, "FFmpeg file audio decode accepts AAC-LC to interleaved S16 mono or stereo");
      using Session = Decoder<AudioDecoderSession, AudioDecoderConfig, AudioFrame, AudioFormat>;
      const auto* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
      auto context = Allocate(codec);
      context->pkt_timebase = {1, Session::kClock};
      context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
      SetExtra(*context, config.initialization_data);
      Check(avcodec_open2(context.get(), codec, nullptr), "Open FFmpeg AAC decoder", CodecErrorCode::Unsupported);
      return std::make_unique<Session>(std::move(context), config);
    });
  }

  CodecResult<std::unique_ptr<VideoEncoderSession>> Open(const VideoEncoderConfig& config) const override {
    return Guard<std::unique_ptr<VideoEncoderSession>>([&]() -> std::unique_ptr<VideoEncoderSession> {
      const auto& input = config.input;
      if (config.codec != CodecId::H264 || config.input_storage != VideoStorage::Cpu ||
          input.pixel_format != VideoPixelFormat::I420 || !input.Validate() || ((input.width | input.height) & 1) ||
          input.width > 8192 || input.height > 8192 || input.visible != VideoRectangle{0, 0, input.width, input.height} ||
          input.rotation != VideoRotation::None || config.max_b_frames.value_or(0) != 0 ||
          !config.frame_rate_numerator || config.frame_rate_numerator > std::numeric_limits<int>::max() ||
          !config.frame_rate_denominator || config.frame_rate_denominator > std::numeric_limits<int>::max() ||
          !config.key_frame_interval || config.key_frame_interval > std::numeric_limits<int>::max() ||
          !config.bitrate || config.bitrate > std::numeric_limits<int>::max() ||
          !config.max_packet_bytes || input.aspect_width > std::numeric_limits<int>::max() ||
          input.aspect_height > std::numeric_limits<int>::max()) {
        Fail(CodecErrorCode::Unsupported, "Software H.264 encoding requires full even CPU I420 frames and no B pictures");
      }
      if ((input.primaries != ColorPrimaries::Unknown && input.primaries != ColorPrimaries::BT709) ||
          (input.transfer != ColorTransfer::Unknown && input.transfer != ColorTransfer::BT709) ||
          (input.matrix != ColorMatrix::Unknown && input.matrix != ColorMatrix::BT709))
        Fail(CodecErrorCode::Unsupported, "This software H.264 path accepts BT.709 SDR or unspecified color");
      const auto* codec = avcodec_find_encoder_by_name("libx264");
      auto context = Allocate(codec);
      context->width = input.width;
      context->height = input.height;
      context->pix_fmt = AV_PIX_FMT_YUV420P;
      context->sample_aspect_ratio = {static_cast<int>(input.aspect_width), static_cast<int>(input.aspect_height)};
      context->time_base = {1, VideoEncode::kClock};
      context->framerate = {static_cast<int>(config.frame_rate_numerator), static_cast<int>(config.frame_rate_denominator)};
      context->gop_size = config.key_frame_interval;
      context->bit_rate = config.bitrate;
      context->max_b_frames = 0;
      context->color_primaries = input.primaries == ColorPrimaries::BT709 ? AVCOL_PRI_BT709 : AVCOL_PRI_UNSPECIFIED;
      context->color_trc = input.transfer == ColorTransfer::BT709 ? AVCOL_TRC_BT709 : AVCOL_TRC_UNSPECIFIED;
      context->colorspace = input.matrix == ColorMatrix::BT709 ? AVCOL_SPC_BT709 : AVCOL_SPC_UNSPECIFIED;
      context->color_range = input.range == ColorRange::Full ? AVCOL_RANGE_JPEG :
                             input.range == ColorRange::Limited ? AVCOL_RANGE_MPEG : AVCOL_RANGE_UNSPECIFIED;
      context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_COPY_OPAQUE;
      const char* profile = config.profile == H264Profile::High ? "high" :
                            config.profile == H264Profile::Main ? "main" : "baseline";
      Check(av_opt_set(context->priv_data, "profile", profile, 0), "Configure H.264 profile");
      Check(av_opt_set(context->priv_data, "preset", "veryfast", 0), "Configure libx264 preset");
      Check(av_opt_set(context->priv_data, "tune", "zerolatency", 0), "Configure libx264 buffering");
      Check(av_opt_set(context->priv_data, "x264-params", "annexb=0:repeat-headers=0", 0), "Configure AVC framing");
      Check(avcodec_open2(context.get(), codec, nullptr), "Open libx264", CodecErrorCode::Unsupported);
      auto data = AvcConfiguration(*context, config.profile);
      return std::make_unique<VideoEncode>(std::move(context), config,
          VideoEncodeFormat{input.width, input.height, input.visible, std::move(data), CodecId::H264, {}});
    });
  }

  CodecResult<std::unique_ptr<AudioEncoderSession>> Open(const AudioEncoderConfig& config) const override {
    return Guard<std::unique_ptr<AudioEncoderSession>>([&]() -> std::unique_ptr<AudioEncoderSession> {
      if (config.codec != CodecId::Aac || !SupportedPcm(config.input) || !config.bitrate ||
          config.bitrate > std::numeric_limits<int>::max() ||
          config.limits.input_bytes < 1024 * config.input.ChannelCount() * 2)
        Fail(CodecErrorCode::Unsupported, "Software AAC encoding requires a full block of interleaved S16 mono or stereo");
      const auto* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
      auto context = Allocate(codec);
      context->sample_rate = config.input.sample_rate;
      context->sample_fmt = AV_SAMPLE_FMT_FLTP;
      context->profile = AV_PROFILE_AAC_LOW;
      context->bit_rate = config.bitrate;
      context->time_base = {1, context->sample_rate};
      av_channel_layout_default(&context->ch_layout, static_cast<int>(config.input.ChannelCount()));
      context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
      Check(avcodec_open2(context.get(), codec, nullptr), "Open FFmpeg AAC encoder", CodecErrorCode::Unsupported);
      if (context->extradata_size < 2 || context->frame_size != 1024 ||
          !MatchesAacLc({reinterpret_cast<const std::byte*>(context->extradata), 2}, config.input))
        Fail(CodecErrorCode::Unsupported, "FFmpeg did not negotiate AAC-LC access units");
      return std::make_unique<AudioEncode>(std::move(context), config);
    });
  }

  template<class Config> CodecResult<CodecNegotiation> ProbeConfiguration(const Config& config) const {
    auto copy = config;
    copy.wakeup = {};
    copy.cleanup_completed = {};
    auto session = Open(copy);
    if (!session.Succeeded()) return CodecResult<CodecNegotiation>::Failure(session.Error());
    return CodecResult<CodecNegotiation>::Success(session.Value()->Negotiated());
  }

  CodecResult<CodecNegotiation> Probe(const VideoDecoderConfig& config) const override { return ProbeConfiguration(config); }
  CodecResult<CodecNegotiation> Probe(const VideoEncoderConfig& config) const override { return ProbeConfiguration(config); }
  CodecResult<CodecNegotiation> Probe(const AudioDecoderConfig& config) const override { return ProbeConfiguration(config); }
  CodecResult<CodecNegotiation> Probe(const AudioEncoderConfig& config) const override { return ProbeConfiguration(config); }
};

} // namespace

std::shared_ptr<CodecProvider> CreateFFmpegCodecProvider() { return std::make_shared<FFmpegProvider>(); }

} // namespace huxerui::avkit
