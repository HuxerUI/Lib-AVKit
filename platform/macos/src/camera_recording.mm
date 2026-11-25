#include "capture/platform_capture.h"

#import <CoreMedia/CoreMedia.h>

#include <stdexcept>

namespace huxerui::avkit::detail {

audio::CaptureClock CameraCaptureClock() { return audio::CaptureClock::CoreMediaHostTime; }

media::MediaTimestamp CameraClockNow() {
  const auto now = CMClockGetTime(CMClockGetHostTimeClock());
  if (!CMTIME_IS_NUMERIC(now) || now.epoch != 0 || now.timescale <= 0)
    throw std::runtime_error("CoreMedia host clock is unavailable");
  return {now.value, static_cast<std::uint32_t>(now.timescale)};
}

void RunCaptureIteration(const std::function<void()>& work) {
  @autoreleasepool { work(); }
}

} // namespace huxerui::avkit::detail
