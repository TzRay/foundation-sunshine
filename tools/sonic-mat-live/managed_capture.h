#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace sonic_live {
  enum class CaptureAvailability { available, cleaning, blocked };

  // The exclusive handle outlives the manager if its child is still restoring
  // the route. An unclean exit leaves the engine creation time for the next
  // manager, including when no manager was alive to collect the exit code.
  inline CaptureAvailability previous_capture(HANDLE file, uint64_t engine) {
    uint64_t previous = 0;
    DWORD bytes = 0;
    const bool known = ReadFile(file, &previous, sizeof(previous), &bytes, nullptr) && bytes == sizeof(previous);
    return known && previous && previous != engine ? CaptureAvailability::available : CaptureAvailability::blocked;
  }

  inline CaptureAvailability capture_availability(const std::filesystem::path &marker, uint64_t engine) {
    HANDLE file = CreateFileW(marker.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      const auto error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND) return CaptureAvailability::available;
      if (error == ERROR_SHARING_VIOLATION || error == ERROR_DELETE_PENDING) return CaptureAvailability::cleaning;
      return CaptureAvailability::blocked;
    }
    const auto status = previous_capture(file, engine);
    CloseHandle(file);
    return status;
  }

  struct ManagedCapture {
    HANDLE change = nullptr, lease = INVALID_HANDLE_VALUE;
    bool clean = true;

    void open(const std::wstring &stop_name, const std::filesystem::path &log, uint64_t engine) {
      change = CreateEventW(nullptr, TRUE, FALSE, (stop_name + L"-replace").c_str());
      if (!change || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("MAT source change event");
      if (log.empty()) return;
      const auto marker = log.parent_path().parent_path() / L"capture.lease";
      lease = CreateFileW(marker.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
      if (lease == INVALID_HANDLE_VALUE) throw std::runtime_error("Acquire managed MAT capture");
      if (GetLastError() == ERROR_ALREADY_EXISTS && previous_capture(lease, engine) != CaptureAvailability::available) {
        clean = false;
        throw std::runtime_error("Previous MAT capture cleanup incomplete");
      }
      SetFilePointer(lease, 0, nullptr, FILE_BEGIN);
      DWORD bytes = 0;
      if (!WriteFile(lease, &engine, sizeof(engine), &bytes, nullptr) || bytes != sizeof(engine) || !FlushFileBuffers(lease))
        throw std::runtime_error("Record managed MAT capture");
    }

    bool replacing(HANDLE stop, HANDLE owner) const {
      return (change && WaitForSingleObject(change, 0) == WAIT_OBJECT_0) ||
        (owner && WaitForSingleObject(owner, 0) == WAIT_OBJECT_0 && WaitForSingleObject(stop, 0) != WAIT_OBJECT_0);
    }

    void route_failed() {
      clean = false;
      // A new audio engine retires its old worker, but cannot restore a
      // partially changed Windows default route. Keep that failure blocked.
      if (lease != INVALID_HANDLE_VALUE && SetFilePointer(lease, 0, nullptr, FILE_BEGIN) == 0)
        SetEndOfFile(lease);
    }

    ~ManagedCapture() {
      if (lease != INVALID_HANDLE_VALUE) {
        FILE_DISPOSITION_INFO disposition { TRUE };
        if (clean) SetFileInformationByHandle(lease, FileDispositionInfo, &disposition, sizeof(disposition));
        CloseHandle(lease);
      }
      if (change) CloseHandle(change);
    }
  };
}
