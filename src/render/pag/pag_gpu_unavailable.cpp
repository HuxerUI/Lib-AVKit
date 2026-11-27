#include "pag_frame_renderer.h"

namespace huxerui::avkit::detail {

std::unique_ptr<PagGpuTarget> CreatePagGpuTarget(render::RenderBackend&, PixelSize) { return {}; }

} // namespace huxerui::avkit::detail
