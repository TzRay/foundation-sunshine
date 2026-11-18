#pragma once
#include <string>

namespace sonic_live {
  struct MatManagerAction {
    bool stop_child;
    bool reset_failure;
  };
  // Identity includes the host generation as well as source/endpoint/engine.
  inline MatManagerAction mat_manager_action(const std::wstring &active, const std::wstring &desired,
    bool child_running, bool requested) {
    const bool changed = active != desired;
    return { child_running && changed,
      !desired.empty() && (changed || (requested && !child_running)) };
  }
}
