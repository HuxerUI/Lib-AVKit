#pragma once

#include <stop_token>

#include <entityx/entityx.h>
#include <huxerui/media_format.h>

namespace huxerui::avkit {

class RenderScene;

namespace detail {

class AudioSystem final : public entityx::System<AudioSystem> {
public:
  void update(entityx::EntityManager& entities, entityx::EventManager&, entityx::TimeDelta) override;
};

media::AudioFrame EvaluateAudioBlock(RenderScene& scene, media::MediaTimestamp start, std::uint32_t samples,
                                     media::AudioFormat format, std::stop_token stop = {});

} // namespace detail
} // namespace huxerui::avkit
