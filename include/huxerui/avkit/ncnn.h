#pragma once

/// @file
/// @brief CPU vision analysis using versioned ncnn model packs. Link AVKit::NCNN.

#include <huxerui/avkit/analysis.h>
#include <huxerui/resource.h>

#include <string_view>

namespace huxerui::avkit::analysis {

/// @brief A trusted ncnn graph and its matching weights.
struct NcnnModelAssets {
  /// Converted ncnn graph from the trusted model pack.
  RawAsset parameters;
  /// Matching weights from the same pack revision.
  RawAsset weights;
};

/// @brief MediaPipe detector and landmark pairs converted using the documented pack recipe.
struct NcnnBodyHandModels {
  /// Full-person detection and body landmarks within the detected region.
  NcnnModelAssets person_detector, body_landmarks;
  /// Palm detection and hand landmarks within a caller-supplied hand region.
  NcnnModelAssets palm_detector, hand_landmarks;
};

/// @brief Model IDs accepted by the ncnn portrait capability and stored in timeline effects.
inline constexpr std::string_view kMediaPipeSelfieModelId = "mediapipe-selfie-256-v1";
inline constexpr std::string_view kPPHumanSegV2LiteModelId = "pp-humansegv2-lite-192-v1";

/// @brief Model packs loaded into one CPU analysis provider.
struct NcnnAnalysisModels {
  /// Optional YuNet face pack.
  std::optional<NcnnModelAssets> face;
  /// Optional body and hand pack with all four model graphs.
  std::optional<NcnnBodyHandModels> body_hand;
  /// MediaPipe Selfie Segmentation 256x256, selected by default.
  std::optional<NcnnModelAssets> portrait;
  /// PP-HumanSegV2-Lite general model converted using the documented 256x256 RGB and coverage interface.
  std::optional<NcnnModelAssets> portrait_wide;
};

/// @brief Loads shared models and opens independent analysis sessions for unrotated SDR I420 CPU frames.
/// Body sessions associate observations with their first valid reference picture. Hand sessions
/// require body-derived regions and return source-space points with the input region index.
/// Portrait sessions use one full picture per job and return R8Unorm foreground coverage.
/// @param models Packaged graphs and weights for the capabilities to expose. Omit unavailable packs.
/// @return Provider with the requested models, or a diagnostic if a supplied pack cannot be loaded.
Result<std::shared_ptr<AnalysisProvider>, std::string> CreateNcnnAnalysisProvider(
    const NcnnAnalysisModels& models);

/// @brief Loads the trusted YuNet 2023mar FP32 graph and weights into a reusable provider.
/// Sessions accept unrotated SDR I420 CPU pictures. In full-frame mode the first valid job supplies the
/// reference picture, which timeline sampling takes from the start of the clip's source trim. Full-frame
/// analysis reports one subject when the current picture has one face that matches the reference.
/// Region-based analysis uses caller-supplied head regions. Inference and completion run on the submitting sequence.
/// @param parameters The converted ncnn text graph from the YuNet model pack.
/// @param weights The matching FP32 ncnn weights from the same pack revision.
/// @return The provider or a diagnostic for missing, unreadable, or incompatible model data.
Result<std::shared_ptr<AnalysisProvider>, std::string> CreateNcnnFaceAnalysisProvider(
    const RawAsset& parameters, const RawAsset& weights);

} // namespace huxerui::avkit::analysis
