#include "timeline_strip.h"
#include "timeline_preview.h"

#include <stdexcept>
#include <type_traits>

#import <CoreImage/CoreImage.h>
#import <ImageIO/ImageIO.h>

#include <huxerui/macos/external_texture.h>

namespace avkit_preview {
namespace {

using namespace huxerui;
using namespace huxerui::avkit;
using namespace huxerui::media;

std::shared_ptr<StripPreviewOptions> options;

struct CFDeleter { void operator()(CFTypeRef value) const { CFRelease(value); } };
template<class Ref> using Owned = std::unique_ptr<std::remove_pointer_t<Ref>, CFDeleter>;

} // namespace

ImageAsset EncodeStripThumbnail(const VideoFrame& frame, PixelSize size) {
  @autoreleasepool {
    auto texture = MakeExternalTexture(frame, {size.width, size.height});
    if (!texture.Succeeded()) throw std::runtime_error(texture.Error().message);
    const auto mailbox = std::dynamic_pointer_cast<huxerui::macos::PixelBufferTexture>(texture.Value());
    const Owned<CVPixelBufferRef> buffer(mailbox ? mailbox->AcquireFrame() : nullptr);
    if (!buffer) throw std::runtime_error("Thumbnail texture has no pixel buffer");
    thread_local const Owned<CFTypeRef> context((__bridge_retained CFTypeRef)[CIContext contextWithOptions:
        @{kCIContextUseSoftwareRenderer: @YES, kCIContextCacheIntermediates: @NO}]);
    auto* image = [CIImage imageWithCVPixelBuffer:buffer.get()];
    const Owned<CGImageRef> picture([(__bridge CIContext*)context.get() createCGImage:image fromRect:CGRectMake(0, 0, size.width, size.height)]);
    const Owned<CFMutableDataRef> bytes(CFDataCreateMutable(nullptr, 0));
    const Owned<CGImageDestinationRef> png(bytes ? CGImageDestinationCreateWithData(bytes.get(), CFSTR("public.png"), 1, nullptr) : nullptr);
    if (!picture || !png) throw std::runtime_error("Cannot create a thumbnail image");
    CGImageDestinationAddImage(png.get(), picture.get(), nullptr);
    if (!CGImageDestinationFinalize(png.get())) throw std::runtime_error("Cannot encode thumbnail PNG");
    return ImageAsset::CopyEncoded({reinterpret_cast<const std::byte*>(CFDataGetBytePtr(bytes.get())), std::size_t(CFDataGetLength(bytes.get()))});
  }
}

void ConfigureTimelineStrip(std::string_view path, std::string_view replacement) {
  ConfigureTimelinePreview(path, "0");
  options.reset(new StripPreviewOptions{Project::Open(File(path)), File(replacement), {TimelineProviders(), EncodeStripThumbnail}});
}

std::shared_ptr<StripPreviewOptions> TimelineStripOptions() { return options; }

} // namespace avkit_preview
