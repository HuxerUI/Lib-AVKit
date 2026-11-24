#pragma once

#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace avkit_preview {

// bgfx owns one process-wide context. Wait on render workers so UI cleanup can still join them.
class WorkbenchRenderLease {
public:
  explicit WorkbenchRenderLease(std::stop_token stop = {}) {
    std::unique_lock lock(mutex_);
    if (ready_.wait(lock, stop, [] { return !occupied_; }) && !stop.stop_requested()) {
      occupied_ = true;
      acquired_ = true;
    }
  }

  ~WorkbenchRenderLease() {
    if (!acquired_) return;
    {
      std::lock_guard lock(mutex_);
      occupied_ = false;
    }
    ready_.notify_all();
  }

  WorkbenchRenderLease(const WorkbenchRenderLease&) = delete;
  WorkbenchRenderLease& operator=(const WorkbenchRenderLease&) = delete;
  explicit operator bool() const { return acquired_; }

private:
  inline static std::mutex mutex_;
  inline static std::condition_variable_any ready_;
  inline static bool occupied_{};
  bool acquired_{};
};

} // namespace avkit_preview
