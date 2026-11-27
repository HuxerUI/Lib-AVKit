#pragma once

#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <utility>

#include <huxerui/avkit/render.h>

namespace huxerui::avkit::render::detail {

// All bgfx backends share this reservation. Native render workers may wait with a stop token.
class BgfxRenderLease {
public:
  explicit BgfxRenderLease(std::stop_token stop = {}) {
    std::unique_lock lock(mutex_);
#if !defined(__EMSCRIPTEN__)
    if (stop.stop_possible()) ready_.wait(lock, stop, [] { return !occupied_; });
#endif
    if (!occupied_ && !stop.stop_requested()) {
      occupied_ = true;
      acquired_ = true;
    }
  }

  ~BgfxRenderLease() {
    if (!acquired_) return;
    {
      std::lock_guard lock(mutex_);
      occupied_ = false;
    }
    ready_.notify_all();
  }

  BgfxRenderLease(const BgfxRenderLease&) = delete;

  BgfxRenderLease& operator=(const BgfxRenderLease&) = delete;

  BgfxRenderLease(BgfxRenderLease&& other) noexcept : acquired_(std::exchange(other.acquired_, false)) {}

  explicit operator bool() const { return acquired_; }

private:
  inline static std::mutex mutex_;
  inline static std::condition_variable_any ready_;
  inline static bool occupied_{};
  bool acquired_{};
};

/// @brief Opens the platform backend, or returns an empty pointer if the wait is cancelled.
std::unique_ptr<RenderBackend> OpenBgfxBackend(ImageInfo target, RenderPlatformHandles handles, std::stop_token stop);

} // namespace huxerui::avkit::render::detail
