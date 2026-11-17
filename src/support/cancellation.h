#pragma once

#include <stop_token>
#include <system_error>

namespace huxerui::avkit::detail {

inline void CheckCancelled(std::stop_token stop) {
  if (stop.stop_requested()) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
}

} // namespace huxerui::avkit::detail
