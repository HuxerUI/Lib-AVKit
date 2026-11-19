#include "video_texture_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <stdexcept>
#include <vector>

#include <huxerui/avkit/bgfx.h>

#include "support/video_geometry.h"

namespace huxerui::avkit::render {

namespace {

constexpr RenderErrorCode kUnsupported = RenderErrorCode::Unsupported;
constexpr RenderErrorCode kInvalidFrame = RenderErrorCode::InvalidFrame;

Result<Bitmap, RenderError> ReadBitmap(RenderBackend& backend, const ColorSurface& surface) {
  using R = Result<Bitmap, RenderError>;
  auto read = ReadBgfxPixels(backend, surface);
  if (!read.Succeeded()) return R::Failure(read.Error());
  const auto bytes = read.Value().AsBytes();
  const auto [width, height] = surface.pixels.image.size;
  const auto stride = std::size_t(width) * 4;
  Bytes pixels(bytes.size());
  const bool bgra = surface.pixels.image.format == PixelFormat::Bgra8Unorm;
  for (std::size_t y = 0; y < height; ++y) {
    const auto row = surface.pixels.origin == RowOrigin::TopLeft ? y : height - 1 - y;
    for (std::size_t x = 0; x < width; ++x) {
      const auto* source = bytes.data() + row * stride + x * 4;
      auto* dest = pixels.data() + y * stride + x * 4;
      const auto alpha = std::to_integer<unsigned>(source[3]);
      for (const auto channel : {0, 1, 2}) {
        const auto value = std::to_integer<unsigned>(source[bgra ? 2 - channel : channel]);
        dest[channel] = std::byte(alpha ? std::min(255U, (value * 255 + alpha / 2) / alpha) : 0);
      }
      dest[3] = source[3];
    }
  }
  return R::Success(Bitmap::FromRgba8(width, height, std::move(pixels)));
}

struct Yuv {
  float y, u, v;
};

float EncodeTransfer(std::byte sample, media::ColorTransfer transfer) {
  const float value = std::to_integer<unsigned>(sample) / 255.0F;
  if (transfer == media::ColorTransfer::SRGB || transfer == media::ColorTransfer::Unknown) return value;
  const float linear = value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
  if (transfer == media::ColorTransfer::Linear) return linear;
  return linear < 0.018F ? linear * 4.5F : 1.099F * std::pow(linear, 0.45F) - 0.099F;
}

detail::EncoderPicture ReadEncoderPicture(RenderBackend& backend, const ColorSurface& surface,
                                         const media::VideoFormat& format, media::MediaTimestamp pts,
                                         std::optional<media::MediaTimestamp> duration) {
  using R = detail::EncoderPicture;
  using namespace media;
  const bool nv12 = format.pixel_format == VideoPixelFormat::NV12;
  if ((!nv12 && format.pixel_format != VideoPixelFormat::I420) || format.matrix == ColorMatrix::BT2020 ||
      format.primaries == ColorPrimaries::BT2020 || format.transfer == ColorTransfer::PQ ||
      format.transfer == ColorTransfer::HLG) {
    return R::Failure({kUnsupported, "CPU texture transfer requires an eight-bit SDR I420 or NV12 encoder"});
  }
  auto read = ReadBgfxPixels(backend, surface);
  if (!read.Succeeded()) return R::Failure(read.Error());
  const auto source = read.Value().AsBytes();
  const auto width = std::size_t(format.width);
  const auto height = std::size_t(format.height);
  const auto chroma_width = (width + 1) / 2;
  const auto chroma_height = (height + 1) / 2;
  const auto area = width * height;
  const auto chroma_area = chroma_width * chroma_height;
  auto storage = std::make_shared<Bytes>(area + chroma_area * 2);
  const bool full = format.range == ColorRange::Full;
  const auto limited = [](float value, float scale, float offset) {
    return std::byte(std::lround(std::clamp(value * scale + offset, 0.0F, 255.0F)));
  };
  const bool bgra = surface.pixels.image.format == PixelFormat::Bgra8Unorm;
  const float kr = format.matrix == ColorMatrix::BT601 ? 0.299F : 0.2126F;
  const float kb = format.matrix == ColorMatrix::BT601 ? 0.114F : 0.0722F;
  const auto sample = [&](std::size_t x, std::size_t y) {
    const auto row = surface.pixels.origin == RowOrigin::TopLeft ? y : height - 1 - y;
    const auto* color = source.data() + (row * width + x) * 4;
    const auto r = EncodeTransfer(color[bgra ? 2 : 0], format.transfer);
    const auto g = EncodeTransfer(color[1], format.transfer);
    const auto b = EncodeTransfer(color[bgra ? 0 : 2], format.transfer);
    const auto y_value = kr * r + (1 - kr - kb) * g + kb * b;
    return Yuv{y_value, (b - y_value) / (2 * (1 - kb)), (r - y_value) / (2 * (1 - kr))};
  };
  for (std::size_t y = 0; y < height; y += 2) {
    for (std::size_t x = 0; x < width; x += 2) {
      float u = 0, v = 0;
      unsigned count = 0;
      for (auto row = y; row < std::min(y + 2, height); ++row) {
        for (auto col = x; col < std::min(x + 2, width); ++col) {
          const auto color = sample(col, row);
          (*storage)[row * width + col] = limited(color.y, full ? 255 : 219, full ? 0 : 16);
          u += color.u;
          v += color.v;
          ++count;
        }
      }
      const auto index = (y / 2) * chroma_width + x / 2;
      const auto u_index = area + (nv12 ? index * 2 : index);
      const auto v_index = nv12 ? u_index + 1 : area + chroma_area + index;
      (*storage)[u_index] = limited(u / count, full ? 255 : 224, 128);
      (*storage)[v_index] = limited(v / count, full ? 255 : 224, 128);
    }
  }
  const BufferReference bytes(*storage, storage);
  std::vector<VideoPlane> planes{{bytes.Slice(0, area), width, 1}};
  if (nv12) {
    planes.push_back({bytes.Slice(area, chroma_area * 2), chroma_width * 2, 2});
  } else {
    planes.push_back({bytes.Slice(area, chroma_area), chroma_width, 1});
    planes.push_back({bytes.Slice(area + chroma_area, chroma_area), chroma_width, 1});
  }
  VideoFrame result(format, pts, duration, std::move(planes));
  if (!result.Validate()) return R::Failure({kInvalidFrame, "CPU encoder picture has invalid timing or plane geometry"});
  return R::Success(std::move(result));
}

Task<detail::ImportedTexture> CompleteImport(detail::ImportedTexture result) {
  co_return result;
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

detail::ImportedTexture ImportCpuVideo(const media::VideoFrame& frame, media::VideoTextureOptions options,
                                       const avkit::detail::VideoTextureFit& fit) {
  using R = detail::ImportedTexture;
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
  const auto byte = [](double value) { return std::byte(std::lround(std::clamp(value, 0., 1.) * 255)); };

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
  const ImageDesc image{{options.width, options.height}, PixelFormat::Rgba8Unorm};
  return R::Success({RetainBgfxPixels(image, BufferReference(*bytes, bytes), stride), AlphaMode::Opaque});
}

ColorSurface RetainBitmap(const Bitmap& bitmap) {
  const auto source = bitmap.Pixels();
  auto bytes = std::make_shared<Bytes>(source.begin(), source.end());
  for (std::size_t i = 0; i < bytes->size(); i += 4) {
    const auto alpha = std::to_integer<unsigned>((*bytes)[i + 3]);
    for (unsigned c = 0; c < 3; ++c) {
      (*bytes)[i + c] = std::byte((std::to_integer<unsigned>((*bytes)[i + c]) * alpha + 127) / 255);
    }
  }
  const ImageDesc desc{{bitmap.PixelWidth(), bitmap.PixelHeight()}, PixelFormat::Rgba8Unorm};
  return {RetainBgfxPixels(desc, BufferReference(*bytes, bytes), std::size_t(desc.size.width) * 4)};
}

Task<detail::ImportedTexture> ImportSnapshot(Task<std::optional<Bitmap>> read) {
  using R = detail::ImportedTexture;
  try {
    const auto snapshot = co_await std::move(read);
    if (!snapshot) co_return R::Failure({kInvalidFrame, "The source texture has no published frame"});
    co_return R::Success(RetainBitmap(*snapshot));
  } catch (const std::exception& error) {
    co_return R::Failure({kUnsupported, error.what()});
  }
}

detail::ImportedTexture AcquireTexture(RenderBackend& backend, const ExternalTexture& texture) {
  try {
    return detail::AcquireBgfxTexture(backend, texture);
  } catch (const std::exception& error) {
    return detail::ImportedTexture::Failure({kUnsupported, error.what()});
  }
}

} // namespace

Task<Result<ColorSurface, RenderError>> ImportBgfxTexture(
    RenderBackend& backend, std::shared_ptr<const ExternalTexture> texture, TextureTransferPolicy policy) {
  using R = detail::ImportedTexture;
  if (!texture) return CompleteImport(R::Failure({kInvalidFrame, "Expected an SDK texture"}));
  auto imported = AcquireTexture(backend, *texture);
  if (imported.Succeeded() || policy == TextureTransferPolicy::GpuOnly) {
    return CompleteImport(std::move(imported));
  }
  try {
    return ImportSnapshot(texture->SnapshotAsync());
  } catch (const std::exception& error) {
    return CompleteImport(R::Failure({kUnsupported, error.what()}));
  }
}

Task<Result<ColorSurface, RenderError>> ImportBgfxVideoFrame(
    RenderBackend& backend, media::VideoFrame frame, media::VideoTextureOptions options, TextureTransferPolicy policy) {
  auto result = detail::ImportDecodedBgfxFrame(backend, frame, options, policy);
  return CompleteImport(result.Succeeded() ? detail::ImportedTexture::Success(std::move(result).Value().surface)
                                           : detail::ImportedTexture::Failure(result.Error()));
}

detail::ImportedFrame detail::ImportDecodedBgfxFrame(RenderBackend& backend, const media::VideoFrame& frame,
                                                     media::VideoTextureOptions options, TextureTransferPolicy policy) {
  using R = ImportedFrame;
  if (!frame.Validate() || !options.width || !options.height || options.width > 8192 || options.height > 8192 ||
      (options.fit != ImageFit::Contain && options.fit != ImageFit::Cover)) {
    return R::Failure({kInvalidFrame, "Invalid video frame or texture options"});
  }
  const auto fit = avkit::detail::FitVideoTexture(frame.format, options);
  const auto wrap = [&](ImportedTexture surface) {
    if (!surface.Succeeded()) return R::Failure(surface.Error());
    return R::Success({std::move(surface).Value(), fit.SourceToTextureUv(options)});
  };
  if (frame.Storage() == media::VideoStorage::Platform) {
    auto texture = media::MakeExternalTexture(frame, options);
    if (!texture.Succeeded()) return R::Failure({kUnsupported, texture.Error().message});
    return wrap(AcquireTexture(backend, *texture.Value()));
  }
  if (policy == TextureTransferPolicy::GpuOnly) {
    return R::Failure({kUnsupported, "A GPU transfer requires a platform video frame"});
  }
  try {
    return wrap(ImportCpuVideo(frame, options, fit));
  } catch (const std::bad_alloc&) {
    return R::Failure({RenderErrorCode::OutOfMemory, "Cannot allocate the CPU video upload"});
  }
}

Result<VideoPreviewImage, RenderError> MakeBgfxPreviewImage(
    RenderBackend& backend, const ColorSurface& surface, media::MediaTimestamp pts, TextureTransferPolicy policy) {
  using R = Result<VideoPreviewImage, RenderError>;
  auto published = detail::PublishBgfxTexture(backend, surface, pts);
  if (published.Succeeded()) return R::Success(std::move(published).Value());
  if (policy == TextureTransferPolicy::GpuOnly || published.Error().code != kUnsupported) {
    return R::Failure(published.Error());
  }
  auto bitmap = ReadBitmap(backend, surface);
  if (!bitmap.Succeeded()) return R::Failure(bitmap.Error());
  return R::Success(std::move(bitmap).Value());
}

Result<media::VideoFrame, RenderError> MakeBgfxEncoderFrame(
    RenderBackend& backend, const ColorSurface& surface, const media::VideoEncoderConfig& config,
    media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration) {
  using R = detail::EncoderPicture;
  const auto& format = config.input;
  if (!format.Validate() || surface.pixels.image.size != PixelSize{format.width, format.height} ||
      pts.timescale <= 0 || (duration && (duration->timescale <= 0 || duration->ticks <= 0))) {
    return R::Failure({kInvalidFrame, "The composite does not match the encoder input geometry or timing"});
  }
  auto frame = detail::EncodeBgfxTexture(backend, surface, config, pts, duration);
  if (frame.Succeeded() || config.input_storage == media::VideoStorage::Platform || frame.Error().code != kUnsupported) {
    return frame;
  }
  return ReadEncoderPicture(backend, surface, format, pts, duration);
}

} // namespace huxerui::avkit::render
