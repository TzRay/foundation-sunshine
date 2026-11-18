#include "bed_route.h"
#include "managed_capture.h"
#include "bed_source.h"
#include "capture_options.h"
#include "components.h"
#include "native_access.h"

int
RunBedCapture(int argc, wchar_t **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  SetConsoleCtrlHandler(Ctrl, TRUE);
  HANDLE engine = nullptr, stop = nullptr, owner = nullptr, codec_lock = INVALID_HANDLE_VALUE;
  std::thread controller;
  Shared shared;
  bool restart_source = false, worker_stopped = true;
  sonic_live::ManagedCapture control;
  struct Close {
    HANDLE &engine, &stop, &owner, &codec;
    ~Close() {
      if (stop) CloseHandle(stop);
      if (owner) CloseHandle(owner);
      if (codec != INVALID_HANDLE_VALUE) CloseHandle(codec);
      if (engine) CloseHandle(engine);
    }
  } close { engine, stop, owner, codec_lock };
  try {
    if (argc == 5 && std::wstring_view(argv[1]) == L"--preflight") {
      const auto pid = std::stoull(argv[2]), created = std::stoull(argv[3]);
      require(pid && pid <= MAXDWORD && created, "Explicit bed engine identity");
      engine = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
      require(engine && native_mat_guard::created(engine) == created && native_mat_guard::system_engine(engine), "Read-only bed engine identity");
      if (!sonic_live::report_components(mat_compat::audio_engine(), sonic_live::find_dolby_access().component)) return 3;
      sonic_live::BedSource source(argv[4], false);
      puts("PREFLIGHT bed endpoint/format/components passed; attach=0 hooks=0 capture=0");
      return 0;
    }
    fs::path log;
    if (argc >= 3 && std::wstring_view(argv[argc - 2]) == L"--log") {
      log = argv[argc - 1];
      argc -= 2;
    }
    redirect_native_log(log);
    if (argc >= 4 && std::wstring_view(argv[argc - 3]) == L"--owner") {
      const auto pid = std::stoull(argv[argc - 2]), created = std::stoull(argv[argc - 1]);
      require(pid && pid <= MAXDWORD && created, "Manager owner identity");
      owner = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD(pid));
      require(owner && native_mat_guard::created(owner) == created && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT, "Manager owner unavailable");
      argc -= 3;
    }
    require((argc == 10 || argc == 11) && (std::wstring_view(argv[1]) == L"--attach" || std::wstring_view(argv[1]) == L"--reattach"),
      "--source bed --attach/--reattach PID CREATED CODEC SHA ENDPOINT PIPE STOP (--continuous | --max-seconds 1..60) [--log NEW_FILE]");
    const auto limit = sonic_live::capture_limit(argc - 9, argv[9], argc == 11 ? argv[10] : nullptr);
    const auto pid = std::stoull(argv[2]), created = std::stoull(argv[3]);
    require(pid && pid <= MAXDWORD && created, "Explicit bed engine identity");
    const auto dolby = sonic_live::find_dolby_access();
    if (!sonic_live::report_components(mat_compat::audio_engine(), dolby.component)) return 3;
    enable_native_access();
    engine = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                           PROCESS_CREATE_THREAD | PROCESS_DUP_HANDLE | SYNCHRONIZE,
      FALSE, DWORD(pid));
    require(engine && native_mat_guard::created(engine) == created && native_mat_guard::system_engine(engine), "Pinned bed engine identity/hash");
    const auto codec = fs::canonical(argv[4]);
    const std::wstring hash_text = argv[5], endpoint = argv[6], pipe = argv[7], event = argv[8];
    std::string hash;
    for (auto digit : hash_text) hash.push_back(static_cast<char>(digit));
    codec_lock = CreateFileW(codec.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(codec_lock != INVALID_HANDLE_VALUE && hash.size() == 64 && native_mat_guard::sha256(codec) == hash, "Reviewed bed worker file/hash");
    require(!endpoint.empty() && pipe.starts_with(L"\\\\.\\pipe\\SunshineMatLive-") &&
              pipe.find_first_of(L"\r\n") == pipe.npos && event.starts_with(L"Local\\SunshineMatLiveStop-"),
      "Bed endpoint/pipe/stop arguments");
    if (owner) control.open(event, log, created);
    auto acl = PipeAcl();
    SECURITY_ATTRIBUTES attributes { sizeof(attributes), acl, FALSE };
    stop = CreateEventW(&attributes, TRUE, FALSE, event.c_str());
    const auto event_error = GetLastError();
    LocalFree(acl);
    require(stop && event_error != ERROR_ALREADY_EXISTS, "Fresh bed stop event");
    const auto loaded = LoadReviewed(engine, DWORD(pid), codec, hash, std::wstring_view(argv[1]) == L"--reattach");
    sonic_live::BedRoute route(endpoint);
    const auto routed_endpoint = route.endpoint();
    const auto epoch = (Qpc() & INT64_MAX) | 1;
    controller = std::thread([&, codec, loaded, routed_endpoint, epoch, limit] {
      try {
        auto source = std::make_unique<sonic_live::BedSource>(routed_endpoint);
        const auto captured_endpoint = source->endpoint();
        const auto source_mask = source->mask();
        sonic_live::BedInput bed(source_mask, sonic_live::BedSubmission::full);
        NativeSession native(engine, codec, loaded, dolby.directory, 0xbed0001, epoch, shared, "loopback-bed", &worker_stopped);
        const auto began = GetTickCount64();
        uint64_t blocks = 0, nonzero = 0;
        uint64_t prior_packets = 0, prior_silent = 0, prior_clipped = 0, reopens = 0, dropped_frames = 0;
        uint64_t peakAgeMs = 0;
        sonic_live::BedReopenAlignment alignment;
        sonic_live::BedBlock block;
        const auto active = [&] { return !interrupted && WaitForSingleObject(stop, 0) == WAIT_TIMEOUT && !control.replacing(stop, owner) && (!limit || GetTickCount64() - began < limit * 1000); };
        while (active()) {
          native.heartbeat();
          try {
            if (!source->next(block)) continue;
          }
          catch (const sonic_live::BedDeviceInvalidated &) {
            const auto reopen_started = GetTickCount64();
            prior_packets += source->packets();
            prior_silent += source->silent_frames();
            prior_clipped += source->clipped();
            dropped_frames += source->buffered_frames();
            source.reset();
            while (active()) {
              native.heartbeat();
              try {
                source = std::make_unique<sonic_live::BedSource>(captured_endpoint);
                break;
              }
              catch (const sonic_live::BedDeviceInvalidated &e) {
                if (GetTickCount64() - reopen_started >= 1000) {
                  Broken(shared, e.what());
                  throw;
                }
                Sleep(20);
              }
            }
            if (!source) break;
            require(source->mask() == source_mask, "BED_REOPEN_FORMAT_CHANGED");
            ++reopens;
            alignment.reopened();
            printf("BED_SOURCE_REOPEN count=%llu gapMs=%llu droppedFrames=%llu sameEncoder=1 samePipe=1 silenceInserted=0\n",
              reopens, GetTickCount64() - reopen_started, dropped_frames);
            continue;
          }
          catch (const std::exception &e) {
            // Preserve the capture fault before NativeSession's cancellation
            // tears down the ring and reports its secondary cancelled status.
            Broken(shared, e.what());
            throw;
          }
          if (!alignment.accept(block.firstQpc, block.lastQpc)) {
            dropped_frames += 480;
            continue;
          }
          const auto ageMs = uint64_t(std::max<int64_t>(0, receiver::qpc() - block.firstQpc)) * 1000 / receiver::hz();
          peakAgeMs = std::max(peakAgeMs, ageMs);
          for (auto sample : block.pcm)
            if (sample != 0) ++nonzero;
          auto packet = bed.data(block.pcm, block.firstQpc, block.lastQpc);
          require(native.send(*packet), "BED_WORKER_REFUSED");
          ++blocks;
          if (blocks % 6000 == 0) {
            printf("BED_PROGRESS blocks=%llu sourceFrames=%llu elapsedMs=%llu captureAgeMs=%llu peakCaptureAgeMs=%llu nonzeroSamples=%llu clipped=%llu\n",
              blocks, blocks * 480, GetTickCount64() - began, ageMs, peakAgeMs, nonzero, prior_clipped + source->clipped());
            peakAgeMs = 0;
          }
        }
        if (source) source->stop();
        if (control.replacing(stop, owner)) {
          Broken(shared, "Managed MAT bed source changed");
          throw sonic_live::BedSourceRestart("Managed MAT bed source changed");
        }
        auto end = bed.finish(receiver::qpc());
        require(native.send(*end), "BED_EOF_REFUSED");
        native.finish();
        require(native.finished_stats().sourceRounds == blocks, "BED_SOURCE_ROUND_MISMATCH");
        printf("BED_STOP verified=1 blocks=%llu sourceFrames=%llu packets=%llu silentFrames=%llu nonzeroSamples=%llu clipped=%llu hooksInstalled=0\n",
          blocks, blocks * 480, prior_packets + (source ? source->packets() : 0), prior_silent + (source ? source->silent_frames() : 0), nonzero,
          prior_clipped + (source ? source->clipped() : 0));
      }
      catch (const sonic_live::BedSourceRestart &e) {
        restart_source = worker_stopped;
        printf("BED_SOURCE_RESTART reason=%s workerStopped=%u requiresReconnect=1 silenceInserted=0\n", e.what(), unsigned(worker_stopped));
        Broken(shared, e.what());
      }
      catch (const std::exception &e) {
        printf("BED_FAILED %s\n", e.what());
        Broken(shared, e.what());
      }
    });
    mat_capture::Descriptor descriptor;
    descriptor.kind = mat_capture::native_object_source;
    descriptor.publisher_pid = GetCurrentProcessId();
    descriptor.publisher_created = native_mat_guard::created(GetCurrentProcess());
    descriptor.engine_pid = DWORD(pid);
    descriptor.engine_created = created;
    descriptor.epoch = epoch;
    descriptor.qpc_hz = receiver::hz();
    puts("ATTACHED source=bed-pcm-to-native-MAT fileInput=0 hooksInstalled=0");
    Serve(shared, pipe, descriptor, [&](bool opening) {
      if (opening) route.start();
      else if (!route.restore()) {
        control.route_failed();
        Broken(shared, "MAT route restoration failed; manual correction required");
      }
    });
    SetEvent(stop);
    controller.join();
    control.clean = control.clean && worker_stopped;
    if (restart_source && control.clean) return 2;
    require(!shared.failed, shared.error.c_str());
    return 0;
  }
  catch (const std::exception &e) {
    printf("BED_FAILED %s win32=%lu\n", e.what(), GetLastError());
    if (stop) SetEvent(stop);
    if (controller.joinable()) controller.join();
    control.clean = control.clean && worker_stopped;
    return 1;
  }
}
