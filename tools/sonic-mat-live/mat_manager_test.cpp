// Default: own-process tests, no installed task/audio engine/endpoint touched.
// --request-installed explicitly exercises the production control handshake.
#include "../../src/spatial_audio/mat_manager_control.h"
#include "mat_manager_policy.h"
#include "managed_capture.h"
#include <iostream>
#include <thread>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

static void check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

static void policy() {
  const std::wstring old = L"native|endpoint|20196|100|14332|200";
  const std::wstring new_pid = L"native|endpoint|20196|100|16632|300";
  const std::wstring reused_pid = L"native|endpoint|20196|100|14332|300";
  auto action = sonic_live::mat_manager_action(old, old, true, true);
  check(!action.stop_child && !action.reset_failure, "Healthy capture must survive client demand");
  action = sonic_live::mat_manager_action(old, old, false, true);
  check(!action.stop_child && action.reset_failure, "Failed capture must retry on client demand");
  action = sonic_live::mat_manager_action(old, old, false, false);
  check(!action.reset_failure, "Fatal failure must not loop without a new request");
  for (const auto &next : { new_pid, reused_pid }) {
    action = sonic_live::mat_manager_action(old, next, true, false);
    check(action.stop_child && action.reset_failure, "Host restart must replace running capture");
    action = sonic_live::mat_manager_action(old, next, false, false);
    check(!action.stop_child && action.reset_failure, "Host restart must clear failure lock");
  }
  action = sonic_live::mat_manager_action(old, L"", true, true);
  check(action.stop_child && !action.reset_failure, "Disabled MAT or absent host must stop capture even on demand");
  action = sonic_live::mat_manager_action(L"", old, false, true);
  check(!action.stop_child && action.reset_failure, "Cold capture must start");
}

static void managed_source_changes() {
  namespace fs = std::filesystem;
  using sonic_live::CaptureAvailability;
  const auto name = L"Local\\MatManagedTest-" + std::to_wstring(GetCurrentProcessId());
  const auto directory = fs::temp_directory_path() / (L"mat-managed-" + std::to_wstring(GetCurrentProcessId()));
  fs::create_directory(directory);
  const auto log = directory / "logs" / "capture.log", marker = directory / "capture.lease";
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE owner = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  {
    sonic_live::ManagedCapture capture;
    capture.open(name, log, 101);
    check(sonic_live::capture_availability(marker, 101) == CaptureAvailability::cleaning, "Old publisher still owns cleanup");
    SetEvent(owner);
    check(capture.replacing(stop, owner), "Lost manager must disconnect the lease");
    SetEvent(stop);
    check(!capture.replacing(stop, owner), "Explicit stop must drain");
    HANDLE change = OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L"-replace").c_str());
    check(change && SetEvent(change) && capture.replacing(stop, owner), "Manager replacement must disconnect the lease");
    CloseHandle(change);
  }
  check(sonic_live::capture_availability(marker, 101) == CaptureAvailability::available, "Clean capture can be replaced");
  {
    sonic_live::ManagedCapture capture;
    capture.open(name, log, 101);
    capture.clean = false;
  }
  check(sonic_live::capture_availability(marker, 101) == CaptureAvailability::blocked, "Manager restart must retain incomplete cleanup");
  check(sonic_live::capture_availability(marker, 102) == CaptureAvailability::available, "A new engine generation can recover");
  {
    sonic_live::ManagedCapture capture;
    capture.open(name, log, 102);
    capture.route_failed();
  }
  check(sonic_live::capture_availability(marker, 103) == CaptureAvailability::blocked, "A new engine cannot restore a failed route");
  fs::remove(marker);
  fs::remove(directory);
  CloseHandle(owner);
  CloseHandle(stop);
}

static void events() {
  const auto base = L"Local\\SunshineMatManagerTest-" + std::to_wstring(GetCurrentProcessId());
  const auto request_name = base + L"-request", ack_name = base + L"-ack";
  struct Fixture {
    HANDLE request = nullptr, ack = nullptr;
    ~Fixture() { if (request) CloseHandle(request); if (ack) CloseHandle(ack); }
  } fixture;
  unsigned starts = 0;
  std::thread manager;
  auto start = [&] {
    ++starts;
    fixture.request = CreateEventW(nullptr, FALSE, FALSE, request_name.c_str());
    fixture.ack = CreateEventW(nullptr, TRUE, FALSE, ack_name.c_str());
    check(fixture.request && fixture.ack, "Create fixture events");
    manager = std::thread([&] {
      if (WaitForSingleObject(fixture.request, 1000) == WAIT_OBJECT_0) SetEvent(fixture.ack);
    });
  };
  try {
    spatial_audio::request_mat_manager(Clock::now() + 2s, {}, start, request_name.c_str(), ack_name.c_str());
  } catch (...) { if (manager.joinable()) manager.join(); throw; }
  manager.join();
  check(starts == 1, "Missing manager must start exactly once");

  // Ack remains signalled from the preceding call. A new call must wait for
  // this request's processing instead of accepting that stale acknowledgement.
  manager = std::thread([&] {
    if (WaitForSingleObject(fixture.request, 1000) == WAIT_OBJECT_0) {
      Sleep(80);
      SetEvent(fixture.ack);
    }
  });
  const auto began = Clock::now();
  try {
    spatial_audio::request_mat_manager(Clock::now() + 2s, {}, start, request_name.c_str(), ack_name.c_str());
  } catch (...) { manager.join(); throw; }
  manager.join();
  check(starts == 1 && Clock::now() - began >= 70ms, "Existing manager must acknowledge without task restart");

  bool timed_out = false;
  try {
    spatial_audio::request_mat_manager(Clock::now() + 80ms, {}, start, request_name.c_str(), ack_name.c_str());
  } catch (const std::runtime_error &) { timed_out = true; }
  check(timed_out && starts == 1, "Unresponsive manager must time out, not restart a healthy task");

  const auto cancel_at = Clock::now() + 40ms;
  bool cancelled = false;
  try {
    spatial_audio::request_mat_manager(Clock::now() + 10s, [&] { return Clock::now() >= cancel_at; },
      start, request_name.c_str(), ack_name.c_str());
  } catch (const std::runtime_error &) { cancelled = true; }
  check(cancelled && Clock::now() < cancel_at + 300ms && starts == 1, "Cancellation must interrupt the acknowledgement wait");

  HANDLE blocked = CreateEventW(nullptr, TRUE, TRUE, (ack_name + L"-blocked").c_str());
  check(blocked != nullptr, "Create blocked status");
  ResetEvent(fixture.request);
  manager = std::thread([&] {
    if (WaitForSingleObject(fixture.request, 1000) == WAIT_OBJECT_0) SetEvent(fixture.ack);
  });
  bool unsafe = false;
  try {
    spatial_audio::request_mat_manager(Clock::now() + 1s, {}, start, request_name.c_str(), ack_name.c_str());
  } catch (const spatial_audio::MatSourceBlocked &) { unsafe = true; }
  manager.join();
  CloseHandle(blocked);
  check(unsafe && starts == 1, "Incomplete cleanup must be returned as blocked");

  unsigned missing_starts = 0;
  timed_out = false;
  const auto missing = base + L"-missing";
  try {
    spatial_audio::request_mat_manager(Clock::now() + 80ms, {}, [&] { ++missing_starts; }, missing.c_str(), missing.c_str());
  } catch (const std::runtime_error &) { timed_out = true; }
  check(timed_out && missing_starts == 1, "Task startup without control endpoint must time out");
}

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--request-installed") {
      spatial_audio::request_mat_manager(Clock::now() + 12s);
      std::cout << "PASS installed MAT manager request acknowledged\n";
      return 0;
    }
    check(argc == 1, "Unexpected test option");
    policy();
    managed_source_changes();
    events();
    std::cout << "PASS host restart/PID reuse, failed/healthy/disabled demand, task startup, acknowledgement and timeouts; nativeCapture=0\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
