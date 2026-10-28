#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace nvenc {
  // Preserve initialization policy (VBR peak ratio and VBV duration) for all
  // codecs. Zero means driver-managed and must remain zero. Use 64-bit math
  // for bitrate * buffer size; both values can be large at 4K/high refresh.
  inline std::uint32_t rescale_rate_control(std::uint32_t value, std::uint32_t old_bps, std::uint32_t new_bps) {
    if (value == 0 || old_bps == 0) return value;
    const auto scaled = static_cast<std::uint64_t>(value) * new_bps / old_bps;
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(scaled, 1, std::numeric_limits<std::uint32_t>::max()));
  }
}  // namespace nvenc
