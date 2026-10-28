#pragma once

#include <cstddef>
#include <algorithm>

namespace platf {
  enum class batch_send_status_e {
    accepted,
    fallback,  // The unsent suffix may be submitted as individual datagrams.
    would_block,
    failed,    // Acceptance is unknown; do not blindly replay this batch.
  };

  struct batch_send_result_t {
    // Socket acceptance, not NIC transmission or remote delivery.
    std::size_t packets_accepted;
    batch_send_status_e status;
    int error = 0;
  };

  template<class SendDatagram>
  std::size_t send_datagram_suffix(std::size_t count, const batch_send_result_t &result, SendDatagram send) {
    auto accepted = std::min(count, result.packets_accepted);
    if (result.status == batch_send_status_e::fallback) {
      for (auto index = accepted; index < count; ++index) {
        if (send(index)) ++accepted;
      }
    }
    return accepted;
  }
}  // namespace platf
