#pragma once
#include "bed_source.h"

namespace sonic_live {
  // Keep the selected Windows audio graph running during idle periods. Windows
  // mixes real application streams and creates the MAT; we never encode here.
  class NativeEndpointKeepalive {
    struct Apartment {
      Apartment() { bed_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "NATIVE_ENDPOINT_COM"); }
      ~Apartment() { CoUninitialize(); }
    } apartment_;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<IMMDevice> endpoint_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> writer_;
    UINT32 capacity_ = 0;
    bool started_ = false;

  public:
    explicit NativeEndpointKeepalive(const std::wstring &endpoint) {
      bed_hr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_)), "NATIVE_ENDPOINT_ENUMERATOR");
      bed_hr(enumerator_->GetDevice(endpoint.c_str(), &endpoint_), "NATIVE_ENDPOINT_DEVICE");
      bed_hr(endpoint_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_), "NATIVE_ENDPOINT_CLIENT");
      WAVEFORMATEX *format = nullptr;
      bed_hr(client_->GetMixFormat(&format), "NATIVE_ENDPOINT_FORMAT");
      const auto hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr);
      CoTaskMemFree(format);
      bed_hr(hr, "NATIVE_ENDPOINT_OPEN");
      bed_hr(client_->GetService(IID_PPV_ARGS(&writer_)), "NATIVE_ENDPOINT_WRITER");
      bed_hr(client_->GetBufferSize(&capacity_), "NATIVE_ENDPOINT_CAPACITY");
      pump();
    }
    void pump() {
      UINT32 padding = 0;
      bed_hr(client_->GetCurrentPadding(&padding), "NATIVE_ENDPOINT_PADDING");
      require(padding <= capacity_, "Native endpoint padding bounds");
      const auto frames = capacity_ - padding;
      if (!frames) return;
      BYTE *bytes = nullptr;
      bed_hr(writer_->GetBuffer(frames, &bytes), "NATIVE_ENDPOINT_BUFFER");
      bed_hr(writer_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT), "NATIVE_ENDPOINT_SILENT_RELEASE");
    }
    void start() {
      if (!started_) {
        bed_hr(client_->Start(), "NATIVE_ENDPOINT_START");
        started_ = true;
      }
    }
    ~NativeEndpointKeepalive() { if (started_) client_->Stop(); }
  };
}
