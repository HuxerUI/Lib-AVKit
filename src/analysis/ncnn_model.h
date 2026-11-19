#pragma once

#include <huxerui/avkit/ncnn.h>

#include <net.h>

namespace huxerui::avkit::analysis::detail {

struct CpuImage {
  int width{}, height{};
  std::vector<unsigned char> bgr;
};

CpuImage ConvertPicture(const media::VideoFrame& image, std::stop_token stop);

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

struct BodyHandModels;
std::shared_ptr<const BodyHandModels> LoadBodyHand(const NcnnBodyHandModels& models);
std::unique_ptr<AnalysisSession> OpenBodyHand(std::shared_ptr<const BodyHandModels> models, const AnalysisConfig& config);

} // namespace huxerui::avkit::analysis::detail
