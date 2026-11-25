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

/// @brief Inspected file with selected tracks and their presentation ranges.
/// The audio track can be absent. In that case `pcm` provides the default mix format.
struct MovieAsset {
  File file;
  media::MediaTrack video;
  media::MediaTrack audio;
  media::MediaPresentationRange video_range;
  media::MediaPresentationRange audio_range;
  media::MediaTimestamp end;
  media::AudioFormat pcm;

  /// @brief Reports whether the file has a selected audio track.
  bool HasAudio() const noexcept { return !audio.descriptions.empty(); }
};

/// @brief Provider candidates for independent audio and video sessions.
struct MovieCodecProviders {
  std::shared_ptr<CodecProvider> platform;
  std::shared_ptr<CodecProvider> software;
};

/// @brief Codec operation associated with a session or fallback diagnostic.
enum class CodecOperation { VideoDecode, AudioDecode, VideoEncode, AudioEncode };

/// @brief Diagnostic emitted when a failed codec operation falls back to another provider.
struct CodecFallback {
  CodecOperation operation;
  std::string from, to;
  media::CodecError cause;
  std::optional<media::MediaTimestamp> resume_at; ///< Recovery time when known.
};

/// @brief Receives fallback diagnostics on the calling worker sequence.
using OnCodecFallback = std::function<void(const CodecFallback&)>;

/// @brief Selects the first compatible video track and, when present, a compatible audio track.
/// @throws std::runtime_error On unsupported media metadata or I/O failure.
MovieAsset ImportMovie(const File& file);

/// @brief Inspects an AVKit PCM WAVE asset or a supported movie's original audio.
MovieAsset ImportMovieAudio(const File& file);

/// @brief Normalizes a local MP3, AAC, or WAVE source to an independently seekable PCM WAVE file.
/// Link AVKit::FFmpeg. The destination is published after a successful decode and conversion.
void PrepareTimelineAudio(const File& source, const File& destination, media::AudioFormat output);

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
  MovieReader(MovieAsset asset, MovieCodecProviders providers, CodecPolicy policy = CodecPolicy::Auto,
              OnCodecFallback on_fallback = {});
  /// @brief Opens a single video or audio track at the specified presentation time.
  /// Reading the unselected track throws std::logic_error.
  MovieReader(MovieAsset asset, MovieCodecProviders providers, media::TrackKind track, media::MediaTimestamp start,
              CodecPolicy policy, OnCodecFallback on_fallback = {});
  ~MovieReader();
  MovieReader(const MovieReader&) = delete;
  MovieReader& operator=(const MovieReader&) = delete;

  /// @brief Reopens the selected track cursors for the requested presentation time.
  void Seek(media::MediaTimestamp target);
  /// @brief Reads the next retained video frame in presentation order.
  /// @return A frame or an empty value at end of stream.
  /// @throws std::system_error With std::errc::operation_canceled when stopped.
  std::optional<media::VideoFrame> ReadVideo(std::stop_token stop = {});
  /// @brief Reopens only the video cursor on the software provider and redecodes a rejected platform frame.
  /// Pass a frame from this reader before admitting the frame to playback or encoding.
  /// The replacement must have the same presentation timestamp and duration. Later seeks keep the software video provider.
  media::VideoFrame RetryVideoFrame(const media::VideoFrame& rejected, const PlatformVideoImportError& cause,
                                  std::stop_token stop = {});
  /// @brief Reads the next retained PCM frame on the movie's audio sample grid.
  /// @return A frame or an empty value at end of stream.
  /// @throws std::system_error With std::errc::operation_canceled when stopped.
  std::optional<media::AudioFrame> ReadAudio(std::stop_token stop = {});

private:
  friend class TimelineSession;
  // Retain mapped CPU preroll for the reverse cache while filtering platform pictures at the seek time.
  std::optional<media::VideoFrame> ReadVideoInternal(std::stop_token stop, std::optional<media::MediaTimestamp> cpu_preroll);
  bool AllowsVideoSequenceHandoff() const noexcept;
  void SetCodecFallbackHandler(OnCodecFallback report);
  static media::VideoFrame RetryDetachedVideoFrame(const MovieAsset& movie, const MovieCodecProviders& providers,
                                                  const media::VideoFrame& rejected, const PlatformVideoImportError& cause,
                                                  const OnCodecFallback& report, std::stop_token stop);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// @brief Requested video output, AAC bitrate in bits per second, and provider policy.
struct FileExportSettings {
  /// @brief Builds limited-range BT.709 settings with the exact-trim AAC policy for this platform.
  static FileExportSettings Sdr(PixelSize size);

  media::VideoEncoderConfig video;
  std::uint32_t audio_bitrate{128000};
  CodecPolicy policy{CodecPolicy::Auto};
  /// Optionally select the software AAC encoder independently of video.
  CodecPolicy audio_policy{CodecPolicy::Auto};
};

/// @brief A compositor could not supply a frame using the negotiated platform storage.
/// File and timeline export can reopen their source and retry using a CPU encoder input.
class PlatformVideoTransferError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// @brief Composites a decoded image into the encoder's confirmed layout and storage.
using ComposeMovieFrame = std::function<media::VideoFrame(const media::VideoFrame&, const media::VideoEncoderConfig&)>;

/// @brief Composites a file and publishes a finalized MP4 from a sibling temporary file.
/// @param destination New local MP4 path.
/// @param compose Worker-sequence compositor. A recoverable platform encoder failure can replay
/// the asset from the start and invoke the compositor again.
void ExportMovie(const MovieAsset& asset, const File& destination, const MovieCodecProviders& providers,
                 const FileExportSettings& settings, const ComposeMovieFrame& compose,
                 OnCodecFallback on_fallback = {});

/// @brief Supplies recording pictures in the layout and storage chosen for the encoder.
using ReadRecordingVideo = std::function<std::optional<media::VideoFrame>(const media::VideoEncoderConfig&)>;
/// @brief Supplies recording PCM in the selected format.
using ReadRecordingAudio = std::function<std::optional<media::AudioFrame>()>;

/// @brief Drains live readers and publishes a finalized MP4 on the caller's worker sequence.
/// @param pcm Format shared by the audio reader and the export clock.
/// @param video Picture reader producing positive-duration frames with contiguous timestamps.
/// @param audio PCM reader producing contiguous samples on the same zero-based timeline. Pass an empty reader for video only.
/// An empty read seals its track. Readers may wait for capture data or throw on interruption.
/// A codec failure after admission aborts the current staged file.
void WriteLiveMovie(const File& destination, const MovieCodecProviders& providers, const FileExportSettings& settings,
                    const media::AudioFormat& pcm, const ReadRecordingVideo& video, const ReadRecordingAudio& audio,
                    OnCodecFallback on_fallback = {});

} // namespace huxerui::avkit
