#pragma once

#include <huxerui/avkit/camera_recording.h>

namespace huxerui::avkit::detail {

struct CameraRecordingVerification {
  static void ScheduleCue(CameraRecording& recording);
  static std::optional<std::int64_t> CueSample(const CameraRecording& recording);
};

} // namespace huxerui::avkit::detail
