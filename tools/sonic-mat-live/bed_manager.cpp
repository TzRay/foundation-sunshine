// User-session supervisor. The scheduled task supplies elevation; capture
// remains a separate, hash-pinned process with its existing pipe contract.
#include "../../src/spatial_audio/mat_auto_source.h"
#include "bed_source.h"
#include "mat_endpoint.h"
#include "mat_retry.h"
#include "managed_capture.h"
#include "mat_manager_policy.h"
#include "../../src/spatial_audio/mat_manager_control.h"
#include "native_access.h"
#include <comdef.h>
#include <fstream>
#include <functiondiscoverykeys_devpkey.h>
#include <shlobj.h>
#include <sstream>
#include <taskschd.h>

namespace {
  template <class T>
  using Ptr = Microsoft::WRL::ComPtr<T>;
  constexpr auto managerStop = L"Local\\SunshineMatLiveManagerStop";
  constexpr auto taskName = spatial_audio::mat_manager_task;
  void
  check(HRESULT hr, const char *what) {
    if (FAILED(hr)) {
      printf("MANAGER_HRESULT operation=%s value=0x%08x\n", what, unsigned(hr));
      throw std::runtime_error(what);
    }
  }
  std::wstring
  wide(const std::string &s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    require(n > 0, "Invalid UTF-8 setting");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), n);
    return out;
  }
  std::string
  utf8(const std::wstring &s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
  }
  std::string
  quote(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
      if (c == '"' || c == '\\') out += '\\';
      if (c >= 32) out += char(c);
    }
    return out + '"';
  }
  std::wstring
  argument(const std::wstring &s) {
    // These are file paths, endpoint IDs and names; none may contain a quote.
    require(s.find_first_of(L"\"\r\n") == s.npos && !s.ends_with(L"\\"), "Manager argument");
    return L"\"" + s + L"\"";
  }
  fs::path
  folder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    check(SHGetKnownFolderPath(id, 0, nullptr, &value), "Known folder");
    fs::path out(value);
    CoTaskMemFree(value);
    return out;
  }
  std::string
  read(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Manager file unavailable");
    return { std::istreambuf_iterator<char>(input), {} };
  }
  std::map<std::string, std::string>
  settings(const fs::path &path) {
    std::map<std::string, std::string> out;
    std::istringstream input(read(path));
    std::string line;
    while (std::getline(input, line)) {
      const auto end = line.find('#');
      line = line.substr(0, end);
      const auto equals = line.find('=');
      if (equals == line.npos) continue;
      auto trim = [](std::string s) {
        const auto first = s.find_first_not_of(" \r\n\t");
        return first == s.npos ? std::string {} : s.substr(first, s.find_last_not_of(" \r\n\t") - first + 1);
      };
      out[trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
    }
    return out;
  }
  bool
  enabled(const std::string &s) { return s == "enabled" || s == "true" || s == "1"; }
  struct Engine {
    DWORD pid = 0;
    uint64_t created = 0;
  };
  Engine
  engine() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    require(snapshot != INVALID_HANDLE_VALUE, "Manager process snapshot");
    PROCESSENTRY32W entry { sizeof(entry) };
    Engine result;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
      if (_wcsicmp(entry.szExeFile, L"audiodg.exe")) continue;
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
      if (process) {
        if (native_mat_guard::system_engine(process)) result = { entry.th32ProcessID, native_mat_guard::created(process) };
        CloseHandle(process);
      }
      if (result.pid) break;
    }
    CloseHandle(snapshot);
    return result;
  }
  Engine
  host_identity(const fs::path &host) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    require(snapshot != INVALID_HANDLE_VALUE, "Manager host snapshot");
    PROCESSENTRY32W entry { sizeof(entry) };
    Engine found;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
      if (_wcsicmp(entry.szExeFile, L"Sunshine.exe")) continue;
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
      if (process) {
        const auto image = native_mat_guard::image(process);
        if (_wcsicmp(image.c_str(), host.c_str()) == 0)
          found = { entry.th32ProcessID, native_mat_guard::created(process) };
        CloseHandle(process);
      }
      if (found.pid && found.created) break;
    }
    CloseHandle(snapshot);
    return found;
  }
  std::wstring
  endpoint(const std::wstring &current) {
    Ptr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "Manager endpoints");
    Ptr<IMMDevice> defaultDevice;
    std::wstring defaultId;
    LPWSTR defaultValue = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &defaultDevice)) &&
        SUCCEEDED(defaultDevice->GetId(&defaultValue))) {
      defaultId = defaultValue;
      CoTaskMemFree(defaultValue);
    }
    Ptr<IMMDeviceCollection> devices;
    check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices), "Active outputs");
    UINT count = 0;
    check(devices->GetCount(&count), "Output count");
    std::vector<std::wstring> candidates;
    std::vector<std::wstring> configuredCandidates;
    for (UINT n = 0; n < count; ++n) {
      Ptr<IMMDevice> device;
      if (FAILED(devices->Item(n, &device))) continue;
      const bool configured = sonic_live::configured_mat(device.Get());
      if (!configured && !sonic_live::supports_mat(device.Get())) continue;
      LPWSTR id = nullptr;
      if (SUCCEEDED(device->GetId(&id))) {
        candidates.emplace_back(id);
        if (configured) configuredCandidates.emplace_back(id);
        CoTaskMemFree(id);
      }
    }
    // Drivers may expose the active Windows Dolby carrier as MAT/MLP while
    // rejecting an application-side MAT 2.x exclusive format query.
    const auto chosen = sonic_live::choose_mat_endpoint(
      configuredCandidates.empty() ? std::move(candidates) : std::move(configuredCandidates), current, defaultId);
    require(!chosen.empty(), "No active MAT output found. Connect a compatible HDMI/DP device and enable Dolby Atmos for home theater.");
    Ptr<IMMDevice> selected;
    check(enumerator->GetDevice(chosen.c_str(), &selected), "Selected MAT output");
    LPWSTR id = nullptr;
    check(selected->GetId(&id), "Selected output ID");
    std::wstring out(id);
    CoTaskMemFree(id);
    // Include the mix format in the desired identity so a format change causes
    // an orderly new epoch, just like a device or audiodg replacement.
    Ptr<IAudioClient> client;
    check(selected->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "Output format client");
    WAVEFORMATEX *format = nullptr;
    check(client->GetMixFormat(&format), "Output mix format");
    std::string bytes(reinterpret_cast<char *>(format), sizeof(WAVEFORMATEX) + format->cbSize);
    CoTaskMemFree(format);
    return out + L"|" + std::to_wstring(receiver::fnv(2166136261u, bytes.data(), bytes.size()));
  }
  uint64_t
  now_ms() {
    FILETIME value;
    GetSystemTimeAsFileTime(&value);
    return ((uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime) / 10000 - 11644473600000ull;
  }
  void
  state(const fs::path &directory, const char *status, const std::string &error, const std::wstring &id, const Engine &audio, HANDLE child, const fs::path &log, const std::string &source) {
    const auto temporary = directory / L"manager.tmp";
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      require(bool(output), "Manager status write");
      output << "{\"state\":" << quote(status) << ",\"last_error\":" << quote(error)
             << ",\"source\":" << quote(source) << ",\"endpoint\":" << quote(utf8(id))
             << ",\"updated_ms\":" << now_ms() << ",\"manager_pid\":" << GetCurrentProcessId()
             << ",\"manager_created\":" << native_mat_guard::created(GetCurrentProcess())
             << ",\"publisher_pid\":" << (child ? GetProcessId(child) : 0)
             << ",\"publisher_created\":" << (child ? native_mat_guard::created(child) : 0)
             << ",\"engine_pid\":" << audio.pid << ",\"engine_created\":" << audio.created
             << ",\"log\":" << quote(utf8(log.wstring())) << "}";
      output.flush();
      require(bool(output), "Manager status flush");
    }
    bool replaced = false;
    for (int n = 0; n < 10; ++n) {
      if (MoveFileExW(temporary.c_str(), (directory / L"manager.json").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        replaced = true;
        break;
      }
      if (GetLastError() != ERROR_SHARING_VIOLATION && GetLastError() != ERROR_ACCESS_DENIED) break;
      Sleep(10);
    }
    require(replaced, "Manager status replace");
  }
  void
  protect(const fs::path &directory) {
    fs::create_directories(directory);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;AU)", SDDL_REVISION_1, &descriptor, nullptr), "Manager directory ACL");
    const auto ok = SetFileSecurityW(directory.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    require(ok, "Protect manager directory");
  }
  void
  register_task(const fs::path &image, const fs::path &config, const fs::path &directory, const fs::path &worker) {
    Ptr<ITaskService> service;
    check(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)), "Task Scheduler");
    check(service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()), "Task Scheduler connect");
    Ptr<ITaskFolder> root;
    check(service->GetFolder(_bstr_t(L"\\"), &root), "Task root");
    Ptr<ITaskDefinition> task;
    check(service->NewTask(0, &task), "New MAT task");
    Ptr<IPrincipal> principal;
    check(task->get_Principal(&principal), "Task principal");
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), "Task user token");
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> user(bytes);
    const auto got = GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes);
    CloseHandle(token);
    require(got, "Task user identity");
    LPWSTR sid = nullptr;
    require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &sid), "Task user SID");
    const std::wstring userSid(sid);
    LocalFree(sid);
    check(principal->put_UserId(_bstr_t(userSid.c_str())), "Task user");
    check(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN), "Interactive task");
    check(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST), "Elevated task");
    Ptr<ITaskSettings> options;
    check(task->get_Settings(&options), "Task settings");
    check(options->put_DisallowStartIfOnBatteries(VARIANT_FALSE), "Task battery start");
    check(options->put_StopIfGoingOnBatteries(VARIANT_FALSE), "Task battery stop");
    check(options->put_ExecutionTimeLimit(_bstr_t(L"PT0S")), "Task duration");
    check(options->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW), "Task singleton");
    check(options->put_StartWhenAvailable(VARIANT_TRUE), "Task availability");
    check(options->put_RestartInterval(_bstr_t(L"PT1M")), "Task restart interval");
    check(options->put_RestartCount(3), "Task restart count");
    Ptr<ITriggerCollection> triggers;
    Ptr<ITrigger> trigger;
    Ptr<ILogonTrigger> logon;
    check(task->get_Triggers(&triggers), "Task triggers");
    check(triggers->Create(TASK_TRIGGER_LOGON, &trigger), "Task logon trigger");
    check(trigger.As(&logon), "Logon trigger interface");
    check(logon->put_UserId(_bstr_t(userSid.c_str())), "Logon user");
    Ptr<IActionCollection> actions;
    Ptr<IAction> action;
    Ptr<IExecAction> exec;
    check(task->get_Actions(&actions), "Task actions");
    check(actions->Create(TASK_ACTION_EXEC, &action), "Task action");
    check(action.As(&exec), "Executable action");
    check(exec->put_Path(_bstr_t(image.c_str())), "Task executable");
    const auto arguments = L"--supervise-bed " + argument(config.wstring()) + L" " + argument(directory.wstring()) + L" " + argument(worker.wstring());
    check(exec->put_Arguments(_bstr_t(arguments.c_str())), "Task arguments");
    Ptr<IRegisteredTask> registered;
    check(root->RegisterTaskDefinition(_bstr_t(taskName), task.Get(), TASK_CREATE_OR_UPDATE, _variant_t(userSid.c_str()), _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(), &registered), "Register MAT task");
    Ptr<IRunningTask> running;
    check(registered->Run(_variant_t(), &running), "Start MAT task");
  }
}  // namespace

int
RunBedManager(int argc, wchar_t **argv) {
  HANDLE stop = nullptr, child = nullptr, request = nullptr, ack = nullptr, blocked = nullptr;
  std::wstring childStop;
  fs::path directory, log;
  Engine audio;
  std::wstring endpointId;
  std::string sourceKind = "bed";
  bool childReady = false;
  auto stop_child = [&](bool replace = false) {
    if (!child) return;
    const auto signal = replace ? childStop + L"-replace" : childStop;
    for (int n = 0; n < 50 && WaitForSingleObject(child, 0) == WAIT_TIMEOUT; ++n) {
      HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, signal.c_str());
      if (event) {
        SetEvent(event);
        CloseHandle(event);
        break;
      }
      Sleep(100);
    }
    require(WaitForSingleObject(child, 15000) == WAIT_OBJECT_0, "Capture did not stop; left running, no forced termination");
    CloseHandle(child);
    child = nullptr;
    childReady = false;
  };
  try {
    if (argc == 5 && std::wstring_view(argv[1]) == L"--supervise-bed") {
      const auto logs = fs::canonical(argv[3]) / L"logs";
      fs::create_directories(logs);
      redirect_native_log(logs / (L"manager-" + std::to_wstring(now_ms()) + L".log"));
      // Detach only this supervisor; never hide a caller's shared terminal.
      // The capture child already starts with CREATE_NO_WINDOW.
      FreeConsole();
    }
    if (argc >= 4 && std::wstring_view(argv[argc - 2]) == L"--log") {
      redirect_native_log(argv[argc - 1]);
      argc -= 2;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--stop-manager") {
      stop = OpenEventW(EVENT_MODIFY_STATE, FALSE, managerStop);
      if (stop) {
        SetEvent(stop);
        CloseHandle(stop);
      }
      return 0;
    }
    enable_native_access();
    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Manager COM");
    struct Apartment {
      ~Apartment() { CoUninitialize(); }
    } apartment;
    if (argc == 3 && std::wstring_view(argv[1]) == L"--install-bed") {
      const auto config = fs::canonical(argv[2]);
      const auto host = fs::canonical(config.parent_path().parent_path() / L"Sunshine.exe");
      require(host.parent_path() == fs::canonical(folder(FOLDERID_ProgramFiles) / L"Sunshine"), "Installed Sunshine config required");
      HANDLE old = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, managerStop);
      if (old) {
        SetEvent(old);
        CloseHandle(old);
        // Its child drains normally. Observe event removal rather than killing
        // an old manager or unloading a DLL from audiodg.
        bool gone = false;
        for (int n = 0; n < 200; ++n) {
          Sleep(100);
          old = OpenEventW(SYNCHRONIZE, FALSE, managerStop);
          if (!old) {
            gone = true;
            break;
          }
          CloseHandle(old);
        }
        require(gone, "Old MAT manager has not stopped");
      }
      const auto image = fs::canonical(native_mat_guard::image(GetCurrentProcess()));
      const auto worker = fs::canonical(image.parent_path() / L"sunshine-sonic-mat-worker.dll");
      const auto imageHash = native_mat_guard::sha256(image), workerHash = native_mat_guard::sha256(worker);
      const auto installed = folder(FOLDERID_ProgramFiles) / L"SunshineSonicMatLive" / wide(imageHash);
      protect(installed);
      const auto target = installed / L"sunshine-plugin-mat-capture.exe";
      // audiodg retains loaded workers. A publisher update must not give a
      // different deployment path the same DLL basename as a pinned worker.
      const auto targetWorker = installed / (L"sunshine-sonic-mat-worker-" + wide(workerHash) + L"-" + wide(imageHash.substr(0, 16)) + L".dll");
      if (!fs::exists(target)) fs::copy_file(image, target);
      if (!fs::exists(targetWorker)) fs::copy_file(worker, targetWorker);
      require(native_mat_guard::sha256(target) == imageHash && native_mat_guard::sha256(targetWorker) == workerHash, "Installed capture hashes");
      directory = folder(FOLDERID_ProgramData) / L"Sunshine" / L"MAT";
      protect(directory);
      protect(directory / L"logs");
      register_task(target, config, directory, targetWorker);
      printf("INSTALLED source=bed task=Sunshine-MAT-PCM image=%ls worker=%ls\n", target.c_str(), targetWorker.c_str());
      return 0;
    }
    require(argc == 5 && std::wstring_view(argv[1]) == L"--supervise-bed", "--install-bed CONFIG | --supervise-bed CONFIG STATE_DIRECTORY WORKER | --stop-manager");
    const auto config = fs::canonical(argv[2]);
    const auto host = fs::canonical(config.parent_path().parent_path() / L"Sunshine.exe");
    directory = fs::canonical(argv[3]);
    const auto image = fs::canonical(native_mat_guard::image(GetCurrentProcess()));
    const auto worker = fs::canonical(argv[4]);
    require(spatial_audio::trusted_mat_auto_publisher(GetCurrentProcess()) && worker.parent_path() == image.parent_path(), "Protected installed MAT manager required");
    const auto workerHash = native_mat_guard::sha256(worker);
    // The elevated task accepts control from same-user, non-elevated Sunshine.
    auto acl = PipeAcl(true);
    SECURITY_ATTRIBUTES attributes { sizeof(attributes), acl, FALSE };
    stop = CreateEventW(&attributes, TRUE, FALSE, managerStop);
    const auto eventError = GetLastError();
    if (stop && eventError != ERROR_ALREADY_EXISTS) {
      request = CreateEventW(&attributes, FALSE, FALSE, spatial_audio::mat_manager_request);
      ack = CreateEventW(&attributes, TRUE, FALSE, spatial_audio::mat_manager_ack);
      blocked = CreateEventW(&attributes, TRUE, FALSE, (std::wstring(spatial_audio::mat_manager_ack) + L"-blocked").c_str());
    }
    LocalFree(acl);
    require(stop && eventError != ERROR_ALREADY_EXISTS, "MAT manager already running");
    require(request && ack && blocked, "MAT manager request events");
    std::wstring active, failedIdentity;
    std::string lastState, lastError, failedError;
    uint64_t retry_after = 0;
    uint64_t child_started = 0;
    sonic_live::MatRetry retry;
    bool demand_pending = false;
    while (WaitForSingleObject(stop, 1000) == WAIT_TIMEOUT) {
      if (WaitForSingleObject(request, 0) == WAIT_OBJECT_0) demand_pending = true;
      const bool requested = demand_pending;
      std::string status = "starting", error;
      std::wstring desired;
      bool source_enabled = false, cleanup_blocked = false, cleanup_pending = false;
      std::unique_ptr<sonic_live::BedSource> bootstrap;
      try {
        auto values = settings(config);
        const auto hostGeneration = host_identity(host);
        sourceKind = values["mat_source"];
        if (!enabled(values["mat_live"]) || (sourceKind != "bed" && sourceKind != "native") ||
            (!values["stream_audio"].empty() && !enabled(values["stream_audio"]))) {
          status = "disabled";
          failedIdentity.clear();
          retry_after = 0;
          retry.reset();
        }
        else if (!hostGeneration.pid || !hostGeneration.created)
          status = "waiting_host";
        else {
          source_enabled = true;
          const auto selected = endpoint(endpointId);
          endpointId = selected.substr(0, selected.find(L'|'));
          audio = engine();
          if (!audio.pid) {
            // Windows retires audiodg when every render client is idle. Open
            // this endpoint briefly so cold boot does not wait for game audio.
            bootstrap = std::make_unique<sonic_live::BedSource>(endpointId);
            for (int n = 0; n < 10 && !audio.pid; ++n) {
              if (WaitForSingleObject(stop, 100) != WAIT_TIMEOUT) break;
              audio = engine();
            }
          }
          require(audio.pid && audio.created, "Waiting for a compatible Windows audio engine");
          desired = wide(sourceKind) + L"|" + selected + L"|" + std::to_wstring(audio.pid) + L"|" + std::to_wstring(audio.created) +
                    L"|" + std::to_wstring(hostGeneration.pid) + L"|" + std::to_wstring(hostGeneration.created);
        }
        if (child && WaitForSingleObject(child, 0) == WAIT_OBJECT_0) {
          DWORD code = 0;
          GetExitCodeProcess(child, &code);
          CloseHandle(child);
          child = nullptr;
          childReady = false;
          if (code == 2 || code == sonic_live::mat_retry_exit) {
            failedIdentity.clear();
            const auto delay = retry.next_delay();
            retry_after = GetTickCount64() + delay;
            printf("MANAGER safely cleaned source interrupted; retry in %llu ms\n", delay);
          }
          else {
            failedIdentity = active;
            retry_after = 0;
            failedError = "Capture exited (" + std::to_string(code) + "); see capture log. Reconnect MAT or toggle MAT to retry.";
          }
        }
        const auto action = sonic_live::mat_manager_action(active, desired, child != nullptr, requested);
        if (action.stop_child) stop_child(source_enabled);
        if (action.reset_failure) {
          failedIdentity.clear();
          retry.reset();
          retry_after = 0;
        }
        if (!desired.empty() && !child) {
          const auto available = sonic_live::capture_availability(directory / L"capture.lease", audio.created);
          cleanup_pending = available == sonic_live::CaptureAvailability::cleaning;
          if (available == sonic_live::CaptureAvailability::blocked) {
            cleanup_blocked = true;
            status = "error";
            error = "Previous MAT capture cleanup incomplete";
          }
          else if (cleanup_pending || GetTickCount64() < retry_after) {
            status = "starting";
          }
          else if (failedIdentity == desired) {
            status = "error";
            error = failedError;
          }
          else {
            if (sourceKind == "bed") { sonic_live::BedSource validate(endpointId, false); }
            const auto id = std::to_wstring(now_ms());
            log = directory / L"logs" / (L"capture-" + id + L".log");
            childStop = L"Local\\SunshineMatLiveStop-managed-" + id;
            std::wstring command = argument(image.wstring()) + L" --source " + wide(sourceKind) +
                                   (sourceKind == "native" ? L" --serve-endpoint " : L" --reattach ") + std::to_wstring(audio.pid) + L" " + std::to_wstring(audio.created) +
                                   L" " + argument(worker.wstring()) + L" " + wide(workerHash) + L" " + argument(endpointId) + L" " + argument(wide(spatial_audio::mat_auto_pipe)) +
                                   L" " + argument(childStop) + (sourceKind == "bed" ? L" --continuous" : L"") + L" --owner " + std::to_wstring(GetCurrentProcessId()) + L" " +
                                   std::to_wstring(native_mat_guard::created(GetCurrentProcess())) + L" --log " + argument(log.wstring());
            STARTUPINFOW startup { sizeof(startup) };
            PROCESS_INFORMATION process {};
            require(CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, image.parent_path().c_str(), &startup, &process), "Start managed bed capture");
            child = process.hProcess;
            child_started = GetTickCount64();
            childReady = false;
            CloseHandle(process.hThread);
            active = desired;
            failedIdentity.clear();
            lastError.clear();
          }
        }
        if (child) {
          // Read the startup marker once per child, not the growing lifetime
          // log every second. Process exit is checked above on every poll.
          const auto marker = sourceKind == "native" ? "NATIVE_MAT_READY source=system-native-output" : "NATIVE_MAT_READY source=loopback-bed";
          if (!childReady && fs::exists(log) && read(log).find(marker) != std::string::npos) childReady = true;
          status = childReady ? "ready" : "starting";
          if (childReady && GetTickCount64() - child_started >= 30000) retry.reset();
        }
      }
      catch (const std::exception &e) {
        stop_child(source_enabled);
        status = "error";
        error = e.what();
      }
      if (status != lastState || error != lastError) {
        printf("MANAGER state=%s error=%s endpoint=%ls\n", status.c_str(), error.c_str(), endpointId.c_str());
        lastState = status;
        lastError = error;
      }
      state(directory, status.c_str(), error, endpointId, audio, child, log, sourceKind);
      // Acknowledge only after applying the current host/config generation.
      // Sunshine must not lease a surviving old pipe during a fast restart.
      if (cleanup_blocked) SetEvent(blocked); else ResetEvent(blocked);
      if (requested && !cleanup_pending) {
        SetEvent(ack);
        demand_pending = false;
      }
    }
    stop_child();
    state(directory, "stopped", {}, endpointId, audio, nullptr, log, sourceKind);
    CloseHandle(stop);
    CloseHandle(request);
    CloseHandle(ack);
    CloseHandle(blocked);
    return 0;
  }
  catch (const std::exception &e) {
    fprintf(stderr, "MANAGER_FAILED %s win32=%lu\n", e.what(), GetLastError());
    try {
      stop_child(true);
    }
    catch (const std::exception &stopError) {
      fprintf(stderr, "MANAGER_STOP_FAILED %s\n", stopError.what());
    }
    if (stop) CloseHandle(stop);
    if (request) CloseHandle(request);
    if (ack) CloseHandle(ack);
    if (blocked) CloseHandle(blocked);
    return 1;
  }
}
