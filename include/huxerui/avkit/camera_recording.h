#pragma once

/// @file
/// @brief Camera frame processing and microphone recording using the AVKit compositor and MP4 writer.

#include <huxerui/avkit/timeline_renderer.h>
#include <huxerui/camera.h>

namespace huxerui::avkit {

struct CameraRecordingStatus {
  enum class State { Idle, Starting, Recording, Paused, Finishing, Complete, Failed };

  State state{State::Idle};
  std::uint32_t sample_rate{};
  std::int64_t samples{}, paused_samples{};
  std::size_t video_frames{};
  std::string error;
  bool operator==(const CameraRecordingStatus&) const = default;
};

namespace detail { struct CameraRecordingVerification; }

/// @brief One camera preview and at most one active MP4 take. Link AVKit::Capture.
/// Connect on the UI sequence before opening the camera. Camera callbacks and AVKit workers own frame processing.
class CameraRecording {
public:
  explicit CameraRecording(PixelSize size, render::TimelineRenderResources resources = {}, OnCodecFallback fallback = {});
  ~CameraRecording();

  /// @brief Adds the frame processor and capture clock observer to the caller's camera options.
  void Connect(camera::CameraOptions& options) const;
  /// @brief Returns the same preview texture throughout the session, including before the first frame.
  std::shared_ptr<ExternalTexture> Texture() const;
  CameraRecordingStatus Status() const;

  /// @brief Enables duration-preserving robot voice. Settings are sampled at PCM block boundaries.
  void SetRobotVoice(bool enabled);
  /// @brief Sets a canvas sticker for both the preview and subsequent video frames.
  void SetSticker(std::optional<CanvasOverlay> sticker);

  /// @brief Begins a take after the microphone has a confirmed PCM format.
  void Start(File destination, audio::AudioReader reader, media::AudioFormat format);
  void Pause(bool paused);
  /// @brief Seals the timeline and lets the recorder finalize the MP4.
  void Stop();
  void Cancel(std::string reason);
  void Close();

private:
  friend struct detail::CameraRecordingVerification;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::avkit
