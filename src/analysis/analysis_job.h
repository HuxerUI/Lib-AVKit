#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

#include <huxerui/avkit/analysis.h>

#include "support/cancellation.h"

namespace huxerui::avkit::detail {

inline std::uint64_t NextAnalysisGeneration() {
  static std::atomic<std::uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

// Owns the completion mailbox and cancels a submitted job if collection is abandoned.
template<class Completion> class PendingAnalysis {
  struct Mailbox {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Completion> completion;
  };

  struct OnStop {
    Mailbox& box;
    std::stop_source& work;

    void operator()() const {
      {
        const std::lock_guard lock(box.mutex);
        box.changed.notify_one();
      }
      work.request_stop();
    }
  };

public:
  explicit PendingAnalysis(std::stop_token stop) : stop_(stop), wake_on_stop_(stop_, OnStop{*box_, work_}) {}

  ~PendingAnalysis() {
    if (pending_) work_.request_stop();
  }

  template<class Submit>
  void Start(analysis::AnalysisJob job, Submit&& submit, std::optional<std::chrono::milliseconds> budget = {}) {
    CheckCancelled(stop_);
    const auto target = job.target;
    const auto id = job.id;
    if (budget) deadline_ = std::chrono::steady_clock::now() + *budget;
    pending_ = true;
    submit(std::move(job), work_.get_token(),
        [mailbox = std::weak_ptr(box_), target, id, deadline = deadline_](Completion result) {
          if (result.job_id != id || result.target != target) return;
          if (const auto queued = mailbox.lock()) {
            const std::lock_guard lock(queued->mutex);
            if (queued->completion || (deadline && std::chrono::steady_clock::now() >= *deadline)) return;
            queued->completion.emplace(std::move(result));
            queued->changed.notify_one();
          }
        });
  }

  std::optional<Completion> Wait() {
    std::unique_lock lock(box_->mutex);
    const auto ready = [&] { return box_->completion.has_value() || stop_.stop_requested(); };
    if (deadline_) box_->changed.wait_until(lock, *deadline_, ready);
    else box_->changed.wait(lock, ready);
    CheckCancelled(stop_);
    pending_ = false;
    if (box_->completion) return std::move(box_->completion);
    lock.unlock();
    work_.request_stop();
    return std::nullopt;
  }

private:
  std::shared_ptr<Mailbox> box_ = std::make_shared<Mailbox>();
  std::stop_source work_;
  std::stop_token stop_;
  std::stop_callback<OnStop> wake_on_stop_;
  std::optional<std::chrono::steady_clock::time_point> deadline_;
  bool pending_{};
};

template<class Completion, class Submit>
std::optional<Completion> WaitForAnalysis(analysis::AnalysisJob job, std::stop_token stop, Submit&& submit) {
  PendingAnalysis<Completion> request(stop);
  request.Start(std::move(job), std::forward<Submit>(submit));
  return request.Wait();
}

} // namespace huxerui::avkit::detail
