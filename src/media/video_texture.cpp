#include "media/video_texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "render/backend.h"
#include "support/video_geometry.h"

namespace huxerui::avkit::detail {
namespace {

using namespace render;

constexpr auto kUnsupported = RenderErrorCode::Unsupported;
constexpr auto kInvalidFrame = RenderErrorCode::InvalidFrame;
constexpr double kBt709DisplayGamma = 2.4;
constexpr std::size_t kTransferSteps = 4096;

const std::array<std::byte, kTransferSteps + 1>& Bt709ToSrgb() {
  static const auto table = [] {
    std::array<std::byte, kTransferSteps + 1> samples{};
    for (std::size_t i = 0; i < samples.size(); ++i) {
      const auto linear = std::pow(double(i) / kTransferSteps, kBt709DisplayGamma);
      const auto srgb = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
      samples[i] = std::byte(std::lround(srgb * 255));
    }
    return samples;
  }();
  return table;
}

struct SampleAxis {
  SampleAxis(double position, std::uint32_t start, std::uint32_t extent) {
    const auto end = start + extent - 1;
    position = std::clamp(position, double(start), double(end));
    first = static_cast<std::uint32_t>(position);
    second = std::min(first + 1, end);
    weight = position - first;
  }

  std::uint32_t first, second;
  double weight;
};

struct VideoSampleAxis {
  SampleAxis luma, chroma;
};

std::vector<VideoSampleAxis> VideoSampleAxes(std::uint32_t length, double crop, bool invert,
                                          std::uint32_t start, std::uint32_t extent) {
  const auto chroma_start = start / 2;
  const auto chroma_extent = (start + extent - 1) / 2 - chroma_start + 1;
  std::vector<VideoSampleAxis> axes;
  axes.reserve(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    const auto t = 0.5 + ((i + 0.5) / length - 0.5) * crop;
    const auto position = start + (invert ? 1 - t : t) * extent;
    axes.push_back({{position - 0.5, start, extent}, {position / 2 - 0.5, chroma_start, chroma_extent}});
  }
  return axes;
}

struct CpuPlaneView {
  explicit CpuPlaneView(const media::VideoPlane& plane)
      : bytes(plane.data.AsBytes()), row_stride(plane.row_stride), pixel_stride(plane.pixel_stride) {}

  double Sample(const SampleAxis& x, const SampleAxis& y, std::size_t channel = 0) const {
    const auto row = [&](std::uint32_t index) {
      const auto* samples = bytes.data() + index * row_stride + channel;
      return std::lerp(double(std::to_integer<unsigned>(samples[x.first * pixel_stride])),
                       double(std::to_integer<unsigned>(samples[x.second * pixel_stride])), x.weight);
    };
    return std::lerp(row(y.first), row(y.second), y.weight);
  }

  std::span<const std::byte> bytes;
  std::size_t row_stride, pixel_stride;
};

Result<ImagePixels, RenderError> FitCpuVideo(const media::VideoFrame& frame, media::VideoTextureOptions options,
                                       const avkit::detail::VideoTextureFit& fit) {
  using R = Result<ImagePixels, RenderError>;
  using namespace media;
  const auto& format = frame.format;
  const bool planar = format.pixel_format == VideoPixelFormat::I420;
  if ((!planar && format.pixel_format != VideoPixelFormat::NV12) ||
      format.primaries == ColorPrimaries::BT2020 || format.matrix == ColorMatrix::BT2020 ||
      format.transfer == ColorTransfer::PQ || format.transfer == ColorTransfer::HLG) {
    return R::Failure({kUnsupported, "CPU video import requires eight-bit SDR I420 or NV12 frames"});
  }

  const auto visible = format.visible;
  const bool rotated = format.rotation == VideoRotation::Clockwise90 || format.rotation == VideoRotation::Clockwise270;
  const auto& [area, crop_width, crop_height] = fit;
  // Resolve crop and bilinear weights once per axis. Quarter turns exchange the output axes.
  const auto columns = VideoSampleAxes(rotated ? area.height : area.width, rotated ? crop_height : crop_width,
      format.rotation == VideoRotation::Clockwise180 || format.rotation == VideoRotation::Clockwise270,
      visible.x, visible.width);
  const auto rows = VideoSampleAxes(rotated ? area.width : area.height, rotated ? crop_width : crop_height,
      format.rotation == VideoRotation::Clockwise180 || format.rotation == VideoRotation::Clockwise90,
      visible.y, visible.height);
  const CpuPlaneView y_plane(frame.planes[0]), u_plane(frame.planes[1]), v_plane(frame.planes[planar ? 2 : 1]);
  const bool full = format.range == ColorRange::Full;
  const double kr = format.matrix == ColorMatrix::BT709 ? 0.2126 : 0.299;
  const double kb = format.matrix == ColorMatrix::BT709 ? 0.0722 : 0.114;
  const auto* transfer = format.transfer == ColorTransfer::BT709 ? &Bt709ToSrgb() : nullptr;
  const auto byte = [transfer](double value) {
    value = std::clamp(value, 0., 1.);
    if (!transfer) return std::byte(std::lround(value * 255));
    return (*transfer)[static_cast<std::size_t>(value * kTransferSteps + 0.5)];
  };

  const auto stride = std::size_t(options.width) * 4;
  auto bytes = std::make_shared<Bytes>(stride * options.height);
  for (std::size_t i = 3; i < bytes->size(); i += 4) (*bytes)[i] = std::byte{255};
  for (std::uint32_t y = 0; y < area.height; ++y) {
    for (std::uint32_t x = 0; x < area.width; ++x) {
      const auto& sx = columns[rotated ? y : x];
      const auto& sy = rows[rotated ? x : y];
      const auto luma = (y_plane.Sample(sx.luma, sy.luma) - (full ? 0 : 16)) / (full ? 255 : 219);
      const auto cb = (u_plane.Sample(sx.chroma, sy.chroma) - 128) / (full ? 255 : 224);
      const auto cr = (v_plane.Sample(sx.chroma, sy.chroma, planar ? 0 : 1) - 128) / (full ? 255 : 224);
      const auto red = luma + 2 * (1 - kr) * cr, blue = luma + 2 * (1 - kb) * cb;
      auto* pixel = bytes->data() + (area.y + y) * stride + (area.x + x) * 4;
      pixel[0] = byte(red);
      pixel[1] = byte((luma - kr * red - kb * blue) / (1 - kr - kb));
      pixel[2] = byte(blue);
    }
  }
  const ImageInfo image{{options.width, options.height}, PixelFormat::Rgba8Unorm};
  return R::Success({image, BufferReference(*bytes, bytes), stride, RowOrigin::TopLeft, AlphaMode::Opaque});
}

} // namespace

Result<ImagePixels, RenderError> FitCpuVideoFrame(const media::VideoFrame& frame, media::VideoTextureOptions options) {
  if (!frame.Validate() || frame.Storage() != media::VideoStorage::Cpu || options.width == 0 || options.height == 0 ||
      options.width > 8192 || options.height > 8192 ||
      (options.fit != ImageFit::Contain && options.fit != ImageFit::Cover))
    return Result<ImagePixels, RenderError>::Failure({kInvalidFrame, "Invalid CPU video frame or target extent"});
  try {
    return FitCpuVideo(frame, options, FitVideoTexture(frame.format, options));
  } catch (const std::bad_alloc&) {
    return Result<ImagePixels, RenderError>::Failure({RenderErrorCode::OutOfMemory, "Cannot allocate fitted CPU video pixels"});
  }
}

Result<ImportedVideoTexture, RenderError> ImportVideoTexture(
    RenderBackend& device, const media::VideoFrame& frame, media::VideoTextureOptions options) {
  using R = Result<ImportedVideoTexture, RenderError>;
  if (!frame.Validate() || frame.Storage() != media::VideoStorage::Platform || !options.width || !options.height ||
      options.width > 8192 || options.height > 8192 || (options.fit != ImageFit::Contain && options.fit != ImageFit::Cover))
    return R::Failure({kInvalidFrame, "Invalid platform video frame or texture options"});
  try {
    auto texture = media::MakeExternalTexture(frame, options);
    if (!texture.Succeeded()) return R::Failure({kUnsupported, texture.Error().message});
    auto imported = device.ImportTexture(*texture.Value(), &frame.format);
    if (!imported.Succeeded()) return R::Failure(imported.Error());
    return R::Success({std::move(imported).Value(), FitVideoTexture(frame.format, options).SourceToTextureUv(options)});
  } catch (const std::bad_alloc&) {
    return R::Failure({RenderErrorCode::OutOfMemory, "Cannot retain the imported video texture"});
  } catch (const std::exception& error) {
    return R::Failure({kUnsupported, error.what()});
  }
}

} // namespace huxerui::avkit::detail
