#pragma once

#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace huxerui::avkit::render::detail {

// bgfx owns one process-wide context. Wait on render workers so UI cleanup can still join them.
class BgfxRenderLease {
public:
  explicit BgfxRenderLease(std::stop_token stop = {}) {
    std::unique_lock lock(mutex_);
    if (ready_.wait(lock, stop, [] { return !occupied_; }) && !stop.stop_requested()) {
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
  explicit operator bool() const { return acquired_; }

private:
  inline static std::mutex mutex_;
  inline static std::condition_variable_any ready_;
  inline static bool occupied_{};
  bool acquired_{};
};

} // namespace huxerui::avkit::render::detail
