#pragma once

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <algorithm>
  #include <functional>

namespace mat_capture {
  // One serial pipe user owns one event. Every pending operation is completed
  // or cancelled and joined before its OVERLAPPED, buffer or event is reused.
  class PipeIo {
    HANDLE event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD error_ = ERROR_SUCCESS;

  public:
    PipeIo() = default;
    PipeIo(const PipeIo &) = delete;
    PipeIo &operator=(const PipeIo &) = delete;
    ~PipeIo() { if (event_) CloseHandle(event_); }
    DWORD error() const { return error_; }

    bool
    transfer(HANDLE pipe, void *buffer, DWORD size, bool writing, DWORD timeout,
      const std::function<bool()> &cancelled = [] { return false; }) {
      error_ = ERROR_SUCCESS;
      if (!event_) {
        error_ = ERROR_INVALID_HANDLE;
        return false;
      }
      auto *bytes = static_cast<BYTE *>(buffer);
      DWORD done = 0;
      const auto began = GetTickCount64();
      while (done < size) {
        if (cancelled()) {
          error_ = ERROR_OPERATION_ABORTED;
          return false;
        }
        if (!ResetEvent(event_)) {
          error_ = GetLastError();
          return false;
        }
        OVERLAPPED operation {};
        operation.hEvent = event_;
        DWORD transferred = 0;
        BOOL okay = writing ? WriteFile(pipe, bytes + done, size - done, &transferred, &operation) :
                              ReadFile(pipe, bytes + done, size - done, &transferred, &operation);
        DWORD code = okay ? ERROR_SUCCESS : GetLastError();
        if (!okay && code == ERROR_IO_PENDING) {
          for (;;) {
            const auto elapsed = GetTickCount64() - began;
            if (cancelled() || elapsed >= timeout) {
              error_ = elapsed >= timeout ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED;
              break;
            }
            const auto wait = WaitForSingleObject(event_, (std::min)(DWORD(20), DWORD(timeout - elapsed)));
            if (wait == WAIT_OBJECT_0) {
              okay = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
              code = okay ? ERROR_SUCCESS : GetLastError();
              break;
            }
            if (wait == WAIT_FAILED) {
              error_ = GetLastError();
              break;
            }
          }
          if (error_) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &transferred, TRUE);
            return false;
          }
        }
        if (!okay || !transferred || transferred > size - done) {
          error_ = code ? code : ERROR_INVALID_DATA;
          return false;
        }
        done += transferred;
      }
      return true;
    }
  };
}  // namespace mat_capture
#endif
