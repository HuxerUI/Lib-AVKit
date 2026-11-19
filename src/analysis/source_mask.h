#pragma once

#include "analysis_job.h"
#include "support/geometry.h"

#include <stdexcept>

namespace huxerui::avkit::detail {

class SourceMask {
public:
  SourceMask(std::shared_ptr<analysis::AnalysisProvider> provider, std::uint64_t source, std::string model_id)
      : provider_(std::move(provider)), source_(source),
        config_{.capability = analysis::AnalysisCapability::PortraitMask, .model_id = std::move(model_id)} {
    if (!provider_) throw std::runtime_error("Background replacement requires a portrait mask provider");
  }

  using Request = PendingAnalysis<analysis::MaskCompletion>;

  std::unique_ptr<Request> Submit(const media::VideoFrame& frame, std::stop_token stop,
                                  std::optional<std::chrono::milliseconds> budget) {
    CheckCancelled(stop);
    Prepare(frame);
    const analysis::FrameKey key{source_, generation_, 0, frame.pts};
    if (sampled_ == key) return {};
    auto request = std::make_unique<Request>(stop);
    request->Start({next_job_++, key, {{key, frame}}, {}},
        [&](auto job, auto token, auto complete) {
          session_->SubmitMask(std::move(job), token, std::move(complete));
        }, budget);
    return request;
  }

  std::optional<analysis::AnalysisTexture> Collect(std::unique_ptr<Request> request) {
    if (!request) return mask_;
    auto result = request->Wait();
    if (!result) {
      Reset();
      return std::nullopt;
    }
    if (!result->result.Succeeded()) throw std::runtime_error("Portrait mask analysis failed: " + result->result.Error());
    auto mask = std::move(result->result).Value();
    if (mask && (!mask->surface.storage || !mask->surface.image.size.width || !mask->surface.image.size.height ||
                 mask->surface.image.format != render::PixelFormat::R8Unorm || !IsUnitRect(mask->valid_uv) ||
                 !IsFinite(mask->source_to_texture_uv))) {
      throw std::runtime_error("Portrait mask analysis returned an invalid coverage texture");
    }
    mask_ = std::move(mask);
    sampled_ = result->target;
    return mask_;
  }

  void Reset() {
    session_.reset();
    sampled_.reset();
    mask_.reset();
  }

private:
  void Prepare(const media::VideoFrame& frame) {
    if (session_ && config_.format == frame.format && config_.storage == frame.Storage()) return;
    Reset();
    config_.format = frame.format;
    config_.storage = frame.Storage();
    auto probe = provider_->Probe(config_);
    if (!probe.Succeeded()) throw std::runtime_error("Unavailable portrait mask capability: " + probe.Error());
    const auto& window = probe.Value();
    if (window.past_frames || window.future_frames || window.stateful)
      throw std::runtime_error("Timeline portrait masks require a stateless single-frame model");
    session_ = provider_->Open(config_);
    if (!session_) throw std::runtime_error("Cannot open portrait mask analysis");
    generation_ = NextAnalysisGeneration();
  }

  std::shared_ptr<analysis::AnalysisProvider> provider_;
  std::uint64_t source_;
  std::uint64_t generation_{};
  std::uint64_t next_job_{1};
  analysis::AnalysisConfig config_;
  std::unique_ptr<analysis::AnalysisSession> session_;
  std::optional<analysis::FrameKey> sampled_;
  std::optional<analysis::AnalysisTexture> mask_;
};

} // namespace huxerui::avkit::detail
