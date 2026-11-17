#pragma once

#include <array>
#include <limits>
#include <numeric>
#include <stdexcept>

#include <huxerui/media_format.h>

namespace huxerui::avkit::detail {

using media::MediaTimestamp;

inline MediaTimestamp AtScale(MediaTimestamp time, std::uint32_t scale) {
  auto value = time.Rescale(scale);
  if (!value) throw std::runtime_error("Media time exceeds the supported range");
  return *value;
}

inline MediaTimestamp Add(MediaTimestamp a, MediaTimestamp b) {
  if (!a.timescale || !b.timescale) throw std::runtime_error("Invalid media clock");
  const auto scale = std::lcm<std::uint64_t>(a.timescale, b.timescale);
  if (scale > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("Incompatible media clocks");
  a = AtScale(a, static_cast<std::uint32_t>(scale));
  b = AtScale(b, static_cast<std::uint32_t>(scale));
  if ((b.ticks > 0 && a.ticks > std::numeric_limits<std::int64_t>::max() - b.ticks) ||
      (b.ticks < 0 && a.ticks < std::numeric_limits<std::int64_t>::min() - b.ticks))
    throw std::runtime_error("Media time overflow");
  return {a.ticks + b.ticks, a.timescale};
}

inline MediaTimestamp Difference(MediaTimestamp end, MediaTimestamp start) {
  if (start.ticks == std::numeric_limits<std::int64_t>::min()) throw std::runtime_error("Media time overflow");
  return Add(end, {-start.ticks, start.timescale});
}

inline bool Before(MediaTimestamp a, MediaTimestamp b) {
  auto cmp = a.Compare(b);
  if (!cmp) throw std::runtime_error("Invalid media timestamps");
  return *cmp < 0;
}

inline std::int64_t ExactTicks(MediaTimestamp time, std::uint32_t scale) {
  const auto converted = AtScale(time, scale);
  if (converted != time) throw std::invalid_argument("Time cannot be represented on the selected grid");
  return converted.ticks;
}

inline MediaTimestamp Retime(MediaTimestamp time, MediaTimestamp previous, MediaTimestamp duration) {
  if (!time.timescale || !previous.timescale || !duration.timescale || time.ticks < 0 ||
      previous.ticks <= 0 || duration.ticks <= 0)
    throw std::invalid_argument("Invalid time in transition curve");
  if (!time.ticks) return {};
  // Cross-cancel before multiplying so an intermediate product cannot reject a representable key time.
  std::array<std::uint64_t, 3> numerator{std::uint64_t(time.ticks), std::uint64_t(duration.ticks), previous.timescale};
  std::array<std::uint64_t, 3> denominator{time.timescale, duration.timescale, std::uint64_t(previous.ticks)};
  for (auto& n : numerator) {
    for (auto& d : denominator) {
      const auto common = std::gcd(n, d);
      n /= common;
      d /= common;
    }
  }
  const auto multiply = [](const auto& factors, std::uint64_t limit) {
    std::uint64_t product = 1;
    for (const auto factor : factors) {
      if (factor > limit / product) throw std::overflow_error("Retimed key cannot be represented as a media timestamp");
      product *= factor;
    }
    return product;
  };
  return {static_cast<std::int64_t>(multiply(numerator, std::numeric_limits<std::int64_t>::max())),
          static_cast<std::uint32_t>(multiply(denominator, std::numeric_limits<std::uint32_t>::max()))};
}

} // namespace huxerui::avkit::detail
