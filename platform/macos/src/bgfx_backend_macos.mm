#include <huxerui/avkit/macos/bgfx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

#include <bgfx/bgfx.h>

#include "avkit_fs_composite.h"
#include "avkit_fs_masked_blend.h"
#include "avkit_vs_composite.h"

namespace huxerui::avkit::macos {
namespace {

using namespace render;

static_assert(BGFX_CONFIG_MULTITHREADED == 0, "The Metal completion wait requires inline submission");

constexpr std::uint64_t kSamplerFlags =
    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_POINT;
std::atomic_flag bgfx_in_use = ATOMIC_FLAG_INIT;

MTLPixelFormat MetalFormat(ImageFormat format) {
  switch (format) {
    case ImageFormat::Rgba8Unorm: return MTLPixelFormatRGBA8Unorm;
    case ImageFormat::Bgra8Unorm: return MTLPixelFormatBGRA8Unorm;
    case ImageFormat::R8Unorm: return MTLPixelFormatR8Unorm;
  }
  throw std::invalid_argument("Unsupported Metal texture format");
}

bgfx::TextureFormat::Enum BgfxFormat(ImageFormat format) {
  switch (format) {
    case ImageFormat::Rgba8Unorm: return bgfx::TextureFormat::RGBA8;
    case ImageFormat::Bgra8Unorm: return bgfx::TextureFormat::BGRA8;
    case ImageFormat::R8Unorm: return bgfx::TextureFormat::R8;
  }
  throw std::invalid_argument("Unsupported bgfx texture format");
}

bool IsColor(ImageFormat format) {
  return format == ImageFormat::Rgba8Unorm || format == ImageFormat::Bgra8Unorm;
}

bool CanSample(id<MTLTexture> texture) {
  return texture != nil && texture.textureType == MTLTextureType2D && texture.sampleCount == 1 &&
         !texture.framebufferOnly &&
         (texture.usage == MTLTextureUsageUnknown || (texture.usage & MTLTextureUsageShaderRead) != 0);
}

class MetalStorage final : public SurfaceStorage {
public:
  explicit MetalStorage(id<MTLTexture> handle) : texture(handle) {}
  id<MTLTexture> texture;
};

id<MTLTexture> TextureFromSurface(const DataSurface& surface) {
  auto storage = std::dynamic_pointer_cast<const MetalStorage>(surface.storage);
  return storage ? storage->texture : nil;
}

DataSurface RetainTexture(id<MTLTexture> texture, ImageFormat format, RowOrigin origin) {
  if (!CanSample(texture) || texture.width > std::numeric_limits<std::uint16_t>::max() ||
      texture.height > std::numeric_limits<std::uint16_t>::max()) {
    throw std::invalid_argument("Expected a sampleable 2D Metal texture");
  }
  const ImageDesc image{{static_cast<std::uint32_t>(texture.width), static_cast<std::uint32_t>(texture.height)}, format};
  return {image, origin, std::make_shared<MetalStorage>(texture)};
}

class BgfxContext {
public:
  explicit BgfxContext(id<MTLDevice> device) {
    if (bgfx_in_use.test_and_set()) {
      throw std::logic_error("An AVKit bgfx backend is already active");
    }
    bgfx::Init init;
    init.type = bgfx::RendererType::Metal;
    init.fallback = false;
    init.platformData.context = (__bridge void*)device;
    init.swapChain.maxFrameLatency = 1;
    if (!bgfx::init(init)) {
      bgfx_in_use.clear();
      throw std::runtime_error("Could not initialize headless bgfx/Metal");
    }
  }

  ~BgfxContext() {
    bgfx::shutdown();
    bgfx_in_use.clear();
  }

  BgfxContext(const BgfxContext&) = delete;
  BgfxContext& operator=(const BgfxContext&) = delete;
};

bgfx::TextureHandle ImportTexture(id<MTLTexture> texture, ImageDesc image, std::uint64_t flags) {
  return bgfx::createTexture2D(static_cast<std::uint16_t>(image.size.width),
                              static_cast<std::uint16_t>(image.size.height), false, 1,
                              BgfxFormat(image.format), flags | kSamplerFlags, nullptr,
                              reinterpret_cast<std::uint64_t>((__bridge void*)texture));
}

template<class Handle> void Destroy(Handle handle) {
  if (bgfx::isValid(handle)) bgfx::destroy(handle);
}

struct Vertex {
  float x, y, z, u, v;
};

bgfx::VertexBufferHandle CreateQuad(bool flip_v) {
  const float top = flip_v ? 1.0F : 0.0F;
  const float bottom = 1.0F - top;
  const Vertex vertices[] = {
      {-1.0F, +1.0F, 0.0F, 0.0F, top},
      {-1.0F, -1.0F, 0.0F, 0.0F, bottom},
      {+1.0F, +1.0F, 0.0F, 1.0F, top},
      {+1.0F, -1.0F, 0.0F, 1.0F, bottom},
  };
  bgfx::VertexLayout layout;
  layout.begin()
      .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
      .end();
  return bgfx::createVertexBuffer(bgfx::copy(vertices, sizeof(vertices)), layout);
}

std::array<float, 4> ClipDestination(const Rect& rect) {
  return {float(2.0 * rect.x + rect.width - 1.0), float(1.0 - 2.0 * rect.y - rect.height),
          rect.width, rect.height};
}

struct ClipQuad {
  std::array<float, 4> destination;
  std::array<float, 4> off_diagonal{};
};

ClipQuad Destination(const Draw& draw) {
  const auto& rect = draw.destination;
  if (!draw.transform) return {ClipDestination(rect)};
  const auto& map = *draw.transform;
  const auto center = map.Apply({rect.x + rect.width / 2, rect.y + rect.height / 2});
  return {{2 * center.x - 1, 1 - 2 * center.y, map.m11 * rect.width, map.m22 * rect.height},
          {-map.m21 * rect.height, -map.m12 * rect.width, 0, 0}};
}

struct Target {
  std::shared_ptr<const MetalStorage> storage;
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
};

void DestroyTarget(const Target& target) {
  Destroy(target.framebuffer);
  Destroy(target.texture);
}

struct Job {
  RenderTicket ticket;
  RenderRequest request;
  const Target* target;
  bool ready{};
  bool failed{};
};

class MetalBackend final : public RenderBackend {
public:
  MetalBackend(id<MTLDevice> device, ImageDesc image) : device_(device), image_(image), context_(device) {}

  void Initialize() {
    if (!FitsDevice(image_.size)) {
      throw std::invalid_argument("The output dimensions exceed the Metal texture limit");
    }

    auto vertex = bgfx::createShader(bgfx::copy(avkit_vs_composite, sizeof(avkit_vs_composite)));
    auto fragment = bgfx::createShader(bgfx::copy(avkit_fs_composite, sizeof(avkit_fs_composite)));
    auto masked_fragment = bgfx::createShader(bgfx::copy(avkit_fs_masked_blend, sizeof(avkit_fs_masked_blend)));
    if (!bgfx::isValid(vertex) || !bgfx::isValid(fragment) || !bgfx::isValid(masked_fragment)) {
      Destroy(vertex);
      Destroy(fragment);
      Destroy(masked_fragment);
      throw std::runtime_error("Could not load the Metal composite shaders");
    }
    program_ = bgfx::createProgram(vertex, fragment);
    masked_program_ = bgfx::createProgram(vertex, masked_fragment);
    Destroy(vertex);
    Destroy(fragment);
    Destroy(masked_fragment);
    sampler_ = bgfx::createUniform("s_source", bgfx::UniformType::Sampler);
    incoming_sampler_ = bgfx::createUniform("s_incoming", bgfx::UniformType::Sampler);
    mask_sampler_ = bgfx::createUniform("s_mask", bgfx::UniformType::Sampler);
    input_flips_ = bgfx::createUniform("u_input_flips", bgfx::UniformType::Vec4);
    mask_parameters_ = bgfx::createUniform("u_mask_parameters", bgfx::UniformType::Vec4);
    mask_region_ = bgfx::createUniform("u_mask_region", bgfx::UniformType::Vec4);
    destination_ = bgfx::createUniform("u_destination", bgfx::UniformType::Vec4);
    off_diagonal_ = bgfx::createUniform("u_off_diagonal", bgfx::UniformType::Vec4);
    top_left_ = CreateQuad(false);
    bottom_left_ = CreateQuad(true);
    if (!bgfx::isValid(program_) || !bgfx::isValid(masked_program_) || !bgfx::isValid(sampler_) ||
        !bgfx::isValid(incoming_sampler_) || !bgfx::isValid(mask_sampler_) || !bgfx::isValid(input_flips_) ||
        !bgfx::isValid(mask_parameters_) || !bgfx::isValid(mask_region_) || !bgfx::isValid(destination_) ||
        !bgfx::isValid(off_diagonal_) || !bgfx::isValid(top_left_) || !bgfx::isValid(bottom_left_)) {
      throw std::runtime_error("Could not create the composite program or quad");
    }

    auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MetalFormat(image_.format)
                                                                          width:image_.size.width
                                                                         height:image_.size.height
                                                                      mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    for (auto& target : targets_) {
      id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
      if (texture == nil) throw std::runtime_error("Could not allocate a Metal render target");
      target.storage = std::make_shared<MetalStorage>(texture);
      target.texture = ImportTexture(texture, image_, BGFX_TEXTURE_RT);
      if (!bgfx::isValid(target.texture)) throw std::runtime_error("Could not import the Metal render target");
      target.framebuffer = bgfx::createFrameBuffer(1, &target.texture, false);
      if (!bgfx::isValid(target.framebuffer)) throw std::runtime_error("Could not create the offscreen framebuffer");
    }
  }

  ~MetalBackend() override {
    for (std::size_t index = 0; index <= subcomposition_targets_.size(); ++index)
      bgfx::resetView(static_cast<bgfx::ViewId>(index));
    for (const auto& target : targets_) DestroyTarget(target);
    for (const auto& target : subcomposition_targets_) DestroyTarget(target);
    Destroy(top_left_);
    Destroy(bottom_left_);
    Destroy(sampler_);
    Destroy(incoming_sampler_);
    Destroy(mask_sampler_);
    Destroy(input_flips_);
    Destroy(mask_parameters_);
    Destroy(mask_region_);
    Destroy(destination_);
    Destroy(off_diagonal_);
    Destroy(masked_program_);
    Destroy(program_);
  }

  SubmitResult TrySubmit(const RenderRequest& request) override {
    if (job_) return SubmitResult::Success(std::nullopt);
    if (!ValidPlan(request)) return SubmitResult::Failure({RenderErrorCode::InvalidPlan, "Invalid composite passes or inputs"});
    for (const auto& input : request.inputs) {
      if (input.alpha != AlphaMode::Premultiplied) {
        return SubmitResult::Failure({RenderErrorCode::Unsupported, "Composite inputs must be premultiplied"});
      }
      if (!IsColor(input.pixels.image.format) || !ValidInput(input.pixels)) {
        return SubmitResult::Failure({RenderErrorCode::InvalidFrame, "Invalid input texture or Metal device"});
      }
    }
    for (const auto& input : request.data_inputs) {
      if (input.image.format != ImageFormat::R8Unorm || !ValidInput(input))
        return SubmitResult::Failure({RenderErrorCode::InvalidFrame, "Expected a Metal R8Unorm coverage texture"});
    }
    for (const auto& target : targets_) {
      if (target.storage.use_count() == 1) {
        const RenderTicket ticket{next_ticket_++};
        job_.emplace(Job{ticket, request, &target});
        return SubmitResult::Success(ticket);
      }
    }
    return SubmitResult::Success(std::nullopt);
  }

  void Pump() override {
    if (!job_ || job_->ready) return;
    job_->failed = !Encode(*job_);
    bgfx::frame(BGFX_FRAME_FLUSH);
    // With one frame in flight, the pinned Metal backend waits on the previous
    // command buffer's completion semaphore at the start of the next submission.
    bgfx::frame(BGFX_FRAME_FLUSH);
    job_->ready = true;
  }

  TakeResult TryTake(RenderTicket ticket) override {
    if (!job_ || job_->ticket != ticket) {
      return TakeResult::Failure({RenderErrorCode::InvalidTicket, "Unknown render ticket"});
    }
    if (!job_->ready) return TakeResult::Success(std::nullopt);
    if (job_->failed) {
      job_.reset();
      return TakeResult::Failure({RenderErrorCode::OutOfMemory, "Could not prepare composite textures"});
    }
    RenderSurface output{{image_, RowOrigin::TopLeft, job_->target->storage}, AlphaMode::Premultiplied};
    job_.reset();
    return TakeResult::Success(std::move(output));
  }

  void Discard(RenderTicket ticket) override {
    if (job_ && job_->ticket == ticket) job_.reset();
  }

private:
  bool ValidPlan(const RenderRequest& request) const {
    if (!request.plan || request.plan->target != image_) return false;
    const auto& limits = bgfx::getCaps()->limits;
    const auto& plan = *request.plan;
    const auto passes = plan.subcompositions.size();
    const auto persistent = targets_.size() + std::max(subcomposition_targets_.size(), passes);
    if (passes >= limits.maxViews || persistent > limits.maxFrameBuffers || persistent > limits.maxTextures ||
        request.inputs.size() > limits.maxTextures - persistent ||
        request.data_inputs.size() > limits.maxTextures - persistent - request.inputs.size()) return false;

    auto remaining_draws = std::size_t(limits.maxDrawCalls);
    const auto validate = [&](std::span<const Draw> draws, std::size_t colors, std::size_t masks) {
      const auto count = std::max<std::size_t>(1, draws.size());
      if (count > remaining_draws) return false;
      remaining_draws -= count;
      return std::ranges::all_of(draws, [&](const Draw& draw) {
        if (draw.input_slot >= colors || draw.destination.IsEmpty()) return false;
        const auto quad = Destination(draw);
        const auto finite = [](float value) { return std::isfinite(value); };
        if (!std::ranges::all_of(quad.destination, finite) || !std::ranges::all_of(quad.off_diagonal, finite)) return false;
        if (!draw.masked_blend) return true;
        const auto& mask = *draw.masked_blend;
        const auto& region = mask.mask_region;
        return mask.incoming_slot < colors && mask.mask_slot < masks &&
               std::isfinite(mask.feather) && mask.feather >= 0 && mask.feather <= 1 && !region.IsEmpty() &&
               std::isfinite(region.x) && std::isfinite(region.y) &&
               std::isfinite(region.width) && std::isfinite(region.height);
      });
    };
    for (const auto& pass : plan.subcompositions) {
      if (!validate(pass.draws, request.inputs.size(), 0)) return false;
    }
    return validate(plan.draws, request.inputs.size() + passes, request.data_inputs.size());
  }

  bool FitsDevice(PixelSize size) const {
    const auto limit = bgfx::getCaps()->limits.maxTextureSize;
    return size.width != 0 && size.height != 0 && size.width <= limit && size.height <= limit &&
           size.width <= std::numeric_limits<std::uint16_t>::max() &&
           size.height <= std::numeric_limits<std::uint16_t>::max();
  }

  bool ValidInput(const DataSurface& input) const {
    id<MTLTexture> texture = TextureFromSurface(input);
    const auto& image = input.image;
    return CanSample(texture) && texture.device == device_ && texture.pixelFormat == MetalFormat(image.format) &&
           texture.width == image.size.width && texture.height == image.size.height && FitsDevice(image.size);
  }

  bool EnsureSubcompositionTargets(std::size_t count) {
    subcomposition_targets_.reserve(count);
    while (subcomposition_targets_.size() < count) {
      Target target;
      target.texture = bgfx::createTexture2D(static_cast<std::uint16_t>(image_.size.width),
                                            static_cast<std::uint16_t>(image_.size.height), false, 1,
                                            BgfxFormat(image_.format), BGFX_TEXTURE_RT | kSamplerFlags);
      if (!bgfx::isValid(target.texture)) return false;
      target.framebuffer = bgfx::createFrameBuffer(1, &target.texture, false);
      if (!bgfx::isValid(target.framebuffer)) {
        Destroy(target.texture);
        return false;
      }
      subcomposition_targets_.push_back(std::move(target));
    }
    return true;
  }

  bool Encode(const Job& job) {
    const auto& plan = *job.request.plan;
    if (!EnsureSubcompositionTargets(plan.subcompositions.size())) return false;
    std::vector<bgfx::TextureHandle> handles;
    handles.reserve(job.request.inputs.size() + job.request.data_inputs.size());
    const auto import = [&](const DataSurface& input) {
      const auto handle = ImportTexture(TextureFromSurface(input), input.image, BGFX_TEXTURE_NONE);
      if (!bgfx::isValid(handle)) {
        for (auto imported : handles) Destroy(imported);
        return false;
      }
      handles.push_back(handle);
      return true;
    };
    for (const auto& input : job.request.inputs) if (!import(input.pixels)) return false;
    for (const auto& input : job.request.data_inputs) if (!import(input)) return false;

    bgfx::ViewId view = 0;
    for (const auto& pass : plan.subcompositions) {
      EncodePass(view, subcomposition_targets_[view].framebuffer, pass.draws, job.request, handles);
      ++view;
    }
    EncodePass(view, job.target->framebuffer, plan.draws, job.request, handles);
    for (auto handle : handles) Destroy(handle);
    return true;
  }

  void EncodePass(bgfx::ViewId view, bgfx::FrameBufferHandle target, std::span<const Draw> draws,
                  const RenderRequest& request, std::span<const bgfx::TextureHandle> handles) {
    const auto external_colors = request.inputs.size();
    const auto color = [&](std::uint32_t slot) {
      return slot < external_colors ? handles[slot] : subcomposition_targets_[slot - external_colors].texture;
    };
    const auto origin = [&](std::uint32_t slot) {
      return slot < external_colors ? request.inputs[slot].pixels.origin : RowOrigin::TopLeft;
    };
    bgfx::setViewFrameBuffer(view, target);
    bgfx::setViewRect(view, 0, 0, static_cast<std::uint16_t>(image_.size.width),
                      static_cast<std::uint16_t>(image_.size.height));
    bgfx::setViewClear(view, BGFX_CLEAR_COLOR, 0x00000000);
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    if (draws.empty()) bgfx::touch(view);
    for (const auto& draw : draws) {
      const auto quad = Destination(draw);
      bgfx::setUniform(destination_, quad.destination.data());
      bgfx::setUniform(off_diagonal_, quad.off_diagonal.data());
      bgfx::setTexture(0, sampler_, color(draw.input_slot));
      auto program = program_;
      auto vertices = origin(draw.input_slot) == RowOrigin::BottomLeft ? bottom_left_ : top_left_;
      if (draw.masked_blend) {
        const auto& blend = *draw.masked_blend;
        const auto flip = [](RowOrigin value) { return value == RowOrigin::BottomLeft ? 1.0F : 0.0F; };
        const std::array flips{flip(origin(draw.input_slot)), flip(origin(blend.incoming_slot)),
                               flip(request.data_inputs[blend.mask_slot].origin), 0.0F};
        const std::array parameters{blend.feather, blend.inverted ? 1.0F : 0.0F, 0.0F, 0.0F};
        const auto& rect = blend.mask_region;
        const std::array region{rect.x, rect.y, rect.width, rect.height};
        bgfx::setUniform(input_flips_, flips.data());
        bgfx::setUniform(mask_parameters_, parameters.data());
        bgfx::setUniform(mask_region_, region.data());
        bgfx::setTexture(1, incoming_sampler_, color(blend.incoming_slot));
        bgfx::setTexture(2, mask_sampler_, handles[external_colors + blend.mask_slot]);
        program = masked_program_;
        vertices = top_left_;
      }
      bgfx::setVertexBuffer(0, vertices);
      std::uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_TRISTRIP;
      if (draw.blend == Blend::PremultipliedOver) {
        state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
      }
      bgfx::setState(state);
      bgfx::submit(view, program);
    }
  }

  id<MTLDevice> device_;
  ImageDesc image_;
  std::array<Target, 2> targets_{};
  std::vector<Target> subcomposition_targets_;
  std::optional<Job> job_;
  std::uint64_t next_ticket_{1};
  bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle masked_program_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle sampler_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle incoming_sampler_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle mask_sampler_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle input_flips_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle mask_parameters_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle mask_region_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle destination_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle off_diagonal_ = BGFX_INVALID_HANDLE;
  bgfx::VertexBufferHandle top_left_ = BGFX_INVALID_HANDLE;
  bgfx::VertexBufferHandle bottom_left_ = BGFX_INVALID_HANDLE;
  // Shutdown drains bgfx before the leases retaining its external textures are destroyed.
  BgfxContext context_;
};

} // namespace

std::unique_ptr<render::RenderBackend> CreateBgfxBackend(id<MTLDevice> device, render::ImageDesc target) {
  if (device == nil) throw std::invalid_argument("A Metal device is required");
  if (!IsColor(target.format)) throw std::invalid_argument("The composite target must be RGBA8 or BGRA8");
  auto backend = std::make_unique<MetalBackend>(device, target);
  backend->Initialize();
  return backend;
}

render::RenderSurface RetainMetalTexture(id<MTLTexture> texture, render::RowOrigin origin) {
  ImageFormat format;
  switch (texture.pixelFormat) {
    case MTLPixelFormatRGBA8Unorm: format = ImageFormat::Rgba8Unorm; break;
    case MTLPixelFormatBGRA8Unorm: format = ImageFormat::Bgra8Unorm; break;
    default: throw std::invalid_argument("Expected RGBA8Unorm or BGRA8Unorm input");
  }
  return {RetainTexture(texture, format, origin), AlphaMode::Premultiplied};
}

render::DataSurface RetainMetalDataTexture(id<MTLTexture> texture, render::RowOrigin origin) {
  if (texture.pixelFormat != MTLPixelFormatR8Unorm) throw std::invalid_argument("Expected R8Unorm mask input");
  return RetainTexture(texture, ImageFormat::R8Unorm, origin);
}

id<MTLTexture> GetMetalTexture(const render::RenderSurface& surface) {
  return TextureFromSurface(surface.pixels);
}

} // namespace huxerui::avkit::macos
