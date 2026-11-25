#pragma once

/// @file
/// @brief Video interchange through SDK texture leases and Lib-Media frames.

#include <functional>
#include <variant>

#include <huxerui/avkit/render.h>
#include <huxerui/external_texture.h>
#include <huxerui/media_codec.h>

namespace huxerui::avkit::render {

/// @brief Selects the allowed storage paths for importing or publishing an image.
enum class TextureTransferPolicy {
  AllowCpuTransfer, ///< Accept CPU frames and allow pixel readback or upload.
  GpuOnly,          ///< Require the platform GPU transfer path.
};

/// @brief A completed HuxerUI preview image with either a texture lease or immutable pixel storage.
using VideoPreviewImage = std::variant<std::shared_ptr<ExternalTexture>, Bitmap>;

/// @brief A prepared GPU frame that can be committed once on the application sequence.
using PreviewFrameCommit = std::function<void()>;

/// @brief Publishes completed composites into one HuxerUI texture for a preview session.
/// Hold the same Texture() in the view as frames are replaced. Serialize publication and Finish.
class BgfxPreviewTexture {
public:
  virtual ~BgfxPreviewTexture() = default;
  virtual std::shared_ptr<ExternalTexture> Texture() const = 0;
  /// @brief Prepares a completed composite on the bgfx sequence without changing Texture()'s revision.
  /// Commit on the application sequence with any accompanying UI state. An empty callback means
  /// the platform publishes directly on the bgfx sequence. Dropping the callback abandons the frame.
  Result<PreviewFrameCommit, RenderError> Prepare(RenderBackend& backend, const ColorSurface& surface);
  /// @brief Publishes immediately on the bgfx sequence.
  Result<void, RenderError> Publish(RenderBackend& backend, const ColorSurface& surface);
  /// @brief Stops publication and keeps the last published frame available to the view.
  virtual void Finish() noexcept = 0;

private:
  virtual PreviewFrameCommit PrepareFrame(const ColorSurface&) { return {}; }
  virtual void PublishFrame(const ColorSurface& surface) = 0;
};

/// @brief Opens a reusable HuxerUI preview texture for a fixed composite size.
/// Open from the application context when the platform provides Canvas-backed GPU presentation.
Result<std::unique_ptr<BgfxPreviewTexture>, RenderError> OpenBgfxPreviewTexture(PixelSize size);

/// @brief Imports the current image of a Camera or other SDK texture.
/// On a failed GPU transfer, a readback can supply the same frame as a pixel upload.
/// Selects the source image at the call site. Call on the backend sequence. The CPU fallback
/// also requires the texture's snapshot sequence.
Task<Result<ColorSurface, RenderError>> ImportBgfxTexture(
    RenderBackend& backend, std::shared_ptr<const ExternalTexture> texture,
    TextureTransferPolicy policy = TextureTransferPolicy::AllowCpuTransfer);

/// @brief Imports a retained decoder frame and maps normalized source coordinates into its fitted texture.
/// Call on the bgfx sequence. Incompatible platform textures return Unsupported so the caller can retry the same
/// source PTS. AllowCpuTransfer accepts supported software-decoded planes for upload.
Result<ImportedVideoFrame, RenderError> ImportDecodedBgfxFrame(
    RenderBackend& backend, const media::VideoFrame& frame, media::VideoTextureOptions options,
    TextureTransferPolicy policy = TextureTransferPolicy::AllowCpuTransfer);

/// @brief Imports a retained decoder frame fitted to the requested presentation size.
/// Platform frames use the platform texture bridge. An incompatible texture returns Unsupported
/// so the caller can retry with a software-decoded frame. AllowCpuTransfer accepts SDR I420 or
/// NV12 CPU planes and uploads the fitted pixels to the compositor.
Task<Result<ColorSurface, RenderError>> ImportBgfxVideoFrame(
    RenderBackend& backend, media::VideoFrame frame, media::VideoTextureOptions options,
    TextureTransferPolicy policy = TextureTransferPolicy::AllowCpuTransfer);

/// @brief Publishes one completed composite for use in a HuxerUI Image.
Result<VideoPreviewImage, RenderError> MakeBgfxPreviewImage(
    RenderBackend& backend, const ColorSurface& surface, media::MediaTimestamp pts,
    TextureTransferPolicy policy = TextureTransferPolicy::AllowCpuTransfer);

/// @brief Converts a completed composite to the negotiated encoder input and preserves PTS and duration.
/// Platform and CPU are distinct encoder contracts. On Unsupported, a session may negotiate a CPU
/// encoder input and retry the same output lease.
Result<media::VideoFrame, RenderError> MakeBgfxEncoderFrame(
    RenderBackend& backend, const ColorSurface& surface, const media::VideoEncoderConfig& config,
    media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration = std::nullopt);

} // namespace huxerui::avkit::render
