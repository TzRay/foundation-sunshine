#pragma once
#include "mat_manager_control.h"
#include "native_mat_pipe_source.h"

#ifdef _WIN32
namespace spatial_audio {
  // A null replacement means the current authenticated pipe and its route
  // survived the restart barrier. Only a recoverable disconnect needs demand.
  template<class Pipe, class Wake>
  std::unique_ptr<Pipe> recover_mat_pipe(Pipe &old, std::chrono::steady_clock::time_point deadline,
    const std::function<bool()> &cancelled, Wake wake) {
    old.cancel_when(cancelled);
    if (old.restart()) return {};
    if (!old.can_reprepare()) throw MatSourceBlocked("MAT source failure is not safe to reprepare");
    old.stop();
    wake(deadline, cancelled);
    require(!cancelled(), "MAT source replacement cancelled");
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    require(remaining.count() > 0, "MAT source replacement deadline");
    auto next = std::make_unique<Pipe>();
    next->cancel_when(cancelled);
    try {
      next->prepare(mat_auto_pipe, {}, true, static_cast<DWORD>(remaining.count()), deadline);
    } catch (const std::exception &e) {
      if (!next->can_reprepare() && !cancelled()) throw MatSourceBlocked(e.what());
      throw;
    }
    if (!next->supports_restart()) throw MatSourceBlocked("Replacement MAT source does not support recovery");
    return next;
  }
}
#endif
