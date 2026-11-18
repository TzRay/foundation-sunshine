#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace spatial_transport {
  // One producer and the shared audio sender. Counter publication and waiting
  // use the same mutex so a completion cannot be lost between check and sleep.
  class MatSendProgress {
    std::mutex mutex_;
    std::condition_variable changed_;

  public:
    std::atomic<uint64_t> completed { 0 };

    // The audio owner calls this only after every submitted send has drained.
    // Both credits change together, even if recovery fails before media resumes.
    void reset_after_drain(uint64_t &submitted) {
      completed.store(0);
      submitted = 0;
    }

    void finish(bool notify) {
      {
        std::lock_guard lock(mutex_);
        completed.fetch_add(1, std::memory_order_release);
      }
      if (notify) changed_.notify_all();
    }

    template<class Cancel>
    bool wait(uint64_t minimum, std::chrono::steady_clock::time_point deadline, Cancel cancelled) {
      using namespace std::chrono_literals;
      std::unique_lock lock(mutex_);
      while (completed.load(std::memory_order_acquire) < minimum) {
        if (cancelled() || std::chrono::steady_clock::now() >= deadline) return false;
        // Global cancellation has a separate event. Bound its observation
        // without polling the sender every 50 microseconds.
        changed_.wait_until(lock, (std::min)(deadline, std::chrono::steady_clock::now() + 10ms));
      }
      return !cancelled();
    }
  };
}
