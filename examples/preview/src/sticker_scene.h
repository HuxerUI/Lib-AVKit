#pragma once

#include <huxerui/avkit_ecs.h>

namespace avkit_preview {

void InstallStickerScene(huxerui::avkit::SceneEvaluator& scene);
void VerifyStickerScene();

huxerui::Point StickerCueCenter(huxerui::media::MediaTimestamp time);
bool HasStickerCue(const huxerui::media::VideoFrame& frame);

} // namespace avkit_preview
