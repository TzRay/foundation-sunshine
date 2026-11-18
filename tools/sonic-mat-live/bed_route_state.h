#pragma once
#include <array>
#include <exception>
#include <string>

namespace sonic_live {
  // One publisher pipe lease owns all three interactive-user default roles.
  // The callbacks keep the lifecycle testable without changing Windows audio.
  class BedRouteState {
    std::wstring endpoint_;
    std::array<std::wstring, 3> previous_;
    bool active_ = false;

  public:
    bool active() const { return active_; }
    template <class Read, class Write>
    void start(const std::wstring &endpoint, Read read, Write write) {
      if (active_) return;
      for (int role = 0; role < 3; ++role) previous_[role] = read(role);
      endpoint_ = endpoint;
      active_ = true;
      for (int role = 0; role < 3; ++role)
        if (previous_[role] != endpoint_) write(role, endpoint_);
    }
    template <class Read, class Write, class Failed>
    bool restore(Read read, Write write, Failed failed) noexcept {
      if (!active_) return true;
      bool okay = true;
      for (int role = 0; role < 3; ++role) {
        try {
          if (!previous_[role].empty() && previous_[role] != endpoint_ && read(role) == endpoint_)
            write(role, previous_[role]);
        }
        catch (const std::exception &e) {
          okay = false;
          failed(role, e.what());
        }
      }
      active_ = false;
      return okay;
    }
  };
}  // namespace sonic_live
