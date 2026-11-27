#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>

namespace huxerui::avkit::detail {

inline double RobotVoiceGain(std::int64_t sample, std::uint32_t sample_rate) {
  constexpr double tau = 2.0 * std::numbers::pi;
  return 0.15 + 0.85 * std::sin(tau * 68 * (sample % sample_rate) / sample_rate);
}

} // namespace huxerui::avkit::detail
