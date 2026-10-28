#pragma once

#include <algorithm>
#include <cstdint>

namespace streaming {
  inline int pcm_s16_bitrate(int sample_rate, int channels) {
    return sample_rate * channels * 16;
  }

  inline int encoded_audio_bitrate(int channels, int requested_bps, bool eac3) {
    const int fallback = channels >= 6 ? 448000 : channels == 2 ? 192000 : 96000;
    return std::min(requested_bps > 0 ? requested_bps : fallback, eac3 ? 384000 : 640000);
  }

  // Audio RS uses four data and two parity shards. RTP/IP/CBC overhead is
  // charged to the session's separate transport allowance, not counted twice.
  inline int audio_with_fec_kbps(int payload_bps) {
    return static_cast<int>((static_cast<std::int64_t>(std::max(payload_bps, 0)) * 6 + 3999) / 4000);
  }
}  // namespace streaming
