#pragma once

#include <huxerui/avkit/ncnn.h>

#include <net.h>

namespace huxerui::avkit::analysis::detail {

class Plane {
public:
  explicit Plane(const media::VideoPlane& plane)
      : bytes_(plane.data.AsBytes()), row_stride_(plane.row_stride), pixel_stride_(plane.pixel_stride) {}

  int At(std::uint32_t x, std::uint32_t y) const {
    return std::to_integer<int>(bytes_[y * row_stride_ + x * pixel_stride_]);
  }

private:
  std::span<const std::byte> bytes_;
  std::size_t row_stride_, pixel_stride_;
};

struct CpuImage {
  int width{}, height{};
  std::vector<unsigned char> bgr;
};

CpuImage ConvertPicture(const media::VideoFrame& image, std::stop_token stop);

ncnn::Mat ResizePictureRgb(const media::VideoFrame& image, int width, int height, std::stop_token stop);

/// The model input is planar RGB. Coordinates in the affine crop are in visible source pixels.
struct ModelCrop {
  Point center;
  Point right;
  Point down;
  float width, height;

  Point ToSource(Point position) const;
};

ncnn::Mat Warp(const CpuImage& picture, const ModelCrop& crop, int size, float low = 0, float high = 1, bool bgr = false);

class Network {
public:
  Network(const NcnnModelAssets& assets, std::span<const char* const> outputs, const char* name);

  ncnn::Extractor Extract(const ncnn::Mat& input) const;

private:
  Bytes weights_;
  ncnn::Net net_;
};

class YuNet;

std::shared_ptr<const YuNet> LoadFace(const NcnnModelAssets& assets);

std::unique_ptr<AnalysisSession> OpenFace(std::shared_ptr<const YuNet> model, const AnalysisConfig& config);

struct BodyHandModels;

std::shared_ptr<const BodyHandModels> LoadBodyHand(const NcnnBodyHandModels& models);

std::unique_ptr<AnalysisSession> OpenBodyHand(std::shared_ptr<const BodyHandModels> models, const AnalysisConfig& config);

struct PortraitModel;

std::shared_ptr<PortraitModel> LoadPortrait(const NcnnModelAssets& assets, const char* name);

std::unique_ptr<AnalysisSession> OpenPortrait(std::shared_ptr<PortraitModel> model, const AnalysisConfig& config);

} // namespace huxerui::avkit::analysis::detail
