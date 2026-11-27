#pragma once

#include <huxerui/avkit/scene.h>

namespace huxerui::avkit::detail {

void InstallTimelineSystems(RenderScene& scene);

void InstallCaptureSystems(RenderScene& scene);

void InstallAudioSystem(RenderScene& scene);

} // namespace huxerui::avkit::detail
