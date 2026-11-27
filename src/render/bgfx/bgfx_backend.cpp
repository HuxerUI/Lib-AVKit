#include "render/bgfx/backend.h"

#include <map>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

#include "render_lease.h"
#include "render/render_passes.h"
#include "texture_bridge.h"
#include "support/geometry.h"
#include "avkit_fs_composite.h"
#include "avkit_fs_masked_blend.h"
#include "avkit_vs_composite.h"

namespace huxerui::avkit::render {

namespace {

using detail::Draw;
using detail::PassImageSlot;
using detail::PassRequest;
using detail::RenderPasses;
using detail::TextureAccess;
using detail::TextureBacking;
using detail::IsColorImageFormat;

static_assert(BGFX_CONFIG_MULTITHREADED == 0, "The bgfx compositor requires inline submission");

constexpr std::uint64_t kSamplerFlags =
    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_POINT;
constexpr std::uint32_t kCoverageSamplerFlags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
const detail::CompositeShaders kCompositeShaders{avkit_vs_composite, avkit_fs_composite, avkit_fs_masked_blend};
class BgfxContext {
public:
  explicit BgfxContext(const bgfx::Init& init) {
    if (!bgfx::init(init)) {
      throw std::runtime_error("Could not initialize the bgfx renderer selected for this platform");
    }
  }

  ~BgfxContext() { bgfx::shutdown(); }

  BgfxContext(const BgfxContext&) = delete;

  BgfxContext& operator=(const BgfxContext&) = delete;
};

bgfx::TextureFormat::Enum BgfxFormat(PixelFormat format) {
  switch (format) {
    case PixelFormat::Rgba8Unorm: return bgfx::TextureFormat::RGBA8;
    case PixelFormat::Bgra8Unorm: return bgfx::TextureFormat::BGRA8;
    case PixelFormat::R8Unorm: return bgfx::TextureFormat::R8;
    case PixelFormat::R32Float: return bgfx::TextureFormat::R32F;
  }
  throw std::invalid_argument("Unsupported bgfx texture format");
}

std::size_t RowBytes(ImageInfo image) {
  const auto width = std::size_t(image.size.width);
  return image.format == PixelFormat::R8Unorm ? width : width * 4;
}

bgfx::TextureHandle CreateTexture(ImageInfo image, std::uint64_t flags,
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

struct ClipQuad {
  std::array<float, 4> destination;
  std::array<float, 4> off_diagonal{};
};

ClipQuad Destination(const Draw& draw) {
  const auto& map = draw.local_to_target;
  const auto x = float(2.0 * map.translate_x + map.m11 + map.m21 - 1.0);
  const auto y = float(1.0 - 2.0 * map.translate_y - map.m12 - map.m22);
  return {{x, y, map.m11, map.m22}, {-map.m21, -map.m12, 0, 0}};
}

struct Target {
  std::shared_ptr<const TextureBacking> storage;
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
};

enum class Transfer : std::uint8_t { Identity, Bt709ToSrgb, SrgbToBt709 };

struct UploadedTexture final : TextureBacking {
  explicit UploadedTexture(bgfx::TextureHandle handle) : handle(handle) {}

  bgfx::TextureHandle handle;
};

struct TransferTextureStorage final : TextureBacking {
  TransferTextureStorage(std::shared_ptr<const TextureBacking> texture, Transfer transfer)
      : texture(std::move(texture)), transfer(transfer) {}
  std::shared_ptr<const TextureBacking> texture;
  Transfer transfer;
};

void DestroyTarget(const Target& target) {
  Destroy(target.framebuffer);
  Destroy(target.texture);
}

struct Job {
  RenderTicket ticket;
  PassRequest request;
  const Target* target;
  bool ready{};
  bool failed{};
};

class BgfxBackend final : public RenderBackend {
public:
  BgfxBackend(ImageInfo image, const bgfx::Init& init, detail::BgfxRenderLease lease,
              std::unique_ptr<detail::BgfxTextureBridge> bridge, std::size_t output_slots)
      : lease_(std::move(lease)), image_(image), bridge_(std::move(bridge)), targets_(output_slots), context_(init) {}

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
    depth_sampler_ = bgfx::createUniform("s_depth", bgfx::UniformType::Sampler);
    input_flips_ = bgfx::createUniform("u_input_flips", bgfx::UniformType::Vec4);
    input_color_ = bgfx::createUniform("u_input_color", bgfx::UniformType::Vec4);
    mask_parameters_ = bgfx::createUniform("u_mask_parameters", bgfx::UniformType::Vec4);
    mask_region_ = bgfx::createUniform("u_mask_region", bgfx::UniformType::Vec4);
    sampling_ = bgfx::createUniform("u_sampling", bgfx::UniformType::Vec4, 6);
    occlusion_ = bgfx::createUniform("u_occlusion", bgfx::UniformType::Vec4, 5);
    destination_ = bgfx::createUniform("u_destination", bgfx::UniformType::Vec4);
    off_diagonal_ = bgfx::createUniform("u_off_diagonal", bgfx::UniformType::Vec4);
    top_left_ = CreateQuad();
    if (!bgfx::isValid(program_) || !bgfx::isValid(masked_program_) || !bgfx::isValid(sampler_) ||
        !bgfx::isValid(incoming_sampler_) || !bgfx::isValid(mask_sampler_) || !bgfx::isValid(input_flips_) ||
        !bgfx::isValid(input_color_) || !bgfx::isValid(mask_parameters_) || !bgfx::isValid(mask_region_) || !bgfx::isValid(destination_) ||
        !bgfx::isValid(off_diagonal_) || !bgfx::isValid(top_left_) || !bgfx::isValid(sampling_) ||
        !bgfx::isValid(depth_sampler_) || !bgfx::isValid(occlusion_)) {
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
      target.storage = bridge_ ? bridge_->CreateOutput(image_) : std::make_shared<TextureBacking>();
      const auto handle = bridge_
          ? bridge_->GetTexture(TextureAccess::Make(image_, TargetOrigin(), target.storage)) : 0;
      if (bridge_ && handle == 0) throw std::runtime_error("Could not import the platform render target");
      target.texture = CreateTexture(image_, BGFX_TEXTURE_RT, nullptr, handle);
      if (!bgfx::isValid(target.texture)) throw std::runtime_error("Could not allocate a bgfx render target");
      target.framebuffer = bgfx::createFrameBuffer(1, &target.texture, false);
      if (!bgfx::isValid(target.framebuffer)) throw std::runtime_error("Could not create the offscreen framebuffer");
    }
  }

  ~BgfxBackend() override {
    for (std::size_t index = 0; index <= offscreen_targets_.size(); ++index)
      bgfx::resetView(static_cast<bgfx::ViewId>(index));
    for (const auto& [index, storage] : uploads_) Destroy(bgfx::TextureHandle{index});
    for (const auto& target : targets_) DestroyTarget(target);
    for (const auto& target : offscreen_targets_) DestroyTarget(target);
    Destroy(completion_);
    Destroy(top_left_);
    Destroy(sampler_);
    Destroy(incoming_sampler_);
    Destroy(mask_sampler_);
    Destroy(depth_sampler_);
    Destroy(input_flips_);
    Destroy(input_color_);
    Destroy(mask_parameters_);
    Destroy(mask_region_);
    Destroy(sampling_);
    Destroy(occlusion_);
    Destroy(destination_);
    Destroy(off_diagonal_);
    Destroy(masked_program_);
    Destroy(program_);
    if (bridge_) {
      bgfx::frame(BGFX_FRAME_FLUSH);
      bridge_->ReleaseOutputResources();
    }
  }

  SubmitResult SubmitPasses(const PassRequest& request) {
    if (job_) return SubmitResult::Success(std::nullopt);
    RetireUploads();
    if (!ValidPlan(request)) return SubmitResult::Failure({RenderErrorCode::InvalidPlan, "Invalid composite passes or inputs"});
    for (const auto& input : request.image_inputs) {
      if (!input.Alpha() || !CanSample(input)) {
        return SubmitResult::Failure({RenderErrorCode::InvalidFrame, "Invalid input surface or texture device"});
      }
    }
    for (const auto& input : request.data_inputs) {
      const auto format = input.Info().format;
      if (format != PixelFormat::R8Unorm && format != PixelFormat::R32Float)
        return SubmitResult::Failure({RenderErrorCode::InvalidFrame, "Expected a coverage or metric depth surface"});
      if (format == PixelFormat::R32Float && !SupportsDepthTexture())
        return SubmitResult::Failure({RenderErrorCode::Unsupported, "The renderer cannot sample an R32Float texture"});
      if (!CanSample(input))
        return SubmitResult::Failure({RenderErrorCode::InvalidFrame, "Invalid data surface or texture device"});
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
                  static_cast<bgfx::ViewId>(job_->request.plan->offscreen_passes.size() + 1));
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
    auto output = TextureAccess::Make(image_, TargetOrigin(), job_->target->storage);
    job_.reset();
    return TakeResult::Success(std::move(output));
  }

  detail::ImportedTexture ImportTexture(const ExternalTexture& texture, const media::VideoFormat* decoded_format) override {
    if (!bridge_) {
      return detail::ImportedTexture::Failure({RenderErrorCode::Unsupported, "No external GPU texture bridge is available"});
    }
    auto input = bridge_->Acquire(texture);
    if (!input.Succeeded()) return input;
    auto& image = input.Value();
    if (!CanSample(image)) {
      return detail::ImportedTexture::Failure({RenderErrorCode::Unsupported, "The GPU image cannot be sampled by this renderer"});
    }
    if (decoded_format && bridge_->SamplesBt709Signal(*decoded_format)) {
      auto storage = std::make_shared<TransferTextureStorage>(TextureAccess::Backing(image), Transfer::Bt709ToSrgb);
      image = TextureAccess::Make(image.Info(), image.Origin(), std::move(storage), image.Alpha());
    }
    return input;
  }

  Result<Texture, RenderError> UploadTexture(const ImagePixels& pixels) override {
    using R = Result<Texture, RenderError>;
    if (!pixels.Validate() || !FitsDevice(pixels.info.size))
      return R::Failure({RenderErrorCode::InvalidFrame, "Invalid pixel rows or texture dimensions"});
    if (!bgfx::isTextureValid(0, false, 1, BgfxFormat(pixels.info.format), BGFX_TEXTURE_NONE))
      return R::Failure({RenderErrorCode::Unsupported, "Unsupported texture upload format"});
    RetireUploads();
    const auto row_bytes = RowBytes(pixels.info);
    const auto size = std::uint64_t(row_bytes) * pixels.info.size.height;
    if (size > std::numeric_limits<std::uint32_t>::max())
      return R::Failure({RenderErrorCode::OutOfMemory, "Texture upload exceeds the bgfx buffer limit"});
    const auto* memory = bgfx::alloc(static_cast<std::uint32_t>(size));
    const auto bytes = pixels.bytes.AsBytes();
    for (std::size_t row = 0; row < pixels.info.size.height; ++row)
      std::memcpy(memory->data + row * row_bytes, bytes.data() + row * pixels.row_bytes, row_bytes);
    const auto handle = CreateTexture(pixels.info, BGFX_TEXTURE_NONE, memory);
    if (!bgfx::isValid(handle))
      return R::Failure({RenderErrorCode::OutOfMemory, "Could not allocate the uploaded texture"});
    try {
      auto storage = std::make_shared<UploadedTexture>(handle);
      uploads_.emplace(handle.idx, storage);
      return R::Success(TextureAccess::Make(pixels.info, pixels.origin, std::move(storage), pixels.alpha));
    } catch (...) {
      Destroy(handle);
      throw;
    }
  }

  bool HasCompletedOutput(const Texture& surface) const { return FindTarget(surface) != nullptr; }


  Texture CreateInteropTexture(ImageInfo image) {
    if (!bridge_ || image.format != image_.format) throw std::runtime_error("No compatible bgfx interop texture bridge");
    auto storage = bridge_->CreateOutput(image);
    if (!storage) throw std::runtime_error("Cannot create a bgfx interop texture");
    return TextureAccess::Make(image, TargetOrigin(), std::move(storage));
  }

  std::uint64_t InteropTexture(const Texture& image) const {
    return bridge_ ? bridge_->GetTexture(image) : 0;
  }

  detail::EncoderPicture Encode(const Texture& surface, const media::VideoEncoderConfig& config,
                                media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration) {
    if (!FindTarget(surface)) {
      return detail::EncoderPicture::Failure({RenderErrorCode::InvalidFrame, "Expected a completed output lease"});
    }
    using R = detail::EncoderPicture;
    if (!bridge_) return R::Failure({RenderErrorCode::Unsupported, "No external GPU texture bridge is available"});
    if (!bridge_->ExpectsBt709Signal(config)) return bridge_->Encode(surface, config, pts, duration);

    if (!encoder_transfer_) {
      encoder_transfer_ = std::make_shared<RenderPasses>(RenderPasses{image_, {Draw{PassImageSlot::Input(0), Blend::Replace}}, {}});
    }
    auto storage = std::make_shared<TransferTextureStorage>(TextureAccess::Backing(surface), Transfer::SrgbToBt709);
    auto input = TextureAccess::Make(surface.Info(), surface.Origin(), std::move(storage), AlphaMode::Opaque);
    const auto submitted = SubmitPasses({encoder_transfer_, {std::move(input)}, {}});
    if (!submitted.Succeeded() || !submitted.Value())
      return R::Failure({RenderErrorCode::Unsupported, "Cannot submit the GPU encoder color conversion"});
    Pump();
    auto converted = TryTake(*submitted.Value());
    if (!converted.Succeeded() || !converted.Value())
      return R::Failure({RenderErrorCode::Unsupported, "Cannot complete the GPU encoder color conversion"});
    return bridge_->Encode(*converted.Value(), config, pts, duration);
  }

  Result<BufferReference, RenderError> ReadPixels(const Texture& surface) {
    using ResultType = Result<BufferReference, RenderError>;
    const auto* target = FindTarget(surface);
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

  const Target* FindTarget(const Texture& surface) const {
    if (!surface || surface.Info() != image_ || surface.Origin() != TargetOrigin()) return nullptr;
    const auto& backing = TextureAccess::Backing(surface);
    const auto* transfer = dynamic_cast<const TransferTextureStorage*>(backing.get());
    const auto& storage = transfer ? transfer->texture : backing;
    for (const auto& target : targets_) {
      if (target.storage == storage) return &target;
    }
    return nullptr;
  }

  bgfx::TextureHandle CreateReadback(PixelSize size) const {
    const ImageInfo image{size, image_.format};
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

  bool ValidPlan(const PassRequest& request) const {
    if (!request.plan || request.plan->target != image_) return false;
    const auto& limits = bgfx::getCaps()->limits;
    const auto& plan = *request.plan;
    const auto passes = plan.offscreen_passes.size();
    const auto persistent = targets_.size() + std::max(offscreen_targets_.size(), passes);
    const auto needs_bridge = [&](const Texture& input) { return !FindTarget(input) && !FindUpload(input); };
    const auto native_inputs = std::ranges::count_if(request.image_inputs, needs_bridge) +
                               std::ranges::count_if(request.data_inputs, needs_bridge);
    const auto reserved_textures = persistent + uploads_.size() + (bridge_ ? 0 : 1);
    if (passes + (bridge_ ? 1 : 2) > limits.maxViews || persistent > limits.maxFrameBuffers ||
        reserved_textures > limits.maxTextures || std::size_t(native_inputs) > limits.maxTextures - reserved_textures)
      return false;

    auto remaining_draws = std::size_t(limits.maxDrawCalls);
    const auto is_data = [&](std::uint32_t slot, PixelFormat format) {
      return slot < request.data_inputs.size() && request.data_inputs[slot].Info().format == format;
    };
    const auto finite_rect = [](const Rect& rect) {
      return !rect.IsEmpty() && std::isfinite(rect.x) && std::isfinite(rect.y) &&
             std::isfinite(rect.width) && std::isfinite(rect.height);
    };
    const auto validate = [&](std::span<const Draw> draws, std::size_t prior_passes) {
      const auto count = std::max<std::size_t>(1, draws.size());
      if (count > remaining_draws) return false;
      remaining_draws -= count;
      const auto has_color = [&](PassImageSlot slot) {
        return slot.IsInput() ? slot.index < request.image_inputs.size() : slot.index < prior_passes;
      };
      return std::ranges::all_of(draws, [&](const Draw& draw) {
        const auto& matrix = draw.local_to_target;
        const auto determinant = double(matrix.m11) * matrix.m22 - double(matrix.m12) * matrix.m21;
        if (!has_color(draw.image_slot) || determinant == 0 || !(draw.opacity >= 0 && draw.opacity <= 1)) return false;
        const auto quad = Destination(draw);
        const auto finite = [](float value) { return std::isfinite(value); };
        if (!std::ranges::all_of(quad.destination, finite) || !std::ranges::all_of(quad.off_diagonal, finite)) return false;
        if (draw.source_sampling && (draw.masked_blend ||
            !avkit::detail::IsFinite(draw.source_sampling->draw_to_source_uv))) return false;
        if (draw.background) {
          const auto& background = *draw.background;
          const auto& color = background.color;
          if (draw.masked_blend || !is_data(background.mask_slot, PixelFormat::R8Unorm) ||
              !avkit::detail::IsFinite(background.draw_to_mask_uv) || !avkit::detail::IsUnitRect(background.valid_uv) ||
              !std::ranges::all_of(std::array{color.red, color.green, color.blue, color.alpha},
                                   [](float value) { return value >= 0 && value <= 1; })) return false;
        }
        if (draw.depth_occlusion) {
          const auto& depth = *draw.depth_occlusion;
          if (draw.masked_blend || draw.blend != Blend::PremultipliedOver ||
              !is_data(depth.depth_slot, PixelFormat::R32Float) || !avkit::detail::IsFinite(depth.target_to_depth_uv) ||
              !avkit::detail::IsUnitRect(depth.valid_uv) || !finite_rect(depth.target_region) ||
              !(depth.layer_depth_meters > 0 && std::isfinite(depth.layer_depth_meters))) return false;
        }
        if (!draw.masked_blend) return true;
        const auto& mask = *draw.masked_blend;
        const auto& region = mask.mask_region;
        return has_color(mask.incoming_image_slot) && is_data(mask.mask_slot, PixelFormat::R8Unorm) &&
               std::isfinite(mask.feather) && mask.feather >= 0 && mask.feather <= 1 && finite_rect(region);
      });
    };
    for (std::size_t index = 0; index < passes; ++index) {
      const auto& pass = plan.offscreen_passes[index];
      if (pass.pag || !validate(pass.draws, index)) return false;
    }
    return validate(plan.final_draws, passes);
  }

  bool FitsDevice(PixelSize size) const {
    const auto limit = bgfx::getCaps()->limits.maxTextureSize;
    return size.width != 0 && size.height != 0 && size.width <= limit && size.height <= limit &&
           size.width <= std::numeric_limits<std::uint16_t>::max() &&
           size.height <= std::numeric_limits<std::uint16_t>::max();
  }

  bool CanSample(const Texture& input) const override {
    if (!input || !FitsDevice(input.Info().size)) return false;
    if (input.Info().format == PixelFormat::R32Float && !SupportsDepthTexture()) return false;
    return FindTarget(input) || FindUpload(input) || (bridge_ && BridgeTexture(input) != 0);
  }

  bool SupportsDepthTexture() const {
    return bgfx::getCaps()->formats[BgfxFormat(PixelFormat::R32Float)] & BGFX_CAPS_FORMAT_TEXTURE_2D;
  }

  std::uint64_t BridgeTexture(const Texture& input) const {
    if (const auto* video = dynamic_cast<const TransferTextureStorage*>(TextureAccess::Backing(input).get())) {
      return bridge_->GetTexture(TextureAccess::Make(input.Info(), input.Origin(), video->texture, input.Alpha()));
    }
    return bridge_->GetTexture(input);
  }

  const UploadedTexture* FindUpload(const Texture& input) const {
    const auto* storage = dynamic_cast<const UploadedTexture*>(TextureAccess::Backing(input).get());
    if (!storage) return nullptr;
    const auto found = uploads_.find(storage->handle.idx);
    return found != uploads_.end() && found->second.lock().get() == storage ? storage : nullptr;
  }

  void RetireUploads() {
    for (auto it = uploads_.begin(); it != uploads_.end();) {
      if (!it->second.expired()) {
        ++it;
        continue;
      }
      Destroy(bgfx::TextureHandle{it->first});
      it = uploads_.erase(it);
    }
  }

  bool EnsureOffscreenTargets(std::size_t count) {
    offscreen_targets_.reserve(count);
    while (offscreen_targets_.size() < count) {
      Target target;
      target.texture = CreateTexture(image_, BGFX_TEXTURE_RT);
      if (!bgfx::isValid(target.texture)) return false;
      target.framebuffer = bgfx::createFrameBuffer(1, &target.texture, false);
      if (!bgfx::isValid(target.framebuffer)) {
        Destroy(target.texture);
        return false;
      }
      offscreen_targets_.push_back(std::move(target));
    }
    return true;
  }

  bool Encode(const Job& job) {
    const auto& plan = *job.request.plan;
    if (!EnsureOffscreenTargets(plan.offscreen_passes.size())) return false;
    std::vector<bgfx::TextureHandle> handles;
    handles.reserve(job.request.image_inputs.size() + job.request.data_inputs.size());
    std::vector<bgfx::TextureHandle> imported;
    imported.reserve(handles.capacity());
    const auto import = [&](const Texture& input) {
      if (const auto* target = FindTarget(input)) {
        handles.push_back(target->texture);
        return true;
      }
      if (const auto* uploaded = FindUpload(input)) {
        handles.push_back(uploaded->handle);
        return true;
      }
      const auto handle = CreateTexture(input.Info(), BGFX_TEXTURE_NONE, nullptr, BridgeTexture(input));
      if (!bgfx::isValid(handle)) {
        for (auto texture : imported) Destroy(texture);
        return false;
      }
      handles.push_back(handle);
      imported.push_back(handle);
      return true;
    };
    for (const auto& input : job.request.image_inputs) if (!import(input)) return false;
    for (const auto& input : job.request.data_inputs) if (!import(input)) return false;

    bgfx::ViewId view = 0;
    for (const auto& pass : plan.offscreen_passes) {
      EncodePass(view, offscreen_targets_[view].framebuffer, pass.draws, job.request, handles);
      ++view;
    }
    EncodePass(view, job.target->framebuffer, plan.final_draws, job.request, handles);
    for (auto handle : imported) Destroy(handle);
    return true;
  }

  void EncodePass(bgfx::ViewId view, bgfx::FrameBufferHandle target, std::span<const Draw> draws,
                  const PassRequest& request, std::span<const bgfx::TextureHandle> handles) {
    const auto image_input_count = request.image_inputs.size();
    const auto color = [&](PassImageSlot slot) {
      return slot.IsInput() ? handles[slot.index] : offscreen_targets_[slot.index].texture;
    };
    const auto origin = [&](PassImageSlot slot) {
      return slot.IsInput() ? request.image_inputs[slot.index].Origin() : TargetOrigin();
    };
    const auto alpha = [&](PassImageSlot slot) {
      const auto mode = slot.IsInput() ? *request.image_inputs[slot.index].Alpha() : AlphaMode::Premultiplied;
      if (mode == AlphaMode::Opaque) return 1.0F;
      return mode == AlphaMode::Straight ? 2.0F : 0.0F;
    };
    const auto transfer = [&](PassImageSlot slot) {
      if (!slot.IsInput()) return float(Transfer::Identity);
      const auto* storage =
          dynamic_cast<const TransferTextureStorage*>(TextureAccess::Backing(request.image_inputs[slot.index]).get());
      return float(storage ? storage->transfer : Transfer::Identity);
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
      bgfx::setTexture(0, sampler_, color(draw.image_slot));
      const std::array modes{alpha(draw.image_slot), draw.masked_blend ? alpha(draw.masked_blend->incoming_image_slot) : 0.0F,
                             transfer(draw.image_slot), draw.masked_blend ? transfer(draw.masked_blend->incoming_image_slot) : 0.0F};
      bgfx::setUniform(input_color_, modes.data());
      auto program = program_;
      const auto flip = [](RowOrigin value) { return value == RowOrigin::BottomLeft ? 1.0F : 0.0F; };
      if (draw.masked_blend) {
        const auto& blend = *draw.masked_blend;
        const std::array flips{flip(origin(draw.image_slot)), flip(origin(blend.incoming_image_slot)),
                               flip(request.data_inputs[blend.mask_slot].Origin()), draw.opacity};
        const std::array parameters{blend.feather, blend.inverted ? 1.0F : 0.0F, 0.0F, 0.0F};
        const auto& rect = blend.mask_region;
        const std::array region{rect.x, rect.y, rect.width, rect.height};
        bgfx::setUniform(input_flips_, flips.data());
        bgfx::setUniform(mask_parameters_, parameters.data());
        bgfx::setUniform(mask_region_, region.data());
        bgfx::setTexture(1, incoming_sampler_, color(blend.incoming_image_slot));
        bgfx::setTexture(2, mask_sampler_, handles[image_input_count + blend.mask_slot]);
        program = masked_program_;
      } else {
        const auto sampling = draw.source_sampling.value_or(SourceSampling{});
        std::array<std::array<float, 4>, 6> uniforms{};
        const auto put = [&](std::size_t index, const Affine2D& uv) {
          uniforms[index] = {uv.m11, uv.m21, uv.translate_x, 0};
          uniforms[index + 1] = {uv.m12, uv.m22, uv.translate_y, 0};
        };
        put(0, sampling.draw_to_source_uv);
        std::array flips{flip(origin(draw.image_slot)), 0.0F, 0.0F, draw.opacity};
        auto coverage = color(draw.image_slot);
        if (draw.background) {
          const auto& background = *draw.background;
          uniforms[0][3] = 1;
          uniforms[1][3] = background.inverted ? 1.0F : 0.0F;
          put(2, background.draw_to_mask_uv);
          const auto& mask = request.data_inputs[background.mask_slot];
          uniforms[2][3] = 0.5F / mask.Info().size.width;
          uniforms[3][3] = 0.5F / mask.Info().size.height;
          const auto& valid = background.valid_uv;
          uniforms[4] = {valid.x, valid.y, valid.width, valid.height};
          const auto& bg = background.color;
          uniforms[5] = {bg.red * bg.alpha, bg.green * bg.alpha, bg.blue * bg.alpha, bg.alpha};
          flips[2] = flip(mask.Origin());
          coverage = handles[image_input_count + background.mask_slot];
        }
        bgfx::setUniform(input_flips_, flips.data());
        bgfx::setUniform(sampling_, uniforms.data(), uniforms.size());
        bgfx::setTexture(2, mask_sampler_, coverage, draw.background ? kCoverageSamplerFlags : UINT32_MAX);
        std::array<std::array<float, 4>, 5> depth_uniforms{};
        auto depth_texture = color(draw.image_slot);
        if (draw.depth_occlusion) {
          const auto& depth = *draw.depth_occlusion;
          const auto& uv = depth.target_to_depth_uv;
          const auto& input = request.data_inputs[depth.depth_slot];
          depth_uniforms[0] = {uv.m11, uv.m21, uv.translate_x, 1.0F / input.Info().size.width};
          depth_uniforms[1] = {uv.m12, uv.m22, uv.translate_y, 1.0F / input.Info().size.height};
          const auto& valid = depth.valid_uv;
          const auto& target_region = depth.target_region;
          depth_uniforms[2] = {valid.x, valid.y, valid.width, valid.height};
          depth_uniforms[3] = {target_region.x, target_region.y, target_region.width, target_region.height};
          depth_uniforms[4] = {depth.layer_depth_meters, flip(input.Origin()), 0, 0};
          depth_texture = handles[image_input_count + depth.depth_slot];
        }
        bgfx::setUniform(occlusion_, depth_uniforms.data(), depth_uniforms.size());
        bgfx::setTexture(3, depth_sampler_, depth_texture, kSamplerFlags);
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

  // Release the reservation after the context and its retained resources have been destroyed.
  detail::BgfxRenderLease lease_;
  ImageInfo image_;
  std::unique_ptr<detail::BgfxTextureBridge> bridge_;
  bgfx::TextureHandle completion_ = BGFX_INVALID_HANDLE;
  std::vector<Target> targets_;
  std::vector<Target> offscreen_targets_;
  std::map<std::uint16_t, std::weak_ptr<const UploadedTexture>> uploads_;
  std::optional<Job> job_;
  std::shared_ptr<const RenderPasses> encoder_transfer_;
  std::uint64_t next_ticket_{1};
  bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle masked_program_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle sampler_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle incoming_sampler_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle mask_sampler_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle depth_sampler_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle input_flips_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle input_color_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle mask_parameters_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle mask_region_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle sampling_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle occlusion_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle destination_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle off_diagonal_ = BGFX_INVALID_HANDLE;
  bgfx::VertexBufferHandle top_left_ = BGFX_INVALID_HANDLE;
  // Shutdown drains bgfx before the leases retaining its external textures are destroyed.
  BgfxContext context_;
};

} // namespace

SubmitResult detail::SubmitBgfxPasses(RenderBackend& backend, const PassRequest& request) {
  return dynamic_cast<BgfxBackend&>(backend).SubmitPasses(request);
}

std::unique_ptr<RenderBackend> detail::CreateBgfxBackend(ImageInfo target, const bgfx::Init& init,
                                                        std::stop_token stop, std::unique_ptr<BgfxTextureBridge> bridge,
                                                        const CompositeShaders* shaders, std::size_t output_slots) {
  if (!IsColorImageFormat(target.format)) throw std::invalid_argument("The composite target must be RGBA8 or BGRA8");
  if (output_slots == 0) throw std::invalid_argument("The compositor needs at least one output slot");
  BgfxRenderLease lease(stop);
  if (stop.stop_requested()) return {};
  if (!lease) throw std::logic_error("An AVKit bgfx backend is already active");
  auto backend = std::make_unique<BgfxBackend>(target, init, std::move(lease), std::move(bridge), output_slots);
  backend->Initialize(shaders ? *shaders : kCompositeShaders);
  if (stop.stop_requested()) return {};
  return backend;
}

std::unique_ptr<RenderBackend> detail::OpenBgfxBackend(ImageInfo target, RenderPlatformHandles handles,
                                                      std::stop_token stop) {
  if (stop.stop_requested()) return {};
  bgfx::Init init;
  init.fallback = false;
  init.swapChain.nwh = handles.window;
  init.swapChain.ndt = handles.display;
  init.swapChain.width = handles.window ? target.size.width : 0;
  init.swapChain.height = handles.window ? target.size.height : 0;
  init.swapChain.maxFrameLatency = 1;
  return CreatePlatformBgfxBackend(target, init, stop);
}

Result<BufferReference, RenderError> ReadBgfxPixels(RenderBackend& backend, const Texture& surface) {
  if (auto* bgfx = dynamic_cast<BgfxBackend*>(&backend)) return bgfx->ReadPixels(surface);
  return Result<BufferReference, RenderError>::Failure({RenderErrorCode::Unsupported, "Expected a bgfx backend"});
}

Result<std::unique_ptr<PreviewTexture>, RenderError> OpenPreviewTexture(PixelSize size) {
  using R = Result<std::unique_ptr<PreviewTexture>, RenderError>;
  if (!size.width || !size.height) return R::Failure({RenderErrorCode::InvalidFrame, "Invalid preview texture size"});
  try {
    if (auto preview = detail::OpenPlatformBgfxPreviewTexture(size)) return R::Success(std::move(preview));
    return R::Failure({RenderErrorCode::Unsupported, "The platform cannot open a reusable bgfx preview texture"});
  } catch (const std::exception& error) {
    return R::Failure({RenderErrorCode::Unsupported, error.what()});
  }
}

namespace {

std::optional<RenderError> ValidatePreviewOutput(RenderBackend& backend, const Texture& surface, Size size) {
  const auto* bgfx = dynamic_cast<const BgfxBackend*>(&backend);
  if (!bgfx || !bgfx->HasCompletedOutput(surface)) {
    return RenderError{RenderErrorCode::InvalidFrame, "Expected a completed bgfx output lease"};
  }
  const auto output = surface.Info().size;
  if (size != Size{float(output.width), float(output.height)}) {
    return RenderError{RenderErrorCode::InvalidFrame, "The composite does not match the preview texture size"};
  }
  return std::nullopt;
}

} // namespace

Result<PreviewFrameCommit, RenderError> PreviewTexture::Prepare(RenderBackend& backend, const render::Texture& surface) {
  using R = Result<PreviewFrameCommit, RenderError>;
  if (auto error = ValidatePreviewOutput(backend, surface, Texture()->IntrinsicSize())) return R::Failure(std::move(*error));
  try {
    return R::Success(PrepareFrame(surface));
  } catch (const std::exception& error) {
    return R::Failure({RenderErrorCode::Unsupported, error.what()});
  }
}

Result<void, RenderError> PreviewTexture::Publish(RenderBackend& backend, const render::Texture& surface) {
  using R = Result<void, RenderError>;
  if (auto error = ValidatePreviewOutput(backend, surface, Texture()->IntrinsicSize())) return R::Failure(std::move(*error));
  try {
    PublishFrame(surface);
    return R::Success();
  } catch (const std::exception& error) {
    return R::Failure({RenderErrorCode::Unsupported, error.what()});
  }
}

Texture detail::CreateBgfxInteropTexture(RenderBackend& backend, ImageInfo image) {
  if (auto* bgfx = dynamic_cast<BgfxBackend*>(&backend)) return bgfx->CreateInteropTexture(image);
  throw std::runtime_error("Expected a bgfx backend");
}

std::uint64_t detail::GetBgfxInteropTexture(RenderBackend& backend, const Texture& image) {
  if (auto* bgfx = dynamic_cast<BgfxBackend*>(&backend)) return bgfx->InteropTexture(image);
  return 0;
}

detail::EncoderPicture detail::EncodeBgfxTexture(RenderBackend& backend, const Texture& surface,
                                                 const media::VideoEncoderConfig& config, media::MediaTimestamp pts,
                                                 std::optional<media::MediaTimestamp> duration) {
  if (auto* bgfx = dynamic_cast<BgfxBackend*>(&backend)) return bgfx->Encode(surface, config, pts, duration);
  return EncoderPicture::Failure({RenderErrorCode::Unsupported, "Expected a bgfx backend"});
}

} // namespace huxerui::avkit::render
