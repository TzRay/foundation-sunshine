#include "native_access.h"
#include "native_output_protocol.h"
#include "native_output_relay.h"
#include "components.h"
#include "native_endpoint_keepalive.h"
#include "bed_route.h"
#include "managed_capture.h"
#include "../../src/platform/windows/mmcss_audio.h"
#include <fstream>

static void prioritize_delivery(const char *role, const platf::mmcss_audio_t &task) {
  // These application threads drain the native callback ring and feed the
  // host. Keep their bounded queues draining under competing application load.
  const bool raised = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != FALSE;
  const DWORD error = raised ? ERROR_SUCCESS : GetLastError();
  printf("NATIVE_DELIVERY_THREAD role=%s priority=%d error=%lu mmcss=%u mmcssError=%lu\n",
    role, GetThreadPriority(GetCurrentThread()), error, unsigned(task.active()), task.error());
}

static bool original_entry(HANDLE process) {
  auto base = Module(GetProcessId(process), L"audioeng.dll");
  BYTE bytes[sizeof(native_output::process_entry)] {};
  BYTE render[sizeof(native_output::render_entry)] {};
  SIZE_T got = 0;
  return base && ReadProcessMemory(process, reinterpret_cast<void *>(base + native_output::process_rva), bytes, sizeof(bytes), &got) &&
         got == sizeof(bytes) && !memcmp(bytes, native_output::process_entry, sizeof(bytes)) &&
         ReadProcessMemory(process, reinterpret_cast<void *>(base + native_output::render_rva), render, sizeof(render), &got) &&
         got == sizeof(render) && !memcmp(render, native_output::render_entry, sizeof(render));
}

static void inventory() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  require(snap != INVALID_HANDLE_VALUE, "Process inventory");
  PROCESSENTRY32W entry { sizeof(entry) };
  if (Process32FirstW(snap, &entry)) do {
    if (_wcsicmp(entry.szExeFile, L"audiodg.exe")) continue;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
    printf("ENGINE pid=%lu created=%llu\n", entry.th32ProcessID, process ? native_mat_guard::created(process) : 0);
    if (process) CloseHandle(process);
  } while (Process32NextW(snap, &entry));
  CloseHandle(snap);
}

int RunNativeOutput(int argc, wchar_t **argv) {
  SetConsoleOutputCP(CP_UTF8);
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 2 && std::wstring(argv[1]) == L"--inventory") { inventory(); return 0; }
  fs::path log_path;
  if (argc >= 3 && std::wstring_view(argv[argc - 2]) == L"--log") {
    log_path = argv[argc - 1];
    argc -= 2;
  }
  uint64_t owner_pid = 0, owner_created = 0;
  if (argc >= 4 && std::wstring_view(argv[argc - 3]) == L"--owner") {
    try { owner_pid = std::stoull(argv[argc - 2]); owner_created = std::stoull(argv[argc - 1]); }
    catch (...) { puts("Invalid manager owner identity"); return 2; }
    if (!owner_pid || owner_pid > MAXDWORD || !owner_created) return 2;
    argc -= 3;
  }
  const bool managed = argc == 9 && std::wstring(argv[1]) == L"--serve-endpoint";
  const bool preflight = argc == 4 && std::wstring(argv[1]) == L"--preflight";
  const bool live = managed || (argc == 10 && std::wstring(argv[1]) == L"--publish-endpoint");
  const bool bound = live || (argc == 9 && std::wstring(argv[1]) == L"--capture-endpoint");
  if (!preflight && !bound && !(argc == 8 && std::wstring(argv[1]) == L"--capture")) {
    puts("native-output-capture --inventory | --preflight PID CREATED | --capture PID CREATED TAP TAP_SHA SECONDS OUTPUT_DIR\n"
         "  --capture-endpoint PID CREATED TAP TAP_SHA SECONDS OUTPUT_DIR ENDPOINT_ID\n"
         "  --publish-endpoint PID CREATED TAP TAP_SHA SECONDS OUTPUT_DIR ENDPOINT_ID PIPE\n"
         "  --serve-endpoint PID CREATED TAP TAP_SHA ENDPOINT_ID PIPE STOP [--owner PID CREATED] [--log NEW_FILE]\n"
         "Capture is bounded to 1..60 seconds, uses a fresh directory, and requires an administrator token. It does not select an endpoint or start audio.");
    return 2;
  }
  HANDLE engine {}, map {}, thread {}, remote_map {}, remote_owner {}, stop {}, owner {}, tap_lock = INVALID_HANDLE_VALUE;
  sonic_live::ManagedCapture control;
  void *arguments {};
  native_output::Ring *ring {};
  bool handed = false, clean = false;
  uint64_t written = 0, total_bytes = 0;
  fs::path directory;
  std::ofstream output;
  Shared shared;
  NativeOutputRelay relay;
  uint64_t source_epoch = 0, qpc_hz = 0, last_callback_qpc = 0;
  uint64_t timing_samples = 0, callback_gap_max_us = 0, callback_gap_over_20ms = 0;
  uint64_t ring_age_max_us = 0, ring_age_over_60ms = 0, ring_peak = 0, reported = GetTickCount64();
  const auto microseconds = [&](uint64_t ticks) { return ticks / qpc_hz * 1000000 + ticks % qpc_hz * 1000000 / qpc_hz; };
  const auto report_delivery = [&](bool final = false) {
    const auto now = GetTickCount64();
    if (!timing_samples || (!final && now - reported < 5000)) return;
    size_t queue = 0, queue_peak = 0;
    uint64_t waits = 0, wait_max_ms = 0, wait_over_10ms = 0;
    {
      std::lock_guard lock(shared.mutex);
      queue = shared.queue.size();
      queue_peak = shared.queue_peak;
      waits = shared.consumer_waits;
      wait_max_ms = shared.consumer_wait_max_ms;
      wait_over_10ms = shared.consumer_wait_over_10ms;
    }
    // All formatting runs in this out-of-process reader, never in audiodg.
    printf("NATIVE_DELIVERY_TIMING sourceEpoch=%llu records=%llu windowSamples=%llu callbackGapMaxUs=%llu callbackGapOver20Ms=%llu ringAgeMaxUs=%llu ringAgeOver60Ms=%llu ringPeak=%llu queue=%zu peakQueue=%zu consumerWaits=%llu consumerWaitMaxMs=%llu consumerWaitOver10Ms=%llu final=%u\n",
      source_epoch, written, timing_samples, callback_gap_max_us, callback_gap_over_20ms,
      ring_age_max_us, ring_age_over_60ms, ring_peak, queue, queue_peak, waits, wait_max_ms, wait_over_10ms, unsigned(final));
    timing_samples = callback_gap_max_us = callback_gap_over_20ms = ring_age_max_us = ring_age_over_60ms = ring_peak = 0;
    reported = now;
  };
  std::thread server;
  std::unique_ptr<sonic_live::NativeEndpointKeepalive> keepalive;
  bool restart = false;
  std::atomic<bool> route_clean { true };
  int result = 1;
  SetConsoleCtrlHandler(Ctrl, TRUE);
  try {
    redirect_native_log(log_path);
    const auto pid64 = std::stoull(argv[2]), creation = std::stoull(argv[3]);
    require(pid64 && pid64 <= MAXDWORD && creation, "Explicit audiodg PID and creation time required");
    const auto seconds = managed ? 0ull : preflight ? 1ull : std::stoull(argv[6]);
    require(managed || (seconds >= 1 && seconds <= 60), "Capture duration must be 1..60 seconds");
    const std::wstring endpoint = bound ? argv[managed ? 6 : 8] : L"";
    require(!bound || (endpoint.size() == 55 && endpoint.starts_with(L"{0.0.0.00000000}.{") && endpoint.back() == L'}'), "Explicit render endpoint ID required");
    const std::wstring pipe = live ? argv[managed ? 7 : 9] : L"";
    require(!live || (pipe.starts_with(L"\\\\.\\pipe\\SunshineMatLive-") && pipe.find_first_of(L"\r\n") == pipe.npos), "Explicit local native MAT pipe required");
    if (!preflight && !managed) {
      directory = fs::absolute(argv[7]);
      require(!fs::exists(directory) && fs::create_directory(directory), "Fresh capture directory required");
      redirect_native_log(directory / "capture.log");
    }
    enable_native_access();
    if (owner_pid) {
      owner = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(owner_pid));
      require(owner && native_mat_guard::created(owner) == owner_created && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT, "Manager owner unavailable");
    }
    if (managed) {
      const std::wstring event = argv[8];
      require(event.starts_with(L"Local\\SunshineMatLiveStop-"), "Native endpoint stop event required");
      if (owner) control.open(event, log_path, creation);
      auto acl = PipeAcl();
      SECURITY_ATTRIBUTES attributes { sizeof(attributes), acl, FALSE };
      stop = CreateEventW(&attributes, TRUE, FALSE, event.c_str());
      const auto error = GetLastError();
      LocalFree(acl);
      require(stop && error != ERROR_ALREADY_EXISTS, "Fresh native endpoint stop event required");
      keepalive = std::make_unique<sonic_live::NativeEndpointKeepalive>(endpoint);
    }
    const auto dolby = sonic_live::find_dolby_access();
    require(sonic_live::report_components(mat_compat::audio_engine(), dolby.component), "Unsupported Windows/Dolby build");
    const DWORD access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE |
                         (preflight ? 0 : PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | PROCESS_DUP_HANDLE);
    engine = OpenProcess(access, FALSE, static_cast<DWORD>(pid64));
    require(engine && native_mat_guard::created(engine) == creation && native_mat_guard::system_engine(engine), "Pinned system audiodg identity");
    require(LoadedTap(static_cast<DWORD>(pid64), fs::canonical(native_mat_guard::system(L"audioeng.dll"))) && original_entry(engine), "Original system Process callback");
    require(LoadedTap(static_cast<DWORD>(pid64), fs::canonical(fs::path(dolby.directory) / L"DolbyMatEnc.dll")) &&
            LoadedTap(static_cast<DWORD>(pid64), fs::canonical(fs::path(dolby.directory) / L"DolbyMatEngine.dll")), "System Dolby modules must already be loaded; no encoder activation attempted");
    printf("PREFLIGHT pid=%llu created=%llu nativeOutput=unverified attach=0\n", pid64, creation);
    if (preflight) { CloseHandle(engine); return 0; }
    const auto tap = fs::canonical(argv[4]);
    const std::wstring wide_hash = argv[5];
    std::string hash;
    for (auto c : wide_hash) {
      require((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'), "SHA256 must be lowercase hexadecimal");
      hash.push_back(static_cast<char>(c));
    }
    tap_lock = CreateFileW(tap.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(tap_lock != INVALID_HANDLE_VALUE && hash.size() == 64 && native_mat_guard::sha256(tap) == hash, "Lock reviewed output tap and verify SHA256");
    if (!managed) {
      output.open(directory / "native-output.mocap", std::ios::binary);
      require(output.good(), "Open capture output");
    }
    LARGE_INTEGER frequency {};
    require(QueryPerformanceFrequency(&frequency), "QPC frequency");
    qpc_hz = frequency.QuadPart;
    if (managed) shared.max_cycle_age_ticks = qpc_hz * Shared::max_native_age_ms / 1000;
    const native_output::FileHeader header { native_output::magic, native_output::version, 1, static_cast<uint32_t>(pid64), creation, static_cast<uint64_t>(frequency.QuadPart) };
    if (!managed) output.write(reinterpret_cast<const char *>(&header), sizeof(header));
    const auto base = LoadReviewed(engine, static_cast<DWORD>(pid64), tap, hash, true);
    map = Map(sizeof(native_output::Ring));
    ring = static_cast<native_output::Ring *>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*ring)));
    require(ring, "Map native output ring");
    memset(ring, 0, sizeof(*ring));
    ring->magic = native_output::magic;
    ring->version = native_output::version;
    ring->qpc_frequency = frequency.QuadPart;
    if (bound) wmemcpy(ring->endpoint, endpoint.c_str(), endpoint.size() + 1);
    printf("CAPTURE_ENDPOINT bound=%u id=%ls\n", unsigned(bound), endpoint.c_str());
    InterlockedExchange64(&ring->heartbeat, GetTickCount64());
    remote_map = DuplicateTo(engine, map);
    remote_owner = DuplicateTo(engine, GetCurrentProcess(), SYNCHRONIZE);
    native_output::StartArgs args { remote_map, remote_owner, static_cast<uint32_t>(seconds) };
    arguments = RemoteCopy(engine, &args, sizeof(args));
    thread = RemoteThread(engine, Export(tap, base, "StartNativeOutput"), arguments);
    handed = true;
    if (live) {
      mat_capture::Descriptor descriptor;
      descriptor.kind = mat_capture::native_object_source;
      descriptor.publisher_pid = GetCurrentProcessId();
      descriptor.publisher_created = native_mat_guard::created(GetCurrentProcess());
      descriptor.engine_pid = static_cast<DWORD>(pid64);
      descriptor.engine_created = creation;
      descriptor.epoch = (Qpc() & INT64_MAX) | 1;
      source_epoch = descriptor.epoch;
      descriptor.qpc_hz = frequency.QuadPart;
      server = std::thread([&, pipe, descriptor, endpoint] {
        try {
          platf::mmcss_audio_t delivery_priority;
          prioritize_delivery("pipe", delivery_priority);
          if (managed) {
            sonic_live::BedRoute route(endpoint);
            Serve(shared, pipe, descriptor, [&](bool opening) {
              if (opening) route.start();
              else if (!route.restore()) {
                route_clean.store(false);
                Broken(shared, "MAT route restoration failed; manual correction required");
              }
            });
          }
          else Serve(shared, pipe, descriptor);
        }
        catch (const std::exception &e) { Broken(shared, e.what()); }
      });
    }
    platf::mmcss_audio_t delivery_priority(live);
    if (live) prioritize_delivery("reader", delivery_priority);
    const auto began = GetTickCount64();
    uint64_t stopped_at = 0;
    bool ready = false;
    for (;;) {
      InterlockedExchange64(&ring->heartbeat, GetTickCount64());
      if (!ready && ring->status == 1) {
        ready = true;
        puts("CAPTURE_READY source=Windows-Dolby-Process copiedOnly=1 endpointChange=0");
        if (keepalive) keepalive->start();
      }
      require(ready || GetTickCount64() - began < 10000, "Output hook startup timeout");
      auto r = native_output::read64(ring->read), w = native_output::read64(ring->write);
      require(w >= r && w - r <= native_output::slot_count, "Output ring bounds");
      ring_peak = (std::max)(ring_peak, uint64_t(w - r));
      while (r < w) {
        const auto &slot = ring->slots[r % native_output::slot_count];
        require(slot.record.sequence == written && slot.record.produced <= native_output::max_bytes &&
                (!bound || (slot.record.endpoint_slot >= 1 && slot.record.endpoint_slot <= native_output::endpoint_count)), "Output record bounds/sequence/endpoint");
        const auto now_qpc = Qpc();
        const auto age_us = microseconds(now_qpc > slot.record.qpc ? now_qpc - slot.record.qpc : 0);
        if (last_callback_qpc && slot.record.qpc >= last_callback_qpc) {
          const auto gap_us = microseconds(slot.record.qpc - last_callback_qpc);
          callback_gap_max_us = (std::max)(callback_gap_max_us, gap_us);
          callback_gap_over_20ms += gap_us >= 20000;
        }
        last_callback_qpc = slot.record.qpc;
        ring_age_max_us = (std::max)(ring_age_max_us, age_us);
        ring_age_over_60ms += age_us >= 60000;
        ++timing_samples;
        if (!managed) {
          output.write(reinterpret_cast<const char *>(&slot.record), sizeof(slot.record));
          output.write(reinterpret_cast<const char *>(slot.bytes), slot.record.produced);
          require(output.good(), "Write raw native output");
        }
        if (live) {
          auto cycle = relay.accept(slot.record, { slot.bytes, slot.record.produced });
          if (cycle) relay.publish(shared, *cycle);
        }
        ++written;
        total_bytes += slot.record.produced;
        InterlockedExchange64(&ring->read, ++r);
      }
      if (control.replacing(stop, owner))
        throw sonic_live::MatSourceInterrupted("Managed native MAT source changed");
      if (interrupted || (seconds && GetTickCount64() - began > (seconds + 10) * 1000) ||
          (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)) {
        if (!stopped_at) stopped_at = GetTickCount64();
        InterlockedExchange(&ring->stop, 1);
      }
      if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) {
        if (native_output::read64(ring->read) != native_output::read64(ring->write)) continue;
        clean = ready && ring->status == 2 && !ring->error && original_entry(engine);
        break;
      }
      require((!seconds || GetTickCount64() - began < (seconds + 15) * 1000) && (!stopped_at || GetTickCount64() - stopped_at < 10000),
        "Output helper stop timeout; live arguments retained until it exits");
      require(WaitForSingleObject(engine, 0) == WAIT_TIMEOUT, "Pinned engine exited");
      if (live) {
        std::lock_guard lock(shared.mutex);
        require(!shared.failed, shared.error.c_str());
        if (shared.ready && GetTickCount64() - shared.last_cycle >= 2000)
          throw sonic_live::MatSourceInterrupted("Native endpoint output stalled");
        require(shared.ready || GetTickCount64() - began < 5000, "Selected endpoint is not producing native Dolby MAT; check its Windows spatial format");
      }
      require(!ring->dropped && !ring->invalid && !ring->failed, "Native output callbacks were lost or invalid; no drop/replay");
      if (keepalive && !stopped_at) keepalive->pump();
      report_delivery();
      Sleep(1);
    }
    if (!managed) {
      output.close();
      require(!output.fail(), "Flush raw native output");
    }
    require(clean && written && total_bytes && !ring->dropped && !ring->invalid && !ring->failed, "No complete loss-free output sample; inspect summary");
    result = 0;
  }
  catch (const sonic_live::BedDeviceInvalidated &e) {
    restart = managed;
    printf("NATIVE_ENDPOINT_INVALIDATED %s\n", e.what());
    Broken(shared, e.what());
  }
  catch (const sonic_live::MatSourceInterrupted &e) {
    restart = managed;
    printf("NATIVE_SOURCE_INTERRUPTED %s\n", e.what());
    Broken(shared, e.what());
  }
  catch (const std::exception &e) {
    printf("FAILED %s win32=%lu\n", e.what(), GetLastError());
    Broken(shared, e.what());
  }
  report_delivery(true);
  if (ring) InterlockedExchange(&ring->stop, 1);
  const bool done = !thread || WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0;
  if (thread && done) clean = ring->status == 2 && !ring->error && original_entry(engine);
  if (!done || (thread && !clean)) result = 1;
  if (server.joinable()) {
    {
      std::lock_guard lock(shared.mutex);
      shared.ended = result == 0;
      if (result != 0) { shared.failed = true; shared.error = "Native output capture ended with an error"; }
      shared.changed.notify_all();
    }
    server.join();
    printf("NATIVE_RELAY cycles=%llu omittedPrefix=%llu partialSuffix=%llu acknowledgedLeases=%llu reencoded=0\n",
      shared.cycles, relay.skipped_bytes(), relay.partial_bytes(), shared.acknowledged_leases);
  }
  keepalive.reset();
  if (output.is_open()) output.close();
  if (ring && !directory.empty()) {
    const auto dropped = native_output::read64(ring->dropped), invalid = native_output::read64(ring->invalid), failed = native_output::read64(ring->failed);
    std::string endpoint_text;
    if (bound) for (auto c : std::wstring_view(ring->endpoint)) endpoint_text += static_cast<char>(c);
    std::ofstream summary(directory / "capture-summary.json");
    summary << "{\n  \"source\": \"Windows-Dolby-Process\",\n  \"captureSha256\": \"" << native_mat_guard::sha256(directory / "native-output.mocap")
            << "\",\n  \"records\": " << written << ",\n  \"bytes\": " << total_bytes
            << ",\n  \"dropped\": " << dropped << ",\n  \"invalid\": " << invalid << ",\n  \"failedCalls\": " << failed
            << ",\n  \"ignoredCalls\": " << native_output::read64(ring->ignored) << ",\n  \"hookError\": " << ring->error
            << ",\n  \"endpointBound\": " << (bound ? "true" : "false")
            << ",\n  \"endpoint\": \"" << endpoint_text << "\""
            << ",\n  \"restored\": " << (clean ? "true" : "false") << ",\n  \"complete\": " << (result == 0 ? "true" : "false")
            << ",\n  \"atmosRecognition\": \"unverified\"\n}\n";
    summary.close();
    if (!summary.good()) result = 1;
    printf("STOP records=%llu bytes=%llu dropped=%lld invalid=%lld failedCalls=%lld restored=%u complete=%u\n", written, total_bytes, dropped, invalid, failed, clean, result == 0);
  }
  if (!handed && engine) { CloseRemote(engine, remote_map); CloseRemote(engine, remote_owner); }
  if (done && arguments) VirtualFreeEx(engine, arguments, 0, MEM_RELEASE);
  if (ring) UnmapViewOfFile(ring);
  if (map) CloseHandle(map);
  if (thread) CloseHandle(thread);
  if (engine) CloseHandle(engine);
  if (stop) CloseHandle(stop);
  if (owner) CloseHandle(owner);
  if (tap_lock != INVALID_HANDLE_VALUE) CloseHandle(tap_lock);
  control.clean = control.clean && done && (!handed || clean);
  if (!route_clean.load()) control.route_failed();
  const auto exit_code = sonic_live::mat_exit_code(restart, handed, done, clean, route_clean.load(), result);
  if (managed) printf("NATIVE_ENDPOINT_STOP records=%llu bytes=%llu helperStarted=%u restored=%u routeRestored=%u result=%d restart=%u\n", written, total_bytes, unsigned(handed), clean, unsigned(route_clean.load()), result, unsigned(exit_code == sonic_live::mat_retry_exit));
  return exit_code;
}

#ifdef MAT_OUTPUT_STANDALONE
int wmain(int argc, wchar_t **argv) { return RunNativeOutput(argc, argv); }
#endif
