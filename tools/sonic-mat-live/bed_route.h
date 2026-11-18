#pragma once
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include "../../src/platform/windows/PolicyConfig.h"
#include "bed_source.h"
#include "bed_route_state.h"

namespace sonic_live {
  // The publisher runs as the interactive user. A SYSTEM Sunshine service's
  // default-output policy does not necessarily route that user's desktop audio.
  class BedRoute {
    struct Apartment {
      Apartment() { bed_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "BED_ROUTE_COM"); }
      ~Apartment() { CoUninitialize(); }
    } apartment_;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<IPolicyConfig> policy_;
    std::wstring endpoint_;
    BedRouteState state_;

    std::wstring
    current(ERole role) {
      Microsoft::WRL::ComPtr<IMMDevice> device;
      if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, role, &device))) return {};
      LPWSTR id = nullptr;
      bed_hr(device->GetId(&id), "BED_ROUTE_CURRENT_ID");
      std::wstring value(id);
      CoTaskMemFree(id);
      return value;
    }

  public:
    explicit BedRoute(const std::wstring &requested) {
      bed_hr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_)), "BED_ROUTE_ENUMERATOR");
      bed_hr(CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy_)), "BED_ROUTE_POLICY");
      Microsoft::WRL::ComPtr<IMMDevice> device;
      if (requested == L"default")
        bed_hr(enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, &device), "BED_ROUTE_DEFAULT");
      else
        bed_hr(enumerator_->GetDevice(requested.c_str(), &device), "BED_ROUTE_ENDPOINT");
      LPWSTR id = nullptr;
      bed_hr(device->GetId(&id), "BED_ROUTE_ENDPOINT_ID");
      endpoint_ = id;
      CoTaskMemFree(id);
    }
    const std::wstring &
    endpoint() const { return endpoint_; }
    void
    start() {
      if (state_.active()) return;
      state_.start(endpoint_, [&](int role) {
        auto id = current(ERole(role));
        printf("BED_ROUTE_SAVE role=%d endpoint=%ls\n", role, id.c_str());
        return id;
      },
        [&](int role, const std::wstring &id) { bed_hr(policy_->SetDefaultEndpoint(id.c_str(), ERole(role)), "BED_ROUTE_SELECT"); });
      printf("BED_ROUTE_SELECTED endpoint=%ls interactiveUser=1\n", endpoint_.c_str());
    }
    bool
    restore() noexcept {
      if (!state_.active()) return true;
      const auto okay = state_.restore([&](int role) { return current(ERole(role)); },
        [&](int role, const std::wstring &id) {
          bed_hr(policy_->SetDefaultEndpoint(id.c_str(), ERole(role)), "BED_ROUTE_RESTORE");
          printf("BED_ROUTE_RESTORED role=%d endpoint=%ls\n", role, id.c_str());
        },
        [](int role, const char *error) { printf("BED_ROUTE_RESTORE_FAILED role=%d %s\n", role, error); });
      puts(okay ? "BED_ROUTE_RELEASED" : "BED_ROUTE_RELEASE_INCOMPLETE");
      return okay;
    }
    ~BedRoute() { restore(); }
  };
}  // namespace sonic_live
