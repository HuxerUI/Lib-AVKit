#pragma once

/// @file
/// @brief Backend-independent render plans, image leases, and submission.
///
/// Render plans carry normalized layout and input indices. A platform bridge supplies retained color
/// and data surfaces, and the backend owns GPU submission and completion.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <huxerui/data.h>
#include <huxerui/geometry.h>

namespace huxerui::avkit {

/// @brief Physical image extent in pixels.
struct PixelSize {
  std::uint32_t width;
  std::uint32_t height;
  bool operator==(const PixelSize&) const = default;
};

namespace render {

/// @brief Color and data texture formats supported by the render contract.
enum class ImageFormat {
  Rgba8Unorm, ///< Color with red, green, blue, and alpha in byte order.
  Bgra8Unorm, ///< Color with blue, green, red, and alpha in byte order.
  R8Unorm,    ///< Single-channel normalized data such as mask coverage.
};

/// @brief Origin of the first stored row. Layout coordinates use a top-left origin.
enum class RowOrigin { TopLeft, BottomLeft };

/// @brief Interpretation of a color surface's alpha channel.
enum class AlphaMode { Opaque, Straight, Premultiplied };

/// @brief Pixel extent and storage format of an image.
struct ImageDesc {
  PixelSize size;
  ImageFormat format;
  bool operator==(const ImageDesc&) const = default;
};

/// @brief Type-erased storage lease retained by a render surface.
class SurfaceStorage {
public:
  virtual ~SurfaceStorage() = default;
};

/// @brief Retained image storage with a known format and row origin.
struct DataSurface {
  ImageDesc image;
  RowOrigin origin{RowOrigin::TopLeft};
  std::shared_ptr<const SurfaceStorage> storage;
};

/// @brief Retained color image with an explicit alpha interpretation.
struct RenderSurface {
  DataSurface pixels;
  AlphaMode alpha{AlphaMode::Premultiplied};
};

/// @brief Operation used to composite a draw into its target.
enum class Blend {
  Replace,            ///< Replace destination color and alpha.
  PremultipliedOver,  ///< Porter-Duff source-over with premultiplied color.
};

/// @brief Crossfades two color inputs according to coverage in a data texture.
struct MaskedBlend {
  std::uint32_t incoming_slot, mask_slot;
  float feather;
  bool inverted{};
  /// Mask bounds in the draw's normalized coordinates. Samples outside these bounds clamp to the edge.
  Rect mask_region{0, 0, 1, 1};
  bool operator==(const MaskedBlend&) const = default;
};

/// @brief One draw in a compositing pass.
struct Draw {
  /// Color slot. Final draws can also address subcomposition outputs after the external color inputs.
  std::uint32_t input_slot;
  Blend blend;
  /// Normalized target rectangle with a top-left origin. The target clips any overflow.
  Rect destination{0, 0, 1, 1};
  std::optional<MaskedBlend> masked_blend;
  /// Optional affine map applied to the rectangle in the same normalized coordinates.
  std::optional<Transform2D> transform;
  bool operator==(const Draw&) const = default;
};

/// @brief Offscreen composition evaluated before the final pass.
struct Subcomposition {
  /// Draws over transparent black at the plan's target size, using external color inputs.
  std::vector<Draw> draws;
  bool operator==(const Subcomposition&) const = default;
};

/// @brief Immutable draw order and output description shared across consumers.
struct RenderPlan {
  ImageDesc target;
  std::vector<Draw> draws;
  /// Rendered before the final draws. Subcomposition i has color slot inputs.size() + i.
  std::vector<Subcomposition> subcompositions;
};

/// @brief Binds a compiled render plan to the retained surfaces needed for one submission.
struct RenderRequest {
  std::shared_ptr<const RenderPlan> plan;
  std::vector<RenderSurface> inputs;
  std::vector<DataSurface> data_inputs;
};

/// @brief Categories of render validation, resource, and submission failures.
enum class RenderErrorCode { InvalidPlan, InvalidFrame, Unsupported, OutOfMemory, InvalidTicket };

/// @brief Render failure suitable for diagnostics at the caller boundary.
struct RenderError {
  RenderErrorCode code;
  std::string message;
};

/// @brief Identifies one accepted submission on its backend instance.
struct RenderTicket {
  std::uint64_t value;
  bool operator==(const RenderTicket&) const = default;
};

using SubmitResult = Result<std::optional<RenderTicket>, RenderError>;
using TakeResult = Result<std::optional<RenderSurface>, RenderError>;

/// @brief Executes render requests and publishes leased results after GPU completion.
/// Use and destroy a backend on its owning execution sequence.
class RenderBackend {
public:
  virtual ~RenderBackend() = default;
  /// @brief Attempts to admit a render request.
  /// @param request Plan and surfaces. An accepted submission retains the necessary inputs.
  /// @return A ticket on admission, an empty success on backpressure, or a render failure.
  virtual SubmitResult TrySubmit(const RenderRequest& request) = 0;
  /// @brief Advances pending submission or completion processing.
  virtual void Pump() = 0;
  /// @brief Takes a completed result for a ticket.
  /// @return A surface ready for external reads, an empty success while pending, or a render failure.
  virtual TakeResult TryTake(RenderTicket ticket) = 0;
  /// @brief Discards the ticket's result. The backend retains in-flight resources until safe to release.
  virtual void Discard(RenderTicket ticket) = 0;
};

} // namespace render
} // namespace huxerui::avkit
