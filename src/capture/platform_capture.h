#pragma once

#include <huxerui/avkit/camera_recording.h>

namespace huxerui::avkit::detail {

audio::CaptureClock CameraCaptureClock();
media::MediaTimestamp CameraClockNow();
void RunCaptureIteration(const std::function<void()>& work);

} // namespace huxerui::avkit::detail
