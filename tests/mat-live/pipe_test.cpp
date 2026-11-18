// Own-process byte-pipe test. Generated carrier only; no audiodg or vendor use.
// The test-source descriptor is intentionally rejected by native preparation.
#define NOMINMAX
#include <windows.h>
#include "../../src/spatial_audio/stream_ring_source.h"
#include "../../tools/sonic-mat-live/native_queue.h"
static void *lastAllocation;
static void *
ObservedAllocation(HANDLE process, void *address, SIZE_T bytes, DWORD type, DWORD protection) {
  return lastAllocation = VirtualAllocEx(process, address, bytes, type, protection);
}
static HANDLE
DeniedThread(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, void *, DWORD, LPDWORD) {
  SetLastError(ERROR_ACCESS_DENIED);
  return nullptr;
}
#define VirtualAllocEx ObservedAllocation
#define CreateRemoteThread DeniedThread
#include "../../tools/mat-live/publisher.h"
#undef CreateRemoteThread
#undef VirtualAllocEx
#include "../../src/spatial_audio/mat_capture_reader.h"
#include "../../src/spatial_audio/mat_auto_source.h"
// Keep the production authentication code, but give this test process its own
// discovery name so the fixture never competes with an installed publisher.
namespace spatial_audio {
  inline const std::string mat_test_auto_pipe = "\\\\.\\pipe\\SunshineMatLive-test-auto-" + std::to_string(GetCurrentProcessId());
}
#define mat_auto_pipe mat_test_auto_pipe
#include "../../src/spatial_audio/native_mat_pipe_source.h"
#include "../../tools/sonic-mat-live/bed_route_state.h"
#include <atomic>
#include <iostream>
static void
CheckPipe(bool okay, const char *why) {
  if (!okay) throw std::runtime_error(why);
}
static void
SerialIoReuse() {
  const auto name = L"\\\\.\\pipe\\SunshineMatLive-io-reuse-" + std::to_wstring(GetCurrentProcessId());
  struct Handles {
    HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
    ~Handles() {
      if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
      if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
    }
  } handles;
  handles.server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
  CheckPipe(handles.server != INVALID_HANDLE_VALUE, "Reusable I/O fixture server");
  handles.client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  CheckPipe(handles.client != INVALID_HANDLE_VALUE, "Reusable I/O fixture client");
  mat_capture::PipeIo client, server;
  uint32_t received = 0;
  CheckPipe(!client.transfer(handles.client, &received, sizeof(received), false, 20) && client.error() == ERROR_TIMEOUT,
    "Pending read must time out before event reuse");
  uint32_t sent = 0x12345678;
  CheckPipe(server.transfer(handles.server, &sent, sizeof(sent), true, 2000) &&
    client.transfer(handles.client, &received, sizeof(received), false, 2000) && received == sent,
    "Timed-out read consumed later data or left a stale event");
  unsigned polls = 0;
  CheckPipe(!client.transfer(handles.client, &received, sizeof(received), false, 2000, [&] { return ++polls > 1; }) &&
    client.error() == ERROR_OPERATION_ABORTED, "Pending read cancellation");
  for (unsigned n = 0; n < 32; ++n) {
    sent = n + 100;
    CheckPipe(server.transfer(handles.server, &sent, sizeof(sent), true, 2000) &&
      client.transfer(handles.client, &received, sizeof(received), false, 2000) && received == sent,
      "Cancelled read or event reuse corrupted the next transfer");
  }
}
static void
SourcePrepareBudget(bool cancel) {
  using namespace std::chrono_literals;
  const auto name = L"\\\\.\\pipe\\SunshineMatLive-prepare-budget-" + std::to_wstring(GetCurrentProcessId()) +
    (cancel ? L"-cancel" : L"-timeout");
  HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
  CheckPipe(pipe != INVALID_HANDLE_VALUE, "Prepare-budget fixture pipe");
  spatial_audio::NativeMatPipeSource source;
  const auto began = std::chrono::steady_clock::now();
  const auto deadline = began + (cancel ? 2s : 80ms);
  source.cancel_when([&] { return cancel && std::chrono::steady_clock::now() >= began + 50ms; });
  std::string error;
  try {
    // The pipe is connected but never returns a descriptor. No real publisher
    // or audio route is used; the explicit producer is this test process.
    source.prepare(fs::path(name).string(), fs::path(native_mat_guard::image(GetCurrentProcess())).string(), true, 2000, deadline);
  } catch (const std::exception &e) { error = e.what(); }
  CloseHandle(pipe);
  const auto elapsed = std::chrono::steady_clock::now() - began;
  CheckPipe(source.state() == spatial_audio::State::fault && error == "MAT pipe I/O timeout/cancel" &&
      elapsed >= (cancel ? 50ms : 80ms) && elapsed < 1s && source.can_reprepare() == !cancel,
    "Preparation deadline/cancellation classification or shared budget");
  std::cout << "PREPARE_BUDGET cancel=" << cancel << " retryable=" << source.can_reprepare()
            << " elapsedMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << '\n';
}
static void
StatusIdentityAndFirstError() {
  spatial_audio::MatLiveStatus status;
  status.acquired(12, 100, 34, 200);
  const auto first = status.snapshot();
  CheckPipe(!status.identify_source(12, 101, 34, 200, "native", "wrong", first.lease_generation),
    "Endpoint accepted a reused publisher PID");
  CheckPipe(!status.identify_source(12, 100, 34, 201, "native", "wrong", first.lease_generation),
    "Endpoint accepted a replaced engine");
  CheckPipe(status.identify_source(12, 100, 34, 200, "native", "endpoint-a", first.lease_generation),
    "Authenticated source endpoint");
  status.started();
  status.failed("First pipe fault", "pipe_payload");
  status.failed("Secondary session exit", "pacing");
  status.record_timing("pipe_read", 250, 62000, 2, 1);
  const auto failed = status.snapshot();
  CheckPipe(failed.first_error == "First pipe fault" && failed.first_error_stage == "pipe_payload" &&
      failed.first_error_time_ms && failed.error_count == 2 && failed.last_error == "Secondary session exit" &&
      failed.endpoint == "endpoint-a" && std::string(failed.state()) == "streaming" &&
      failed.timings.at("pipe_read").samples == 250 && failed.timings.at("pipe_read").over_60ms == 1,
    "Secondary failure or observer hid the first cause/current source");
  status.released(true);
  status.acquired(12, 100, 34, 200);
  CheckPipe(!status.identify_source(12, 100, 34, 200, "native", "stale", first.lease_generation),
    "A status poll from an old lease identified a reconnected lease");
  const auto reconnected = status.snapshot();
  CheckPipe(reconnected.first_error.empty() && reconnected.first_error_stage.empty() && !reconnected.error_count &&
      reconnected.endpoint.empty() && reconnected.timings.empty() && reconnected.lease_generation != first.lease_generation,
    "New lease retained old source/error/timings");
  status.released(false);
}
static void
DrainClock() {
  auto input = std::make_unique<receiver::Shared>();
  auto output = std::make_unique<spatial_stream::Output>();
  const uint64_t nonce[2] { 1, 2 };
  sonic_live::initialize_native_queue(*input, *output, 42, 7, nonce);
  spatial_audio::StreamRingSource source(*output, 7, 42);
  CheckPipe(source.start(), "Drain clock source start");
  std::vector<uint8_t> cycle(spatial_audio::carrier_bytes);
  memcpy(cycle.data(), "\x72\xf8\x1f\x4e\x16\x00\x02\x00", 8);
  const auto captured = receiver::qpc() + receiver::hz();
  for (unsigned n = 0; n < 4; ++n) {
    spatial_stream::Block block {};
    block.bytes = spatial_audio::process_bytes;
    block.frames = 480;
    block.sampleStart = n * 480;
    block.phase = n < 2 ? 2 : 3;
    block.captureQpc = n < 2 ? captured + n * receiver::hz() / 100 : 0;
    block.completedQpc = captured - receiver::hz();
    CheckPipe(spatial_stream::publish(*output, block, cycle.data() + (n % 2) * spatial_audio::process_bytes), "Drain clock publish");
  }
  spatial_audio::Frame frame;
  CheckPipe(source.read(frame) == spatial_audio::Read::frame && source.capture_qpc() == captured, "Source capture timestamp retained");
  CheckPipe(source.read(frame) == spatial_audio::Read::frame && source.phase() == 3 && source.capture_qpc() == captured + receiver::hz() / 50, "Drain switched from capture time to completion time");
  receiver::put(output->completed, 1);
  CheckPipe(source.read(frame) == spatial_audio::Read::ended, "Drain clock source EOS");
}
static void
StartupFailureCleanup() {
  // Real own-process allocation; the OS denies writes on the first handle.
  // The second case injects thread-creation failure before any entry can run.
  for (bool copyFailure : { true, false }) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | (copyFailure ? 0 : PROCESS_VM_WRITE), FALSE, GetCurrentProcessId());
    CheckPipe(process != nullptr, "Open restricted own-process handle");
    lastAllocation = nullptr;
    bool rejected = false;
    try {
      if (copyFailure) {
        const int value = 42;
        RemoteCopy(process, &value, sizeof(value));
      }
      else
        LoadTap(process, GetCurrentProcessId(), L"mat-cleanup-test-not-loaded.dll");
    }
    catch (const std::runtime_error &error) {
      rejected = std::string(error.what()) == (copyFailure ? "WriteProcessMemory" : "CreateRemoteThread");
    }
    MEMORY_BASIC_INFORMATION memory {};
    const bool released = lastAllocation && VirtualQueryEx(process, lastAllocation, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_FREE;
    CloseHandle(process);
    CheckPipe(rejected && released, "Startup failure retained remote argument memory");
  }
}
static void
Run(unsigned mode) {
  Shared shared;
  shared.ready = true;
  shared.encoder = 42;
  shared.last_cycle = GetTickCount64();
  const auto name = L"\\\\.\\pipe\\SunshineMatLive-eos-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(mode);
  mat_capture::Descriptor descriptor { mat_capture::test_source, GetCurrentProcessId(), GetCurrentProcessId(), 1, 1, 7, 42, 10000000 };
  std::string serverError;
  std::atomic<bool> finished = false;
  HANDLE client = INVALID_HANDLE_VALUE;
  std::thread server([&] {try{Serve(shared,name,descriptor);}catch(const std::exception& e){serverError=e.what();}finished=true; });
  std::exception_ptr failure;
  try {
    auto until = GetTickCount64() + 2000;
    while (client == INVALID_HANDLE_VALUE && GetTickCount64() < until) {
      client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (client == INVALID_HANDLE_VALUE) Sleep(1);
    }
    CheckPipe(client != INVALID_HANDLE_VALUE, "Connect own-process pipe");
    char command[8];
    memcpy(command, "MCAPOPEN", 8);
    CheckPipe(Io(client, command, 8, true, 2000), "OPEN");
    uint8_t header[mat_capture::descriptor_bytes];
    CheckPipe(Io(client, header, sizeof(header), false, 2000), "Descriptor");
    mat_capture::Descriptor decoded;
    CheckPipe(!mat_capture::decode_descriptor(header, decoded), "Generated fixture must stay forbidden as native input");
    memcpy(command, "MCAPPLAY", 8);
    CheckPipe(Io(client, command, 8, true, 2000), "PLAY");
    until = GetTickCount64() + 2000;
    for (;;) {
      bool playing = false;
      {
        std::lock_guard lock(shared.mutex);
        playing = shared.playing;
      }
      if (playing) break;
      CheckPipe(GetTickCount64() < until, "PLAY admission");
      Sleep(1);
    }
    Cycle cycle {};
    memcpy(cycle.bytes.data(), "\x72\xf8\x1f\x4e\x16\x00\x02\x00", 8);
    cycle.qpc = Qpc() + (mode == 0 ? descriptor.qpc_hz : 0);
    {
      std::lock_guard lock(shared.mutex);
      shared.queue.push_back(cycle);
      shared.last_cycle = GetTickCount64();
      shared.ended = true;
      shared.changed.notify_all();
    }
    // Make the producer finish writing before consuming the tail. A write
    // completing into a pipe buffer does not mean the client consumed it.
    Sleep(100);
    uint64_t next = 0, last = 0;
    spatial_audio::Frame frame;
    bool ackAttempted = false, ended = false;
    auto io = [&](void *data, size_t n, bool writing) {
      if (writing) {
        ackAttempted = true;
        if (mode == 2) throw std::runtime_error("Simulated acknowledgement cancellation");
        if (mode == 1) mat_put64(static_cast<uint8_t *>(data) + 8, 999);
      }
      CheckPipe(Io(client, data, (DWORD) n, writing, 2000), "Tail pipe I/O");
    };
    CheckPipe(mat_capture::read_record(io, next, last, 7, 42, frame) == spatial_audio::Read::frame && next == 1 && frame.epoch == 7 && frame.domain == 42 && std::equal(frame.bytes.begin(), frame.bytes.end(), cycle.bytes.begin()), "Production reader final cycle");
    try {
      ended = mat_capture::read_record(io, next, last, 7, 42, frame) == spatial_audio::Read::ended;
    }
    catch (...) {
      if (mode != 2) throw;
    }
    CheckPipe(ackAttempted && (ended == (mode != 2)), "Unacknowledged EOS must not become clean end");
    if (mode == 2) {
      auto began = GetTickCount64();
      while (!finished && GetTickCount64() - began < 3000) Sleep(1);
      CheckPipe(finished, "Missing acknowledgement must be bounded");
    }
    CloseHandle(client);
    client = INVALID_HANDLE_VALUE;
    server.join();
    CheckPipe(serverError.empty(), "Server exception");
    CheckPipe(shared.acknowledged_leases == (mode == 0 ? 1u : 0u), "Wrong/missing end acknowledgement accepted");
    CheckPipe(mode == 0 ? shared.delivered_sha256 == "b51894f456731ba3171663abe5305090f366ee778eb56cece1009e90005f4898" : shared.delivered_sha256.empty(), "Producer stream SHA oracle / failed lease publication");
  }
  catch (...) {
    failure = std::current_exception();
  }
  if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
  if (server.joinable()) {
    Broken(shared, "Test cleanup");
    server.join();
  }
  if (failure) std::rethrow_exception(failure);
}
static void
ColdSourceReady(bool cancel) {
  Shared shared;
  shared.encoder = 42;
  const auto name = L"\\\\.\\pipe\\SunshineMatLive-cold-ready-" + std::to_wstring(GetCurrentProcessId()) + (cancel ? L"-cancel" : L"-ready");
  LARGE_INTEGER hz {};
  QueryPerformanceFrequency(&hz);
  mat_capture::Descriptor descriptor { mat_capture::test_source, GetCurrentProcessId(), GetCurrentProcessId(), 1, 1, 7, 42, uint64_t(hz.QuadPart) };
  std::string serverError;
  std::thread server([&] { try { Serve(shared, name, descriptor); } catch (const std::exception &e) { serverError = e.what(); } });
  std::thread producer;
  if (!cancel) producer = std::thread([&] {
    Sleep(150);
    std::lock_guard lock(shared.mutex);
    shared.ready = true;
    shared.last_cycle = GetTickCount64();
    shared.changed.notify_all();
  });
  const auto began = GetTickCount64();
  spatial_audio::NativeMatPipeSource source;
  source.cancel_when([&] { return cancel && GetTickCount64() - began >= 50; });
  std::string error;
  try {
    // This uses production preparation, including descriptor rejection. Test
    // bytes must reach that rejection only after readiness, without OPN3 fallback.
    source.prepare(fs::path(name).string(), fs::path(native_mat_guard::image(GetCurrentProcess())).string(), true, 2000);
  } catch (const std::exception &e) { error = e.what(); }
  const auto elapsed = GetTickCount64() - began;
  bool readyAtReturn;
  { std::lock_guard lock(shared.mutex); readyAtReturn = shared.ready; }
  Broken(shared, "Cold-ready fixture cleanup");
  if (producer.joinable()) producer.join();
  server.join();
  CheckPipe(serverError.empty(), "Cold-ready server failed");
  if (cancel) {
    CheckPipe(!readyAtReturn && elapsed < 1000 && error == "MAT pipe cancelled", "Cancel while waiting for initial source readiness");
  } else {
    CheckPipe(readyAtReturn && elapsed >= 100 && elapsed < 2000 && source.supports_restart() &&
      error == "Native MAT descriptor (test/file sources forbidden)", "Cold source failed before readiness, lost recovery negotiation, or bypassed native authentication");
  }
  printf("COLD_SOURCE_READY cancel=%u elapsedMs=%llu readyAtReturn=%u recoveryRetained=%u error=%s\n",
    unsigned(cancel), elapsed, unsigned(readyAtReturn), unsigned(source.supports_restart()), error.c_str());
}
static void
DeliveryLeaseFailure(bool expireWrite = false) {
  Shared shared;
  shared.ready = true;
  shared.encoder = 42;
  shared.last_cycle = GetTickCount64();
  const auto name = L"\\\\.\\pipe\\SunshineMatLive-delivery-failure-" + std::to_wstring(GetCurrentProcessId()) + (expireWrite ? L"-age" : L"-capacity");
  mat_capture::Descriptor descriptor { mat_capture::test_source, GetCurrentProcessId(), GetCurrentProcessId(), 1, 1, 7, 42, 10000000 };
  std::atomic<unsigned> starts {0}, restores {0};
  std::exception_ptr failure;
  HANDLE client = INVALID_HANDLE_VALUE;
  std::thread server([&] {
    try {
      Serve(shared, name, descriptor, [&](bool opening) {
        if (opening) {
          CheckPipe(starts.load() == restores.load(), "New lease opened before old route restoration");
          ++starts;
        } else ++restores;
      });
    } catch (...) { Broken(shared, "Delivery fixture server failed"); }
  });
  const auto open = [&] {
    const auto deadline = GetTickCount64() + 2000;
    do {
      client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (client != INVALID_HANDLE_VALUE) break;
      Sleep(1);
    } while (GetTickCount64() < deadline);
    CheckPipe(client != INVALID_HANDLE_VALUE, "Fresh lease after delivery failure");
    char command[8];
    memcpy(command, "MCAPOPN3", 8);
    uint8_t header[mat_capture::descriptor_bytes];
    CheckPipe(Io(client, command, 8, true, 2000) && Io(client, header, sizeof(header), false, 2000), "Delivery fixture OPEN");
  };
  try {
    open();
    char command[8];
    memcpy(command, "MCAPPLAY", 8);
    CheckPipe(Io(client, command, 8, true, 2000), "Delivery fixture PLAY");
    const auto deadline = GetTickCount64() + 2000;
    for (;;) {
      std::lock_guard lock(shared.mutex);
      if (shared.playing) break;
      CheckPipe(GetTickCount64() < deadline, "Delivery fixture playing");
      // The mutex is released on each iteration.
      std::this_thread::yield();
    }
    {
      std::lock_guard lock(shared.mutex);
      for (unsigned n = 0; n < 10; ++n) {
        Cycle cycle {};
        memcpy(cycle.bytes.data(), "\x72\xf8\x1f\x4e\x16\x00\x02\x00", 8);
        cycle.qpc = Qpc();
        shared.queue.push_back(cycle);
      }
      shared.changed.notify_all();
    }
    Sleep(100); // An unread client fills the 128 KiB pipe and blocks a write.
    {
      std::lock_guard lock(shared.mutex);
      CheckPipe(shared.queue.size() > 0 && shared.queue.size() < 10, "Fixture must have an in-flight blocked write");
      if (expireWrite) {
        // All queued and in-flight cycles are now older than this one-tick
        // budget. The actual production write callback must expire the lease.
        shared.max_cycle_age_ticks = 1;
      } else {
        shared.lease_failed = true;
        shared.playing = false;
      }
      shared.changed.notify_all();
    }
    const auto cancelled = GetTickCount64();
    while (!restores && GetTickCount64() - cancelled < 1000) Sleep(1);
    CheckPipe(restores == 1, "Blocked write was not cancelled before its 2-second timeout");
    CloseHandle(client);
    client = INVALID_HANDLE_VALUE;
    open();
    {
      std::lock_guard lock(shared.mutex);
      CheckPipe(!shared.failed && !shared.lease_failed && shared.ready && shared.queue.empty() && starts == 2,
        "Fresh lease retained old failure/data or lost native source");
    }
    CloseHandle(client);
    client = INVALID_HANDLE_VALUE;
  } catch (...) { failure = std::current_exception(); }
  if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
  Broken(shared, "Delivery test cleanup");
  server.join();
  if (failure) std::rethrow_exception(failure);
  CheckPipe(starts == 2 && restores == 2, "Delivery route ownership was not balanced");
}
static void
Reconnect(bool sourceFailure) {
  Shared shared;
  shared.ready = true;
  shared.encoder = 42;
  shared.last_cycle = GetTickCount64();
  const auto name = L"\\\\.\\pipe\\SunshineMatLive-reconnect-" + std::to_wstring(GetCurrentProcessId()) + (sourceFailure ? L"-fault" : L"-resume");
  mat_capture::Descriptor descriptor { mat_capture::test_source, GetCurrentProcessId(), GetCurrentProcessId(), 1, 1, 7, 42, 10000000 };
  std::exception_ptr failure;
  std::string serverError;
  HANDLE client = INVALID_HANDLE_VALUE;
  std::thread server([&] {try{Serve(shared,name,descriptor);}catch(const std::exception& e){serverError=e.what();} });
  const auto open = [&] {
    auto until = GetTickCount64() + 2000;
    do {
      client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (client != INVALID_HANDLE_VALUE) break;
      Sleep(1);
    } while (GetTickCount64() < until);
    CheckPipe(client != INVALID_HANDLE_VALUE, "Reconnect exclusive lease");
    char command[8];
    memcpy(command, "MCAPOPEN", 8);
    CheckPipe(Io(client, command, 8, true, 2000), "Reconnect OPEN");
    uint8_t header[mat_capture::descriptor_bytes];
    CheckPipe(Io(client, header, sizeof(header), false, 2000), "Reconnect descriptor");
    mat_capture::Descriptor decoded;
    CheckPipe(!mat_capture::decode_descriptor(header, decoded), "Reconnect fixture must not authenticate as native");
  };
  const auto play = [&] {
    char command[8];
    memcpy(command, "MCAPPLAY", 8);
    CheckPipe(Io(client, command, 8, true, 2000), "Reconnect PLAY");
    auto until = GetTickCount64() + 2000;
    for (;;) {
      {
        std::lock_guard lock(shared.mutex);
        if (shared.playing) break;
      }
      CheckPipe(GetTickCount64() < until, "Reconnect PLAY admission");
      Sleep(1);
    }
  };
  const auto cycle = [](uint8_t tag) {Cycle c{};memcpy(c.bytes.data(),"\x72\xf8\x1f\x4e\x16\x00\x02\x00",8);c.bytes[8]=tag;c.qpc=Qpc();return c; };
  const auto enqueue = [&](const Cycle &c, bool end) {std::lock_guard lock(shared.mutex);shared.queue.push_back(c);shared.last_cycle=GetTickCount64();shared.ended=end;shared.changed.notify_all(); };
  const auto read = [&](uint64_t &next, uint64_t &last, spatial_audio::Frame &frame) { return mat_capture::read_record([&](void *p, size_t n, bool writing) { CheckPipe(Io(client, p, (DWORD) n, writing, 2000), "Reconnect record I/O"); }, next, last, 7, 42, frame); };
  try {
    // An already leased publisher can also belong to a different host process.
    open();
    CheckPipe(spatial_audio::NativeMatPipeSource::busy(name), "DESCRIBE must detect an occupied pipe without taking a lease");
    spatial_audio::NativeMatPipeSource contender;
    const auto began = std::chrono::steady_clock::now();
    try { contender.prepare(std::filesystem::path(name).string(), "unused", true, 2000, began + std::chrono::seconds(2), false); }
    catch (const std::runtime_error &) {}
    CheckPipe(contender.can_reprepare() && std::chrono::steady_clock::now() - began < std::chrono::milliseconds(300), "Busy DESCRIBE must be immediately retryable");
    // DESCRIBE/prepare cancellation must release the single lease before PLAY.
    CloseHandle(client);
    client = INVALID_HANDLE_VALUE;
    open();
    play();
    if (sourceFailure) {
      Broken(shared, "Controlled source failure, no clean EOS");
      uint64_t next = 0, last = 0;
      spatial_audio::Frame frame;
      bool rejected = false;
      try {
        read(next, last, frame);
      }
      catch (...) {
        rejected = true;
      }
      CheckPipe(rejected, "Source failure fabricated EOS");
    }
    else {
      enqueue(cycle(0x11), false);
      uint64_t next = 0, last = 0;
      spatial_audio::Frame frame;
      CheckPipe(read(next, last, frame) == spatial_audio::Read::frame && next == 1 && frame.bytes[8] == 0x11, "First lease data");
      CloseHandle(client);
      client = INVALID_HANDLE_VALUE;
      // A subsequent real cycle makes the writer observe the disconnect.
      enqueue(cycle(0x22), false);
      open();
      {
        std::lock_guard lock(shared.mutex);
        CheckPipe(!shared.playing && !shared.failed && shared.queue.empty(), "Disconnected lease retained data/admission");
        shared.queue.push_back(cycle(0x33));
      }
      play();
      {
        std::lock_guard lock(shared.mutex);
        CheckPipe(shared.queue.empty(), "New PLAY retained pre-PLAY data");
      }
      enqueue(cycle(0x44), true);
      next = 0;
      last = 0;
      CheckPipe(read(next, last, frame) == spatial_audio::Read::frame && next == 1 && frame.bytes[8] == 0x44, "Reconnect sequence/cutoff mixed old cycle");
      CheckPipe(read(next, last, frame) == spatial_audio::Read::ended, "Reconnect clean EOS");
    }
    CloseHandle(client);
    client = INVALID_HANDLE_VALUE;
    server.join();
    CheckPipe(serverError.empty() && shared.acknowledged_leases == (sourceFailure ? 0u : 1u), "Reconnect lease/EOS accounting");
    CheckPipe(!shared.playing && shared.queue.empty(), "Reconnect final lease release");
  }
  catch (...) {
    failure = std::current_exception();
  }
  if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
  if (server.joinable()) {
    Broken(shared, "Reconnect test cleanup");
    server.join();
  }
  if (failure) std::rethrow_exception(failure);
}
static void
Restart(unsigned mode) {
  Shared shared;
  shared.ready = true;
  shared.encoder = 42;
  shared.last_cycle = GetTickCount64();
  const auto name = L"\\\\.\\pipe\\SunshineMatLive-restart-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(mode);
  // This descriptor exercises only the byte barrier. NativeMatPipeSource still
  // rejects this process as audiodg; no authenticated native source is mocked.
  mat_capture::Descriptor descriptor { mat_capture::native_object_source, GetCurrentProcessId(), GetCurrentProcessId(), 1, 1, 7, 42, 10000000 };
  std::array<std::wstring, 3> defaults { L"console-original", L"media-original", L"communications-original" };
  const auto original = defaults;
  sonic_live::BedRouteState route;
  std::atomic<unsigned> starts = 0, restores = 0;
  std::string serverError;
  HANDLE client = INVALID_HANDLE_VALUE;
  std::thread server([&] {
    try {
      Serve(shared, name, descriptor, [&](bool opening) {
        const auto read = [&](int role) { return defaults[role]; };
        const auto write = [&](int role, const std::wstring &endpoint) { defaults[role] = endpoint; };
        if (opening) { route.start(L"capture", read, write); ++starts; }
        else { route.restore(read, write, [](int, const char *) {}); ++restores; }
      });
    }
    catch (const std::exception &e) { serverError = e.what(); }
  });
  std::exception_ptr failure;
  try {
    const auto until = GetTickCount64() + 2000;
    do {
      client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (client == INVALID_HANDLE_VALUE) Sleep(1);
    } while (client == INVALID_HANDLE_VALUE && GetTickCount64() < until);
    CheckPipe(client != INVALID_HANDLE_VALUE, "Restart own-process connection");
    const auto io = [&](void *p, size_t bytes, bool writing) {
      CheckPipe(Io(client, p, DWORD(bytes), writing, 2000), "Restart pipe I/O");
    };
    char command[8];
    memcpy(command, mode == 1 ? "MCAPOPEN" : "MCAPOPN3", 8);
    io(command, sizeof(command), true);
    uint8_t header[mat_capture::descriptor_bytes];
    io(header, sizeof(header), false);
    const auto play = [&] {
      memcpy(command, "MCAPPLAY", 8);
      io(command, sizeof(command), true);
      const auto deadline = GetTickCount64() + 2000;
      for (;;) {
        { std::lock_guard lock(shared.mutex); if (shared.playing) break; }
        CheckPipe(GetTickCount64() < deadline, "Restart PLAY admission");
        Sleep(1);
      }
    };
    const auto enqueue = [&](uint8_t tag) {
      Cycle cycle {};
      memcpy(cycle.bytes.data(), "\x72\xf8\x1f\x4e\x16\x00\x02\x00", 8);
      cycle.bytes[8] = tag;
      cycle.qpc = Qpc();
      std::lock_guard lock(shared.mutex);
      shared.queue.push_back(cycle);
      shared.last_cycle = GetTickCount64();
      shared.changed.notify_all();
    };
    play();
    enqueue(0x11);
    uint64_t next = 0, last = 0;
    spatial_audio::Frame frame;
    CheckPipe(mat_capture::read_record(io, next, last, 7, 42, frame) == spatial_audio::Read::frame && next == 1,
      "Restart initial generation");
    enqueue(0x22); // May already be buffered; the barrier must consume it.
    if (mode == 2) Broken(shared, "Controlled damaged source");
    memcpy(command, "MCAPREST", 8);
    bool restarted = false;
    try {
      io(command, sizeof(command), true);
      mat_capture::restart_barrier(io, next, last, descriptor);
      restarted = true;
    }
    catch (...) { if (mode != 1 && mode != 2) throw; }
    CheckPipe(restarted == (mode != 1 && mode != 2), "Legacy/damaged source accepted restart");
    if (restarted) {
      CheckPipe(next == 0 && last == 0 && starts == 1 && restores == 0, "Restart changed route lease or retained sequence");
      {
        std::lock_guard lock(shared.mutex);
        CheckPipe(!shared.playing && shared.queue.empty(), "Restart retained old source admission/queue");
      }
      if (mode != 3) {
        play();
        enqueue(0x33);
        CheckPipe(mat_capture::read_record(io, next, last, 7, 42, frame) == spatial_audio::Read::frame &&
            frame.sequence == 0 && next == 1 && frame.bytes[8] == 0x33,
          "Restart crossed generation bytes/counter");
      }
      // Mode 3 cancels while the publisher is awaiting the new PLAY.
    }
  }
  catch (...) { failure = std::current_exception(); }
  if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
  Broken(shared, "Restart fixture stopped");
  server.join();
  if (failure) std::rethrow_exception(failure);
  CheckPipe(serverError.empty() && starts == 1 && restores == 1 && defaults == original && !route.active(),
    "Restart/cancel did not restore the original three-role snapshot exactly once");
}
static void
RestartBarrierRejects() {
  mat_capture::Descriptor expected { mat_capture::native_object_source, 1, 2, 3, 4, 7, 42, 10000000 };
  for (unsigned mode = 0; mode < 4; ++mode) {
    uint8_t header[mat_capture::descriptor_bytes];
    auto received = expected;
    if (mode == 0) ++received.engine_created;
    if (mode == 1) ++received.encoder;
    mat_capture::encode_descriptor(header, received);
    if (mode == 2) mat_capture::encode_record(header, mat_capture::error_kind, 2, 10, nullptr, 0);
    size_t offset = 0;
    uint64_t next = 2, last = 9;
    bool rejected = false;
    try {
      mat_capture::restart_barrier([&](void *p, size_t bytes, bool writing) {
        CheckPipe(!writing && mode != 3 && offset + bytes <= sizeof(header), "Restart barrier cancelled");
        memcpy(p, header + offset, bytes);
        offset += bytes;
      }, next, last, expected);
    }
    catch (...) { rejected = true; }
    CheckPipe(rejected && next == 2 && last == 9, "Failed restart reset identity/counters");
  }
}
static void
RejectNativePeer(unsigned mode) {
  // A mock publisher may exercise rejection/cleanup, but can never pass the
  // production audiodg identity check or supply a native stream to the host.
  const auto name = mode == 3 ? fs::path(spatial_audio::mat_auto_pipe).wstring() : L"\\\\.\\pipe\\SunshineMatLive-reject-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(mode);
  HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
  CheckPipe(pipe != INVALID_HANDLE_VALUE, "Mock publisher pipe");
  std::string peerError;
  std::atomic<bool> disconnected = false;
  std::thread peer([&] {try{
        OVERLAPPED connection{};connection.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);CheckPipe(connection.hEvent!=nullptr,"Mock connect event");
        BOOL okay=ConnectNamedPipe(pipe,&connection);DWORD code=okay?0:GetLastError();
        if(code==ERROR_PIPE_CONNECTED)okay=TRUE;
        if(!okay&&code==ERROR_IO_PENDING){
            DWORD n=0;auto waited=WaitForSingleObject(connection.hEvent,2000);
            if(waited!=WAIT_OBJECT_0){CancelIoEx(pipe,&connection);GetOverlappedResult(pipe,&connection,&n,TRUE);CloseHandle(connection.hEvent);throw std::runtime_error("Mock connect timeout");}
            okay=GetOverlappedResult(pipe,&connection,&n,FALSE);
        }
        const auto connectError=okay?0:GetLastError();CloseHandle(connection.hEvent);
        if(mode==3&&!okay&&(connectError==ERROR_NO_DATA||connectError==ERROR_PIPE_NOT_CONNECTED||connectError==ERROR_BROKEN_PIPE)){
            disconnected=true;return; // Rejection can finish before the server observes connect.
        }
        CheckPipe(okay,"Mock source connected");
        if(mode==3){
            char next;auto began=GetTickCount64();disconnected=!Io(pipe,&next,1,false,1000)&&GetTickCount64()-began<1000;
            return; // Automatic discovery must reject us before sending OPEN.
        }
        char open[8];CheckPipe(Io(pipe,open,8,false,2000),"Mock OPEN read");
        if(mode==4){
            CheckPipe(!memcmp(open,"MCAPOPN3",8),"Recovery capability probe");
            DisconnectNamedPipe(pipe); // An old publisher rejects before selecting a route.
            OVERLAPPED reconnect{};reconnect.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            CheckPipe(reconnect.hEvent!=nullptr,"Fallback connect event");
            BOOL accepted=ConnectNamedPipe(pipe,&reconnect);DWORD reconnectCode=accepted?0:GetLastError();
            if(reconnectCode==ERROR_PIPE_CONNECTED)accepted=TRUE;
            if(!accepted&&reconnectCode==ERROR_IO_PENDING){
                DWORD n=0;
                if(WaitForSingleObject(reconnect.hEvent,2000)!=WAIT_OBJECT_0){CancelIoEx(pipe,&reconnect);GetOverlappedResult(pipe,&reconnect,&n,TRUE);CloseHandle(reconnect.hEvent);throw std::runtime_error("Fallback connect timeout");}
                accepted=GetOverlappedResult(pipe,&reconnect,&n,FALSE);
            }
            CloseHandle(reconnect.hEvent);
            CheckPipe(accepted&&Io(pipe,open,8,false,2000),"Fallback OPEN read");
        }
        CheckPipe(!memcmp(open,"MCAPOPEN",8),"Mock OPEN");
        mat_capture::Descriptor d{mode==0?mat_capture::test_source:mat_capture::native_object_source,GetCurrentProcessId(),GetCurrentProcessId(),1,1,7,42,10000000};
        if(mode){const auto created=native_mat_guard::created(GetCurrentProcess());d.publisher_created=d.engine_created=created;}
        if(mode==2)++d.publisher_pid;
        uint8_t bytes[mat_capture::descriptor_bytes];mat_capture::encode_descriptor(bytes,d);CheckPipe(Io(pipe,bytes,sizeof(bytes),true,2000),"Mock descriptor");
        // prepare() must close the rejected lease while its source object is
        // still alive, instead of retaining it until launch-ticket destruction.
        char next;auto began=GetTickCount64();disconnected=!Io(pipe,&next,1,false,1000)&&GetTickCount64()-began<1000;
    }catch(const std::exception& e){peerError=e.what();} });
  spatial_audio::NativeMatPipeSource source;
  bool rejected = false;
  try {
    source.prepare(fs::path(name).string(), mode == 3 ? "" : fs::path(native_mat_guard::image(GetCurrentProcess())).string(), mode == 4,
      0, std::chrono::steady_clock::now() + std::chrono::seconds(2));
  }
  catch (...) {
    rejected = true;
  }
  peer.join();
  CloseHandle(pipe);
  if (!(rejected && peerError.empty() && disconnected)) std::cerr << "REJECT_MODE " << mode << " source=" << source.error() << " peer=" << peerError << " closed=" << disconnected << '\n';
  CheckPipe(rejected && peerError.empty() && disconnected, "Rejected native preparation retained the lease");
  CheckPipe(source.state() == spatial_audio::State::fault && !source.error().empty() && !source.start(), "Rejected native source state/diagnostic");
  CheckPipe(!source.can_reprepare(), "Identity/format failure must not become a retryable budget expiry");
  const auto error = source.error();
  CheckPipe(!source.restart() && source.error() == error && (mode != 4 || !source.supports_restart()),
    "Faulted source restarted or old publisher advertised restart support");
  source.stop();
  source.stop();
  CheckPipe(source.state() == spatial_audio::State::fault && source.error() == error, "Stop erased source failure");
  const auto observed = spatial_audio::mat_live_status.snapshot();
  CheckPipe(observed.leases == 0 && observed.running == 0 && observed.last_error == error && std::string(observed.state()) == "error", "Rejected source status retained a lease/lost its error");
}
int
main() {
  try {
    SerialIoReuse();
    StatusIdentityAndFirstError();
    DrainClock();
    StartupFailureCleanup();
    const fs::path programs = L"C:\\Program Files";
    const std::string hash(64, 'a');
    const auto installed = programs / L"SunshineSonicMatLive" / hash / L"sunshine-plugin-mat-capture.exe";
    CheckPipe(spatial_audio::mat_auto_image(installed, programs, hash), "Automatic installed capture identity");
    CheckPipe(!spatial_audio::mat_auto_image(installed, programs, std::string(64, 'b')) &&
                !spatial_audio::mat_auto_image(installed.parent_path() / L"other.exe", programs, hash) &&
                !spatial_audio::mat_auto_image(installed, programs / L"other", hash) &&
                !spatial_audio::trusted_mat_auto_publisher(GetCurrentProcess()),
      "Automatic source accepted wrong executable/root/hash/process");
    spatial_audio::MatLiveStatus status;
    CheckPipe(std::string(status.snapshot().state()) == "unobserved", "Configuration is not readiness");
    status.acquired();
    status.started();
    status.failed("Competing lease failed");
    CheckPipe(std::string(status.snapshot().state()) == "streaming", "Failed contender hid active stream");
    std::thread observer([&] {for(unsigned i=0;i<10000;++i){const auto s=status.snapshot();CheckPipe(s.leases==1&&s.running==1,"Status observation changed ownership");} });
    for (unsigned i = 0; i < 10000; ++i) status.failed("Competing lease failed");
    observer.join();
    status.released(true);
    CheckPipe(std::string(status.snapshot().state()) == "error", "Release lost latest failure");
    status.acquired();
    CheckPipe(std::string(status.snapshot().state()) == "ready" && status.snapshot().last_error.empty(), "Reconnect did not clear stale error");
    status.released(false);
    CheckPipe(std::string(status.snapshot().state()) == "idle", "Release falsely advertises readiness");
    for (unsigned mode = 0; mode < 3; ++mode) Run(mode);
    Reconnect(false);
    Reconnect(true);
    ColdSourceReady(false);
    ColdSourceReady(true);
    SourcePrepareBudget(false);
    SourcePrepareBudget(true);
    DeliveryLeaseFailure();
    DeliveryLeaseFailure(true);
    for (unsigned mode = 0; mode < 4; ++mode) Restart(mode);
    RestartBarrierRejects();
    for (unsigned mode = 0; mode < 5; ++mode) RejectNativePeer(mode);
    for (unsigned mode = 0; mode < 3; ++mode) {
      uint8_t record[mat_capture::record_bytes];
      uint64_t next = mode == 0 ? 0 : 1, last = 0;
      spatial_audio::Frame frame;
      mat_capture::encode_record(record, mode == 2 ? mat_capture::error_kind : mat_capture::end_kind, next, 1, nullptr, 0);
      if (mode == 1) record[28] ^= 1;
      bool wrote = false, failed = false;
      try {
        mat_capture::read_record([&](void *p, size_t n, bool writing) {if(writing)wrote=true;else {CheckPipe(n==sizeof(record),"Control read size");memcpy(p,record,n);} }, next, last, 7, 42, frame);
      }
      catch (...) {
        failed = true;
      }
      CheckPipe(failed && !wrote, "Invalid/empty/error EOF acknowledged");
    }
    std::cout << "PASS production pipe/reader delayed tail, exact EOS acknowledgement, wrong/missing/cancelled acknowledgement, prepare cancellation, streaming disconnect/reconnect, fresh PLAY cutoff/sequence, source failure without EOS, native peer rejection/release and invalid/empty/error EOF; nativeCapture=0\n";
    return 0;
  }
  catch (const std::exception &e) {
    std::cerr << "FAIL_PIPE_TAIL " << e.what() << " nativeCapture=0\n";
    return 1;
  }
}
