#pragma once
#include "process_helpers.h"
#include "../../src/spatial_audio/native_mat_guard.h"
#include "../../src/spatial_audio/mat_capture_protocol.h"
#include "../../src/spatial_audio/mat_pipe_io.h"
#include "../../src/spatial_audio/encoded_audio_source.h"
#include <array>
#include <functional>
#include <sddl.h>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")
using spatial_audio::require;
struct Cycle {
  std::array<uint8_t, MAT_WIRE_CYCLE> bytes;
  uint64_t qpc;
};
struct Shared {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Cycle> queue;
  bool failed = false, ended = false, playing = false, ready = false;
  // Delivery failure belongs to one pipe lease, not the native source.
  bool lease_failed = false;
  // Four queued cycles, the incoming cycle, and three more during the former
  // 60 ms grace period. The ring reader stores this bounded burst without waiting.
  static constexpr size_t max_lease_cycles = 8;
  static constexpr uint64_t max_native_age_ms = 80 + 60;
  uint64_t max_cycle_age_ticks = 0; // Native capture sets this from its QPC frequency.
  bool too_old(uint64_t captured, uint64_t now) const {
    return max_cycle_age_ticks && now > captured && now - captured > max_cycle_age_ticks;
  }
  uint64_t encoder = 0, play_qpc = 0, last_cycle = 0, cycles = 0, acknowledged_leases = 0;
  uint64_t consumer_waits = 0, consumer_wait_max_ms = 0;
  size_t queue_peak = 0;
  uint64_t consumer_wait_over_10ms = 0;
  std::string error, delivered_sha256;
};
// Metadata-only acceptance oracle for the post-PLAY stream; no payload storage.
class LeaseDigest {
  BCRYPT_ALG_HANDLE algorithm_ {};
  BCRYPT_HASH_HANDLE hash_ {};
  std::vector<BYTE> object_;
  void
  close() {
    if (hash_) BCryptDestroyHash(hash_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    hash_ = nullptr;
    algorithm_ = nullptr;
  }

public:
  LeaseDigest() {
    try {
      DWORD size = 0, got = 0;
      require(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "Lease SHA256 algorithm");
      require(BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, (PUCHAR) &size, sizeof(size), &got, 0) >= 0, "Lease SHA256 object");
      object_.resize(size);
      require(BCryptCreateHash(algorithm_, &hash_, object_.data(), size, nullptr, 0, 0) >= 0, "Lease SHA256 init");
    }
    catch (...) {
      close();
      throw;
    }
  }
  ~LeaseDigest() { close(); }
  void
  update(const uint8_t *p, size_t n) { require(n <= MAXDWORD && BCryptHashData(hash_, const_cast<PUCHAR>(p), (ULONG) n, 0) >= 0, "Lease SHA256 update"); }
  std::string
  finish() {
    BYTE digest[32];
    require(BCryptFinishHash(hash_, digest, 32, 0) >= 0, "Lease SHA256 finish");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (auto b : digest) {
      out += hex[b >> 4];
      out += hex[b & 15];
    }
    return out;
  }
};
static uint64_t
Qpc() {
  LARGE_INTEGER n {};
  require(QueryPerformanceCounter(&n) != FALSE, "QPC");
  return n.QuadPart;
}
static void
Broken(Shared &s, const char *message) {
  std::lock_guard lock(s.mutex);
  if (s.failed) return;
  s.failed = true;
  s.error = message;
  s.queue.clear();
  s.changed.notify_all();
}
static uintptr_t
LoadedTap(DWORD pid, const fs::path &expected) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
  require(snap != INVALID_HANDLE_VALUE, "Enumerate loaded tap identity");
  MODULEENTRY32W entry {};
  entry.dwSize = sizeof(entry);
  uintptr_t address = 0;
  try {
    require(Module32FirstW(snap, &entry) != FALSE, "Read loaded module list");
    do {
      if (_wcsicmp(entry.szModule, expected.filename().c_str())) continue;
      auto actual = fs::canonical(entry.szExePath);
      require(!address && _wcsicmp(expected.c_str(), actual.c_str()) == 0, "Loaded tap path differs; no reuse");
      address = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
    } while (Module32NextW(snap, &entry));
  }
  catch (...) {
    CloseHandle(snap);
    throw;
  }
  CloseHandle(snap);
  return address;
}
static bool
Io(HANDLE pipe, void *buffer, DWORD size, bool write, DWORD timeout, std::function<bool()> cancelled = [] { return false; }) {
  // Convenience for one-off diagnostics/tests. The production server below
  // retains its PipeIo for the entire serial lease.
  mat_capture::PipeIo transfer;
  return transfer.transfer(pipe, buffer, size, write, timeout, [&] { return interrupted || cancelled(); });
}
static PSECURITY_DESCRIPTOR
PipeAcl(bool medium_integrity = false) {
  HANDLE token {};
  require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE, "Pipe token");
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> bytes(size);
  bool okay = GetTokenInformation(token, TokenUser, bytes.data(), size, &size) != FALSE;
  CloseHandle(token);
  require(okay, "Pipe token user");
  LPWSTR sid {};
  require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &sid) != FALSE, "Pipe SID");
  std::wstring acl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + std::wstring(sid) + L")";
  LocalFree(sid);
  if (medium_integrity) acl += L"S:(ML;;NW;;;ME)";
  PSECURITY_DESCRIPTOR result {};
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &result, nullptr) != FALSE, "Private pipe ACL");
  return result;
}
static void
Serve(Shared &shared, const std::wstring &name, mat_capture::Descriptor descriptor, std::function<void(bool)> route = {}) {
  auto acl = PipeAcl();
  SECURITY_ATTRIBUTES attributes { sizeof(attributes), acl, FALSE };
  try {
    while (!interrupted) {
      {
        std::unique_lock lock(shared.mutex);
        // A visible listener means a descriptor can be served. Opening it before
        // the first cycle made recovery mistake startup for a rejected protocol.
        while (!shared.ready && !shared.failed && !shared.ended && !interrupted)
          shared.changed.wait_for(lock, std::chrono::milliseconds(20));
        if (shared.failed || shared.ended || interrupted) break;
      }
      HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 131072, 4096, 0, &attributes);
      require(pipe != INVALID_HANDLE_VALUE, "Exclusive local MAT pipe");
      mat_capture::PipeIo transfer;
      const auto io = [&](void *buffer, DWORD size, bool write, DWORD timeout, std::function<bool()> cancelled = [] { return false; }) {
        return transfer.transfer(pipe, buffer, size, write, timeout, [&] { return interrupted || cancelled(); });
      };
      uint64_t sequence = 0, lease_started = 0;
      const char *stage = "pipe_open";
      uint64_t write_samples = 0, write_max_us = 0, write_over_10ms = 0, write_over_60ms = 0;
      uint64_t queue_wait_max_us = 0, source_age_max_us = 0, reported = GetTickCount64();
      const auto elapsed_us = [&](uint64_t start) {
        const auto elapsed = Qpc() - start;
        return elapsed / descriptor.qpc_hz * 1000000 + elapsed % descriptor.qpc_hz * 1000000 / descriptor.qpc_hz;
      };
      const auto report = [&](bool final = false) {
        const auto now = GetTickCount64();
        if (!write_samples || (!final && now - reported < 5000)) return;
        printf("LEASE_DELIVERY_TIMING publisher=%lu sourceEpoch=%llu cycles=%llu windowSamples=%llu writeMaxUs=%llu writeOver10Ms=%llu writeOver60Ms=%llu queueWaitMaxUs=%llu sourceAgeMaxUs=%llu final=%u\n",
          GetCurrentProcessId(), descriptor.epoch, sequence, write_samples, write_max_us, write_over_10ms, write_over_60ms,
          queue_wait_max_us, source_age_max_us, unsigned(final));
        write_samples = write_max_us = write_over_10ms = write_over_60ms = queue_wait_max_us = source_age_max_us = 0;
        reported = now;
      };
      struct RouteLease {
        std::function<void(bool)> &route;
        bool active = false;
        ~RouteLease() { if (active) route(false); }
      } routing { route };
      try {
        OVERLAPPED o {};
        o.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(o.hEvent != nullptr, "Pipe connect event");
        BOOL connected = ConnectNamedPipe(pipe, &o);
        DWORD code = connected ? 0 : GetLastError();
        if (!connected && code == ERROR_PIPE_CONNECTED) connected = TRUE;
        if (!connected && code == ERROR_IO_PENDING) {
          while (WaitForSingleObject(o.hEvent, 20) == WAIT_TIMEOUT && !interrupted) {
            std::lock_guard lock(shared.mutex);
            if (shared.failed || shared.ended) break;
          }
          DWORD n = 0;
          if (interrupted) { CancelIoEx(pipe, &o); }
          else {
            std::lock_guard lock(shared.mutex);
            if (shared.failed || shared.ended) CancelIoEx(pipe, &o);
          }
          connected = GetOverlappedResult(pipe, &o, &n, TRUE);
        }
        CloseHandle(o.hEvent);
        const auto unavailable = [&] {std::lock_guard lock(shared.mutex);return shared.failed||shared.ended; };
        char command[8];
        require(connected && io(command, 8, false, 2000, unavailable), "Live source lease open");
        const bool restart_supported = !memcmp(command, "MCAPOPN3", 8);
        require(restart_supported || !memcmp(command, "MCAPOPEN", 8), "Live source lease open");
        {
          std::lock_guard lock(shared.mutex);
          require(shared.ready && !shared.failed && GetTickCount64() - shared.last_cycle < 2000, "Native MAT not ready");
          shared.lease_failed = false;
          descriptor.encoder = shared.encoder;
        }
        if (route) {
          stage = "route_select";
          routing.active = true;
          route(true);
        }
        uint8_t header[mat_capture::descriptor_bytes];
        mat_capture::encode_descriptor(header, descriptor);
        bool restarting;
        do {
        restarting = false;
        sequence = 0;
        stage = "pipe_descriptor";
        require(io(header, sizeof(header), true, 2000, unavailable), "Live source descriptor");
        stage = "pipe_play";
        require(io(command, 8, false, 30000, unavailable) && !memcmp(command, "MCAPPLAY", 8), "Live source lease play timeout/disconnect");
        {
          std::lock_guard lock(shared.mutex);
          require(!shared.failed && shared.ready && GetTickCount64() - shared.last_cycle < 2000, "Source stale at PLAY");
          shared.queue.clear();
          shared.lease_failed = false;
          shared.play_qpc = Qpc();
          shared.playing = true;
          shared.consumer_waits = shared.consumer_wait_max_ms = 0;
          shared.consumer_wait_over_10ms = shared.queue_peak = 0;
          shared.changed.notify_all();
        }
        lease_started = GetTickCount64();
        printf("LEASE_PLAY publisher=%lu encoder=0x%llx\n", GetCurrentProcessId(), descriptor.encoder);
        uint64_t last_capture_qpc = 0;
        LeaseDigest digest;
        while (!interrupted) {
          // Read only commands from this lease. The route owner and its first
          // role snapshot survive a logical restart on the same physical pipe.
          DWORD available = 0;
          require(PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr), "Live source consumer disconnected");
          if (available) {
            stage = "pipe_restart";
            require(restart_supported && io(command, 8, false, 2000, unavailable) && !memcmp(command, "MCAPREST", 8),
              "Live source restart not negotiated/invalid");
            {
              std::lock_guard lock(shared.mutex);
              require(!shared.failed && !shared.ended && !shared.lease_failed && shared.ready && shared.encoder == descriptor.encoder &&
                GetTickCount64() - shared.last_cycle < 2000, "Native MAT source cannot restart");
              shared.playing = false;
              shared.queue.clear();
              shared.changed.notify_all();
            }
            report(true);
            printf("LEASE_RESTART publisher=%lu encoder=0x%llx previousCycles=%llu routeRetained=1\n",
              GetCurrentProcessId(), descriptor.encoder, sequence);
            restarting = true;
            break;
          }
          stage = "source_queue";
          Cycle cycle {};
          bool end = false;
          {
            std::unique_lock lock(shared.mutex);
            const auto wait_began = Qpc();
            shared.changed.wait_for(lock, std::chrono::milliseconds(20), [&] { return shared.failed || shared.ended || shared.lease_failed || !shared.queue.empty(); });
            queue_wait_max_us = (std::max)(queue_wait_max_us, elapsed_us(wait_began));
            require(!shared.failed, "Native capture failed (no EOS)");
            require(!shared.lease_failed, "MAT delivery lease exceeded buffer budget; source retained");
            if (shared.queue.empty()) {
              if (shared.ended)
                end = true;
              else {
                require(GetTickCount64() - shared.last_cycle < 2000, "Native capture stalled");
                continue;
              }
            }
            else {
              cycle = std::move(shared.queue.front());
              shared.queue.pop_front();
              shared.changed.notify_all();
            }
          }
          uint8_t record[mat_capture::record_bytes];
          // EOS belongs to the same source timeline as the last cycle; a
          // virtual endpoint may report source QPC ahead of wall-clock QPC.
          const auto record_qpc = end && last_capture_qpc ? last_capture_qpc + descriptor.qpc_hz / 50 : end ? Qpc() : cycle.qpc;
          mat_capture::encode_record(record, end ? mat_capture::end_kind : mat_capture::cycle_kind, sequence, record_qpc,
            end ? nullptr : cycle.bytes.data(), end ? 0 : cycle.bytes.size());
          stage = "pipe_write";
          const auto write_began = Qpc();
          if (!end && write_began > cycle.qpc) source_age_max_us = (std::max)(source_age_max_us, elapsed_us(cycle.qpc));
          const auto delivery_cancelled = [&] {
            std::lock_guard lock(shared.mutex);
            // Also bound the cycle already removed from the queue for writing.
            // This callback cancels and joins a pending write before lease close.
            if (!end && shared.too_old(cycle.qpc, Qpc())) {
              shared.lease_failed = true;
              shared.playing = false;
              shared.changed.notify_all();
            }
            return shared.failed || shared.lease_failed;
          };
          const bool wrote = io(record, sizeof(record), true, 2000, delivery_cancelled) &&
            (end || io(cycle.bytes.data(), (DWORD) cycle.bytes.size(), true, 2000, delivery_cancelled));
          const auto write_us = elapsed_us(write_began);
          ++write_samples;
          write_max_us = (std::max)(write_max_us, write_us);
          write_over_10ms += write_us >= 10000;
          write_over_60ms += write_us >= 60000;
          require(wrote, "Live source consumer disconnected/backpressure");
          if (end) {
            // DisconnectNamedPipe discards buffered unread bytes.
            // Keep this lease alive until the client has consumed
            // the exact EOS; a completed WriteFile is insufficient.
            // Never use unbounded FlushFileBuffers on a lost peer.
            uint8_t ack[mat_capture::end_ack_bytes];
            stage = "pipe_end_ack";
            require(io(ack, sizeof(ack), false, 2000) && mat_capture::valid_end_ack(ack, sequence), "Live source end acknowledgement failed/timed out");
            auto sha = digest.finish();
            {
              std::lock_guard lock(shared.mutex);
              ++shared.acknowledged_leases;
              shared.delivered_sha256 = sha;
            }
            printf("LEASE_END_ACK cycles=%llu sha256=%s\n", sequence, sha.c_str());
            break;
          }
          digest.update(cycle.bytes.data(), cycle.bytes.size());
          last_capture_qpc = cycle.qpc;
          ++sequence;
          report();
        }
        } while (restarting && !interrupted);
      }
      catch (const std::exception &e) {
        printf("LEASE_CLOSED %s stage=%s cycles=%llu elapsedMs=%llu\n", e.what(), stage, sequence, lease_started ? GetTickCount64() - lease_started : 0);
      }
      report(true);
      {
        std::lock_guard lock(shared.mutex);
        shared.playing = false;
        shared.queue.clear();
        shared.changed.notify_all();
        printf("LEASE_QUEUE_STATS waits=%llu maxWaitMs=%llu waitsOver10Ms=%llu peakQueue=%zu\n",
          shared.consumer_waits, shared.consumer_wait_max_ms, shared.consumer_wait_over_10ms, shared.queue_peak);
      }
      CancelIoEx(pipe, nullptr);
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
    }
  }
  catch (...) {
    LocalFree(acl);
    throw;
  }
  LocalFree(acl);
}
