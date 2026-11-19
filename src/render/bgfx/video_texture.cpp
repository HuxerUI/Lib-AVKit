#include "video_texture_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

#include <huxerui/avkit/bgfx.h>

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

double SamplePlane(const media::VideoPlane& plane, media::VideoRectangle bounds, double x, double y,
                   std::size_t channel = 0) {
  const auto right = bounds.x + bounds.width - 1;
  const auto bottom = bounds.y + bounds.height - 1;
  x = std::clamp(x, double(bounds.x), double(right));
  y = std::clamp(y, double(bounds.y), double(bottom));
  const auto x0 = static_cast<std::uint32_t>(x), y0 = static_cast<std::uint32_t>(y);
  const auto x1 = std::min(x0 + 1, right), y1 = std::min(y0 + 1, bottom);
  const auto bytes = plane.data.AsBytes();
  const auto read = [&](std::uint32_t col, std::uint32_t row) {
    return double(std::to_integer<unsigned>(bytes[row * plane.row_stride + col * plane.pixel_stride + channel]));
  };
  return std::lerp(std::lerp(read(x0, y0), read(x1, y0), x - x0),
                   std::lerp(read(x0, y1), read(x1, y1), x - x0), y - y0);
}

detail::ImportedTexture ImportCpuVideo(const media::VideoFrame& frame, media::VideoTextureOptions options) {
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
  double width = double(visible.width) * format.aspect_width / format.aspect_height;
  double height = visible.height;
  if (format.rotation == VideoRotation::Clockwise90 || format.rotation == VideoRotation::Clockwise270)
    std::swap(width, height);
  const auto scale_x = options.width / width, scale_y = options.height / height;
  VideoRectangle area{0, 0, options.width, options.height};
  double crop_width = 1, crop_height = 1;
  if (options.fit == ImageFit::Cover) {
    const auto scale = std::max(scale_x, scale_y);
    crop_width = scale_x / scale;
    crop_height = scale_y / scale;
  } else {
    const auto scale = std::min(scale_x, scale_y);
    area.width = static_cast<std::uint32_t>(std::clamp(std::lround(width * scale), 1L, long(options.width)));
    area.height = static_cast<std::uint32_t>(std::clamp(std::lround(height * scale), 1L, long(options.height)));
    area.x = (options.width - area.width) / 2;
    area.y = (options.height - area.height) / 2;
  }
  const VideoRectangle chroma{visible.x / 2, visible.y / 2,
      (visible.x + visible.width - 1) / 2 - visible.x / 2 + 1,
      (visible.y + visible.height - 1) / 2 - visible.y / 2 + 1};
  const bool full = format.range == ColorRange::Full;
  const double kr = format.matrix == ColorMatrix::BT709 ? 0.2126 : 0.299;
  const double kb = format.matrix == ColorMatrix::BT709 ? 0.0722 : 0.114;
  const auto byte = [](double value) { return std::byte(std::lround(std::clamp(value, 0., 1.) * 255)); };

  const auto stride = std::size_t(options.width) * 4;
  auto bytes = std::make_shared<Bytes>(stride * options.height);
  for (std::size_t i = 3; i < bytes->size(); i += 4) (*bytes)[i] = std::byte{255};
  for (std::uint32_t y = 0; y < area.height; ++y) {
    for (std::uint32_t x = 0; x < area.width; ++x) {
      const auto u = 0.5 + ((x + 0.5) / area.width - 0.5) * crop_width;
      const auto v = 0.5 + ((y + 0.5) / area.height - 0.5) * crop_height;
      auto sx = u, sy = v;
      switch (format.rotation) {
        case VideoRotation::None: break;
        case VideoRotation::Clockwise90: sx = v; sy = 1 - u; break;
        case VideoRotation::Clockwise180: sx = 1 - u; sy = 1 - v; break;
        case VideoRotation::Clockwise270: sx = 1 - v; sy = u; break;
      }
      sx = visible.x + sx * visible.width;
      sy = visible.y + sy * visible.height;
      const auto luma = (SamplePlane(frame.planes[0], visible, sx - 0.5, sy - 0.5) - (full ? 0 : 16)) /
          (full ? 255 : 219);
      const auto cb = (SamplePlane(frame.planes[1], chroma, sx / 2 - 0.5, sy / 2 - 0.5) - 128) / (full ? 255 : 224);
      const auto cr = (SamplePlane(frame.planes[planar ? 2 : 1], chroma, sx / 2 - 0.5, sy / 2 - 0.5, planar ? 0 : 1) -
          128) / (full ? 255 : 224);
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
  return CompleteImport(detail::ImportDecodedBgfxFrame(backend, frame, options, policy));
}

detail::ImportedTexture detail::ImportDecodedBgfxFrame(RenderBackend& backend, const media::VideoFrame& frame,
                                                        media::VideoTextureOptions options, TextureTransferPolicy policy) {
  using R = ImportedTexture;
  if (!frame.Validate() || !options.width || !options.height || options.width > 8192 || options.height > 8192 ||
      (options.fit != ImageFit::Contain && options.fit != ImageFit::Cover)) {
    return R::Failure({kInvalidFrame, "Invalid video frame or texture options"});
  }
  if (frame.Storage() == media::VideoStorage::Platform) {
    auto texture = media::MakeExternalTexture(frame, options);
    if (!texture.Succeeded()) return R::Failure({kUnsupported, texture.Error().message});
    return AcquireTexture(backend, *texture.Value());
  }
  if (policy == TextureTransferPolicy::GpuOnly) {
    return R::Failure({kUnsupported, "A GPU transfer requires a platform video frame"});
  }
  try {
    return ImportCpuVideo(frame, options);
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
