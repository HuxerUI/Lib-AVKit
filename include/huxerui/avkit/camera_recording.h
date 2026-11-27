#pragma once

/// @file
/// @brief Camera frame processing and microphone recording using the AVKit compositor and MP4 writer.

#include <huxerui/avkit/timeline.h>
#include <huxerui/camera.h>

namespace huxerui::avkit {

/// @brief Current take state and progress published by the recording worker.
struct CameraRecordingStatus {
  enum class State { Idle, Starting, Recording, Paused, Finishing, Complete, Failed };

  /// Lifecycle state. Complete means the MP4 has been published.
  State state{State::Idle};
  /// PCM sample rate for this take. Zero before the clock is established.
  std::uint32_t sample_rate{};
  /// Current take position in sample frames, excluding paused intervals.
  std::int64_t samples{};
  /// Number of sample frames excluded during pauses.
  std::int64_t paused_samples{};
  /// Number of video frames accepted for the take.
  std::size_t video_frames{};
  /// Diagnostic supplied on capture or recording failure.
  std::string error;

  bool operator==(const CameraRecordingStatus&) const = default;
};

/// @brief One camera preview and at most one active MP4 take. Link AVKit::Capture.
/// Connect on the UI sequence before opening the camera. Camera callbacks and AVKit workers own frame processing.
class CameraRecording {
public:
  /// @brief Opens a live preview worker and its reusable preview texture.
  /// @param size Positive even output extent in pixels, up to 8192 on either axis.
  /// @param scene Shared resources and extensions for video and audio evaluation.
  /// @param fallback Optional observer for codec fallback during a take.
  /// @throws std::invalid_argument If the output extent is invalid.
  explicit CameraRecording(PixelSize size, SceneConfiguration scene = {}, OnCodecFallback fallback = {});

  ~CameraRecording();

  /// @brief Adds the frame processor and capture clock observer to the caller's camera options.
  /// @param options Camera options to update before opening the session. AVKit chooses the processed output size.
  void Connect(camera::CameraOptions& options) const;

  /// @brief Returns the same preview texture throughout the session, including before the first frame.
  std::shared_ptr<ExternalTexture> Texture() const;

  /// @brief Takes a synchronized snapshot of the current preview or recording state.
  /// @return Latest progress or failure details. Poll after Stop while the MP4 finishes.
  CameraRecordingStatus Status() const;

  /// @brief Changes voice processing at the next PCM block boundary.
  /// @param effect Duration-preserving effect, or empty for unprocessed input.
  void SetVoiceEffect(std::optional<VoiceEffect> effect);

  /// @brief Saves the currently published composed camera picture as JPEG.
  /// @param destination New file in an existing writable directory.
  /// Call on the owning UI sequence.
  Task<void> CapturePhoto(File destination) const;

  /// @brief Waits for microphone input, records a take, and finalizes the output after Stop.
  /// @param destination Local MP4 output path.
  /// @param camera Active camera used to surface interruptions.
  /// @param microphone Active microphone handle on the owning UI sequence.
  Task<CameraRecordingStatus> Record(File destination, camera::CameraSession camera, audio::AudioCaptureSession microphone);

  /// @brief Replaces all live overlays in back-to-front order, with unique instance IDs.
  /// An empty interval uses the scene clock; a positive interval is sampled on the current take or preview clock.
  /// @param overlays Complete layer list, with unique instance IDs and normalized placement.
  /// @throws std::invalid_argument If instance IDs are duplicated.
  void SetOverlays(std::vector<OverlayInstance> overlays);

  /// @brief Begins a take after the microphone has a confirmed PCM format.
  /// @param destination Local MP4 destination. The output is published after finalization.
  /// @param reader Microphone PCM reader supplying capture samples.
  /// @param format Confirmed PCM rate, packing, representation, and channel layout.
  void Start(File destination, audio::AudioReader reader, media::AudioFormat format);

  /// @brief Requests pause or resume for the current take.
  /// @param paused True to pause, false to resume. Calls without an active take have no effect.
  void Pause(bool paused);

  /// @brief Seals the timeline and lets the recorder finalize the MP4.
  /// Returns while the worker finishes the file. Observe Complete or Failed through Status.
  void Stop();

  /// @brief Fails the current take and abandons its staged output.
  /// @param reason Diagnostic exposed in the recording status. Calls without an active take have no effect.
  void Cancel(std::string reason);

  /// @brief Shuts down frame processing, the preview texture, and any active take.
  void Close();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace huxerui::avkit
