#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <cstdio>
#include <sstream>
#include "audio_session_options.h"

namespace {
  using Microsoft::WRL::ComPtr;
  using sonic_live::audio_sessions::quote;

  std::string utf8(const wchar_t *text) {
    const int length = int(wcslen(text));
    if (!length) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), bytes, nullptr, nullptr);
    return result;
  }
  void check(HRESULT hr, const char *operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(uint32_t(hr)));
  }
  std::string id(IMMDevice *device) {
    LPWSTR value = nullptr;
    check(device->GetId(&value), "GetId");
    std::string result = utf8(value);
    CoTaskMemFree(value);
    return result;
  }
  std::string name(IMMDevice *device) {
    ComPtr<IPropertyStore> store;
    PROPVARIANT value {};
    std::string result;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
        SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
      result = utf8(value.pwszVal);
    PropVariantClear(&value);
    return result;
  }
  std::string process(DWORD pid) {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle) return {};
    wchar_t path[32768];
    DWORD size = DWORD(std::size(path));
    std::string result;
    if (QueryFullProcessImageNameW(handle, 0, path, &size)) result = utf8(path);
    CloseHandle(handle);
    return result;
  }
  void sessions(std::ostream &out, IMMDevice *device) {
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<IAudioSessionEnumerator> list;
    HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager);
    if (SUCCEEDED(hr)) hr = manager->GetSessionEnumerator(&list);
    int count = 0;
    if (SUCCEEDED(hr)) hr = list->GetCount(&count);
    out << ",\"sessionReadHresult\":" << uint32_t(hr) << ",\"sessions\":[";
    for (int i = 0; SUCCEEDED(hr) && i < count; ++i) {
      if (i) out << ',';
      ComPtr<IAudioSessionControl> control;
      ComPtr<IAudioSessionControl2> details;
      HRESULT read = list->GetSession(i, &control);
      if (SUCCEEDED(read)) read = control.As(&details);
      DWORD pid = 0;
      AudioSessionState state = AudioSessionStateInactive;
      HRESULT pidHr = FAILED(read) ? read : details->GetProcessId(&pid);
      HRESULT stateHr = FAILED(read) ? read : details->GetState(&state);
      // Non-S_OK success may describe a cross-process session, not one PID.
      const bool knownPid = pidHr == S_OK;
      const auto image = knownPid && pid ? process(pid) : std::string();
      out << "{\"pid\":" << (knownPid ? std::to_string(pid) : "null")
          << ",\"processImage\":" << (image.empty() ? "null" : quote(image))
          << ",\"state\":" << (FAILED(stateHr) ? "null" : quote(state == AudioSessionStateActive ? "active" : state == AudioSessionStateExpired ? "expired" : "inactive"))
          << ",\"processReadHresult\":" << uint32_t(pidHr) << ",\"stateReadHresult\":" << uint32_t(stateHr) << '}';
    }
    out << ']';
  }
}

int RunAudioSessions(int argc, wchar_t **argv) {
  std::wstring capture;
  try { capture = sonic_live::audio_sessions::capture_endpoint(argc, argv); }
  catch (const std::exception &e) { fprintf(stderr, "%s\n", e.what()); return 2; }
  try {
    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
    struct Com { ~Com() { CoUninitialize(); } } com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "MMDeviceEnumerator");
    const auto captureId = utf8(capture.c_str());
    DWORD windowsSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &windowsSession);
    std::ostringstream out;
    out << "{\"schema\":1,\"readOnly\":true,\"windowsSessionId\":" << windowsSession
        << ",\"captureEndpoint\":" << (captureId.empty() ? "null" : quote(captureId)) << ",\"defaults\":[";
    constexpr const char *roles[] = { "console", "multimedia", "communications" };
    for (int role = 0; role < 3; ++role) {
      if (role) out << ',';
      ComPtr<IMMDevice> device;
      const auto hr = enumerator->GetDefaultAudioEndpoint(eRender, ERole(role), &device);
      out << "{\"role\":" << quote(roles[role]) << ",\"endpoint\":" << (SUCCEEDED(hr) ? quote(id(device.Get())) : "null")
          << ",\"hresult\":" << uint32_t(hr) << '}';
    }
    ComPtr<IMMDeviceCollection> devices;
    check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &devices), "EnumAudioEndpoints");
    UINT count = 0;
    check(devices->GetCount(&count), "Endpoint count");
    out << "],\"endpoints\":[";
    for (UINT i = 0; i < count; ++i) {
      if (i) out << ',';
      ComPtr<IMMDevice> device;
      check(devices->Item(i, &device), "Endpoint item");
      DWORD state = 0;
      check(device->GetState(&state), "Endpoint state");
      const auto endpoint = id(device.Get());
      out << "{\"id\":" << quote(endpoint) << ",\"name\":" << quote(name(device.Get())) << ",\"state\":" << state
          << ",\"isCaptureEndpoint\":" << (captureId.empty() ? "null" : endpoint == captureId ? "true" : "false");
      if (state == DEVICE_STATE_ACTIVE) sessions(out, device.Get());
      else out << ",\"sessions\":[],\"sessionReadHresult\":null";
      out << '}';
    }
    out << "]}\n";
    SetConsoleOutputCP(CP_UTF8);
    fputs(out.str().c_str(), stdout);
    return 0;
  }
  catch (const std::exception &e) { fprintf(stderr, "Audio session diagnostic failed: %s\n", e.what()); return 1; }
}
