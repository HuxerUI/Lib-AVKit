#include "ncnn_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <datareader.h>

namespace huxerui::avkit::analysis::detail {

using namespace media;

namespace {

struct I420Color {
  explicit I420Color(const VideoFormat& format)
      : full(format.range == ColorRange::Full),
        kr(format.matrix == ColorMatrix::BT601 ? 0.299F : 0.2126F),
        kb(format.matrix == ColorMatrix::BT601 ? 0.114F : 0.0722F), kg(1.F - kr - kb),
        luma_scale(full ? 1.F : 255.F / 219.F), chroma_scale(full ? 1.F : 255.F / 224.F) {}

  std::array<float, 3> Rgb(float y, float u, float v) const {
    const auto luma = (y - (full ? 0 : 16)) * luma_scale;
    const auto cb = (u - 128) * chroma_scale;
    const auto cr = (v - 128) * chroma_scale;
    return {luma + 2 * (1 - kr) * cr,
            luma - 2 * (kb * (1 - kb) * cb + kr * (1 - kr) * cr) / kg,
            luma + 2 * (1 - kb) * cb};
  }

  bool full;
  float kr, kb, kg, luma_scale, chroma_scale;
};

} // namespace

CpuImage ConvertPicture(const VideoFrame& image, std::stop_token stop) {
  const auto& format = image.format;
  const auto& visible = format.visible;
  const Plane y(image.planes[0]), u(image.planes[1]), v(image.planes[2]);
  const I420Color color(format);
  const auto encode = [](float value) { return static_cast<unsigned char>(std::clamp(std::lround(value), 0L, 255L)); };
  std::vector<unsigned char> bgr(std::size_t(visible.width) * visible.height * 3);
  for (std::uint32_t row = 0; row < visible.height; ++row) {
    if (stop.stop_requested()) return {};
    for (std::uint32_t column = 0; column < visible.width; ++column) {
      const auto sx = column + visible.x, sy = row + visible.y;
      const auto rgb = color.Rgb(y.At(sx, sy), u.At(sx / 2, sy / 2), v.At(sx / 2, sy / 2));
      const auto offset = (std::size_t(row) * visible.width + column) * 3;
      bgr[offset] = encode(rgb[2]);
      bgr[offset + 1] = encode(rgb[1]);
      bgr[offset + 2] = encode(rgb[0]);
    }
  }
  return {int(visible.width), int(visible.height), std::move(bgr)};
}

ncnn::Mat ResizePictureRgb(const VideoFrame& image, int width, int height, std::stop_token stop) {
  const auto& visible = image.format.visible;
  const I420Color color(image.format);
  const Plane luma(image.planes[0]), cb(image.planes[1]), cr(image.planes[2]);
  ncnn::Mat input(width, height, 3);
  if (input.empty()) throw std::runtime_error("Cannot allocate ncnn image input");
  for (int row = 0; row < height; ++row) {
    if (stop.stop_requested()) return {};
    const auto y = std::clamp((row + 0.5F) * visible.height / height - 0.5F, 0.F, float(visible.height - 1));
    const auto y0 = visible.y + std::uint32_t(y);
    const auto y1 = std::min(y0 + 1, visible.y + visible.height - 1);
    const auto chroma_y = (visible.y + std::uint32_t(y + 0.5F)) / 2;
    const auto dy = y - std::floor(y);
    std::array<float*, 3> rgb_rows{input.channel(0).row(row), input.channel(1).row(row), input.channel(2).row(row)};
    for (int column = 0; column < width; ++column) {
      const auto x = std::clamp((column + 0.5F) * visible.width / width - 0.5F, 0.F, float(visible.width - 1));
      const auto x0 = visible.x + std::uint32_t(x);
      const auto x1 = std::min(x0 + 1, visible.x + visible.width - 1);
      const auto chroma_x = (visible.x + std::uint32_t(x + 0.5F)) / 2;
      const auto dx = x - std::floor(x);
      const auto y_sample = std::lerp(std::lerp(float(luma.At(x0, y0)), float(luma.At(x1, y0)), dx),
                                     std::lerp(float(luma.At(x0, y1)), float(luma.At(x1, y1)), dx), dy);
      const auto rgb = color.Rgb(y_sample, cb.At(chroma_x, chroma_y), cr.At(chroma_x, chroma_y));
      for (std::size_t channel = 0; channel < rgb.size(); ++channel)
        rgb_rows[channel][column] = std::clamp(rgb[channel] / 255.F, 0.F, 1.F);
    }
  }
  return input;
}

Network::Network(const NcnnModelAssets& assets, std::span<const char* const> outputs, const char* name)
    : weights_(assets.weights.ReadBytes()) {
  const auto parameters = assets.parameters.ReadBytes();
  if (parameters.empty() || weights_.empty()) throw std::runtime_error(std::string(name) + " needs a graph and weights");
  net_.opt.use_vulkan_compute = false;
  net_.opt.use_fp16_storage = false;
  net_.opt.use_fp16_arithmetic = false;
  net_.opt.use_bf16_storage = false;
  net_.opt.num_threads = 1;
  auto graph = reinterpret_cast<const unsigned char*>(parameters.data());
  const ncnn::DataReaderFromMemory graph_reader(graph, parameters.size());
  if (net_.load_param(graph_reader) || net_.input_names().size() != 1 ||
      std::string_view(net_.input_names().front()) != "in0" || net_.output_names().size() != outputs.size())
    throw std::runtime_error(std::string("Invalid ") + name + " graph");
  for (const auto* output : outputs) {
    if (std::ranges::none_of(net_.output_names(), [output](const char* candidate) {
          return std::string_view(candidate) == output;
        })) throw std::runtime_error(std::string("Invalid ") + name + " output names");
  }
  const auto* begin = reinterpret_cast<const unsigned char*>(weights_.data());
  auto data = begin;
  const ncnn::DataReaderFromMemory weights_reader(data, weights_.size());
  if (net_.load_model(weights_reader) || data != begin + weights_.size())
    throw std::runtime_error(std::string("Invalid ") + name + " weights");
}

ncnn::Extractor Network::Extract(const ncnn::Mat& input) const {
  auto extractor = net_.create_extractor();
  if (input.empty() || extractor.input("in0", input)) throw std::runtime_error("Cannot set ncnn image input");
  return extractor;
}

} // namespace huxerui::avkit::analysis::detail
