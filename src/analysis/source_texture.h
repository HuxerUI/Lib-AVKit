#pragma once

#include "analysis_job.h"
#include "support/geometry.h"

#include <stdexcept>

namespace huxerui::avkit::detail {

template<analysis::AnalysisCapability Capability> class SourceTexture {
  static constexpr bool kDepth = Capability == analysis::AnalysisCapability::DepthMap;
  static constexpr auto kSubmit = kDepth ? "depth map" : "portrait mask";
  static constexpr auto kFormat = kDepth ? render::PixelFormat::R32Float : render::PixelFormat::R8Unorm;

public:
  SourceTexture(std::shared_ptr<analysis::AnalysisProvider> provider, std::uint64_t source, std::string model_id = {})
      : provider_(std::move(provider)), source_(source),
        config_{.capability = Capability, .model_id = std::move(model_id)} {
    if (!provider_) throw std::runtime_error("Missing analysis provider for " + std::string(kSubmit));
  }

  using Request = PendingAnalysis<analysis::AnalysisImageCompletion>;

  std::unique_ptr<Request> Submit(const media::VideoFrame& frame, std::stop_token stop,
                                  std::optional<std::chrono::milliseconds> budget) {
    CheckCancelled(stop);
    Prepare(frame);
    const analysis::FrameKey key{source_, generation_, 0, frame.pts};
    if (sampled_ == key) return {};
    auto request = std::make_unique<Request>(stop);
    request->Start({next_job_++, key, {{key, frame}}, {}},
        [&](auto job, auto token, auto complete) {
          if constexpr (kDepth) session_->SubmitDepth(std::move(job), token, std::move(complete));
          else session_->SubmitMask(std::move(job), token, std::move(complete));
        }, budget);
    return request;
  }

  struct Sample {
    render::PixelInput input;
    Affine2D source_to_texture_uv;
    Rect valid_uv;
  };

  std::optional<Sample> Collect(std::unique_ptr<Request> request) {
    if (!request) return sample_;
    auto result = request->Wait();
    if (!result) {
      Reset();
      return std::nullopt;
    }
    if (!result->result.Succeeded()) throw std::runtime_error(std::string(kSubmit) + " analysis failed: " + result->result.Error());
    auto image = std::move(result->result).Value();
    sample_.reset();
    if (image) {
      if (!IsUnitRect(result->valid_uv) || !IsFinite(result->source_to_texture_uv))
        throw std::runtime_error(std::string(kSubmit) + " analysis returned an invalid UV mapping");
      const auto info = render::GetImageInfo(*image);
      if (!info.size.width || !info.size.height || info.format != kFormat)
        throw std::runtime_error(std::string(kSubmit) + " analysis returned an invalid image format");
      if (const auto* pixels = std::get_if<render::ImagePixels>(&*image); pixels && !pixels->Validate())
        throw std::runtime_error(std::string(kSubmit) + " analysis returned invalid pixel rows");
      sample_.emplace(Sample{std::move(*image), result->source_to_texture_uv, result->valid_uv});
    }
    sampled_ = result->target;
    return sample_;
  }

  void Reset() {
    session_.reset();
    sampled_.reset();
    sample_.reset();
  }

private:
  void Prepare(const media::VideoFrame& frame) {
    if (session_ && config_.format == frame.format && config_.storage == frame.Storage()) return;
    Reset();
    config_.format = frame.format;
    config_.storage = frame.Storage();
    auto probe = provider_->Probe(config_);
    if (!probe.Succeeded()) throw std::runtime_error("Unavailable " + std::string(kSubmit) + " capability: " + probe.Error());
    const auto& window = probe.Value();
    if (window.past_frames || window.future_frames || window.stateful)
      throw std::runtime_error("Timeline " + std::string(kSubmit) + " analysis requires a stateless single-frame model");
    session_ = provider_->Open(config_);
    if (!session_) throw std::runtime_error("Cannot open " + std::string(kSubmit) + " analysis");
    generation_ = NextAnalysisGeneration();
  }

  std::shared_ptr<analysis::AnalysisProvider> provider_;
  std::uint64_t source_;
  std::uint64_t generation_{};
  std::uint64_t next_job_{1};
  analysis::AnalysisConfig config_;
  std::unique_ptr<analysis::AnalysisSession> session_;
  std::optional<analysis::FrameKey> sampled_;
  std::optional<Sample> sample_;
};

using SourceMask = SourceTexture<analysis::AnalysisCapability::PortraitMask>;
using SourceDepth = SourceTexture<analysis::AnalysisCapability::DepthMap>;

} // namespace huxerui::avkit::detail
