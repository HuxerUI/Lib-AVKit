#pragma once

#include <memory>

#include <huxerui/external_texture.h>

#if defined(__OBJC__)
#include <huxerui/avkit/analysis.h>
#include <huxerui/avkit/macos/bgfx.h>
#include <huxerui/macos/external_texture.h>
#include <huxerui/media_format.h>
#endif

namespace avkit_preview {

#if defined(__OBJC__)
huxerui::avkit::render::RenderSurface CreatePreviewSticker(id<MTLDevice> device, bool cue = false);
huxerui::avkit::render::RenderSurface CreateTrackingSticker(id<MTLDevice> device, huxerui::avkit::analysis::TrackedPart part);
bool HasPreviewStickerCue(id<MTLTexture> texture, huxerui::media::MediaTimestamp time);
huxerui::Bytes ReadPreviewPixels(id<MTLTexture> texture);
void PublishPreviewSurface(huxerui::macos::MetalTexture& mailbox, const huxerui::avkit::render::RenderSurface& surface);
#endif
std::shared_ptr<huxerui::ExternalTexture> CreateMetalPreview();
int VerifyMetalPreview();

} // namespace avkit_preview
