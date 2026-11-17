#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <huxerui/camera.h>

namespace avkit_preview {

struct LiveCaptureStatus {
  enum class State { Idle, Starting, Recording, Paused, Finishing, Complete, Failed };

  State state{State::Idle};
  std::uint32_t sample_rate{};
  std::int64_t samples{}, paused_samples{};
  std::optional<std::int64_t> cue_sample;
  std::size_t video_frames{};
  std::string error;
  bool operator==(const LiveCaptureStatus&) const = default;
};

class LiveCapture {
public:
  LiveCapture();
  ~LiveCapture();

  std::shared_ptr<const huxerui::camera::FrameObserver> Observer() const;
  std::shared_ptr<huxerui::ExternalTexture> Texture() const;
  LiveCaptureStatus Status() const;

  void Start(huxerui::File file, huxerui::audio::AudioReader reader, huxerui::media::AudioFormat format);
  void SetRobotVoice(bool enabled);
  void Pause(bool paused);
  void Stop();
  void Interrupt(std::string reason);
  void ScheduleVerificationCue();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

void VerifyLiveMovie(const huxerui::File& output, const LiveCaptureStatus& expected);

} // namespace avkit_preview
