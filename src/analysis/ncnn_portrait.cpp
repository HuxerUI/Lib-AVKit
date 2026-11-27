#include "ncnn_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#endif

namespace huxerui::avkit::analysis::detail {
namespace {

constexpr int kMaskSize = 256;
constexpr std::array kOutputs{"out0"};
using MaskResult = decltype(AnalysisImageCompletion::result);

struct PortraitRequest {
  AnalysisConfig config;
  AnalysisJob job;
  std::stop_token stop;
  AnalysisSession::ImageCallback complete;
};

MaskResult Infer(const Network& model, const PortraitRequest& request) {
  const auto& [config, job, stop, complete] = request;
  if (stop.stop_requested()) return MaskResult::Success(std::nullopt);
  if (job.frames.size() != 1 || job.frames.front().key != job.target || !job.regions.empty())
    return MaskResult::Failure("Portrait segmentation needs exactly one full target picture");
  const auto& frame = job.frames.front().image;
  if (frame.format != config.format || frame.Storage() != config.storage || frame.pts != job.target.pts || !frame.Validate())
    return MaskResult::Failure("Portrait picture does not match the opened input format or PTS");

  try {
    const auto input = ResizePictureRgb(frame, kMaskSize, kMaskSize, stop);
    if (stop.stop_requested()) return MaskResult::Success(std::nullopt);
    auto extractor = model.Extract(input);
    ncnn::Mat output;
    if (extractor.extract(kOutputs.front(), output) || output.w != kMaskSize || output.h != kMaskSize ||
        (output.dims != 2 && output.dims != 3) || output.c != 1 || output.elempack != 1 || output.elemsize != sizeof(float))
      return MaskResult::Failure("Unexpected ncnn portrait model output");
    if (stop.stop_requested()) return MaskResult::Success(std::nullopt);

    auto bytes = std::make_shared<Bytes>(kMaskSize * kMaskSize);
    for (int row = 0; row < kMaskSize; ++row) {
      const auto* probabilities = output.row(row);
      for (int col = 0; col < kMaskSize; ++col) {
        const auto value = probabilities[col];
        if (!std::isfinite(value)) return MaskResult::Failure("Non-finite foreground probability");
        (*bytes)[row * kMaskSize + col] = std::byte(int(std::clamp(value, 0.F, 1.F) * 255.F + 0.5F));
      }
    }
    return MaskResult::Success(render::ImagePixels{
        {{kMaskSize, kMaskSize}, render::PixelFormat::R8Unorm},
        BufferReference(*bytes, bytes), kMaskSize});
  } catch (const std::exception& error) {
    return MaskResult::Failure(error.what());
  }
}

void Run(const Network& model, PortraitRequest request) {
  auto result = Infer(model, request);
  request.job.frames.clear();
  if (request.stop.stop_requested()) result = MaskResult::Success(std::nullopt);
  request.complete({request.job.id, request.job.target, std::move(result)});
}

} // namespace

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)

struct PortraitModel {
  PortraitModel(const NcnnModelAssets& assets, const char* name)
      : state(std::make_shared<State>(assets, name)), worker([state = state](std::stop_token stop) {
          std::unique_lock lock(state->mutex);
          while (state->ready.wait(lock, stop, [&] { return !state->requests.empty(); }) && !stop.stop_requested()) {
            auto request = std::move(state->requests.front());
            state->requests.pop_front();
            lock.unlock();
            Run(state->model, std::move(request));
            lock.lock();
          }
        }) {}

  ~PortraitModel() {
    // A completion may release the last owner of this provider on its worker.
    if (worker.get_id() == std::this_thread::get_id()) {
      worker.request_stop();
      worker.detach();
    }
  }

  void Submit(PortraitRequest request) {
    if (request.stop.stop_requested()) {
      request.complete({request.job.id, request.job.target, MaskResult::Success(std::nullopt)});
      return;
    }
    {
      const std::lock_guard lock(state->mutex);
      std::erase_if(state->requests, [](const auto& queued) { return queued.stop.stop_requested(); });
      state->requests.push_back(std::move(request));
    }
    state->ready.notify_one();
  }

private:
  struct State {
    State(const NcnnModelAssets& assets, const char* name) : model(assets, kOutputs, name) {}

    Network model;
    std::mutex mutex;
    std::condition_variable_any ready;
    std::deque<PortraitRequest> requests;
  };

  std::shared_ptr<State> state;
  std::jthread worker;
};

#else

struct PortraitModel {
  PortraitModel(const NcnnModelAssets& assets, const char* name) : model(assets, kOutputs, name) {}

  void Submit(PortraitRequest request) { Run(model, std::move(request)); }

private:
  Network model;
};

#endif

namespace {

class PortraitSession final : public AnalysisSession {
public:
  PortraitSession(std::shared_ptr<PortraitModel> model, AnalysisConfig config) : model_(std::move(model)), config_(config) {}

  void SubmitMask(AnalysisJob job, std::stop_token stop, ImageCallback complete) override {
    model_->Submit({config_, std::move(job), stop, std::move(complete)});
  }

private:
  std::shared_ptr<PortraitModel> model_;
  AnalysisConfig config_;
};

} // namespace

std::shared_ptr<PortraitModel> LoadPortrait(const NcnnModelAssets& assets, const char* name) {
  return std::make_shared<PortraitModel>(assets, name);
}

std::unique_ptr<AnalysisSession> OpenPortrait(std::shared_ptr<PortraitModel> model, const AnalysisConfig& config) {
  return std::make_unique<PortraitSession>(std::move(model), config);
}

} // namespace huxerui::avkit::analysis::detail
