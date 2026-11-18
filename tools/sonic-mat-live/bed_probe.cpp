// Bounded M0 diagnostic: synthetic PCM -> real, pinned native encoder.
// No object tap, endpoint changes, network publisher, or installed-host changes.
#include "bed_input.h"
#include "components.h"
#include "native_session.h"
#include <algorithm>
#include <io.h>
#include <vector>

namespace {
  struct Options {
    DWORD pid = 0;
    uint64_t created = 0;
    fs::path codec, log;
    std::string hash;
    uint32_t mask = 0x63f, seconds = 1, tone = sonic_live::bed_wave_mask;
    sonic_live::BedSubmission submission = sonic_live::BedSubmission::full;
    bool reattach = false, matrix = false;
  };
  uint64_t
  number(const wchar_t *text, uint64_t maximum) {
    require(text && *text, "Missing positive integer");
    uint64_t result = 0;
    for (auto p = text; *p; ++p) {
      require(*p >= L'0' && *p <= L'9', "Expected positive decimal integer");
      const auto digit = uint64_t(*p - L'0');
      require(digit <= maximum && result <= (maximum - digit) / 10, "Integer exceeds bound");
      result = result * 10 + digit;
    }
    require(result > 0, "Positive integer required");
    return result;
  }
  Options
  options(int argc, wchar_t **argv) {
    Options result;
    for (int n = 1; n < argc; ++n) {
      const std::wstring_view key = argv[n];
      if (key == L"--reattach") {
        result.reattach = true;
        continue;
      }
      if (key == L"--matrix") {
        result.matrix = true;
        continue;
      }
      require(n + 1 < argc, "Missing probe option value");
      const wchar_t *value = argv[++n];
      const std::wstring_view text = value;
      if (key == L"--engine-pid")
        result.pid = static_cast<DWORD>(number(value, MAXDWORD));
      else if (key == L"--engine-created")
        result.created = number(value, UINT64_MAX);
      else if (key == L"--codec")
        result.codec = value;
      else if (key == L"--log")
        result.log = value;
      else if (key == L"--codec-sha256") {
        require(text.size() == 64 && text.find_first_not_of(L"0123456789abcdef") == text.npos, "Lowercase SHA256 required");
        for (auto digit : text) result.hash.push_back(static_cast<char>(digit));
      }
      else if (key == L"--seconds")
        result.seconds = static_cast<uint32_t>(number(value, 10));
      else if (key == L"--mask") {
        if (text == L"2.0")
          result.mask = 0x3;
        else if (text == L"5.1")
          result.mask = 0x60f;
        else if (text == L"5.1-back")
          result.mask = 0x3f;
        else if (text == L"7.1")
          result.mask = 0x63f;
        else if (text == L"7.1.4")
          result.mask = sonic_live::bed_wave_mask;
        else
          throw std::runtime_error("Mask must be 2.0, 5.1, 5.1-back, 7.1, or diagnostic 7.1.4");
      }
      else if (key == L"--bed") {
        require(text == L"full" || text == L"subset", "Bed submission must be full or subset");
        result.submission = text == L"full" ? sonic_live::BedSubmission::full : sonic_live::BedSubmission::subset;
      }
      else if (key == L"--channel") {
        result.tone = 0;
        if (text == L"all") result.tone = sonic_live::bed_wave_mask;
        for (const auto &channel : sonic_live::bed_channels) {
          const std::string name = channel.name;
          if (text == std::wstring(name.begin(), name.end())) result.tone = channel.wave;
        }
        require(result.tone, "Unknown test-tone channel");
      }
      else
        throw std::runtime_error("Unknown bed probe option");
    }
    require(result.pid && result.created && !result.codec.empty() && result.hash.size() == 64,
      "Explicit engine PID, creation FILETIME, codec path, and SHA256 required");
    require(result.mask & result.tone, "Test-tone channel absent from selected input mask");
    return result;
  }
  void
  wait_until(int64_t target) {
    while (receiver::qpc() < target) {
      require(!interrupted, "Bed probe cancelled");
      Sleep(1);
    }
  }
  void
  enable_native_access() {
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token), "Open probe token");
    TOKEN_ELEVATION elevation {};
    DWORD bytes = 0;
    TOKEN_PRIVILEGES privilege {};
    privilege.PrivilegeCount = 1;
    privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool enabled = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) && elevation.TokenIsElevated &&
                   LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &privilege.Privileges[0].Luid);
    if (enabled) {
      SetLastError(ERROR_SUCCESS);
      enabled = AdjustTokenPrivileges(token, FALSE, &privilege, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    }
    const auto error = GetLastError();
    CloseHandle(token);
    printf("BED_PROBE_ACCESS elevated=%u debugPrivilege=%u\n", elevation.TokenIsElevated, unsigned(enabled));
    SetLastError(error);
    require(enabled, "Administrator token with SeDebugPrivilege required; no injection attempted");
  }
  int
  run(Options args) {
    HANDLE engine = nullptr, codec_lock = INVALID_HANDLE_VALUE;
    struct Close {
      HANDLE &engine, &codec;
      ~Close() {
        if (codec != INVALID_HANDLE_VALUE) CloseHandle(codec);
        if (engine) CloseHandle(engine);
      }
    } close { engine, codec_lock };
    try {
      const auto dolby = sonic_live::find_dolby_access();
      if (!sonic_live::report_components(mat_compat::audio_engine(), dolby.component)) return 3;
      args.codec = fs::canonical(args.codec);
      codec_lock = CreateFileW(args.codec.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
      require(codec_lock != INVALID_HANDLE_VALUE && native_mat_guard::sha256(args.codec) == args.hash, "Reviewed worker file/hash");
      enable_native_access();
      engine = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                             PROCESS_CREATE_THREAD | PROCESS_DUP_HANDLE | SYNCHRONIZE,
        FALSE, args.pid);
      require(engine && native_mat_guard::created(engine) == args.created && native_mat_guard::system_engine(engine),
        "Pinned audiodg identity/hash; native probe requires administrator access");
      const auto loaded = LoadReviewed(engine, args.pid, args.codec, args.hash, args.reattach);
      const uint64_t epoch = (receiver::qpc() & INT64_MAX) | 1;
      const uint64_t domain = 0xbed0001;
      Shared output;
      sonic_live::BedInput bed(args.mask, args.submission);
      const auto blocks = args.seconds * 100;
      const char *mode = args.submission == sonic_live::BedSubmission::full ? "full" : "subset";
      printf("BED_PROBE_START mask=0x%x channels=%u bed=%s submittedChannels=%u blocks=%u dynamicObjects=0 tone=0x%x hooks=0\n",
        args.mask, bed.channels(), mode, bed.submitted_channels(), blocks, args.tone);
      NativeSession session(engine, args.codec, loaded, dolby.directory, domain, epoch, output, "bed-probe");
      std::vector<float> pcm(size_t(bed.channels()) * receiver::frames);
      const auto origin = receiver::qpc(), frequency = receiver::hz();
      for (uint32_t block = 0; block < blocks; ++block) {
        const auto first = origin + int64_t(block) * frequency / 100;
        wait_until(first);
        std::fill(pcm.begin(), pcm.end(), 0.f);
        for (const auto &channel : sonic_live::bed_channels) {
          if (!(args.mask & args.tone & channel.wave)) continue;
          const auto lane = std::popcount(args.mask & (channel.wave - 1));
          const double tone = channel.wave == SPEAKER_LOW_FREQUENCY ? 60. : 330. + 55. * lane;
          for (uint32_t frame = 0; frame < receiver::frames; ++frame) {
            const auto sample = uint64_t(block) * receiver::frames + frame;
            pcm[size_t(frame) * bed.channels() + lane] = float(.15 * std::sin(6.283185307179586 * tone * double(sample) / 48000.));
          }
        }
        auto packet = bed.data(pcm, first, first + int64_t(receiver::frames - 1) * frequency / 48000);
        require(session.send(*packet), "Native bed input refused; inspect worker counters");
      }
      wait_until(origin + int64_t(blocks) * frequency / 100);
      auto end = bed.finish(receiver::qpc());
      require(session.send(*end), "Native bed end refused");
      session.finish();
      const auto &stats = session.finished_stats();
      require(stats.sourceRounds == blocks && stats.sourceSamples == uint64_t(blocks) * receiver::frames * bed.submitted_channels(),
        "Native bed source-round/sample count mismatch");
      const auto expected_cycles = (stats.warmRounds + stats.sourceRounds + stats.drainRounds) / 2;
      require(output.ready && output.ended && !output.failed && output.cycles == expected_cycles,
        "Native bed carrier count/end mismatch");
      printf("BED_PROBE_PASS mask=0x%x bed=%s sourceRounds=%llu sourceSamples=%llu cycles=%llu eof=1 "
             "nativeEncoderAccepted=1 carrierValidated=1 speakerMappingVerified=0 hardwarePlaybackVerified=0\n",
        args.mask, mode, stats.sourceRounds, stats.sourceSamples, output.cycles);
      return 0;
    }
    catch (const std::exception &e) {
      printf("BED_PROBE_FAILED %s win32=%lu\n", e.what(), GetLastError());
      return 1;
    }
  }
}  // namespace

int
wmain(int argc, wchar_t **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  SetConsoleCtrlHandler(Ctrl, TRUE);
  if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
    puts("bed-probe --engine-pid PID --engine-created FILETIME --codec DLL --codec-sha256 HASH "
         "[--mask 2.0|5.1|5.1-back|7.1|7.1.4] [--bed full|subset] [--seconds 1..10] "
         "[--channel all|FL|...] [--reattach] [--matrix] [--log NEW_FILE]");
    return 0;
  }
  Options args;
  try {
    args = options(argc, argv);
    if (!args.log.empty()) {
      require(!fs::exists(args.log), "Fresh probe log required");
      FILE *log = nullptr;
      require(_wfreopen_s(&log, args.log.c_str(), L"w", stdout) == 0 && log, "Open probe log");
      require(_dup2(_fileno(stdout), _fileno(stderr)) == 0, "Redirect probe diagnostics");
      setvbuf(stdout, nullptr, _IONBF, 0);
    }
  }
  catch (const std::exception &e) {
    printf("BED_PROBE_ARGUMENTS %s attach=0\n", e.what());
    return 2;
  }
  if (!args.matrix) return run(args);
  for (auto mode : { sonic_live::BedSubmission::full, sonic_live::BedSubmission::subset }) {
    for (uint32_t mask : { 0x3u, 0x60fu, 0x63fu }) {
      args.submission = mode;
      args.mask = mask;
      const auto code = run(args);
      if (code) return code;
      args.reattach = true;
    }
  }
  puts("BED_PROBE_MATRIX_PASS cases=6 speakerMappingVerified=0 hardwarePlaybackVerified=0");
  return 0;
}
