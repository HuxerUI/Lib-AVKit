#pragma once

#include "video_texture_internal.h"

#include <span>

#include <bgfx/bgfx.h>

namespace huxerui::avkit::render::detail {

/// @brief Supplies externally owned textures for a platform graphics device.
class BgfxTextureBridge {
public:
  virtual ~BgfxTextureBridge() = default;
  virtual std::shared_ptr<const SurfaceStorage> CreateOutput(ImageDesc image) const = 0;
  /// @return A texture on the bridge's device matching the surface description, or zero.
  virtual std::uint64_t GetTexture(const ImageSurface& surface) const = 0;
  virtual void WaitForCompletion() const = 0;
  virtual void ReleaseOutputResources() {}
  virtual ImportedTexture Acquire(const ExternalTexture&) {
    return ImportedTexture::Failure({RenderErrorCode::Unsupported, "The texture cannot be imported by this renderer"});
  }
  /// @return Whether the fitted decoder texture still carries the BT.709 signal transfer after GPU import.
  virtual bool SamplesBt709Signal(const media::VideoFormat&) const { return false; }
  /// @return Whether the platform video encoder expects RGB already encoded with the BT.709 transfer.
  virtual bool ExpectsBt709Signal(const media::VideoEncoderConfig&) const { return false; }
  virtual PublishedTexture Publish(const ColorSurface&, media::MediaTimestamp) {
    return PublishedTexture::Failure({RenderErrorCode::Unsupported, "This renderer cannot publish a GPU preview texture"});
  }
  virtual EncoderPicture Encode(const ColorSurface& surface, const media::VideoEncoderConfig& config,
                                media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration) {
    if (config.input_storage != media::VideoStorage::Platform) {
      return EncoderPicture::Failure({RenderErrorCode::Unsupported, "This renderer has no CPU video transfer"});
    }
    const auto published = Publish(surface, pts);
    if (!published.Succeeded()) return EncoderPicture::Failure(published.Error());
    auto frame = media::MakeVideoFrame(published.Value(), config.input, pts, duration);
    if (!frame.Succeeded()) {
      return EncoderPicture::Failure({RenderErrorCode::Unsupported, frame.Error().message});
    }
    return EncoderPicture::Success(std::move(frame).Value());
  }
};

struct CompositeShaders {
  std::span<const std::uint8_t> vertex;
  std::span<const std::uint8_t> fragment;
  std::span<const std::uint8_t> masked_fragment;
};

/// @brief Creates the compositor using the context and texture bridge selected by the platform.
std::unique_ptr<RenderBackend> CreateBgfxBackend(ImageDesc target, const bgfx::Init& init,
                                                 std::unique_ptr<BgfxTextureBridge> bridge = {},
                                                 const CompositeShaders* shaders = nullptr,
                                                 std::size_t output_slots = 2);

/// @brief Configures platform-specific context and texture sharing for the compositor.
std::unique_ptr<RenderBackend> CreatePlatformBgfxBackend(ImageDesc target, bgfx::Init init);

} // namespace huxerui::avkit::render::detail
