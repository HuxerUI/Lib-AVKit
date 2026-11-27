#pragma once

#include <huxerui/avkit/render.h>

namespace huxerui::avkit::render {

/// @brief Identifies one accepted submission on its backend instance.
struct RenderTicket {
  /// Opaque submission identifier scoped to its issuing backend.
  std::uint64_t value;

  bool operator==(const RenderTicket&) const = default;
};

using SubmitResult = Result<std::optional<RenderTicket>, RenderError>;
using TakeResult = Result<std::optional<Texture>, RenderError>;

/// @brief Executes render requests and publishes leased results after GPU completion.
/// Use and destroy a backend on its owning execution sequence.
class RenderBackend {
public:
  virtual ~RenderBackend() = default;

  RenderBackend(const RenderBackend&) = delete;

  RenderBackend& operator=(const RenderBackend&) = delete;

  RenderBackend() = default;

  /// @brief Shares a native GPU texture on this backend's device.
  /// @param texture Retained producer texture.
  /// @param format Optional decoder color metadata for imported video.
  virtual Result<Texture, RenderError> ImportTexture(const ExternalTexture& texture,
      const media::VideoFormat* format = nullptr) = 0;

  /// @brief Checks whether an existing texture can be sampled on this device.
  /// @param texture Producer texture to use on the device's owning sequence.
  virtual bool CanSample(const Texture& texture) const = 0;

  /// @brief Uploads retained CPU pixels to this backend's device.
  /// @param pixels Packed rows with a valid pixel description and stride.
  virtual Result<Texture, RenderError> UploadTexture(const ImagePixels& pixels) = 0;

  /// @brief Advances pending submission or completion processing.
  virtual void Pump() = 0;

  /// @brief Takes a completed result for a ticket.
  /// @param ticket Outstanding submission issued by this backend. A completed result can be taken once.
  /// @return A surface ready for external reads, an empty success while pending, or a render failure.
  virtual TakeResult TryTake(RenderTicket ticket) = 0;
};

} // namespace huxerui::avkit::render
