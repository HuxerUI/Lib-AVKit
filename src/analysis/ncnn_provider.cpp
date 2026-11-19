#include "ncnn_model.h"

#include <limits>
#include <utility>

namespace huxerui::avkit::analysis {
namespace {

using namespace media;
using namespace detail;
using ProviderResult = Result<std::shared_ptr<AnalysisProvider>, std::string>;

class NcnnProvider final : public AnalysisProvider {
public:
  NcnnProvider(std::shared_ptr<const YuNet> face, std::shared_ptr<const BodyHandModels> body_hand,
               std::shared_ptr<PortraitModel> portrait, std::shared_ptr<PortraitModel> portrait_wide)
      : face_(std::move(face)), body_hand_(std::move(body_hand)), portrait_(std::move(portrait)),
        portrait_wide_(std::move(portrait_wide)) {}

  Result<AnalysisWindow, std::string> Probe(const AnalysisConfig& config) const override {
    using ProbeResult = Result<AnalysisWindow, std::string>;
    if (config.capability != AnalysisCapability::PortraitMask && !config.model_id.empty())
      return ProbeResult::Failure("The ncnn landmark capability uses its default model pack");
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
      case AnalysisCapability::PortraitMask:
        if (config.uses_roi) return ProbeResult::Failure("Portrait coverage requires a full picture");
        if (!Portrait(config.model_id)) {
          const auto id = config.model_id.empty() ? kMediaPipeSelfieModelId : std::string_view(config.model_id);
          return ProbeResult::Failure("Unavailable ncnn portrait model: " + std::string(id));
        }
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
      return ProbeResult::Failure("ncnn input dimensions exceed the addressable image size");
    return ProbeResult::Success(AnalysisWindow{.stateful = config.capability == AnalysisCapability::BodyLandmarks});
  }

  std::unique_ptr<AnalysisSession> Open(const AnalysisConfig& config) override {
    if (!Probe(config).Succeeded()) return {};
    if (config.capability == AnalysisCapability::FaceLandmarks) return OpenFace(face_, config);
    if (config.capability == AnalysisCapability::PortraitMask) return OpenPortrait(Portrait(config.model_id), config);
    return OpenBodyHand(body_hand_, config);
  }

private:
  std::shared_ptr<PortraitModel> Portrait(std::string_view id) const {
    if (id.empty() || id == kMediaPipeSelfieModelId) return portrait_;
    if (id == kPPHumanSegV2LiteModelId) return portrait_wide_;
    return {};
  }

  std::shared_ptr<const YuNet> face_;
  std::shared_ptr<const BodyHandModels> body_hand_;
  std::shared_ptr<PortraitModel> portrait_, portrait_wide_;
};

} // namespace

ProviderResult CreateNcnnAnalysisProvider(const NcnnAnalysisModels& models) {
  try {
    if (!models.face && !models.body_hand && !models.portrait && !models.portrait_wide)
      return ProviderResult::Failure("At least one ncnn model pack is required");
    auto face = models.face ? LoadFace(*models.face) : nullptr;
    auto body_hand = models.body_hand ? LoadBodyHand(*models.body_hand) : nullptr;
    auto portrait = models.portrait ? LoadPortrait(*models.portrait, "MediaPipe Selfie Segmentation") : nullptr;
    auto wide = models.portrait_wide ? LoadPortrait(*models.portrait_wide, "PP-HumanSegV2-Lite") : nullptr;
    return ProviderResult::Success(std::make_shared<NcnnProvider>(std::move(face), std::move(body_hand),
                                                                  std::move(portrait), std::move(wide)));
  } catch (const std::exception& error) {
    return ProviderResult::Failure(error.what());
  }
}

ProviderResult CreateNcnnFaceAnalysisProvider(const RawAsset& parameters, const RawAsset& weights) {
  return CreateNcnnAnalysisProvider({NcnnModelAssets{parameters, weights}, {}});
}

} // namespace huxerui::avkit::analysis
