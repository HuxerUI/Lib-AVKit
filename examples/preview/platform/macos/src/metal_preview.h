#pragma once

#include <memory>

#include <huxerui/external_texture.h>

#if defined(__OBJC__)
#import <Metal/Metal.h>

#include <huxerui/avkit/render.h>
#include <huxerui/macos/external_texture.h>
#include <huxerui/media_format.h>
#endif

namespace avkit_preview {

#if defined(__OBJC__)
huxerui::avkit::render::ColorSurface CreatePreviewSticker(id<MTLDevice> device, bool cue = false);
bool HasPreviewStickerCue(id<MTLTexture> texture, huxerui::media::MediaTimestamp time);
huxerui::Bytes ReadPreviewPixels(id<MTLTexture> texture);
void PublishPreviewSurface(huxerui::macos::MetalTexture& mailbox, const huxerui::avkit::render::ColorSurface& surface);
#endif
std::shared_ptr<huxerui::ExternalTexture> CreateMetalPreview();
int VerifyMetalPreview();

} // namespace avkit_preview
