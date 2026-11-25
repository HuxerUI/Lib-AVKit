#pragma once

#include <string_view>

#include <huxerui/avkit/camera_recording.h>

namespace avkit_preview {

int VerifyCapture(std::string_view directory, bool composited);
void VerifyLiveMovie(const huxerui::File& output, const huxerui::avkit::CameraRecordingStatus& expected,
                     std::optional<std::int64_t> cue_sample);

} // namespace avkit_preview
