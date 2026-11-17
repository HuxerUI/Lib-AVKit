#pragma once

/// @file
/// @brief Codec provider negotiation, session lifecycle, and Lib-Media and FFmpeg factories.
///
/// Session calls are serialized on a caller-owned codec sequence. Providers use Lib-Media packet,
/// frame, and output-event types so a caller can switch implementations at a recovery boundary.

#include <memory>
#include <optional>
#include <string_view>

#include <huxerui/media_codec.h>

namespace huxerui::avkit {

/// @brief Implementation reported by a provider for an exact configuration.
enum class CodecImplementation { Hardware, Software, Unknown };

/// @brief Provider selection policy for file sessions.
enum class CodecPolicy {
  Auto,          ///< Try the configured platform provider first and use software when applicable.
  SoftwareOnly,  ///< Open sessions through the configured software provider.
};

/// @brief Storage and pixel layout accepted or delivered by a video codec session.
struct VideoFrameContract {
  media::VideoStorage storage;
  media::VideoPixelFormat layout;
  bool operator==(const VideoFrameContract&) const = default;
};

/// @brief A provider's implementation and, for video, its frame contract.
struct CodecNegotiation {
  CodecImplementation implementation;
  std::optional<VideoFrameContract> video;
};

/// @brief One encoder or decoder session on the caller's codec sequence.
/// Poll TryRead after submitting or sealing input. The configuration's wakeup can announce progress
/// made asynchronously.
template<class In, class Out> class CodecSession {
public:
  using Input = In;
  using Output = Out;

  virtual ~CodecSession() = default;
  /// @brief Returns the contract confirmed when the session opened.
  [[nodiscard]] virtual CodecNegotiation Negotiated() const noexcept = 0;
  /// @brief Attempts to submit a packet or frame.
  /// @return Accepted on admission or WouldBlock when the caller must retain and retry the input.
  virtual media::CodecResult<media::SubmitStatus> TrySubmit(const Input& input) = 0;
  /// @brief Reads the next available format, frame, packet, or terminal event.
  /// @return Empty success while the session has no event ready.
  virtual media::CodecResult<std::optional<Output>> TryRead() = 0;
  /// @brief Seals input so queued output and the end-of-stream event can be read.
  virtual media::CodecResult<void> FinishInput() = 0;
  /// @brief Retires the codec session and releases its resources.
  virtual void Close() noexcept = 0;
};

using VideoDecoderSession = CodecSession<media::EncodedPacket, media::VideoDecodeOutput>;
using VideoEncoderSession = CodecSession<media::VideoFrame, media::VideoEncodeOutput>;
using AudioDecoderSession = CodecSession<media::EncodedPacket, media::AudioDecodeOutput>;
using AudioEncoderSession = CodecSession<media::AudioFrame, media::AudioEncodeOutput>;

/// @brief Probes exact codec configurations and opens independent sessions.
/// Probe runs on a worker sequence. Open makes a separate resource request and can fail after a
/// successful Probe. A session confirms storage and layout, and format events describe its stream.
class CodecProvider {
public:
  virtual ~CodecProvider() = default;
  /// @brief Returns a provider label whose storage lives at least as long as the provider.
  [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
  virtual media::CodecResult<CodecNegotiation> Probe(const media::VideoDecoderConfig&) const = 0;
  virtual media::CodecResult<CodecNegotiation> Probe(const media::VideoEncoderConfig&) const = 0;
  virtual media::CodecResult<CodecNegotiation> Probe(const media::AudioDecoderConfig&) const = 0;
  virtual media::CodecResult<CodecNegotiation> Probe(const media::AudioEncoderConfig&) const = 0;
  virtual media::CodecResult<std::unique_ptr<VideoDecoderSession>> Open(const media::VideoDecoderConfig&) const = 0;
  virtual media::CodecResult<std::unique_ptr<VideoEncoderSession>> Open(const media::VideoEncoderConfig&) const = 0;
  virtual media::CodecResult<std::unique_ptr<AudioDecoderSession>> Open(const media::AudioDecoderConfig&) const = 0;
  virtual media::CodecResult<std::unique_ptr<AudioEncoderSession>> Open(const media::AudioEncoderConfig&) const = 0;
};

/// @brief Creates the Lib-Media adapter. Link AVKit::Media.
std::shared_ptr<CodecProvider> CreateLibMediaCodecProvider();
/// @brief Creates the software codec adapter. Link AVKit::FFmpeg.
std::shared_ptr<CodecProvider> CreateFFmpegCodecProvider();

} // namespace huxerui::avkit
