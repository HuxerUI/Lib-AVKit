#include "movie_renderer.h"

#include "metal_verify.h"
#include "sticker_scene.h"

#include <huxerui/avkit/bgfx.h>

#include <stdexcept>
#include <utility>

#import <Foundation/Foundation.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::avkit::render;
using namespace huxerui::media;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

Resources OpenPreviewResources() {
  auto* directory = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"HuxerUI" isDirectory:YES];
  Check(directory != nil, "Cannot locate the preview resource package");
  return Resources::OpenPackage(File(directory.fileSystemRepresentation));
}

const OverlayAssets& PreviewAssets() {
  static const auto assets = LoadOverlayAssets(OpenPreviewResources());
  return assets;
}

} // namespace

struct MovieRenderer::Impl {
  explicit Impl(PixelSize size)
      : target_{size, render::PixelFormat::Bgra8Unorm},
        timeline_(target_, CreateBgfxBackend(target_), PreviewTimelineResources(PreviewAssets())),
        sticker_(CreatePreviewSticker(device_)) {
    InstallStickerScene(scene_);
  }

  ColorSurface Draw(const VideoFrame& input, bool cue) {
    return DrawSticker(ImportFrame(input).surface, input.pts, cue);
  }

  ImportedVideoFrame ImportFrame(const VideoFrame& input) { return timeline_.ImportFrame(input); }

  ColorSurface Draw(const ColorSurface& input, MediaTimestamp time, bool cue) {
    return DrawSticker(input, time, cue);
  }

private:
  ColorSurface DrawSticker(const ColorSurface& base, MediaTimestamp time, bool cue) {
    if (cue && !cue_) cue_ = CreatePreviewSticker(device_, true);
    return timeline_.Render({scene_.Evaluate(time, target_), {base, cue ? *cue_ : sticker_}});
  }

public:
  TimelineRenderer& Timeline() { return timeline_; }

private:
  id<MTLDevice> device_ = MTLCreateSystemDefaultDevice();
  ImageDesc target_;
  SceneEvaluator scene_;
  TimelineRenderer timeline_;
  ColorSurface sticker_;
  std::optional<ColorSurface> cue_;
};

MovieRenderer::MovieRenderer(PixelSize size) : impl_(std::make_unique<Impl>(size)) {}
MovieRenderer::~MovieRenderer() = default;

ImportedVideoFrame MovieRenderer::ImportFrame(const VideoFrame& input) { return impl_->ImportFrame(input); }

ColorSurface MovieRenderer::Draw(const ColorSurface& input, MediaTimestamp time, bool cue) {
  return impl_->Draw(input, time, cue);
}

ColorSurface MovieRenderer::Draw(const VideoFrame& input, bool cue) { return impl_->Draw(input, cue); }

ColorSurface MovieRenderer::Draw(const TimelineFrame& frame, PagTransitionRenderTiming* timing) {
  @autoreleasepool { return Timeline().Draw(frame, {.transition = timing}); }
}

ColorSurface MovieRenderer::Draw(const TimelineFrame& frame, PagOverlayStats& stats, bool batch_overlays) {
  @autoreleasepool { return Timeline().Draw(frame, {.overlays = &stats, .batch_overlays = batch_overlays}); }
}

VideoFrame MovieRenderer::EncoderFrame(const ColorSurface& output, const VideoEncoderConfig& config,
                                       MediaTimestamp pts, std::optional<MediaTimestamp> duration) {
  return Timeline().EncoderFrame(output, config, pts, duration);
}

VideoFrame MovieRenderer::EncodeFrame(const VideoFrame& input, const VideoEncoderConfig& config, bool cue) {
  @autoreleasepool {
    return EncoderFrame(Draw(input, cue), config, input.pts, input.duration);
  }
}

TimelineRenderer& MovieRenderer::Timeline() { return impl_->Timeline(); }

std::unique_ptr<PagTransition> OpenPreviewPagTransition(PixelSize size) {
  return std::make_unique<PagTransition>(PreviewAssets().transition, size, 1, 0);
}

std::unique_ptr<PagAnimation> OpenPreviewPagSticker(const ResourceRef& resource, const StickerReplacements& replacements,
                                                    RenderBackend* backend) {
  auto asset = ResolvePreviewPagSticker(PreviewAssets(), resource, replacements);
  return std::make_unique<PagAnimation>(std::move(asset.encoded), asset.texts, asset.images, backend);
}

} // namespace avkit_preview
