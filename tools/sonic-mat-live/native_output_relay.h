#pragma once
#include "native_output_protocol.h"
#include "../mat-live/publisher.h"
#include "mat_retry.h"
#include <optional>

// The pinned Windows wrapper emits 480-frame/30720-byte halves. Assemble them
// without interpreting or changing the native PCM/metadata. Only an initial
// partial half may be omitted; never resynchronize a damaged live stream.
class NativeOutputRelay {
  Cycle pending_ {};
  uint64_t wrapper_ = 0, implementation_ = 0, last_qpc_ = 0, next_ = 0;
  uint32_t endpoint_slot_ = 0;
  bool first_half_ = false, aligned_ = false;
  uint64_t skipped_ = 0;

public:
  std::optional<Cycle> accept(const native_output::Record &record, std::span<const BYTE> bytes) {
    require(record.sequence == next_++ && record.qpc > last_qpc_ && record.qpc <= INT64_MAX,
      "Native output sequence/clock discontinuity");
    last_qpc_ = record.qpc;
    require(record.wrapper && record.implementation && record.endpoint_slot >= 1 && record.endpoint_slot <= native_output::endpoint_count &&
              record.frames == 480 && record.result >= 0 && record.produced == bytes.size() && record.capacity >= bytes.size(),
      "Native output endpoint/callback contract");
    if (!wrapper_) {
      wrapper_ = record.wrapper;
      implementation_ = record.implementation;
      endpoint_slot_ = record.endpoint_slot;
    }
    if (wrapper_ != record.wrapper || implementation_ != record.implementation || endpoint_slot_ != record.endpoint_slot)
      throw sonic_live::MatSourceInterrupted("Native renderer changed; a new source epoch is required");
    if (bytes.empty()) return {};
    require(bytes.size() == spatial_audio::process_bytes, "Unexpected native output quantum size");
    if (!first_half_) {
      const bool preamble = bytes[0] == 0x72 && bytes[1] == 0xf8 && bytes[2] == 0x1f && bytes[3] == 0x4e;
      if (!preamble) {
        require(!aligned_ && !skipped_, "Native output lost MAT cycle alignment");
        skipped_ = bytes.size();
        return {};
      }
      memcpy(pending_.bytes.data(), bytes.data(), bytes.size());
      first_half_ = aligned_ = true;
      return {};
    }
    memcpy(pending_.bytes.data() + spatial_audio::process_bytes, bytes.data(), bytes.size());
    spatial_audio::validate_carrier(pending_.bytes);
    pending_.qpc = record.qpc; // Actual completion time; not an invented source PTS.
    first_half_ = false;
    return pending_;
  }
  uint64_t encoder() const { return implementation_; }
  uint64_t skipped_bytes() const { return skipped_; }
  uint64_t partial_bytes() const { return first_half_ ? spatial_audio::process_bytes : 0; }

  void publish(Shared &pipe, const Cycle &cycle) const {
    std::unique_lock lock(pipe.mutex);
    require(!pipe.failed, pipe.error.c_str());
    if (!pipe.ready) {
      printf("NATIVE_MAT_READY source=system-native-output carrierBytes=61440 endpointSlot=%u renderer=0x%llx skippedPrefix=%llu reencoded=0\n",
        endpoint_slot_, wrapper_, skipped_);
      pipe.changed.notify_all();
    }
    pipe.ready = true;
    pipe.encoder = implementation_;
    pipe.last_cycle = GetTickCount64();
    ++pipe.cycles;
    if (pipe.playing && !pipe.lease_failed && cycle.qpc >= pipe.play_qpc) {
      const auto now = Qpc();
      if (pipe.queue.size() >= Shared::max_lease_cycles || pipe.too_old(cycle.qpc, now) ||
          (!pipe.queue.empty() && pipe.too_old(pipe.queue.front().qpc, now))) {
        // Do not block the ring reader behind a slow network consumer. Close
        // this generation (including any partial pipe write), then allow a
        // new authenticated lease. Never skip a cycle within the old lease.
        pipe.lease_failed = true;
        pipe.playing = false;
        pipe.changed.notify_all();
        return;
      }
      pipe.queue.push_back(cycle);
      pipe.queue_peak = (std::max)(pipe.queue_peak, pipe.queue.size());
      pipe.changed.notify_all();
    }
  }
};
