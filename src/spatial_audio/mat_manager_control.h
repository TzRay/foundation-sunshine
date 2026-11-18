#pragma once

namespace spatial_audio {
  inline constexpr wchar_t mat_manager_task[] = L"Sunshine MAT PCM";
  // Sunshine may run in a different Windows session from the interactive task.
  inline constexpr wchar_t mat_manager_request[] = L"Global\\SunshineMatLiveManagerRequest";
  inline constexpr wchar_t mat_manager_ack[] = L"Global\\SunshineMatLiveManagerAck";
}

#if defined(_WIN32)
  #include "encoded_audio_source.h"
  #include <windows.h>
  #include <taskschd.h>
  #include <wrl/client.h>
  #include <chrono>
  #include <functional>
  #include <future>
  #include <mutex>
  #include <thread>
  #include <system_error>
  #pragma comment(lib, "taskschd.lib")
  #pragma comment(lib, "ole32.lib")
  #pragma comment(lib, "oleaut32.lib")

namespace spatial_audio {
  inline void start_mat_manager_task() {
    const auto check = [](HRESULT hr, const char *operation) {
      if (FAILED(hr)) throw std::system_error(static_cast<int>(hr), std::system_category(), operation);
    };
    const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr != RPC_E_CHANGED_MODE) check(hr, "MAT Task Scheduler COM");
    struct Apartment {
      bool owned;
      ~Apartment() { if (owned) CoUninitialize(); }
    } apartment { SUCCEEDED(hr) };
    Microsoft::WRL::ComPtr<ITaskService> service;
    check(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)), "MAT Task Scheduler");
    VARIANT empty {};
    check(service->Connect(empty, empty, empty, empty), "MAT Task Scheduler connect");
    Microsoft::WRL::ComPtr<ITaskFolder> folder;
    BSTR root = SysAllocString(L"\\");
    const auto folder_hr = service->GetFolder(root, &folder);
    SysFreeString(root);
    check(folder_hr, "MAT task folder");
    Microsoft::WRL::ComPtr<IRegisteredTask> task;
    BSTR name = SysAllocString(mat_manager_task);
    const auto task_hr = folder->GetTask(name, &task);
    SysFreeString(name);
    check(task_hr, "MAT task not installed");
    Microsoft::WRL::ComPtr<IRunningTask> running;
    // The registered task owns elevation and the interactive user identity.
    // Its IGNORE_NEW policy prevents duplicate managers.
    check(task->Run(empty, &running), "Start MAT task");
  }

  class MatSourceBlocked: public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };

  // Task Scheduler RPC can outlive a DESCRIBE or a cancelled audio generation.
  // Share an outstanding launch while each caller retains its own wait budget.
  inline std::shared_future<void> start_mat_manager_async() {
    static std::mutex mutex;
    static std::shared_future<void> pending;
    std::lock_guard lock(mutex);
    if (!pending.valid() || pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      std::packaged_task<void()> task(start_mat_manager_task);
      pending = task.get_future().share();
      std::thread(std::move(task)).detach();
    }
    return pending;
  }

  inline void request_mat_manager(std::chrono::steady_clock::time_point deadline,
    const std::function<bool()> &cancelled = {}, const std::function<void()> &start = {},
    const wchar_t *request_name = mat_manager_request, const wchar_t *ack_name = mat_manager_ack) {
    struct Events {
      HANDLE request = nullptr, ack = nullptr;
      ~Events() { if (request) CloseHandle(request); if (ack) CloseHandle(ack); }
    } events;
    const auto open = [](DWORD access, const wchar_t *name) {
      const auto event = OpenEventW(access, FALSE, name);
      const auto error = GetLastError();
      if (!event && error != ERROR_FILE_NOT_FOUND)
        throw std::system_error(error, std::system_category(), "Open MAT manager control");
      return event;
    };
    const auto time_left = [&] {
      require(!cancelled || !cancelled(), "MAT manager request cancelled");
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      require(left > 0, "MAT manager request timed out");
      return static_cast<DWORD>((std::min)(left, int64_t(20)));
    };
    std::shared_future<void> launch;
    bool started = false;
    while (!events.request || !events.ack) {
      time_left();
      if (!events.request) events.request = open(EVENT_MODIFY_STATE, request_name);
      if (!events.ack) events.ack = open(EVENT_MODIFY_STATE | SYNCHRONIZE, ack_name);
      if (events.request && events.ack) break;
      if (!started) {
        if (start) start(); else launch = start_mat_manager_async();
        started = true;
      }
      if (launch.valid() && launch.wait_for(std::chrono::seconds(0)) == std::future_status::ready) launch.get();
      Sleep((std::min)(time_left(), DWORD(10)));
    }
    require(ResetEvent(events.ack) && SetEvent(events.request), "Request MAT capture");
    DWORD wait;
    do { wait = WaitForSingleObject(events.ack, time_left()); } while (wait == WAIT_TIMEOUT);
    require(wait == WAIT_OBJECT_0, "MAT manager acknowledgement failed");
    time_left();
    if (HANDLE blocked = open(SYNCHRONIZE, (std::wstring(ack_name) + L"-blocked").c_str())) {
      const bool unsafe = WaitForSingleObject(blocked, 0) == WAIT_OBJECT_0;
      CloseHandle(blocked);
      if (unsafe) throw MatSourceBlocked("Previous MAT capture cleanup incomplete");
    }
  }
}
#endif
