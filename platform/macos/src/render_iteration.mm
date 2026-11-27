#include "render/render_iteration.h"

namespace huxerui::avkit::render::detail {

bool RunRenderIteration(const std::function<bool()>& work) {
  @autoreleasepool { return work(); }
}

} // namespace huxerui::avkit::render::detail
