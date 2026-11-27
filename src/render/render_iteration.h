#pragma once

#include <functional>

namespace huxerui::avkit::render::detail {

/// @brief Runs one worker iteration and releases platform-scoped temporary resources.
/// @param work Operation to execute on the current thread.
bool RunRenderIteration(const std::function<bool()>& work);

} // namespace huxerui::avkit::render::detail
