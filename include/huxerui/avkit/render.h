#pragma once

/// @file
/// @brief Backend-independent render plans, image leases, and submission.
///
/// Render plans own the inputs and normalized layout for one output. The renderer lowers the tree to GPU passes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/avkit/types.h>
#include <huxerui/data.h>
#include <huxerui/color.h>
#include <huxerui/file.h>
#include <huxerui/geometry.h>

namespace huxerui { class ExternalTexture; }

namespace huxerui::avkit {

class ResourceCatalog;
struct MaskImage;
struct PagLayerSample;

namespace detail {
class TextureBacking;
struct TextureAccess;
}

namespace render {

/// @brief Color and data texture formats supported by the render contract.
enum class PixelFormat {
  Rgba8Unorm, ///< Color with red, green, blue, and alpha in byte order.
  Bgra8Unorm, ///< Color with blue, green, red, and alpha in byte order.
  R8Unorm,    ///< Single-channel normalized data such as mask coverage.
  R32Float,   ///< Single-channel, 32-bit floating-point data such as metric depth.
};

/// @brief Origin of the first stored row. Layout coordinates use a top-left origin.
enum class RowOrigin { TopLeft, BottomLeft };

/// @brief Interpretation of a color image's alpha channel.
enum class AlphaMode { Opaque, Straight, Premultiplied };

/// @brief Pixel extent and storage format of an image.
struct ImageInfo {
  /// Physical width and height in pixels.
  PixelSize size{};
  /// Stored component layout and precision.
  PixelFormat format{PixelFormat::Rgba8Unorm};

  bool operator==(const ImageInfo&) const = default;
};

/// @brief A GPU texture retained through submission and completion on its render device.
/// Copies retain the same backing. A default-constructed texture is empty.
class Texture {
public:
  /// @brief Reports whether this image retains backing storage.
  explicit operator bool() const noexcept { return static_cast<bool>(backing_); }

  /// @brief Returns the physical extent and pixel format.
  ImageInfo Info() const noexcept { return info_; }

  /// @brief Returns the first stored row's logical edge.
  RowOrigin Origin() const noexcept { return origin_; }

  /// @return The alpha interpretation for a color image. Numeric and empty images have no alpha mode.
  std::optional<AlphaMode> Alpha() const noexcept { return backing_ ? alpha_ : std::nullopt; }

private:
  friend struct avkit::detail::TextureAccess;
  ImageInfo info_;
  RowOrigin origin_{RowOrigin::TopLeft};
  std::optional<AlphaMode> alpha_;
  std::shared_ptr<const avkit::detail::TextureBacking> backing_;
};

/// @brief Retained CPU pixels. Treat the selected byte range as immutable while it is retained.
/// Row padding is allowed between packed rows.
struct ImagePixels {
  /// Dimensions and numeric layout of the packed pixels.
  ImageInfo info;
  /// Retained bytes covering the used rows and any inter-row padding.
  BufferReference bytes;
  /// Distance in bytes between consecutive rows.
  std::size_t row_bytes{};
  /// Logical edge represented by the first stored row.
  RowOrigin origin{RowOrigin::TopLeft};
  /// Color alpha interpretation. Empty uses premultiplied alpha for color.
  std::optional<AlphaMode> alpha;

  /// @brief Checks dimensions, stride, and retained byte coverage.
  bool Validate() const noexcept;

  bool operator==(const ImagePixels&) const noexcept = default;
};

/// @brief Sampled pixels before pass preparation, held in CPU memory or a GPU texture.
/// Effects use both storage types for mask and depth inputs. Drawable nodes expose their concrete storage type.
using PixelInput = std::variant<ImagePixels, Texture>;

/// @brief Returns the pixel dimensions and format of a sampled input.
/// @param input CPU pixels or a retained GPU texture.
inline ImageInfo GetImageInfo(const PixelInput& input) noexcept {
  if (const auto* pixels = std::get_if<ImagePixels>(&input)) return pixels->info;
  return std::get<Texture>(input).Info();
}

/// @brief Operation used to composite a draw into its target.
enum class Blend {
  Replace,            ///< Replace destination color and alpha.
  PremultipliedOver,  ///< Porter-Duff source-over with premultiplied color.
};

/// @brief Evaluated coverage for a transition between two compositions.
struct MaskTransitionSample {
  PixelInput mask; ///< R8Unorm coverage in either storage type.
  /// Feather amount in [0, 1].
  float feather;
  bool inverted{};
  /// Mask bounds in normalized target coordinates. Samples outside clamp to the edge.
  Rect mask_region{0, 0, 1, 1};
};

/// @brief Frame-local foreground coverage and the background compositing settings.
struct BackgroundMask {
  PixelInput mask; ///< R8Unorm coverage in either storage type.
  /// Maps draw-local coordinates to logical mask UVs.
  Affine2D draw_to_mask_uv;
  /// Valid mask region. Samples outside it contribute zero coverage.
  Rect valid_uv{0, 0, 1, 1};
  /// Straight RGBA color composited behind the covered foreground.
  Color color;
  bool inverted{}; ///< Inverts coverage inside valid_uv.
  /// Output blending for the replacement. Empty inherits the foreground draw's blend.
  std::optional<Blend> blend;
};

/// @brief Maps the unit draw rectangle to logical image UVs before row-origin handling.
struct SourceSampling {
  Affine2D draw_to_source_uv;

  bool operator==(const SourceSampling&) const = default;
};

/// @brief Evaluated media time and asset for a two-input PAG transition.
struct PagTransitionSample {
  /// PAG asset with outgoing and incoming slots.
  ResourceRef resource;
  /// Stable identity used by the render worker's transition cache.
  InstanceId instance;
  /// Elapsed time within the overlap and the full overlap duration.
  media::MediaTimestamp offset, duration;
};

/// @brief How a drawable result is placed and blended into its parent.
struct NodePlacement {
  Blend blend{Blend::PremultipliedOver};
  /// Maps the unit rectangle of this result into normalized parent coordinates.
  Affine2D local_to_target;
  float opacity{1.0F};
};

/// @brief Operation performed by a render node.
enum class RenderNodeKind { Image, Texture, Pag, Composite, BackgroundReplacement, DepthOcclusion, MaskTransition, PagTransition };

/// @brief Base of the operations supported by a render plan.
class RenderNode {
public:
  virtual ~RenderNode() = default;

  /// @brief Identifies the concrete operation for backend dispatch.
  virtual RenderNodeKind Kind() const noexcept = 0;
};

/// @brief Draws retained CPU pixels. Pass preparation uploads the pixels on the render sequence.
struct ImageNode final : RenderNode {
  /// @param image Decoded, packed CPU pixels retained through upload.
  /// @param layout Placement in the parent target.
  /// @param uv Optional mapping from the unit rectangle to the uploaded texture's logical UVs.
  ImageNode(ImagePixels image, NodePlacement layout = {}, std::optional<SourceSampling> uv = {})
      : pixels(std::move(image)), placement(layout), sampling(uv) {}

  RenderNodeKind Kind() const noexcept override { return RenderNodeKind::Image; }

  ImagePixels pixels;
  NodePlacement placement;
  /// Empty samples the full pixel rectangle.
  std::optional<SourceSampling> sampling;
};

/// @brief Draws an existing GPU texture.
struct TextureNode final : RenderNode {
  /// @param image Prepared texture sampled by this draw.
  /// @param layout Placement in the parent target.
  /// @param uv Optional mapping from the unit rectangle to texture UVs.
  TextureNode(Texture image, NodePlacement layout = {}, std::optional<SourceSampling> uv = {})
      : texture(std::move(image)), placement(layout), sampling(uv) {}

  RenderNodeKind Kind() const noexcept override { return RenderNodeKind::Texture; }

  Texture texture;
  NodePlacement placement;
  /// Empty samples the full texture including any padding supplied by the importer.
  std::optional<SourceSampling> sampling;
};

/// @brief Draws an evaluated PAG composition. The renderer may batch adjacent PAG nodes.
struct PagNode final : RenderNode {
  /// @param frame Evaluated local time, resource, and replacements retained for this draw.
  /// @param layout Placement in the parent target.
  PagNode(std::shared_ptr<const PagLayerSample> frame, NodePlacement layout = {})
      : sample(std::move(frame)), placement(layout) {}

  RenderNodeKind Kind() const noexcept override { return RenderNodeKind::Pag; }

  std::shared_ptr<const PagLayerSample> sample;
  NodePlacement placement;
};

/// @brief Composites children in back-to-front order in normalized target coordinates.
/// The root and isolated intermediate passes use the physical size of RenderPlan::target.
struct CompositeNode final : RenderNode {
  RenderNodeKind Kind() const noexcept override { return RenderNodeKind::Composite; }

  std::vector<std::unique_ptr<const RenderNode>> children;
  /// Applies when this composite is itself a child. Roots use identity placement.
  NodePlacement placement;
};

/// @brief Replaces the background of an image using foreground coverage.
/// The result inherits the foreground's geometry and opacity. The coverage can select the output blend.
struct BackgroundReplacementNode final : RenderNode {
  /// @param image ImageNode or TextureNode containing the foreground and its placement.
  /// @param mask Coverage in the foreground's draw coordinates and replacement color.
  BackgroundReplacementNode(std::unique_ptr<const RenderNode> image, BackgroundMask mask)
      : foreground(std::move(image)), coverage(std::move(mask)) {}

  RenderNodeKind Kind() const noexcept override { return RenderNodeKind::BackgroundReplacement; }

  std::unique_ptr<const RenderNode> foreground;
  BackgroundMask coverage;
};

/// @brief Tests an image result against scene depth registered to the parent target.
struct DepthOcclusionNode final : RenderNode {
  /// @param input Image, PAG composition, or background replacement using source-over blending.
  /// @param image Retained R32Float depth in meters.
  /// @param uv Mapping from normalized parent target coordinates to logical depth UVs.
  /// @param valid Region of the depth image with usable measurements.
  /// @param region Area covered by the clip that supplied depth.
  /// @param meters Positive optical-axis depth for the child. Invalid scene samples hide it.
  DepthOcclusionNode(std::unique_ptr<const RenderNode> input, PixelInput image, Affine2D uv, Rect valid, Rect region, float meters)
      : child(std::move(input)), depth(std::move(image)), target_to_depth_uv(uv), valid_uv(valid),
        target_region(region), layer_depth_meters(meters) {}

  RenderNodeKind Kind() const noexcept override { return RenderNodeKind::DepthOcclusion; }

  std::unique_ptr<const RenderNode> child;
  PixelInput depth;
  Affine2D target_to_depth_uv;
  Rect valid_uv;
  Rect target_region;
  float layer_depth_meters;
};

/// @brief Mixes two isolated compositions using the evaluated mask.
struct MaskTransitionNode final : RenderNode {
  /// @param children Outgoing and incoming compositions, respectively.
  /// @param effect Evaluated coverage for this frame.
  /// @param layout Placement of the mixed result in the parent target.
  MaskTransitionNode(std::array<CompositeNode, 2> children, MaskTransitionSample effect, NodePlacement layout = {})
      : sides(std::move(children)), sample(std::move(effect)), placement(layout) {}

  RenderNodeKind Kind() const noexcept override { return RenderNodeKind::MaskTransition; }

  std::array<CompositeNode, 2> sides;
  MaskTransitionSample sample;
  NodePlacement placement;
};

/// @brief Mixes two isolated compositions using the evaluated PAG asset.
struct PagTransitionNode final : RenderNode {
  /// @param children Outgoing and incoming compositions, respectively.
  /// @param effect Asset and local time for this frame.
  /// @param layout Placement of the mixed result in the parent target.
  PagTransitionNode(std::array<CompositeNode, 2> children, PagTransitionSample effect, NodePlacement layout = {})
      : sides(std::move(children)), sample(std::move(effect)), placement(layout) {}

  RenderNodeKind Kind() const noexcept override { return RenderNodeKind::PagTransition; }

  std::array<CompositeNode, 2> sides;
  PagTransitionSample sample;
  NodePlacement placement;
};

/// @brief Immutable render tree and output contract for one evaluation.
struct RenderPlan {
  /// Physical extent and format of the output attachment.
  ImageInfo target;
  /// Full-canvas composite. Children can cover any portion of it.
  CompositeNode root;
  /// Exact resource catalog snapshot used by the scene and retained through rendering.
  std::shared_ptr<const ResourceCatalog> resources;
};

/// @brief Categories of render validation, resource, and submission failures.
enum class RenderErrorCode { InvalidPlan, InvalidFrame, Unsupported, OutOfMemory, InvalidTicket };

/// @brief Render failure suitable for diagnostics at the caller boundary.
struct RenderError {
  /// Machine-readable reason for the failure.
  RenderErrorCode code;
  /// Diagnostic describing the rejected operation.
  std::string message;
};

/// @brief Platform window and display handles for context creation.
/// The compositor renders to offscreen textures. Web requires a retained canvas selector as `window`,
/// and the context must be used on the browser main thread.
struct RenderPlatformHandles {
  /// Optional platform window handle. On Web this points to a retained canvas selector.
  void* window{};
  /// Optional platform display or connection handle.
  void* display{};
};

class RenderBackend;

/// @brief A prepared GPU frame that can be committed once on the application sequence.
using PreviewFrameCommit = std::function<void()>;

/// @brief Publishes completed composites into one HuxerUI texture for a preview session.
/// Hold the same Texture() in the view as frames are replaced. Serialize publication and Finish.
class PreviewTexture {
public:
  virtual ~PreviewTexture() = default;

  /// @brief Returns the stable HuxerUI texture for this preview session.
  virtual std::shared_ptr<ExternalTexture> Texture() const = 0;

  /// @brief Stops publication and keeps the last published frame available to the view.
  virtual void Finish() noexcept = 0;

private:
  friend class TimelineRenderer;

  Result<PreviewFrameCommit, RenderError> Prepare(RenderBackend& backend, const render::Texture& surface);

  Result<void, RenderError> Publish(RenderBackend& backend, const render::Texture& surface);

  virtual PreviewFrameCommit PrepareFrame(const render::Texture&) { return {}; }

  virtual void PublishFrame(const render::Texture& surface) = 0;
};

/// @brief Opens a reusable HuxerUI preview texture for a fixed composite size.
/// Open from the application context when the platform provides Canvas-backed GPU presentation.
/// @param size Fixed physical dimensions of the composites to publish.
/// @return Preview publisher, or a diagnostic when the platform or extent is unsupported.
Result<std::unique_ptr<PreviewTexture>, RenderError> OpenPreviewTexture(PixelSize size);

} // namespace render

} // namespace huxerui::avkit
