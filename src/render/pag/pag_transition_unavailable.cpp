#include "render/pag/pag_transition.h"

#include <stdexcept>

namespace huxerui::avkit {

struct PagTransition::Impl {};

PagTransition::PagTransition(const ResourceRef&, const ResourceCatalog&, PixelSize) {
  throw std::runtime_error("The platform has no GPU PAG transition adapter");
}

PagTransition::~PagTransition() = default;

render::Texture PagTransition::Sample(media::MediaTimestamp, media::MediaTimestamp,
    const render::Texture&, const render::Texture&) {
  throw std::runtime_error("The platform has no GPU PAG transition adapter");
}

} // namespace huxerui::avkit
