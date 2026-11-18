#include "bed_route_state.h"
#include <cstdio>
#include <stdexcept>

static void check(bool okay, const char *message) {
  if (!okay) throw std::runtime_error(message);
}
int main() {
  try {
    const std::array<std::wstring, 3> original { L"console", L"media", L"communications" };
    auto defaults = original;
    auto read = [&](int role) { return defaults[role]; };
    auto write = [&](int role, const std::wstring &id) { defaults[role] = id; };
    unsigned failed = 0;
    auto error = [&](int role, const char *) { failed |= 1u << role; };

    // Reproduce the old two-owner ordering: the new host snapshots Steam,
    // then the previous publisher restores, then the new host selects Steam.
    // The new publisher also saves Steam and neither new owner can restore.
    sonic_live::BedRouteState previous, next;
    previous.start(L"Steam", read, write);
    const auto hostOriginal = defaults; // audio_control_t::capture_default_device_roles
    previous.restore(read, write, error);
    // audio_control_t::set_default_device_roles writes every role, including
    // roles that matched Steam at the earlier snapshot.
    for (int role = 0; role < 3; ++role) write(role, L"Steam");
    next.start(L"Steam", read, write);
    for (int role = 0; role < 3; ++role)
      if (defaults[role] == L"Steam" && hostOriginal[role] != L"Steam") write(role, hostOriginal[role]);
    next.restore(read, write, error);
    check(defaults == std::array<std::wstring, 3> { L"Steam", L"Steam", L"Steam" }, "Two-owner ordering reproduction");
    puts("REPRO two route owners can lose all original defaults during reconnect");

    // Only the publisher owns the route now. Its pipe loop closes the prior
    // lease before admitting the next; preserve the original on each reconnect.
    defaults = original;
    sonic_live::BedRouteState route;
    for (int connection = 0; connection < 3; ++connection) {
      route.start(L"Steam", read, write);
      route.start(L"Steam", read, write); // A repeated start must not overwrite the backup.
      check(route.restore(read, write, error) && defaults == original, "Single-owner reconnect restoration");
    }
    route.start(L"Steam", read, write);
    defaults[1] = L"user-selected";
    route.restore(read, write, error);
    check(defaults[0] == original[0] && defaults[1] == L"user-selected" && defaults[2] == original[2], "User route override was replaced");

    defaults = original;
    route.start(L"Steam", read, write);
    check(!route.restore(read, [&](int role, const std::wstring &id) {
      if (role == 0) throw std::runtime_error("Original console device unavailable");
      write(role, id);
    }, error), "Failed role reported as restored");
    check(failed == 1 && defaults[0] == L"Steam" && defaults[1] == original[1] && defaults[2] == original[2], "One unavailable default skipped restoration of the other roles");

    defaults = original;
    bool selectionFailed = false;
    try {
      route.start(L"Steam", read, [&](int role, const std::wstring &id) {
        if (role == 1) throw std::runtime_error("Selection failed");
        write(role, id);
      });
    }
    catch (const std::runtime_error &) {
      selectionFailed = true;
    }
    check(selectionFailed && route.active(), "Partial selection lost its restore state");
    check(route.restore(read, write, error) && defaults == original, "Partial selection rollback");
    puts("PASS route ownership, reconnect, user override and independent role restoration; hardwareRoutesChanged=0");
    return 0;
  }
  catch (const std::exception &e) {
    fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
