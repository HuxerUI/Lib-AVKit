#define GL_SILENCE_DEPRECATION

#include <huxerui/avkit/pag.h>

#include "bgfx_metal_texture.h"
#include "animation/pag_frame_renderer.h"
#include "support/media_time.h"

#import <CoreVideo/CoreVideo.h>
#import <OpenGL/CGLIOSurface.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <pag/pag.h>

namespace huxerui::avkit {
namespace {

using namespace render;
using Clock = std::chrono::steady_clock;

void Check(bool valid, const char* message) {
  if (!valid) throw std::runtime_error(message);
}

class GlContext {
public:
  GlContext() {
    const CGLPixelFormatAttribute attrs[]{kCGLPFAOpenGLProfile,
        static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core), kCGLPFAAccelerated,
        static_cast<CGLPixelFormatAttribute>(0)};
    CGLPixelFormatObj format = nullptr;
    GLint count = 0;
    Check(CGLChoosePixelFormat(attrs, &format, &count) == kCGLNoError && format,
          "Cannot choose an accelerated OpenGL format for PAG");
    const auto status = CGLCreateContext(format, nullptr, &context_);
    CGLReleasePixelFormat(format);
    Check(status == kCGLNoError && context_, "Cannot create an OpenGL context for PAG");
  }
  ~GlContext() { CGLReleaseContext(context_); }
  CGLContextObj Get() const { return context_; }

private:
  CGLContextObj context_{};
};

class CurrentContext {
public:
  explicit CurrentContext(CGLContextObj context) : previous_(CGLGetCurrentContext()) {
    Check(CGLSetCurrentContext(context) == kCGLNoError, "Cannot bind the PAG OpenGL context");
  }
  ~CurrentContext() { CGLSetCurrentContext(previous_); }

private:
  CGLContextObj previous_;
};

struct PixelBufferRelease {
  void operator()(CVPixelBufferRef pixels) const { CVPixelBufferRelease(pixels); }
};

class SharedTexture {
public:
  SharedTexture(id<MTLDevice> device, CGLContextObj context, PixelSize size) : context_(context) {
    auto* attributes = @{
        (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
    };
    CVPixelBufferRef buffer = nullptr;
    Check(CVPixelBufferCreate(nullptr, size.width, size.height, kCVPixelFormatType_32BGRA,
                             (__bridge CFDictionaryRef)attributes, &buffer) == kCVReturnSuccess && buffer,
          "Cannot allocate a PAG IOSurface pixel buffer");
    buffer_.reset(buffer);
    const auto backing = CVPixelBufferGetIOSurface(buffer);
    Check(backing != nullptr, "PAG pixel buffer is missing its IOSurface");
    auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                           width:size.width height:size.height mipmapped:NO];
    descriptor.storageMode = device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    metal = [device newTextureWithDescriptor:descriptor iosurface:backing plane:0];
    Check(metal != nil, "Cannot create the PAG IOSurface Metal view");

    CurrentContext current(context_);
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_RECTANGLE, texture_);
    const auto status = CGLTexImageIOSurface2D(context_, GL_TEXTURE_RECTANGLE, GL_RGBA8, size.width, size.height,
                                              GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, backing, 0);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    if (status != kCGLNoError || glGetError() != GL_NO_ERROR || !texture_) {
      glDeleteTextures(1, &texture_);
      throw std::runtime_error("Cannot bind the PAG IOSurface OpenGL view");
    }
  }

  ~SharedTexture() {
    const auto previous = CGLGetCurrentContext();
    if (CGLSetCurrentContext(context_) == kCGLNoError) glDeleteTextures(1, &texture_);
    CGLSetCurrentContext(previous);
  }

  pag::BackendTexture BackendTexture() const {
    return {pag::GLTextureInfo{texture_, GL_TEXTURE_RECTANGLE, GL_RGBA8},
            static_cast<int>(metal.width), static_cast<int>(metal.height)};
  }

  std::shared_ptr<pag::PAGImage> Image() const {
    // Rebind the IOSurface view after Metal has completed its write.
    glBindTexture(GL_TEXTURE_RECTANGLE, texture_);
    auto image = pag::PAGImage::FromTexture(BackendTexture(), pag::ImageOrigin::TopLeft);
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    Check(bool(image), "PAG could not sample the IOSurface OpenGL view");
    return image;
  }

  id<MTLTexture> metal;

private:
  CGLContextObj context_;
  std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, PixelBufferRelease> buffer_;
  GLuint texture_{};
};

struct OutputSlot {
  OutputSlot(id<MTLDevice> device, CGLContextObj context, PixelSize size)
      : texture(device, context, size), surface(macos::RetainMetalTexture(texture.metal)) {
    CurrentContext current(context);
    drawable = pag::PAGSurface::MakeFrom(texture.BackendTexture(), pag::ImageOrigin::TopLeft);
    Check(bool(drawable), "PAG could not render into the IOSurface OpenGL view");
  }

  SharedTexture texture;
  ColorSurface surface;
  std::shared_ptr<pag::PAGSurface> drawable;
};

class PagMetalTarget final : public detail::PagGpuTarget {
public:
  explicit PagMetalTarget(PixelSize size) : size_(size), device_(MTLCreateSystemDefaultDevice()) {
    Check(device_ != nil, "No Metal device for the PAG render target");
  }

  ColorSurface Draw(pag::PAGPlayer& player, PagOverlayStats* stats) override {
    auto found = std::ranges::find_if(outputs_, [](const auto& slot) { return slot->surface.pixels.storage.use_count() == 1; });
    auto& slot = *(found != outputs_.end() ? *found : outputs_.emplace_back(
        std::make_unique<OutputSlot>(device_, context_.Get(), size_)));
    CurrentContext current(context_.Get());
    player.setSurface(slot.drawable);
    if (stats) ++stats->flushes;
    player.flush();
    const auto waiting = stats ? Clock::now() : Clock::time_point{};
    glFinish();
    if (stats) stats->gpu_sync += Clock::now() - waiting;
    Check(glGetError() == GL_NO_ERROR, "PAG texture rendering failed");
    return slot.surface;
  }

private:
  GlContext context_;
  PixelSize size_;
  id<MTLDevice> device_;
  std::vector<std::unique_ptr<OutputSlot>> outputs_;
};

class Interop {
public:
  Interop(id<MTLDevice> device, PixelSize size)
      : device(device), queue_([device newCommandQueue]), from(device, context.Get(), size), to(device, context.Get(), size) {
    Check(queue_ != nil, "Cannot create the PAG Metal copy queue");
  }

  void CopyInputs(id<MTLTexture> outgoing, id<MTLTexture> incoming) {
    auto* commands = [queue_ commandBuffer];
    if (outgoing.pixelFormat == MTLPixelFormatBGRA8Unorm && incoming.pixelFormat == MTLPixelFormatBGRA8Unorm) {
      auto* blit = [commands blitCommandEncoder];
      Check(blit != nil, "Cannot prepare the PAG Metal texture copies");
      const auto copy = [&](id<MTLTexture> source, id<MTLTexture> destination) {
        [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(source.width, source.height, 1)
                   toTexture:destination destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
      };
      copy(outgoing, from.metal);
      copy(incoming, to.metal);
      [blit endEncoding];
    } else {
      if (!conversion_) conversion_ = CreateConversionPipeline();
      const auto stage = [&](id<MTLTexture> source, id<MTLTexture> destination) {
        auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        auto* color = pass.colorAttachments[0];
        color.texture = destination;
        color.loadAction = MTLLoadActionDontCare;
        color.storeAction = MTLStoreActionStore;
        auto* encoder = [commands renderCommandEncoderWithDescriptor:pass];
        Check(encoder != nil, "Cannot stage the PAG transition input on the GPU");
        [encoder setRenderPipelineState:conversion_];
        [encoder setFragmentTexture:source atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
      };
      stage(outgoing, from.metal);
      stage(incoming, to.metal);
    }
    [commands commit];
    [commands waitUntilCompleted];
    Check(commands.status == MTLCommandBufferStatusCompleted, "The PAG Metal texture copies failed");
  }

  OutputSlot& AcquireOutput(PixelSize size) {
    for (auto& slot : outputs_)
      if (slot->surface.pixels.storage.use_count() == 1) return *slot;
    return *outputs_.emplace_back(std::make_unique<OutputSlot>(device, context.Get(), size));
  }

  GlContext context;
  id<MTLDevice> device;

private:
  id<MTLRenderPipelineState> CreateConversionPipeline() const {
    // Metal preserves logical RGBA channels when writing the OpenGL-compatible BGRA IOSurface.
    constexpr auto source = R"(
      #include <metal_stdlib>
      using namespace metal;

      vertex float4 stage_vertex(uint index [[vertex_id]]) {
        return float4(index == 1 ? 3.0 : -1.0, index == 2 ? 3.0 : -1.0, 0.0, 1.0);
      }

      fragment float4 stage_fragment(float4 position [[position]], texture2d<float, access::read> pixels [[texture(0)]]) {
        return pixels.read(uint2(position.xy));
      }
    )";
    NSError* error = nil;
    auto* library = [device newLibraryWithSource:[NSString stringWithUTF8String:source] options:nil error:&error];
    if (!library) throw std::runtime_error(error ? error.localizedDescription.UTF8String :
                                                   "Cannot compile the PAG Metal input conversion");
    auto* config = [[MTLRenderPipelineDescriptor alloc] init];
    config.vertexFunction = [library newFunctionWithName:@"stage_vertex"];
    config.fragmentFunction = [library newFunctionWithName:@"stage_fragment"];
    config.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    auto* pipeline = [device newRenderPipelineStateWithDescriptor:config error:&error];
    if (!pipeline) throw std::runtime_error(error ? error.localizedDescription.UTF8String :
                                                    "Cannot create the PAG Metal input conversion pipeline");
    return pipeline;
  }

  id<MTLCommandQueue> queue_;
  id<MTLRenderPipelineState> conversion_ = nil;

public:
  SharedTexture from, to;

private:
  std::vector<std::unique_ptr<OutputSlot>> outputs_;
};

id<MTLTexture> TextureFor(const ColorSurface& surface, PixelSize size) {
  auto texture = macos::GetMetalTexture(surface);
  const auto format = surface.pixels.image.format;
  const bool matches_format = texture &&
      ((format == PixelFormat::Bgra8Unorm && texture.pixelFormat == MTLPixelFormatBGRA8Unorm) ||
       (format == PixelFormat::Rgba8Unorm && texture.pixelFormat == MTLPixelFormatRGBA8Unorm));
  if (!matches_format || surface.pixels.image.size != size || texture.width != size.width ||
      texture.height != size.height || surface.pixels.origin != RowOrigin::TopLeft ||
      surface.alpha != AlphaMode::Premultiplied)
    throw std::invalid_argument("PAG transition requires completed top-left premultiplied RGBA8 or BGRA8 Metal textures");
  return texture;
}

} // namespace

std::unique_ptr<detail::PagGpuTarget> detail::CreatePagGpuTarget(render::RenderBackend&, PixelSize size) {
  return std::make_unique<PagMetalTarget>(size);
}

struct PagTransition::Impl {
  Impl(BufferReference bytes, PixelSize size, int outgoing, int incoming)
      : encoded(std::move(bytes)), size(size), outgoing_index(outgoing), incoming_index(incoming) {
    if (!size.width || !size.height || size.width > 8192 || size.height > 8192)
      throw std::invalid_argument("Invalid PAG transition output dimensions");
    const auto data = encoded.AsBytes();
    composition = pag::PAGFile::Load(data.data(), data.size());
    if (!composition) throw std::invalid_argument("Invalid PAG transition composition");
    const auto editable = composition->getEditableIndices(pag::LayerType::Image);
    if (outgoing == incoming || std::ranges::find(editable, outgoing) == editable.end() ||
        std::ranges::find(editable, incoming) == editable.end())
      throw std::invalid_argument("A PAG transition needs two distinct editable image indices");
    const auto count = std::floor(double(composition->duration()) * composition->frameRate() / 1'000'000);
    if (!std::isfinite(count) || count < 1 || count > std::numeric_limits<std::uint32_t>::max())
      throw std::invalid_argument("Invalid PAG transition frame count");
    frames = static_cast<std::uint32_t>(count);
    player.setComposition(composition);
    player.setScaleMode(pag::PAGScaleMode::Stretch);
    player.setMaxFrameRate(composition->frameRate());
  }

  BufferReference encoded;
  PixelSize size;
  int outgoing_index, incoming_index;
  std::uint32_t frames;
  std::unique_ptr<Interop> gpu;
  std::shared_ptr<pag::PAGFile> composition;
  pag::PAGPlayer player;
};

PagTransition::PagTransition(BufferReference encoded, PixelSize size, int outgoing, int incoming)
    : impl_(std::make_unique<Impl>(std::move(encoded), size, outgoing, incoming)) {}
PagTransition::~PagTransition() = default;

ColorSurface PagTransition::Sample(media::MediaTimestamp offset, media::MediaTimestamp duration,
    const ColorSurface& outgoing, const ColorSurface& incoming, PagTransitionTiming* timing) {
  auto& state = *impl_;
  const auto index = detail::UniformFrameIndex(offset, duration, state.frames);
  auto from = TextureFor(outgoing, state.size);
  auto to = TextureFor(incoming, state.size);
  if (from.device != to.device || (state.gpu && state.gpu->device != from.device))
    throw std::invalid_argument("PAG transition input textures must use the same device");
  if (!state.gpu) state.gpu = std::make_unique<Interop>(from.device, state.size);
  auto& gpu = *state.gpu;
  const auto started = timing ? Clock::now() : Clock::time_point{};
  gpu.CopyInputs(from, to);
  const auto copied = timing ? Clock::now() : Clock::time_point{};
  auto& output = gpu.AcquireOutput(state.size);
  CurrentContext current(gpu.context.Get());
  state.player.setSurface(output.drawable);
  state.composition->replaceImage(state.outgoing_index, gpu.from.Image());
  state.composition->replaceImage(state.incoming_index, gpu.to.Image());
  // PAG converts progress to whole microseconds. Use the interior of the selected authored frame.
  state.player.setProgress((index + 0.5) / state.frames);
  const auto bound = timing ? Clock::now() : Clock::time_point{};
  Check(state.player.flush(), "PAG could not render the GPU transition");
  const auto flushed = timing ? Clock::now() : Clock::time_point{};
  glFinish();
  Check(glGetError() == GL_NO_ERROR, "The PAG OpenGL transition failed");
  if (timing) *timing = {copied - started, bound - copied, flushed - bound, Clock::now() - flushed};
  return output.surface;
}

} // namespace huxerui::avkit
