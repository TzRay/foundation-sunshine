#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace spatial_audio {
  class MatLiveStatus {
    static uint64_t
    now_ms() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

  public:
    struct Timing {
      uint64_t samples = 0, max_us = 0, over_10ms = 0, over_60ms = 0, updated_ms = 0;
    };
    struct Snapshot {
      unsigned leases = 0, running = 0;
      bool observed = false;
      std::string last_error;
      std::string first_error, first_error_stage, source, endpoint;
      uint64_t first_error_time_ms = 0, error_count = 0;
      uint32_t publisher_pid = 0, engine_pid = 0;
      uint64_t publisher_created = 0, engine_created = 0;
      uint64_t lease_generation = 0;
      std::map<std::string, Timing> timings;
      const char *
      state() const {
        if (running) return "streaming";
        if (leases) return "ready";
        if (!last_error.empty()) return "error";
        return observed ? "idle" : "unobserved";
      }
    };
    Snapshot
    snapshot() const {
      std::lock_guard lock(mutex_);
      return value_;
    }
    void
    acquired(uint32_t publisher_pid = 0, uint64_t publisher_created = 0, uint32_t engine_pid = 0, uint64_t engine_created = 0) {
      std::lock_guard lock(mutex_);
      if (!value_.leases) {
        ++value_.lease_generation;
        value_.first_error.clear();
        value_.first_error_stage.clear();
        value_.first_error_time_ms = value_.error_count = 0;
        value_.source.clear();
        value_.endpoint.clear();
        value_.timings.clear();
        value_.publisher_pid = publisher_pid;
        value_.publisher_created = publisher_created;
        value_.engine_pid = engine_pid;
        value_.engine_created = engine_created;
      }
      ++value_.leases;
      value_.observed = true;
      value_.last_error.clear();
    }
    void
    started() {
      std::lock_guard lock(mutex_);
      ++value_.running;
    }
    void
    paused() {
      std::lock_guard lock(mutex_);
      --value_.running;
    }
    void
    released(bool running) {
      std::lock_guard lock(mutex_);
      --value_.leases;
      if (running) --value_.running;
    }
    void
    failed(const std::string &error, const std::string &stage = "unknown") {
      std::lock_guard lock(mutex_);
      value_.observed = true;
      value_.last_error = error;
      ++value_.error_count;
      if (value_.first_error.empty()) {
        value_.first_error = error;
        value_.first_error_stage = stage;
        value_.first_error_time_ms = now_ms();
      }
    }
    // Called by delivery threads at their reporting interval, never per packet.
    void
    record_timing(const std::string &stage, uint64_t samples, uint64_t max_us, uint64_t over_10ms = 0, uint64_t over_60ms = 0) {
      std::lock_guard lock(mutex_);
      value_.timings[stage] = { samples, max_us, over_10ms, over_60ms, now_ms() };
    }
    // A fresh manager heartbeat may identify the endpoint only when it names
    // the publisher and engine already authenticated by the active pipe lease.
    bool
    identify_source(uint32_t publisher_pid, uint64_t publisher_created, uint32_t engine_pid, uint64_t engine_created,
      const std::string &source, const std::string &endpoint, uint64_t expected_generation) {
      std::lock_guard lock(mutex_);
      if (!value_.leases || value_.lease_generation != expected_generation || !publisher_pid || !publisher_created ||
          publisher_pid != value_.publisher_pid || publisher_created != value_.publisher_created ||
          engine_pid != value_.engine_pid || engine_created != value_.engine_created) return false;
      value_.source = source;
      value_.endpoint = endpoint;
      return true;
    }

  private:
    mutable std::mutex mutex_;
    Snapshot value_;
  };

  inline MatLiveStatus mat_live_status;
}  // namespace spatial_audio
