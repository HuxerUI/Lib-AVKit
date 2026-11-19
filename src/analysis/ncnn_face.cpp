#include "ncnn_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <datareader.h>
#include <net.h>

namespace huxerui::avkit::analysis {
namespace detail {

using namespace media;

class Plane {
public:
  explicit Plane(const VideoPlane& plane)
      : bytes_(plane.data.AsBytes()), row_stride_(plane.row_stride), pixel_stride_(plane.pixel_stride) {}

  int At(std::uint32_t x, std::uint32_t y) const {
    return std::to_integer<int>(bytes_[y * row_stride_ + x * pixel_stride_]);
  }

private:
  std::span<const std::byte> bytes_;
  std::size_t row_stride_, pixel_stride_;
};

CpuImage ConvertPicture(const VideoFrame& image, std::stop_token stop) {
  const auto& format = image.format;
  const auto& visible = format.visible;
  const Plane y(image.planes[0]), u(image.planes[1]), v(image.planes[2]);
  const bool full = format.range == ColorRange::Full;
  const auto kr = format.matrix == ColorMatrix::BT601 ? 0.299F : 0.2126F;
  const auto kb = format.matrix == ColorMatrix::BT601 ? 0.114F : 0.0722F;
  const float kg = 1.F - kr - kb;
  const float luma_scale = full ? 1.F : 255.F / 219.F;
  const float chroma_scale = full ? 1.F : 255.F / 224.F;
  const auto encode = [](float value) { return static_cast<unsigned char>(std::clamp(std::lround(value), 0L, 255L)); };
  std::vector<unsigned char> bgr(std::size_t(visible.width) * visible.height * 3);
  for (std::uint32_t row = 0; row < visible.height; ++row) {
    if (stop.stop_requested()) return {};
    for (std::uint32_t column = 0; column < visible.width; ++column) {
      const auto sx = column + visible.x, sy = row + visible.y;
      const float luma = (y.At(sx, sy) - (full ? 0 : 16)) * luma_scale;
      const float cb = (u.At(sx / 2, sy / 2) - 128) * chroma_scale;
      const float cr = (v.At(sx / 2, sy / 2) - 128) * chroma_scale;
      const auto offset = (std::size_t(row) * visible.width + column) * 3;
      bgr[offset] = encode(luma + 2 * (1 - kb) * cb);
      bgr[offset + 1] = encode(luma - 2 * (kb * (1 - kb) * cb + kr * (1 - kr) * cr) / kg);
      bgr[offset + 2] = encode(luma + 2 * (1 - kr) * cr);
    }
  }
  return {int(visible.width), int(visible.height), std::move(bgr)};
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

} // namespace detail
namespace {

using namespace media;
using namespace detail;
using Detections = std::vector<TrackingDetection>;
using DetectionResult = Result<std::optional<Detections>, std::string>;
using ProviderResult = Result<std::shared_ptr<AnalysisProvider>, std::string>;

constexpr int kInputSize = 640;
constexpr float kDetectionThreshold = 0.70F;
constexpr float kOverlapThreshold = 0.30F;
constexpr std::array kOutputNames{"out0", "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8", "out9",
                                   "out10", "out11"};
constexpr std::array kPointNames{"right_eye", "left_eye", "nose_tip", "right_mouth_corner", "left_mouth_corner"};

struct Face {
  Rect bounds;
  float score;
  std::array<Point, kPointNames.size()> points;
};

float Overlap(const Rect& a, const Rect& b) {
  const auto width = std::max(0.F, std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x));
  const auto height = std::max(0.F, std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y));
  const auto area = width * height;
  return area / (a.width * a.height + b.width * b.height - area);
}

bool HasOutputShape(const ncnn::Mat& output, int cells, int values) {
  return output.dims == 2 && output.h == cells && output.w == values && output.elempack == 1 &&
         output.elemsize == sizeof(float);
}

void DecodeFaces(std::vector<Face>& faces, const ncnn::Mat& classes, const ncnn::Mat& objects,
                 const ncnn::Mat& boxes, const ncnn::Mat& landmarks, int stride) {
  const auto side = kInputSize / stride;
  const auto cells = side * side;
  if (!HasOutputShape(classes, cells, 1) || !HasOutputShape(objects, cells, 1) ||
      !HasOutputShape(boxes, cells, 4) || !HasOutputShape(landmarks, cells, 10))
    throw std::runtime_error("Unexpected YuNet output shape");

  const float step = float(stride) / kInputSize;
  for (int index = 0; index < cells; ++index) {
    const auto score = std::sqrt(std::clamp(classes.row(index)[0], 0.F, 1.F) *
                                 std::clamp(objects.row(index)[0], 0.F, 1.F));
    if (!(score >= kDetectionThreshold)) continue;
    const int column = index % side, row = index / side;
    const auto* box = boxes.row(index);
    const auto width = std::exp(box[2]) * step, height = std::exp(box[3]) * step;
    const auto x = (column + box[0]) * step - width / 2;
    const auto y = (row + box[1]) * step - height / 2;
    if (!(width > 0 && height > 0 && std::isfinite(width * height) && std::isfinite(x) && std::isfinite(y))) continue;
    Face face{{x, y, width, height}, score, {}};
    const auto* points = landmarks.row(index);
    for (std::size_t i = 0; i < face.points.size(); ++i)
      face.points[i] = {(column + points[2 * i]) * step, (row + points[2 * i + 1]) * step};
    faces.push_back(face);
  }
}

class YuNet {
public:
  explicit YuNet(const NcnnModelAssets& assets) : model_(assets, kOutputNames, "YuNet") {}

  std::optional<Face> Detect(const ncnn::Mat& input) const {
    auto extractor = model_.Extract(input);
    std::array<ncnn::Mat, kOutputNames.size()> outputs;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      if (extractor.extract(kOutputNames[i], outputs[i])) throw std::runtime_error("Cannot extract YuNet landmarks");
    }
    std::vector<Face> faces;
    constexpr std::array strides{8, 16, 32};
    for (std::size_t i = 0; i < strides.size(); ++i)
      DecodeFaces(faces, outputs[i], outputs[i + 3], outputs[i + 6], outputs[i + 9], strides[i]);
    if (faces.empty()) return std::nullopt;
    const auto best = std::ranges::max_element(faces, {}, &Face::score);
    if (std::ranges::any_of(faces, [&](const Face& face) {
          return &face != &*best && Overlap(face.bounds, best->bounds) <= kOverlapThreshold;
        })) return std::nullopt;
    return *best;
  }

private:
  Network model_;
};

constexpr int kPatchSize = 8;
constexpr float kReferenceSimilarity = 0.78F;
using FacePatch = std::array<float, kPatchSize * kPatchSize>;

std::optional<FacePatch> Describe(const VideoFrame& frame, const Face& face) {
  const auto& area = frame.format.visible;
  const float width = float(area.width), height = float(area.height);
  const auto& right = face.points[0];
  const auto& left = face.points[1];
  const Point origin{(left.x + right.x) * width / 2, (left.y + right.y) * height / 2};
  const Point axis{(left.x - right.x) * width, (left.y - right.y) * height};
  if (!(std::hypot(axis.x, axis.y) >= 2.F)) return std::nullopt;

  // Normalize the luma patch by eye position, scale, roll, and brightness before comparing it.
  const Plane y(frame.planes[0]);
  FacePatch patch;
  constexpr std::array offsets{-0.5F, 0.F, 0.5F};
  for (int row = 0; row < kPatchSize; ++row) {
    for (int col = 0; col < kPatchSize; ++col) {
      float luma = 0;
      for (const auto vertical : offsets) {
        const float v = (row + 0.5F + vertical) / kPatchSize * 1.85F - 0.65F;
        for (const auto horizontal : offsets) {
          const float u = (col + 0.5F + horizontal) / kPatchSize * 1.65F - 0.825F;
          const auto sx = origin.x + u * axis.x - v * axis.y;
          const auto sy = origin.y + u * axis.y + v * axis.x;
          if (!(sx >= 0 && sy >= 0 && sx < width && sy < height)) return std::nullopt;
          luma += y.At(area.x + std::uint32_t(sx), area.y + std::uint32_t(sy));
        }
      }
      patch[row * kPatchSize + col] = luma / (offsets.size() * offsets.size());
    }
  }
  const float mean = std::accumulate(patch.begin(), patch.end(), 0.F) / patch.size();
  for (auto& value : patch) value -= mean;
  const auto energy = std::inner_product(patch.begin(), patch.end(), patch.begin(), 0.F);
  if (energy < 16.F * patch.size()) return std::nullopt;
  const auto length = std::sqrt(energy);
  for (auto& value : patch) value /= length;
  return patch;
}

TrackingDetection Landmarks(const Face& face) {
  TrackingDetection result{{1, {}}, {}};
  result.landmarks.points.reserve(face.points.size() + 1);
  result.landmarks.points.push_back({"face_center", {face.bounds.x + face.bounds.width / 2,
                                                    face.bounds.y + face.bounds.height / 2}, {}});
  for (std::size_t i = 0; i < face.points.size(); ++i)
    result.landmarks.points.push_back({kPointNames[i], face.points[i], {}});
  return result;
}

class FaceSession final : public AnalysisSession {
public:
  FaceSession(std::shared_ptr<const YuNet> model, AnalysisConfig config) : model_(std::move(model)), config_(config) {}

  void Submit(AnalysisJob job, std::stop_token stop, Completion complete) override {
    auto result = Detect(job, stop);
    job.frames.clear();
    complete({job.id, job.target, std::move(result)});
  }

private:
  DetectionResult Detect(const AnalysisJob& job, std::stop_token stop) {
    if (stop.stop_requested()) return DetectionResult::Success(std::nullopt);
    if (job.frames.size() != 1 || job.frames.front().key != job.target || (!config_.uses_roi && !job.regions.empty()))
      return DetectionResult::Failure("YuNet needs exactly one target picture");
    const auto& frame = job.frames.front().image;
    if (frame.format != config_.format || frame.Storage() != config_.storage || frame.pts != job.target.pts ||
        !frame.Validate()) return DetectionResult::Failure("YuNet picture does not match the opened input format or PTS");
    if (referenced_ && !reference_) return DetectionResult::Success(Detections{});

    try {
      const auto picture = ConvertPicture(frame, stop);
      if (stop.stop_requested()) return DetectionResult::Success(std::nullopt);
      if (config_.uses_roi) {
        auto detections = DetectRegions(picture, job.regions, stop);
        if (stop.stop_requested()) return DetectionResult::Success(std::nullopt);
        return DetectionResult::Success(std::move(detections));
      }
      const auto input = ncnn::Mat::from_pixels_resize(picture.bgr.data(), ncnn::Mat::PIXEL_BGR, picture.width,
                                                       picture.height, kInputSize, kInputSize);
      const auto face = model_->Detect(input);
      const auto appearance = face ? Describe(frame, *face) : std::nullopt;
      if (stop.stop_requested()) return DetectionResult::Success(std::nullopt);
      if (!referenced_) {
        reference_ = appearance;
        referenced_ = true;
      }
      if (!appearance ||
          std::inner_product(appearance->begin(), appearance->end(), reference_->begin(), 0.F) < kReferenceSimilarity)
        return DetectionResult::Success(Detections{});
      return DetectionResult::Success(Detections{Landmarks(*face)});
    } catch (const std::exception& error) {
      return DetectionResult::Failure(error.what());
    }
  }

  Detections DetectRegions(const CpuImage& image, const std::vector<AnalysisRegion>& regions,
                           std::stop_token stop) const {
    Detections detections;
    for (std::size_t i = 0; i < regions.size() && !stop.stop_requested(); ++i) {
      const auto& region = regions[i];
      const auto& bounds = region.bounds;
      if (region.part != TrackedPart::Face || !(bounds.width > 0 && bounds.height > 0)) continue;
      const ModelCrop crop{{(bounds.x + bounds.width / 2) * image.width, (bounds.y + bounds.height / 2) * image.height},
                           {1, 0}, {0, 1}, bounds.width * image.width, bounds.height * image.height};
      const auto input = Warp(image, crop, kInputSize, 0, 255, true);
      auto face = model_->Detect(input);
      if (!face) continue;
      auto detection = Landmarks(*face);
      detection.region = i;
      detection.landmarks.subject_id = region.person.value;
      for (auto& landmark : detection.landmarks.points) {
        landmark.position = {bounds.x + landmark.position.x * bounds.width, bounds.y + landmark.position.y * bounds.height};
      }
      detections.push_back(std::move(detection));
    }
    return detections;
  }

  std::shared_ptr<const YuNet> model_;
  AnalysisConfig config_;
  std::optional<FacePatch> reference_;
  bool referenced_{};
};

class NcnnProvider final : public AnalysisProvider {
public:
  NcnnProvider(std::shared_ptr<const YuNet> face, std::shared_ptr<const BodyHandModels> body_hand)
      : face_(std::move(face)), body_hand_(std::move(body_hand)) {}

  Result<AnalysisWindow, std::string> Probe(const AnalysisConfig& config) const override {
    using ProbeResult = Result<AnalysisWindow, std::string>;
    switch (config.capability) {
      case AnalysisCapability::FaceLandmarks:
        if (!face_) return ProbeResult::Failure("YuNet model pack is missing");
        break;
      case AnalysisCapability::BodyLandmarks:
        if (!body_hand_ || config.uses_roi) return ProbeResult::Failure("Body landmarks require a full picture and a body pack");
        break;
      case AnalysisCapability::HandLandmarks:
        if (!body_hand_ || !config.uses_roi) return ProbeResult::Failure("Hand landmarks require body regions and a hand pack");
        break;
    }
    const auto& format = config.format;
    if (config.storage != VideoStorage::Cpu || format.pixel_format != VideoPixelFormat::I420 ||
        format.rotation != VideoRotation::None || format.matrix == ColorMatrix::BT2020 ||
        format.transfer == ColorTransfer::PQ || format.transfer == ColorTransfer::HLG ||
        format.transfer == ColorTransfer::Linear || !format.Validate())
      return ProbeResult::Failure("ncnn requires unrotated SDR I420 CPU pictures");
    const auto& visible = format.visible;
    if (visible.width > std::numeric_limits<int>::max() / 3 || visible.height > std::numeric_limits<int>::max() ||
        std::size_t(visible.width) > std::numeric_limits<std::size_t>::max() / 3 / visible.height)
      return ProbeResult::Failure("YuNet input dimensions exceed the addressable image size");
    return ProbeResult::Success(AnalysisWindow{.stateful = config.capability == AnalysisCapability::BodyLandmarks});
  }

  std::unique_ptr<AnalysisSession> Open(const AnalysisConfig& config) override {
    if (!Probe(config).Succeeded()) return {};
    if (config.capability == AnalysisCapability::FaceLandmarks) return std::make_unique<FaceSession>(face_, config);
    return OpenBodyHand(body_hand_, config);
  }

private:
  std::shared_ptr<const YuNet> face_;
  std::shared_ptr<const BodyHandModels> body_hand_;
};

} // namespace

ProviderResult CreateNcnnAnalysisProvider(const NcnnAnalysisModels& models) {
  try {
    if (!models.face && !models.body_hand) return ProviderResult::Failure("At least one ncnn model pack is required");
    auto face = models.face ? std::make_shared<YuNet>(*models.face) : nullptr;
    auto body_hand = models.body_hand ? LoadBodyHand(*models.body_hand) : nullptr;
    return ProviderResult::Success(std::make_shared<NcnnProvider>(std::move(face), std::move(body_hand)));
  } catch (const std::exception& error) {
    return ProviderResult::Failure(error.what());
  }
}

ProviderResult CreateNcnnFaceAnalysisProvider(const RawAsset& parameters, const RawAsset& weights) {
  return CreateNcnnAnalysisProvider({NcnnModelAssets{parameters, weights}, {}});
}

} // namespace huxerui::avkit::analysis
