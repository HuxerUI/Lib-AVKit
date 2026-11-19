#pragma once

/// @file
/// @brief CPU landmark analysis using versioned ncnn model packs. Link AVKit::NCNN.

#include <huxerui/avkit/analysis.h>
#include <huxerui/resource.h>

namespace huxerui::avkit::analysis {

/// @brief A trusted ncnn graph and its matching weights.
struct NcnnModelAssets {
  RawAsset parameters;
  RawAsset weights;
};

/// @brief MediaPipe detector and landmark pairs converted using the documented pack recipe.
struct NcnnBodyHandModels {
  NcnnModelAssets person_detector, body_landmarks;
  NcnnModelAssets palm_detector, hand_landmarks;
};

/// @brief Model packs loaded into one CPU landmark provider.
struct NcnnAnalysisModels {
  std::optional<NcnnModelAssets> face;
  std::optional<NcnnBodyHandModels> body_hand;
};

/// @brief Loads shared models and opens independent analysis sessions for unrotated SDR I420 CPU frames.
/// Body sessions associate observations with their first valid reference picture. Hand sessions
/// require body-derived regions and return source-space points with the input region index.
[[nodiscard]] Result<std::shared_ptr<AnalysisProvider>, std::string> CreateNcnnAnalysisProvider(
    const NcnnAnalysisModels& models);

/// @brief Loads the trusted YuNet 2023mar FP32 graph and weights into a reusable provider.
/// @param parameters The converted ncnn text graph from the YuNet model pack.
/// @param weights The matching FP32 ncnn weights from the same pack revision.
/// @return The provider or a diagnostic for missing, unreadable, or incompatible model data.
/// Sessions accept unrotated SDR I420 CPU pictures. In full-frame mode the first valid job supplies the
/// reference picture, which timeline sampling takes from the start of the clip's source trim. Full-frame
/// analysis reports one subject when the current picture has one face that matches the reference.
/// Region-based analysis uses caller-supplied head regions. Inference and completion run on the submitting sequence.
[[nodiscard]] Result<std::shared_ptr<AnalysisProvider>, std::string> CreateNcnnFaceAnalysisProvider(
    const RawAsset& parameters, const RawAsset& weights);

} // namespace huxerui::avkit::analysis
