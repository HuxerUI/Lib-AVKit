#include "capture/platform_capture.h"

#include <ctime>
#include <stdexcept>

namespace huxerui::avkit::detail {

audio::CaptureClock CameraCaptureClock() { return audio::CaptureClock::BootTime; }

media::MediaTimestamp CameraClockNow() {
  timespec now{};
  if (clock_gettime(CLOCK_BOOTTIME, &now) != 0) throw std::runtime_error("Android boot-time clock is unavailable");
  return {static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec, 1'000'000'000};
}

void RunCaptureIteration(const std::function<void()>& work) { work(); }

} // namespace huxerui::avkit::detail
