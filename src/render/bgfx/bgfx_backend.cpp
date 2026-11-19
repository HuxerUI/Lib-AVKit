#include <huxerui/avkit/bgfx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

#include "texture_bridge.h"
#include "support/geometry.h"
#include "avkit_fs_composite.h"
#include "avkit_fs_masked_blend.h"
#include "avkit_vs_composite.h"

namespace huxerui::avkit::render {

namespace {

static_assert(BGFX_CONFIG_MULTITHREADED == 0, "The bgfx compositor requires inline submission");

constexpr std::uint64_t kSamplerFlags =
    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_POINT;
constexpr std::uint32_t kCoverageSamplerFlags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
const detail::CompositeShaders kCompositeShaders{avkit_vs_composite, avkit_fs_composite, avkit_fs_masked_blend};
std::atomic_flag bgfx_in_use = ATOMIC_FLAG_INIT;

class BgfxContext {
public:
  explicit BgfxContext(const bgfx::Init& init) {
    if (bgfx_in_use.test_and_set()) throw std::logic_error("An AVKit bgfx backend is already active");
    if (!bgfx::init(init)) {
      bgfx_in_use.clear();
      throw std::runtime_error("Could not initialize the bgfx renderer selected for this platform");
    }
  }

  ~BgfxContext() {
    bgfx::shutdown();
    bgfx_in_use.clear();
  }

  BgfxContext(const BgfxContext&) = delete;
  BgfxContext& operator=(const BgfxContext&) = delete;
};

bgfx::TextureFormat::Enum BgfxFormat(PixelFormat format) {
  switch (format) {
    case PixelFormat::Rgba8Unorm: return bgfx::TextureFormat::RGBA8;
    case PixelFormat::Bgra8Unorm: return bgfx::TextureFormat::BGRA8;
    case PixelFormat::R8Unorm: return bgfx::TextureFormat::R8;
  }
  throw std::invalid_argument("Unsupported bgfx texture format");
}

bool IsColor(PixelFormat format) {
  return format == PixelFormat::Rgba8Unorm || format == PixelFormat::Bgra8Unorm;
}

std::size_t RowBytes(ImageDesc image) {
  const auto width = std::size_t(image.size.width);
  return image.format == PixelFormat::R8Unorm ? width : width * 4;
}

bgfx::TextureHandle CreateTexture(ImageDesc image, std::uint64_t flags,
                                  const bgfx::Memory* memory = nullptr, std::uint64_t external = 0) {
  return bgfx::createTexture2D(static_cast<std::uint16_t>(image.size.width),
                              static_cast<std::uint16_t>(image.size.height), false, 1,
                              BgfxFormat(image.format), flags | kSamplerFlags, memory, external);
}

template<class Handle> void Destroy(Handle handle) {
  if (bgfx::isValid(handle)) bgfx::destroy(handle);
}

struct Vertex {
  float x, y, z, u, v;
};

bgfx::VertexBufferHandle CreateQuad() {
  const Vertex vertices[] = {
      {-1.0F, +1.0F, 0.0F, 0.0F, 0.0F},
      {-1.0F, -1.0F, 0.0F, 0.0F, 1.0F},
      {+1.0F, +1.0F, 0.0F, 1.0F, 0.0F},
      {+1.0F, -1.0F, 0.0F, 1.0F, 1.0F},
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
  std::shared_ptr<const SurfaceStorage> storage;
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

class BgfxBackend final : public RenderBackend {
public:
  BgfxBackend(ImageDesc image, const bgfx::Init& init, std::unique_ptr<detail::BgfxTextureBridge> bridge)
      : image_(image), bridge_(std::move(bridge)), context_(init) {}

  void Initialize(const detail::CompositeShaders& shaders) {
    if (!FitsDevice(image_.size)) {
      throw std::invalid_argument("The output dimensions exceed the bgfx texture limit");
    }

    const auto load = [](std::span<const std::uint8_t> shader) {
      return bgfx::createShader(bgfx::copy(shader.data(), static_cast<std::uint32_t>(shader.size())));
    };
    auto vertex = load(shaders.vertex);
    auto fragment = load(shaders.fragment);
    auto masked_fragment = load(shaders.masked_fragment);
    if (!bgfx::isValid(vertex) || !bgfx::isValid(fragment) || !bgfx::isValid(masked_fragment)) {
      Destroy(vertex);
      Destroy(fragment);
      Destroy(masked_fragment);
      throw std::runtime_error("Could not load the bgfx composite shaders");
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
    input_alpha_ = bgfx::createUniform("u_input_alpha", bgfx::UniformType::Vec4);
    mask_parameters_ = bgfx::createUniform("u_mask_parameters", bgfx::UniformType::Vec4);
    mask_region_ = bgfx::createUniform("u_mask_region", bgfx::UniformType::Vec4);
    sampling_ = bgfx::createUniform("u_sampling", bgfx::UniformType::Vec4, 6);
    destination_ = bgfx::createUniform("u_destination", bgfx::UniformType::Vec4);
    off_diagonal_ = bgfx::createUniform("u_off_diagonal", bgfx::UniformType::Vec4);
    top_left_ = CreateQuad();
    if (!bgfx::isValid(program_) || !bgfx::isValid(masked_program_) || !bgfx::isValid(sampler_) ||
        !bgfx::isValid(incoming_sampler_) || !bgfx::isValid(mask_sampler_) || !bgfx::isValid(input_flips_) ||
        !bgfx::isValid(input_alpha_) || !bgfx::isValid(mask_parameters_) || !bgfx::isValid(mask_region_) || !bgfx::isValid(destination_) ||
        !bgfx::isValid(off_diagonal_) || !bgfx::isValid(top_left_) || !bgfx::isValid(sampling_)) {
      throw std::runtime_error("Could not create the composite program or quad");
    }

    const auto format = BgfxFormat(image_.format);
    if (!bgfx::isTextureValid(0, false, 1, format, BGFX_TEXTURE_RT)) {
      throw std::invalid_argument("The renderer does not support the composite target format");
    }
    if (!bridge_) {
      completion_ = CreateReadback({1, 1});
      if (!bgfx::isValid(completion_)) throw std::runtime_error("Could not allocate the completion texture");
    }
    for (auto& target : targets_) {
      target.storage = bridge_ ? bridge_->CreateOutput(image_) : std::make_shared<SurfaceStorage>();
      const auto handle = bridge_ ? bridge_->GetTexture({image_, TargetOrigin(), target.storage}) : 0;
      if (bridge_ && handle == 0) throw std::runtime_error("Could not import the platform render target");
      target.texture = CreateTexture(image_, BGFX_TEXTURE_RT, nullptr, handle);
      if (!bgfx::isValid(target.texture)) throw std::runtime_error("Could not allocate a bgfx render target");
      target.framebuffer = bgfx::createFrameBuffer(1, &target.texture, false);
      if (!bgfx::isValid(target.framebuffer)) throw std::runtime_error("Could not create the offscreen framebuffer");
    }
  }

  ~BgfxBackend() override {
    for (std::size_t index = 0; index <= subcomposition_targets_.size(); ++index)
      bgfx::resetView(static_cast<bgfx::ViewId>(index));
    for (const auto& target : targets_) DestroyTarget(target);
    for (const auto& target : subcomposition_targets_) DestroyTarget(target);
    Destroy(completion_);
    Destroy(top_left_);
    Destroy(sampler_);
    Destroy(incoming_sampler_);
    Destroy(mask_sampler_);
    Destroy(input_flips_);
    Destroy(input_alpha_);
    Destroy(mask_parameters_);
    Destroy(mask_region_);
    Destroy(sampling_);
    Destroy(destination_);
    Destroy(off_diagonal_);
    Destroy(masked_program_);
    Destroy(program_);
    if (bridge_) {
      bgfx::frame(BGFX_FRAME_FLUSH);
      bridge_->ReleaseOutputResources();
    }
  }

  SubmitResult TrySubmit(const RenderRequest& request) override {
    if (job_) return SubmitResult::Success(std::nullopt);
    if (!ValidPlan(request)) return SubmitResult::Failure({RenderErrorCode::InvalidPlan, "Invalid composite passes or inputs"});
    for (const auto& input : request.inputs) {
      if (!IsColor(input.pixels.image.format) || !ValidInput(input.pixels)) {
        return SubmitResult::Failure({RenderErrorCode::InvalidFrame, "Invalid input surface or texture device"});
      }
    }
    for (const auto& input : request.data_inputs) {
      if (input.image.format != PixelFormat::R8Unorm || !ValidInput(input))
        return SubmitResult::Failure({RenderErrorCode::InvalidFrame, "Expected an R8Unorm coverage surface"});
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
    if (job_->failed || bridge_) {
      bgfx::frame(BGFX_FRAME_FLUSH);
      if (bridge_) bridge_->WaitForCompletion();
    } else {
      // A one-pixel readback fences the offscreen writes without downloading the output image.
      std::array<std::byte, 4> pixel{};
      ReadTexture(job_->target->texture, completion_, {1, 1}, pixel.data(),
                  static_cast<bgfx::ViewId>(job_->request.plan->subcompositions.size() + 1));
    }
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
    ColorSurface output{{image_, TargetOrigin(), job_->target->storage}, AlphaMode::Premultiplied};
    job_.reset();
    return TakeResult::Success(std::move(output));
  }

  void Discard(RenderTicket ticket) override {
    if (job_ && job_->ticket == ticket) job_.reset();
  }

  detail::ImportedTexture Acquire(const ExternalTexture& texture) {
    if (!bridge_) {
      return detail::ImportedTexture::Failure({RenderErrorCode::Unsupported, "No external GPU texture bridge is available"});
    }
    auto input = bridge_->Acquire(texture);
    if (input.Succeeded() && !ValidInput(input.Value().pixels)) {
      return detail::ImportedTexture::Failure({RenderErrorCode::Unsupported, "The GPU image cannot be sampled by this renderer"});
    }
    return input;
  }

  detail::PublishedTexture Publish(const ColorSurface& surface, media::MediaTimestamp pts) {
    if (!FindTarget(surface.pixels)) {
      return detail::PublishedTexture::Failure({RenderErrorCode::InvalidFrame, "Expected a completed output lease"});
    }
    if (bridge_) return bridge_->Publish(surface, pts);
    return detail::PublishedTexture::Failure({RenderErrorCode::Unsupported, "No external GPU texture bridge is available"});
  }

  detail::EncoderPicture Encode(const ColorSurface& surface, const media::VideoEncoderConfig& config,
                                media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration) {
    if (!FindTarget(surface.pixels)) {
      return detail::EncoderPicture::Failure({RenderErrorCode::InvalidFrame, "Expected a completed output lease"});
    }
    if (bridge_) return bridge_->Encode(surface, config, pts, duration);
    return detail::EncoderPicture::Failure({RenderErrorCode::Unsupported, "No external GPU texture bridge is available"});
  }

  Result<BufferReference, RenderError> ReadPixels(const ColorSurface& surface) {
    using ResultType = Result<BufferReference, RenderError>;
    const auto* target = FindTarget(surface.pixels);
    if (!target) return ResultType::Failure({RenderErrorCode::InvalidFrame, "Expected a completed output lease"});
    const auto size = std::uint64_t(image_.size.width) * image_.size.height * 4;
    if (size > std::numeric_limits<std::size_t>::max()) {
      return ResultType::Failure({RenderErrorCode::OutOfMemory, "Readback size exceeds addressable memory"});
    }
    auto pixels = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(size));
    const auto staging = CreateReadback(image_.size);
    if (!bgfx::isValid(staging)) {
      return ResultType::Failure({RenderErrorCode::Unsupported, "Could not allocate a readback texture"});
    }
    ReadTexture(target->texture, staging, image_.size, pixels->data(), 0);
    Destroy(staging);
    return ResultType::Success(BufferReference(*pixels, pixels));
  }

private:
  RowOrigin TargetOrigin() const {
    return bgfx::getCaps()->originBottomLeft ? RowOrigin::BottomLeft : RowOrigin::TopLeft;
  }

  const Target* FindTarget(const ImageSurface& surface) const {
    if (surface.image != image_ || surface.origin != TargetOrigin() || !surface.storage) return nullptr;
    for (const auto& target : targets_) {
      if (target.storage == surface.storage) return &target;
    }
    return nullptr;
  }

  bgfx::TextureHandle CreateReadback(PixelSize size) const {
    const ImageDesc image{size, image_.format};
    constexpr auto flags = BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK;
    if (!bgfx::isTextureValid(0, false, 1, BgfxFormat(image.format), flags)) return BGFX_INVALID_HANDLE;
    return CreateTexture(image, flags);
  }

  static void ReadTexture(bgfx::TextureHandle source, bgfx::TextureHandle staging, PixelSize size,
                          void* pixels, bgfx::ViewId view) {
    bgfx::TextureRegion region, source_region;
    region.init(staging);
    source_region.init(source, 0, 0, static_cast<std::uint16_t>(size.width),
                       static_cast<std::uint16_t>(size.height));
    bgfx::blit(view, region, source_region);
    bgfx::frame(BGFX_FRAME_FLUSH);
    const auto ready = bgfx::read(region, pixels);
    while (static_cast<std::int32_t>(bgfx::frame(BGFX_FRAME_FLUSH) - ready) < 0) {}
  }

  bool ValidPlan(const RenderRequest& request) const {
    if (!request.plan || request.plan->target != image_) return false;
    const auto& limits = bgfx::getCaps()->limits;
    const auto& plan = *request.plan;
    const auto passes = plan.subcompositions.size();
    const auto persistent = targets_.size() + std::max(subcomposition_targets_.size(), passes);
    const auto reserved_textures = persistent + (bridge_ ? 0 : 1);
    if (passes + (bridge_ ? 1 : 2) > limits.maxViews || persistent > limits.maxFrameBuffers ||
        reserved_textures > limits.maxTextures ||
        request.inputs.size() > limits.maxTextures - reserved_textures ||
        request.data_inputs.size() > limits.maxTextures - reserved_textures - request.inputs.size()) return false;

    auto remaining_draws = std::size_t(limits.maxDrawCalls);
    const auto masks = request.data_inputs.size();
    const auto validate = [&](std::span<const Draw> draws, std::size_t colors, bool final_pass) {
      const auto count = std::max<std::size_t>(1, draws.size());
      if (count > remaining_draws) return false;
      remaining_draws -= count;
      return std::ranges::all_of(draws, [&](const Draw& draw) {
        if (draw.input_slot >= colors || draw.destination.IsEmpty()) return false;
        const auto quad = Destination(draw);
        const auto finite = [](float value) { return std::isfinite(value); };
        if (!std::ranges::all_of(quad.destination, finite) || !std::ranges::all_of(quad.off_diagonal, finite)) return false;
        if (draw.source_sampling) {
          const auto& sampling = *draw.source_sampling;
          if (draw.masked_blend || !avkit::detail::IsFinite(sampling.draw_to_source_uv)) return false;
          if (sampling.background) {
            const auto& background = *sampling.background;
            const auto& color = background.color;
            if (background.mask_slot >= masks || !avkit::detail::IsFinite(background.draw_to_mask_uv) ||
                !avkit::detail::IsUnitRect(background.valid_uv) ||
                !std::ranges::all_of(std::array{color.red, color.green, color.blue, color.alpha},
                                     [](float value) { return value >= 0 && value <= 1; })) return false;
          }
        }
        if (!draw.masked_blend) return true;
        const auto& mask = *draw.masked_blend;
        const auto& region = mask.mask_region;
        return final_pass && mask.incoming_slot < colors && mask.mask_slot < masks &&
               std::isfinite(mask.feather) && mask.feather >= 0 && mask.feather <= 1 && !region.IsEmpty() &&
               std::isfinite(region.x) && std::isfinite(region.y) &&
               std::isfinite(region.width) && std::isfinite(region.height);
      });
    };
    for (const auto& pass : plan.subcompositions) {
      if (!validate(pass.draws, request.inputs.size(), false)) return false;
    }
    return validate(plan.draws, request.inputs.size() + passes, true);
  }

  bool FitsDevice(PixelSize size) const {
    const auto limit = bgfx::getCaps()->limits.maxTextureSize;
    return size.width != 0 && size.height != 0 && size.width <= limit && size.height <= limit &&
           size.width <= std::numeric_limits<std::uint16_t>::max() &&
           size.height <= std::numeric_limits<std::uint16_t>::max();
  }

  bool ValidInput(const ImageSurface& input) const {
    if (!FitsDevice(input.image.size)) return false;
    if (FindTarget(input)) return true;
    if (GetImagePixels(input))
      return bgfx::isTextureValid(0, false, 1, BgfxFormat(input.image.format), BGFX_TEXTURE_NONE);
    return bridge_ && bridge_->GetTexture(input) != 0;
  }

  bgfx::TextureHandle ImportInput(const ImageSurface& input) const {
    if (const auto pixels = GetImagePixels(input)) {
      const auto row_bytes = RowBytes(input.image);
      const auto size = std::uint64_t(row_bytes) * input.image.size.height;
      if (size > std::numeric_limits<std::uint32_t>::max()) return BGFX_INVALID_HANDLE;
      const auto* memory = bgfx::alloc(static_cast<std::uint32_t>(size));
      const auto bytes = pixels->bytes.AsBytes();
      for (std::size_t row = 0; row < input.image.size.height; ++row) {
        std::memcpy(memory->data + row * row_bytes, bytes.data() + row * pixels->row_bytes, row_bytes);
      }
      return CreateTexture(input.image, BGFX_TEXTURE_NONE, memory);
    }
    return CreateTexture(input.image, BGFX_TEXTURE_NONE, nullptr, bridge_->GetTexture(input));
  }

  bool EnsureSubcompositionTargets(std::size_t count) {
    subcomposition_targets_.reserve(count);
    while (subcomposition_targets_.size() < count) {
      Target target;
      target.texture = CreateTexture(image_, BGFX_TEXTURE_RT);
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
    std::vector<bgfx::TextureHandle> imported;
    imported.reserve(handles.capacity());
    const auto import = [&](const ImageSurface& input) {
      if (const auto* target = FindTarget(input)) {
        handles.push_back(target->texture);
        return true;
      }
      const auto handle = ImportInput(input);
      if (!bgfx::isValid(handle)) {
        for (auto texture : imported) Destroy(texture);
        return false;
      }
      handles.push_back(handle);
      imported.push_back(handle);
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
    for (auto handle : imported) Destroy(handle);
    return true;
  }

  void EncodePass(bgfx::ViewId view, bgfx::FrameBufferHandle target, std::span<const Draw> draws,
                  const RenderRequest& request, std::span<const bgfx::TextureHandle> handles) {
    const auto external_colors = request.inputs.size();
    const auto color = [&](std::uint32_t slot) {
      return slot < external_colors ? handles[slot] : subcomposition_targets_[slot - external_colors].texture;
    };
    const auto origin = [&](std::uint32_t slot) {
      return slot < external_colors ? request.inputs[slot].pixels.origin : TargetOrigin();
    };
    const auto alpha = [&](std::uint32_t slot) {
      const auto mode = slot < external_colors ? request.inputs[slot].alpha : AlphaMode::Premultiplied;
      if (mode == AlphaMode::Opaque) return 1.0F;
      return mode == AlphaMode::Straight ? 2.0F : 0.0F;
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
      const std::array modes{alpha(draw.input_slot), draw.masked_blend ? alpha(draw.masked_blend->incoming_slot) : 0.0F,
                             0.0F, 0.0F};
      bgfx::setUniform(input_alpha_, modes.data());
      auto program = program_;
      const auto flip = [](RowOrigin value) { return value == RowOrigin::BottomLeft ? 1.0F : 0.0F; };
      if (draw.masked_blend) {
        const auto& blend = *draw.masked_blend;
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
      } else {
        const auto sampling = draw.source_sampling.value_or(SourceSampling{});
        std::array<std::array<float, 4>, 6> uniforms{};
        const auto put = [&](std::size_t index, const Transform2D& uv) {
          uniforms[index] = {uv.m11, uv.m21, uv.translate_x, 0};
          uniforms[index + 1] = {uv.m12, uv.m22, uv.translate_y, 0};
        };
        put(0, sampling.draw_to_source_uv);
        std::array flips{flip(origin(draw.input_slot)), 0.0F, 0.0F, 0.0F};
        auto coverage = color(draw.input_slot);
        if (sampling.background) {
          const auto& background = *sampling.background;
          uniforms[0][3] = 1;
          put(2, background.draw_to_mask_uv);
          const auto& mask = request.data_inputs[background.mask_slot];
          uniforms[2][3] = 0.5F / mask.image.size.width;
          uniforms[3][3] = 0.5F / mask.image.size.height;
          const auto& valid = background.valid_uv;
          uniforms[4] = {valid.x, valid.y, valid.width, valid.height};
          const auto& bg = background.color;
          uniforms[5] = {bg.red * bg.alpha, bg.green * bg.alpha, bg.blue * bg.alpha, bg.alpha};
          flips[2] = flip(mask.origin);
          coverage = handles[external_colors + background.mask_slot];
        }
        bgfx::setUniform(input_flips_, flips.data());
        bgfx::setUniform(sampling_, uniforms.data(), uniforms.size());
        bgfx::setTexture(2, mask_sampler_, coverage, sampling.background ? kCoverageSamplerFlags : UINT32_MAX);
      }
      bgfx::setVertexBuffer(0, top_left_);
      std::uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_TRISTRIP;
      if (draw.blend == Blend::PremultipliedOver) {
        state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
      }
      bgfx::setState(state);
      bgfx::submit(view, program);
    }
  }

  ImageDesc image_;
  std::unique_ptr<detail::BgfxTextureBridge> bridge_;
  bgfx::TextureHandle completion_ = BGFX_INVALID_HANDLE;
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
  bgfx::UniformHandle input_alpha_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle mask_parameters_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle mask_region_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle sampling_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle destination_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle off_diagonal_ = BGFX_INVALID_HANDLE;
  bgfx::VertexBufferHandle top_left_ = BGFX_INVALID_HANDLE;
  // Shutdown drains bgfx before the leases retaining its external textures are destroyed.
  BgfxContext context_;
};

} // namespace

std::unique_ptr<RenderBackend> detail::CreateBgfxBackend(ImageDesc target, const bgfx::Init& init,
                                                        std::unique_ptr<BgfxTextureBridge> bridge,
                                                        const CompositeShaders* shaders) {
  if (!IsColor(target.format)) throw std::invalid_argument("The composite target must be RGBA8 or BGRA8");
  auto backend = std::make_unique<BgfxBackend>(target, init, std::move(bridge));
  backend->Initialize(shaders ? *shaders : kCompositeShaders);
  return backend;
}

std::unique_ptr<RenderBackend> CreateBgfxBackend(ImageDesc target, BgfxPlatformHandles handles) {
  bgfx::Init init;
  init.type = bgfx::RendererType::AVKIT_BGFX_RENDERER;
  init.fallback = false;
  init.swapChain.nwh = handles.window;
  init.swapChain.ndt = handles.display;
  init.swapChain.width = handles.window ? target.size.width : 0;
  init.swapChain.height = handles.window ? target.size.height : 0;
  init.swapChain.maxFrameLatency = 1;
  return detail::CreatePlatformBgfxBackend(target, init);
}

ImageSurface RetainBgfxPixels(ImageDesc image, BufferReference bytes, std::size_t row_bytes, RowOrigin origin) {
  BgfxFormat(image.format);
  const auto [width, height] = image.size;
  if (width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX) {
    throw std::invalid_argument("Invalid bgfx upload dimensions");
  }
  return RetainImagePixels(image, {std::move(bytes), row_bytes}, origin);
}

Result<BufferReference, RenderError> ReadBgfxPixels(RenderBackend& backend, const ColorSurface& surface) {
  if (auto* bgfx = dynamic_cast<BgfxBackend*>(&backend)) return bgfx->ReadPixels(surface);
  return Result<BufferReference, RenderError>::Failure({RenderErrorCode::Unsupported, "Expected a bgfx backend"});
}

detail::ImportedTexture detail::AcquireBgfxTexture(RenderBackend& backend, const ExternalTexture& texture) {
  if (auto* bgfx = dynamic_cast<BgfxBackend*>(&backend)) return bgfx->Acquire(texture);
  return ImportedTexture::Failure({RenderErrorCode::Unsupported, "Expected a bgfx backend"});
}

detail::PublishedTexture detail::PublishBgfxTexture(RenderBackend& backend, const ColorSurface& surface,
                                                   media::MediaTimestamp pts) {
  if (auto* bgfx = dynamic_cast<BgfxBackend*>(&backend)) return bgfx->Publish(surface, pts);
  return PublishedTexture::Failure({RenderErrorCode::Unsupported, "Expected a bgfx backend"});
}

detail::EncoderPicture detail::EncodeBgfxTexture(RenderBackend& backend, const ColorSurface& surface,
                                                 const media::VideoEncoderConfig& config, media::MediaTimestamp pts,
                                                 std::optional<media::MediaTimestamp> duration) {
  if (auto* bgfx = dynamic_cast<BgfxBackend*>(&backend)) return bgfx->Encode(surface, config, pts, duration);
  return EncoderPicture::Failure({RenderErrorCode::Unsupported, "Expected a bgfx backend"});
}

} // namespace huxerui::avkit::render
