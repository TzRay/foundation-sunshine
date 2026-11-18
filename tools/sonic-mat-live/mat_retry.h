#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace sonic_live {
  // Only known transient source failures may request a new publisher. The
  // caller must additionally verify helper and route cleanup before exit.
  struct MatSourceInterrupted : std::runtime_error {
    using std::runtime_error::runtime_error;
  };
  constexpr int mat_retry_exit = 3;
  inline int mat_exit_code(bool retryable, bool helper_started, bool done, bool clean, bool route_clean, int result) {
    // A known transient failure before helper startup has no hook to restore.
    // Once handed to the helper, both completion and restoration are mandatory.
    return retryable && done && (!helper_started || clean) && route_clean ? mat_retry_exit : result;
  }
  struct MatRetry {
    unsigned failures = 0;
    uint64_t next_delay() {
      const auto delay = uint64_t(1000) << (std::min)(failures, 3u);
      failures = (std::min)(failures + 1, 4u);
      return (std::min)(delay, uint64_t(5000));
    }
    void reset() { failures = 0; }
  };
}
