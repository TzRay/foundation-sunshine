#pragma once

#include <windows.h>
#include <avrt.h>

namespace platf {
  // Keep registration and restoration on the thread that performs audio I/O.
  // This is the same MMCSS task used by the Windows WASAPI capture path.
  class mmcss_audio_t {
    HANDLE task_ = nullptr;
    DWORD error_ = ERROR_SUCCESS;

  public:
    explicit mmcss_audio_t(bool enabled = true) {
      if (enabled) {
        DWORD index = 0;
        task_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
        if (!task_) error_ = GetLastError();
      }
    }
    mmcss_audio_t(const mmcss_audio_t &) = delete;
    mmcss_audio_t &operator=(const mmcss_audio_t &) = delete;
    ~mmcss_audio_t() {
      if (task_) AvRevertMmThreadCharacteristics(task_);
    }
    bool active() const { return task_ != nullptr; }
    DWORD error() const { return error_; }
  };
}
