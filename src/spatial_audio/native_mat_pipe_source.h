#pragma once
#include "encoded_audio_source.h"
#include "mat_capture_protocol.h"
#include "mat_capture_reader.h"
#include "mat_live_status.h"
#include "mat_pipe_io.h"
#include "mat_auto_source.h"
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <filesystem>
namespace spatial_audio {
  class NativeMatPipeSource final: public EncodedAudioSource {
    HANDLE pipe_ = INVALID_HANDLE_VALUE, publisher_ = nullptr, engine_ = nullptr;
    mat_capture::PipeIo pipe_io_;
    mat_capture::Descriptor native_ {};
    State state_ = State::preparing;
    uint64_t next_ = 0, last_qpc_ = 0;
    std::string error_;
    bool status_leased_ = false, status_running_ = false;
    bool restart_supported_ = false;
    bool reprepare_allowed_ = false;
    std::function<bool()> cancelled_ = [] { return false; };
    MatLiveStatus::Timing read_timing_, age_timing_;
    std::chrono::steady_clock::time_point reported_ = std::chrono::steady_clock::now();
    static void
    observe(MatLiveStatus::Timing &timing, uint64_t us) {
      ++timing.samples;
      timing.max_us = (std::max)(timing.max_us, us);
      timing.over_10ms += us >= 10000;
      timing.over_60ms += us >= 60000;
    }
    void
    report(bool final = false) noexcept {
      const auto now = std::chrono::steady_clock::now();
      if (!read_timing_.samples || (!final && now - reported_ < std::chrono::seconds(5))) return;
      try {
        mat_live_status.record_timing("pipe_read", read_timing_.samples, read_timing_.max_us, read_timing_.over_10ms, read_timing_.over_60ms);
        mat_live_status.record_timing("pipe_source_age", age_timing_.samples, age_timing_.max_us, age_timing_.over_10ms, age_timing_.over_60ms);
      }
      catch (...) {
        // Reporting also runs during stop/destruction. A diagnostic snapshot
        // allocation must not turn normal lease cleanup into termination.
      }
      fprintf(stdout, "MAT_PIPE_TIMING publisher=%lu sourceEpoch=%llu cycles=%llu samples=%llu readMaxUs=%llu readOver10Ms=%llu readOver60Ms=%llu sourceAgeMaxUs=%llu final=%u\n",
        static_cast<unsigned long>(native_.publisher_pid), static_cast<unsigned long long>(native_.epoch),
        static_cast<unsigned long long>(next_), static_cast<unsigned long long>(read_timing_.samples),
        static_cast<unsigned long long>(read_timing_.max_us), static_cast<unsigned long long>(read_timing_.over_10ms),
        static_cast<unsigned long long>(read_timing_.over_60ms), static_cast<unsigned long long>(age_timing_.max_us), unsigned(final));
      read_timing_ = {};
      age_timing_ = {};
      reported_ = now;
    }
    void
    close() noexcept {
      if (pipe_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe_, nullptr);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
      }
      if (publisher_) {
        CloseHandle(publisher_);
        publisher_ = nullptr;
      }
      if (engine_) {
        CloseHandle(engine_);
        engine_ = nullptr;
      }
      if (status_leased_) {
        mat_live_status.released(status_running_);
        status_leased_ = false;
        status_running_ = false;
      }
    }
    static uint64_t
    created(HANDLE p) {
      FILETIME c {}, e {}, k {}, u {};
      require(GetProcessTimes(p, &c, &e, &k, &u) != 0, "MAT process creation time");
      return (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    }
    static bool
    alive(HANDLE p) {
      DWORD code = 0;
      return GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    }
    static std::wstring
    image(HANDLE p) {
      wchar_t path[32768];
      DWORD n = 32768;
      require(QueryFullProcessImageNameW(p, 0, path, &n) != 0, "MAT publisher/engine image");
      return std::wstring(path, n);
    }
    void
    io(void *data, DWORD bytes, bool writing, DWORD timeout = 2000) {
      if (pipe_io_.transfer(pipe_, data, bytes, writing, timeout, cancelled_)) return;
      const auto code = pipe_io_.error();
      reprepare_allowed_ = code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED ||
        code == ERROR_NO_DATA || code == ERROR_TIMEOUT;
      require(pipe_io_.error() != ERROR_TIMEOUT && pipe_io_.error() != ERROR_OPERATION_ABORTED, "MAT pipe I/O timeout/cancel");
      throw std::runtime_error("MAT pipe disconnected/I/O failure");
    }

  public:
    NativeMatPipeSource() = default;
    NativeMatPipeSource(const NativeMatPipeSource &) = delete;
    NativeMatPipeSource &
    operator=(const NativeMatPipeSource &) = delete;
    ~NativeMatPipeSource() override { stop(); }

    static bool busy(const std::filesystem::path &name) {
      // Probe without opening a lease or changing its audio route.
      if (WaitNamedPipeW(name.c_str(), NMPWAIT_NOWAIT)) return false;
      const auto error = GetLastError();
      return error == ERROR_SEM_TIMEOUT || error == ERROR_PIPE_BUSY;
    }

    void
    prepare(const std::string &name, const std::string &producer, bool tryRecovery = false, DWORD connect_wait_ms = 0,
            std::chrono::steady_clock::time_point prepare_deadline = std::chrono::steady_clock::time_point::max(), bool wait_if_busy = true) {
      require(state_ == State::preparing, "Native MAT source lease already prepared/closed");
      try {
        require(!name.empty() && name.starts_with("\\\\.\\pipe\\SunshineMatLive-") &&
                  (!producer.empty() || name == mat_auto_pipe),
          "Local native MAT source required");
        require(name.find_first_of("\r\n") == std::string::npos, "MAT pipe name");
        // Keep attempt expiry separate from global cancellation. A pending I/O
        // uses the remaining budget as its timeout, so expiry stays retryable.
        const auto prepare_timeout = [&](DWORD maximum) {
          if (cancelled_()) {
            reprepare_allowed_ = false;
            throw std::runtime_error("MAT pipe cancelled");
          }
          const auto now = std::chrono::steady_clock::now();
          if (now >= prepare_deadline) {
            reprepare_allowed_ = true;
            throw std::runtime_error("MAT source preparation deadline");
          }
          const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(prepare_deadline - now);
          return remaining < std::chrono::milliseconds(maximum) ? DWORD(remaining.count()) : maximum;
        };
        const auto prepare_io = [&](void *data, DWORD bytes, bool writing) {
          io(data, bytes, writing, prepare_timeout(2000));
        };
        const auto connect = [&](DWORD wait_ms, bool wait_busy) {
          const auto deadline = GetTickCount64() + wait_ms;
          do {
            prepare_timeout(0);
            pipe_ = CreateFileW(std::filesystem::path(name).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) break;
            const auto code = GetLastError();
            reprepare_allowed_ = code == ERROR_PIPE_BUSY || code == ERROR_FILE_NOT_FOUND;
            if (!reprepare_allowed_ || (code == ERROR_PIPE_BUSY && !wait_busy) || !wait_ms || GetTickCount64() >= deadline) break;
            Sleep(10);
          } while (true);
          require(pipe_ != INVALID_HANDLE_VALUE, "Native MAT capture is not armed or is leased");
          reprepare_allowed_ = false;
          ULONG id = 0;
          require(GetNamedPipeServerProcessId(pipe_, &id), "Native MAT pipe publisher PID");
          publisher_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
          require(publisher_ != nullptr, "MAT publisher process");
          if (producer.empty()) require(trusted_mat_auto_publisher(publisher_), "Untrusted automatic MAT source");
          return id;
        };
        ULONG pid = connect(connect_wait_ms, wait_if_busy);
        char command[8];
        uint8_t header[mat_capture::descriptor_bytes];
        restart_supported_ = tryRecovery;
        try {
          memcpy(command, tryRecovery ? "MCAPOPN3" : "MCAPOPEN", 8);
          prepare_io(command, 8, true);
          prepare_io(header, sizeof(header), false);
        }
        catch (...) {
          const auto code = pipe_io_.error();
          if (!tryRecovery || cancelled_() || (code != ERROR_BROKEN_PIPE && code != ERROR_PIPE_NOT_CONNECTED && code != ERROR_NO_DATA)) throw;
          close();
          restart_supported_ = false;
          pid = connect(2000, true);
          memcpy(command, "MCAPOPEN", 8);
          prepare_io(command, 8, true);
          prepare_io(header, sizeof(header), false);
        }
        reprepare_allowed_ = false;
        require(mat_capture::decode_descriptor(header, native_), "Native MAT descriptor (test/file sources forbidden)");
        require(pid == native_.publisher_pid, "Native MAT pipe publisher PID");
        // Standard-user Sunshine may query audiodg but cannot acquire
        // SYNCHRONIZE on it. Querying its exit code needs no such right.
        engine_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, native_.engine_pid);
        require(publisher_ && engine_ && created(publisher_) == native_.publisher_created && created(engine_) == native_.engine_created,
          "MAT publisher/engine identity changed");
        if (!producer.empty()) {
          auto expected = std::filesystem::canonical(producer).wstring();
          auto actual = std::filesystem::canonical(image(publisher_)).wstring();
          require(_wcsicmp(expected.c_str(), actual.c_str()) == 0, "MAT pipe belongs to unexpected producer image");
        }
        wchar_t system[MAX_PATH];
        require(GetSystemDirectoryW(system, MAX_PATH) > 0, "Windows system directory");
        auto target = (std::filesystem::path(system) / L"audiodg.exe").wstring();
        require(_wcsicmp(image(engine_).c_str(), target.c_str()) == 0, "Native MAT engine image");
        state_ = State::idle;
        reprepare_allowed_ = false;
        mat_live_status.acquired(native_.publisher_pid, native_.publisher_created, native_.engine_pid, native_.engine_created);
        status_leased_ = true;
      }
      catch (const std::exception &e) {
        error_ = e.what();
        mat_live_status.failed(error_, "pipe_prepare");
        state_ = State::fault;
        close();
        throw;
      }
    }
    const mat_capture::Descriptor &
    identity() const { return native_; }
    Descriptor
    descriptor() const override { return {}; }
    void
    cancel_when(std::function<bool()> fn) { cancelled_ = std::move(fn); }
    bool
    supports_restart() const { return restart_supported_; }
    bool
    can_reprepare() const { return reprepare_allowed_; }
    bool
    restart() {
      if (!restart_supported_ || pipe_ == INVALID_HANDLE_VALUE) return false;
      if (state_ != State::idle && state_ != State::running) return false;
      try {
        if (WaitForSingleObject(publisher_, 0) != WAIT_TIMEOUT || !alive(engine_)) {
          reprepare_allowed_ = true;
          throw std::runtime_error("Native MAT owner exited");
        }
        // A previous client preparation can leave an authenticated source paused.
        if (state_ == State::idle) return true;
        const auto deadline = GetTickCount64() + 2000;
        const auto bounded_io = [&](void *data, size_t bytes, bool writing) {
          const auto now = GetTickCount64();
          require(now < deadline, "Native MAT restart barrier timeout");
          io(data, DWORD(bytes), writing, DWORD(deadline - now));
        };
        char command[8];
        memcpy(command, "MCAPREST", sizeof(command));
        bounded_io(command, sizeof(command), true);
        mat_capture::restart_barrier(bounded_io, next_, last_qpc_, native_);
        report(true);
        if (status_running_) { mat_live_status.paused(); status_running_ = false; }
        state_ = State::idle;
        return true;
      }
      catch (const std::exception &e) {
        if (error_.empty()) error_ = e.what();
        if (!cancelled_()) mat_live_status.failed(error_, "pipe_restart");
        state_ = State::fault;
        close();
        return false;
      }
    }
    bool
    start() override {
      if (state_ != State::idle) return false;
      try {
        char command[8];
        memcpy(command, "MCAPPLAY", 8);
        io(command, 8, true);
        state_ = State::running;
        mat_live_status.started();
        status_running_ = true;
        reported_ = std::chrono::steady_clock::now();
        return true;
      }
      catch (const std::exception &e) {
        error_ = e.what();
        if (!cancelled_()) mat_live_status.failed(error_, "pipe_play");
        state_ = State::fault;
        close();
        return false;
      }
    }
    Read
    read(Frame &out) override {
      if (state_ == State::stopped) return Read::ended;
      if (state_ != State::running) return Read::failed;
      const auto began = std::chrono::steady_clock::now();
      const char *stage = "pipe_owner";
      try {
        if (WaitForSingleObject(publisher_, 0) != WAIT_TIMEOUT || !alive(engine_)) {
          reprepare_allowed_ = true;
          throw std::runtime_error("Native MAT owner exited");
        }
        auto status = mat_capture::read_record([this, &stage](void *p, size_t n, bool writing) {
          stage = writing ? "pipe_end_ack" : n == mat_capture::record_bytes ? "pipe_header" : "pipe_payload";
          io(p, (DWORD) n, writing);
        }, next_, last_qpc_, native_.epoch, native_.encoder, out);
        observe(read_timing_, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count());
        if (status == Read::frame) {
          LARGE_INTEGER now {};
          QueryPerformanceCounter(&now);
          const auto age = now.QuadPart > static_cast<int64_t>(last_qpc_) ? uint64_t(now.QuadPart) - last_qpc_ : 0;
          observe(age_timing_, age / native_.qpc_hz * 1000000 + age % native_.qpc_hz * 1000000 / native_.qpc_hz);
        }
        report(status == Read::ended);
        if (status == Read::ended) {
          state_ = State::stopped;
          close();
        }
        return status;
      }
      catch (const std::exception &e) {
        error_ = e.what();
        observe(read_timing_, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count());
        if (!cancelled_()) mat_live_status.failed(error_, stage);
        report(true);
        state_ = State::fault;
        close();
        return Read::failed;
      }
    }
    void
    stop() override {
      report(true);
      close();
      if (state_ != State::fault) state_ = State::stopped;
    }
    State
    state() const override { return state_; }
    std::string
    error() const override { return error_; }
  };
}  // namespace spatial_audio
#endif
