#include <huxerui/avkit/pag.h>

#include <stdexcept>

namespace huxerui::avkit {

struct PagTransition::Impl {};

PagTransition::PagTransition(BufferReference, PixelSize, int, int) {
  throw std::runtime_error("The platform has no GPU PAG transition adapter");
}

PagTransition::~PagTransition() = default;

render::ColorSurface PagTransition::Sample(media::MediaTimestamp, media::MediaTimestamp,
    const render::ColorSurface&, const render::ColorSurface&, PagTransitionTiming*) {
  throw std::runtime_error("The platform has no GPU PAG transition adapter");
}

} // namespace huxerui::avkit
