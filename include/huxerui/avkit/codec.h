#pragma once

/// @file
/// @brief Codec provider negotiation, session lifecycle, and Lib-Media and FFmpeg factories.
///
/// Session calls are serialized on a caller-owned codec sequence. A negotiated handoff permits a new owning
/// sequence after the previous caller stops. Providers use Lib-Media packet, frame, and output-event types.

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
  /// CPU planes or platform-backed frames.
  media::VideoStorage storage;
  /// Exact plane layout or opaque platform format.
  media::VideoPixelFormat layout;

  bool operator==(const VideoFrameContract&) const = default;
};

/// @brief A provider's implementation and, for video, its frame contract.
struct CodecNegotiation {
  /// Hardware, software, or an unknown implementation for this configuration.
  CodecImplementation implementation;
  /// Accepted video encoder input or delivered video decoder output. Empty for audio.
  std::optional<VideoFrameContract> video;
  bool allow_sequence_handoff{}; ///< The open session can be handed to a different sequence between calls.
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
  virtual CodecNegotiation Negotiated() const noexcept = 0;

  /// @brief Attempts to submit a packet or frame.
  /// @param input Packet or frame matching this session's open contract.
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
  virtual std::string_view Name() const noexcept = 0;

  /// @brief Checks whether this video decoder configuration is supported.
  /// @param config Compressed stream description, queue limits, and output requirements.
  /// @return The reported implementation and any video storage and pixel layout, or a codec error.
  virtual media::CodecResult<CodecNegotiation> Probe(const media::VideoDecoderConfig& config) const = 0;

  /// @brief Checks whether this video encoder configuration is supported.
  /// @param config Input image format and storage, encoder settings, and queue limits.
  /// @return The reported implementation and any video storage and pixel layout, or a codec error.
  virtual media::CodecResult<CodecNegotiation> Probe(const media::VideoEncoderConfig& config) const = 0;

  /// @brief Checks whether this audio decoder configuration is supported.
  /// @param config Compressed stream description, queue limits, and PCM requirements.
  /// @return The reported implementation and any video storage and pixel layout, or a codec error.
  virtual media::CodecResult<CodecNegotiation> Probe(const media::AudioDecoderConfig& config) const = 0;

  /// @brief Checks whether this audio encoder configuration is supported.
  /// @param config PCM input format, encoder settings, and queue limits.
  /// @return The reported implementation and any video storage and pixel layout, or a codec error.
  virtual media::CodecResult<CodecNegotiation> Probe(const media::AudioEncoderConfig& config) const = 0;

  /// @brief Opens an independent video decoder session. Decodes compressed packets to retained video pictures.
  /// @param config Compressed stream description, queue limits, and output requirements.
  /// @return An open session with a confirmed contract, or a codec error. A prior probe does not reserve resources.
  virtual media::CodecResult<std::unique_ptr<VideoDecoderSession>> Open(const media::VideoDecoderConfig& config) const = 0;

  /// @brief Opens an independent video encoder session. Encodes video pictures to compressed packets.
  /// @param config Input image format and storage, encoder settings, and queue limits.
  /// @return An open session with a confirmed contract, or a codec error. A prior probe does not reserve resources.
  virtual media::CodecResult<std::unique_ptr<VideoEncoderSession>> Open(const media::VideoEncoderConfig& config) const = 0;

  /// @brief Opens an independent audio decoder session. Decodes compressed packets to PCM blocks.
  /// @param config Compressed stream description, queue limits, and PCM requirements.
  /// @return An open session with a confirmed contract, or a codec error. A prior probe does not reserve resources.
  virtual media::CodecResult<std::unique_ptr<AudioDecoderSession>> Open(const media::AudioDecoderConfig& config) const = 0;

  /// @brief Opens an independent audio encoder session. Encodes PCM blocks to compressed packets.
  /// @param config PCM input format, encoder settings, and queue limits.
  /// @return An open session with a confirmed contract, or a codec error. A prior probe does not reserve resources.
  virtual media::CodecResult<std::unique_ptr<AudioEncoderSession>> Open(const media::AudioEncoderConfig& config) const = 0;
};

/// @brief Creates the Lib-Media adapter. Link AVKit::Media.
/// @return Provider using the installed Lib-Media backends.
std::shared_ptr<CodecProvider> CreateLibMediaCodecProvider();

/// @brief Creates the software codec adapter. Link AVKit::FFmpeg.
/// @return Provider using the supported FFmpeg software codecs.
std::shared_ptr<CodecProvider> CreateFFmpegCodecProvider();

} // namespace huxerui::avkit
