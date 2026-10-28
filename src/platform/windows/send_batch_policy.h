#pragma once

#include <winsock2.h>
#include "src/platform/send_batch.h"

namespace platf {
  inline batch_send_result_t classify_batch_send_failure(int error) {
    switch (error) {
      case WSAEINVAL:
      case WSAENOPROTOOPT:
      case WSAEOPNOTSUPP:
      case WSAEMSGSIZE:
        // A rejected call can be retried as datagrams. In particular EINVAL
        // can also mean an invalid source address: never cache it as a
        // permanent USO capability failure. send() retains PKTINFO recovery.
        return { 0, batch_send_status_e::fallback, error };
      case WSAEWOULDBLOCK:
        return { 0, batch_send_status_e::would_block, error };
      default:
        // Other failures do not prove that no USO segments reached the NIC.
        return { 0, batch_send_status_e::failed, error };
    }
  }
}  // namespace platf
