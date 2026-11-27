#pragma once

/// @file
/// @brief File import, per-track decode cursors, and staged MP4 output.
///
/// Link AVKit::Media. The file reader selects one supported SDR video track and an optional mono or stereo
/// audio track. Movie ranges and decoded PTS use the file's presentation clock.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>

#include <huxerui/avkit/codec.h>
#include <huxerui/avkit/render.h>
#include <huxerui/file.h>
#include <huxerui/media_container.h>

namespace huxerui::avkit {

namespace detail { class TimelineVideoSource; }

/// @brief Inspected file with selected tracks and their presentation ranges.
/// The audio track can be absent. In that case `pcm` provides the default mix format.
struct MovieAsset {
  /// Local source file opened by readers.
  File file;
  /// Selected video track. Empty for audio-only assets.
  media::MediaTrack video;
  /// Selected audio track. Empty when the asset has no usable audio.
  media::MediaTrack audio;
  /// Presentation interval covered by the selected picture track.
  media::MediaPresentationRange video_range;
  /// Presentation interval covered by the selected sound track.
  media::MediaPresentationRange audio_range;
  /// End of the selected presentation timeline.
  media::MediaTimestamp end;
  /// Decoded PCM format or the default mix format when audio is absent.
  media::AudioFormat pcm;

  /// @brief Reports whether the file has a selected audio track.
  /// @return True when the inspected asset has a selected audio track.
  bool HasAudio() const noexcept { return !audio.descriptions.empty(); }
};

/// @brief Provider candidates for independent audio and video sessions.
struct CodecProviders {
  /// Preferred provider under the automatic policy.
  std::shared_ptr<CodecProvider> platform;
  /// Software provider used by policy or on recoverable failure.
  std::shared_ptr<CodecProvider> software;
};

/// @brief Codec operation associated with a session or fallback diagnostic.
enum class CodecOperation { VideoDecode, AudioDecode, VideoEncode, AudioEncode };

/// @brief Diagnostic emitted when a failed codec operation falls back to another provider.
struct CodecFallback {
  /// Decoder or encoder operation that switched provider.
  CodecOperation operation;
  /// Names of the previous and replacement providers.
  std::string from, to;
  /// Original error that triggered the switch.
  media::CodecError cause;
  /// Exact recovery time if known. Empty when a restart has no frame position.
  std::optional<media::MediaTimestamp> resume_at;
};

/// @brief Receives fallback diagnostics on the calling worker sequence.
using OnCodecFallback = std::function<void(const CodecFallback&)>;

/// @brief Selects the first compatible video track and, when present, a compatible audio track.
/// @param file Supported local video file.
/// @return Selected tracks and their mapped presentation ranges.
/// @throws std::runtime_error On unsupported media metadata or I/O failure.
MovieAsset ImportMovie(const File& file);

/// @brief Selects a compatible audio track independently of any video track.
/// @param file Local audio file or movie with compatible audio.
/// @return Selected audio track, presentation range, and decoded PCM format.
/// @throws std::runtime_error If the file or audio track is unsupported or unavailable.
MovieAsset ImportMovieAudio(const File& file);

/// @brief A decoded platform video frame could not be imported by the compositor.
class PlatformVideoImportError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// @brief Pulls decoded frames in movie presentation time on a worker sequence.
/// Each reader owns its selected track cursors and decoder sessions.
class MovieReader {
public:
  /// @brief Opens the movie's selected video and audio tracks at the start.
  /// @param asset Inspected movie with the tracks and their presentation ranges.
  /// @param providers Candidate codec providers for the selected tracks.
  /// @param policy Which providers the reader may open.
  /// @param on_fallback Optional diagnostic callback on the reading sequence.
  MovieReader(MovieAsset asset, CodecProviders providers, CodecPolicy policy = CodecPolicy::Auto,
              OnCodecFallback on_fallback = {});

  /// @brief Opens a single video or audio track at the specified presentation time.
  /// Reading the unselected track throws std::logic_error.
  /// @param asset Inspected file containing the requested track.
  /// @param providers Candidate codec providers.
  /// @param track Opens only the selected video or audio track.
  /// @param start Initial seek position on the file's presentation clock.
  /// @param policy Which providers the reader may open.
  /// @param on_fallback Optional diagnostic callback on the reading sequence.
  MovieReader(MovieAsset asset, CodecProviders providers, media::TrackKind track, media::MediaTimestamp start,
              CodecPolicy policy, OnCodecFallback on_fallback = {});

  ~MovieReader();

  MovieReader(const MovieReader&) = delete;

  MovieReader& operator=(const MovieReader&) = delete;

  /// @brief Reopens the selected track cursors for the requested presentation time.
  /// @param target New position on the file's presentation clock. Decoding can begin at an earlier random-access packet.
  void Seek(media::MediaTimestamp target);

  /// @brief Reads the next retained video frame in presentation order.
  /// @param stop Cooperatively interrupts the decode or wait for codec progress.
  /// @return A frame or an empty value at end of stream.
  /// @throws std::system_error With std::errc::operation_canceled when stopped.
  std::optional<media::VideoFrame> ReadVideo(std::stop_token stop = {});

  /// @brief Reopens only the video cursor on the software provider and redecodes a rejected platform frame.
  /// Pass a frame from this reader before admitting the frame to playback or encoding.
  /// The replacement must have the same presentation timestamp and duration. Later seeks keep the software video provider.
  /// @param rejected Retained platform picture returned by this reader's video cursor.
  /// @param cause Texture import failure recorded in the fallback diagnostic.
  /// @param stop Cancels software reopening or decoding.
  /// @return Software-decoded picture with the same PTS and duration.
  media::VideoFrame RetryVideoFrame(const media::VideoFrame& rejected, const PlatformVideoImportError& cause,
                                  std::stop_token stop = {});

  /// @brief Reads the next retained PCM frame on the movie's audio sample grid.
  /// @param stop Cooperatively interrupts the decode or wait for codec progress.
  /// @return A frame or an empty value at end of stream.
  /// @throws std::system_error With std::errc::operation_canceled when stopped.
  std::optional<media::AudioFrame> ReadAudio(std::stop_token stop = {});

private:
  friend class detail::TimelineVideoSource;

  // Retain mapped CPU preroll for the reverse cache while filtering platform pictures at the seek time.
  std::optional<media::VideoFrame> ReadVideoInternal(std::stop_token stop, std::optional<media::MediaTimestamp> cpu_preroll);

  bool AllowsVideoSequenceHandoff() const noexcept;

  void SetCodecFallbackHandler(OnCodecFallback report);

  static media::VideoFrame RetryDetachedVideoFrame(const MovieAsset& movie, const CodecProviders& providers,
                                                  const media::VideoFrame& rejected, const PlatformVideoImportError& cause,
                                                  const OnCodecFallback& report, std::stop_token stop);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// @brief Requested video output, AAC bitrate in bits per second, and provider policy.
struct FileExportSettings {
  /// @brief Builds limited-range BT.709 settings with the exact-trim AAC policy for this platform.
  /// @param size Physical extent of the encoder input.
  /// @return Standard SDR settings with the default audio bitrate.
  static FileExportSettings Sdr(PixelSize size);

  /// Requested picture format, storage, rate, and encoding parameters.
  media::VideoEncoderConfig video;
  /// Requested AAC bitrate in bits per second.
  std::uint32_t audio_bitrate{128000};
  /// Provider policy for video and file decoding.
  CodecPolicy policy{CodecPolicy::Auto};
  /// Provider policy for AAC encoding, applied independently of video.
  CodecPolicy audio_policy{CodecPolicy::Auto};
};

/// @brief Checks a requested export's video encoder using the same candidates as MP4 writing.
/// @param providers Platform and software candidates.
/// @param settings Requested output format and provider policy.
/// @return Negotiated input contract of the first available encoder.
media::CodecResult<CodecNegotiation> ProbeVideoExport(const CodecProviders& providers, const FileExportSettings& settings);

/// @brief A compositor could not supply a frame using the negotiated platform storage.
/// File and timeline export can reopen their source and retry using a CPU encoder input.
class PlatformEncoderInputError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// @brief Supplies recording pictures in the layout and storage chosen for the encoder.
using ReadRecordingVideo = std::function<std::optional<media::VideoFrame>(const media::VideoEncoderConfig&)>;
/// @brief Supplies recording PCM in the selected format.
using ReadRecordingAudio = std::function<std::optional<media::AudioFrame>()>;

/// @brief Drains live readers and publishes a finalized MP4 on the caller's worker sequence.
/// An empty read seals its track. Readers may wait for capture data or throw on interruption.
/// A codec failure after admission aborts the current staged file.
/// @param destination Local MP4 destination for the finalized take.
/// @param providers Candidate encoders for the negotiated video and PCM formats.
/// @param settings Requested video configuration, audio bitrate, and provider policies.
/// @param pcm Format shared by the audio reader and the export clock.
/// @param video Picture reader producing positive-duration frames with contiguous timestamps.
/// @param audio PCM reader producing contiguous samples on the same zero-based timeline. Pass an empty reader for video only.
/// @param on_fallback Optional diagnostics when a provider is changed before frames are admitted.
void WriteLiveMovie(const File& destination, const CodecProviders& providers, const FileExportSettings& settings,
                    const media::AudioFormat& pcm, const ReadRecordingVideo& video, const ReadRecordingAudio& audio,
                    OnCodecFallback on_fallback = {});

} // namespace huxerui::avkit
