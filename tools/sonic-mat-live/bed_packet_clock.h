#pragma once
#include <audioclient.h>
#include <cstdint>
#include <stdexcept>

namespace sonic_live {
  // These failures end the current epoch. The manager may reopen the source
  // only after the old native worker has actually stopped.
  class BedSourceRestart: public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };
  class BedDeviceInvalidated: public BedSourceRestart {
  public:
    using BedSourceRestart::BedSourceRestart;
  };
  // Reopening WASAPI may return a block that overlaps the last accepted block.
  // Drop complete overlapping blocks without changing the encoder's timeline.
  class BedReopenAlignment {
    int64_t last_ = 0;
    bool reopening_ = false;

  public:
    void reopened() { reopening_ = true; }
    bool accept(int64_t first, int64_t last) {
      if (reopening_ && first <= last_) return false;
      reopening_ = false;
      last_ = last;
      return true;
    }
  };
  inline void
  check_bed_packet_clock(bool seen, DWORD flags, uint64_t position, uint64_t timestamp,
                         uint64_t last_position, uint64_t last_timestamp) {
    if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || !timestamp)
      throw std::runtime_error("BED_TIMESTAMP_ERROR");
    if (seen && (position < last_position || timestamp < last_timestamp))
      throw std::runtime_error("BED_CLOCK_REVERSED");
    if (seen && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY))
      throw BedSourceRestart("BED_DISCONTINUITY");
  }
}  // namespace sonic_live
