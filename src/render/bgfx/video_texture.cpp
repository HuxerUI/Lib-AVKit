#include "video_texture_internal.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>
#include <vector>

#include "render/bgfx/backend.h"

namespace huxerui::avkit::render {

namespace {

constexpr RenderErrorCode kUnsupported = RenderErrorCode::Unsupported;
constexpr RenderErrorCode kInvalidFrame = RenderErrorCode::InvalidFrame;

struct Yuv {
  float y, u, v;
};

// BT.1886 display transfer for normalized SDR black and white, matching the platform BT.709 output.
constexpr double kBt709DisplayGamma = 2.4;
float EncodeTransfer(std::byte sample, media::ColorTransfer transfer) {
  const float value = std::to_integer<unsigned>(sample) / 255.0F;
  if (transfer == media::ColorTransfer::SRGB || transfer == media::ColorTransfer::Unknown) return value;
  const float linear = value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
  if (transfer == media::ColorTransfer::Linear) return linear;
  return static_cast<float>(std::pow(linear, 1.0 / kBt709DisplayGamma));
}

detail::EncoderPicture MakeEncoderPicture(std::span<const std::byte> source, bool bgra, RowOrigin origin,
                                         bool straight_alpha, const media::VideoFormat& format,
                                         media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration) {
  using R = detail::EncoderPicture;
  using namespace media;
  const bool nv12 = format.pixel_format == VideoPixelFormat::NV12;
  if ((!nv12 && format.pixel_format != VideoPixelFormat::I420) || format.matrix == ColorMatrix::BT2020 ||
      format.primaries == ColorPrimaries::BT2020 || format.transfer == ColorTransfer::PQ ||
      format.transfer == ColorTransfer::HLG) {
    return R::Failure({kUnsupported, "CPU texture transfer requires an eight-bit SDR I420 or NV12 encoder"});
  }
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
  const float kr = format.matrix == ColorMatrix::BT601 ? 0.299F : 0.2126F;
  const float kb = format.matrix == ColorMatrix::BT601 ? 0.114F : 0.0722F;
  const auto sample = [&](std::size_t x, std::size_t y) {
    const auto row = origin == RowOrigin::TopLeft ? y : height - 1 - y;
    const auto* color = source.data() + (row * width + x) * 4;
    const auto channel = [&](unsigned index) {
      const auto value = straight_alpha ? std::byte((std::to_integer<unsigned>(color[index]) *
          std::to_integer<unsigned>(color[3]) + 127) / 255) : color[index];
      return EncodeTransfer(value, format.transfer);
    };
    const auto r = channel(bgra ? 2 : 0), g = channel(1), b = channel(bgra ? 0 : 2);
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

detail::EncoderPicture ReadEncoderPicture(RenderBackend& backend, const Texture& surface,
                                         const media::VideoFormat& format, media::MediaTimestamp pts,
                                         std::optional<media::MediaTimestamp> duration) {
  auto read = ReadBgfxPixels(backend, surface);
  if (!read.Succeeded()) return detail::EncoderPicture::Failure(read.Error());
  return MakeEncoderPicture(read.Value().AsBytes(), surface.Info().format == PixelFormat::Bgra8Unorm,
                            surface.Origin(), false, format, pts, duration);
}

} // namespace

detail::EncoderPicture detail::EncodeCpuBitmap(const Bitmap& bitmap, const media::VideoFormat& format,
                                              media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration) {
  if (!bitmap.HasValue() || bitmap.PixelWidth() != format.width || bitmap.PixelHeight() != format.height)
    return EncoderPicture::Failure({kInvalidFrame, "Bitmap must match the encoder's coded size"});
  return MakeEncoderPicture(bitmap.Pixels(), false, RowOrigin::TopLeft, true, format, pts, duration);
}

Result<media::VideoFrame, RenderError> MakeBgfxEncoderFrame(
    RenderBackend& backend, const Texture& surface, const media::VideoEncoderConfig& config,
    media::MediaTimestamp pts, std::optional<media::MediaTimestamp> duration) {
  using R = detail::EncoderPicture;
  const auto& format = config.input;
  if (!format.Validate() || surface.Info().size != PixelSize{format.width, format.height} ||
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
