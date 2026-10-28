/** @file src/streaming/transport.h
 * @brief Bounded video transport and reference-chain recovery policies.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace streaming {
  // Negotiated through /serverinfo and the authenticated /launch or /resume.
  // Unknown/absent values always retain the legacy pacing policy.
  inline bool smooth_pacing_requested(std::string_view value) {
    return value == "smooth-v1";
  }

  inline std::size_t video_batch_size(std::size_t payload_bytes, std::size_t prefix_bytes, bool smooth) {
    if (prefix_bytes > 64 * 1024 || payload_bytes > 64 * 1024 - prefix_bytes) return 1;
    const auto packet_bytes = payload_bytes + prefix_bytes;
    if (packet_bytes == 0 || packet_bytes > 64 * 1024) return 1;
    return std::max<std::size_t>(1, std::min<std::size_t>(smooth ? 16 : 64, 64 * 1024 / packet_bytes));
  }

  // One batch per deadline. Account from the actual wake/send time to prevent
  // a late wake from releasing several overdue batches at once. The nominal
  // 800 Mbps rate is unchanged; smaller bursts trade wakeups for smoothness.
  class video_batch_pacer_t {
  public:
    using clock = std::chrono::steady_clock;
    clock::time_point deadline(clock::time_point now) const {
      return std::max(next_, now);
    }
    void sent(clock::time_point started, std::size_t bytes) {
      next_ = started + std::chrono::nanoseconds(bytes * 10);  // 100 MB/s
    }

  private:
    clock::time_point next_ {};
  };

  struct recovery_decision_t {
    bool discard;
    bool request_idr;
    bool overflow;
  };

  // Used only by the video sender. The queue is shared, so an overflow
  // conservatively invalidates every session's chain when it next dequeues.
  // The first retained IDR is sufficient, even if it predates the notification.
  class encoded_queue_recovery_t {
  public:
    recovery_decision_t observe(std::uint64_t dropped, bool idr) {
      const bool overflow = dropped != observed_drops_;
      observed_drops_ = dropped;
      if (overflow) {
        awaiting_idr_ = true;
        requested_ = false;  // An earlier recovery IDR may itself have been dropped.
      }
      if (idr) {
        awaiting_idr_ = false;
        requested_ = false;
      }
      const bool request = awaiting_idr_ && !requested_;
      requested_ = requested_ || request;
      return { awaiting_idr_, request, overflow };
    }

  private:
    std::uint64_t observed_drops_ = 0;
    bool awaiting_idr_ = true;
    bool requested_ = false;
  };
}  // namespace streaming
