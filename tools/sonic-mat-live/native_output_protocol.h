#pragma once
#include <windows.h>
#include <cstdint>

// Diagnostic file/ring only. This is not the Sunshine live transport.
namespace native_output {
  constexpr uint32_t magic = 0x3154554f, version = 2;
  constexpr uint32_t slot_count = 64, max_bytes = 65536;
  constexpr uint32_t process_rva = 0x31e00, dolby_process_rva = 0x4e00;
  inline constexpr BYTE process_entry[] = { 0x48, 0x83, 0xec, 0x38, 0x48, 0x8b, 0x49, 0x08 };
  constexpr uint32_t render_rva = 0xfa3b0, status_vtable_rva = 0xfe508, endpoint_table_rva = 0x1d82e0;
  inline constexpr BYTE render_entry[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x55 };
  constexpr uint32_t endpoint_chars = 56, endpoint_count = 35, endpoint_stride = 57;

#pragma pack(push, 1)
  struct FileHeader {
    uint32_t magic, version, source, pid; // source: 1=system callback, 2=own-process test
    uint64_t created, qpc_frequency;
  };
  struct Record {
    uint64_t sequence, qpc, wrapper, implementation;
    uint32_t frames, capacity, produced;
    int32_t result;
    uint32_t tid, endpoint_slot; // v2: table index + 1, zero for unbound diagnostics
  };
#pragma pack(pop)
  static_assert(sizeof(FileHeader) == 32 && sizeof(Record) == 56);
  struct Slot {
    Record record;
    BYTE bytes[max_bytes];
  };
  struct alignas(64) Ring {
    uint32_t magic, version;
    volatile LONG status, stop, error, writer;
    alignas(64) volatile LONG64 write, read, heartbeat;
    alignas(64) volatile LONG64 calls, ignored, invalid, dropped, failed;
    uint64_t qpc_frequency;
    wchar_t endpoint[endpoint_chars]; // Empty only for unbound diagnostics.
    Slot slots[slot_count];
  };
  struct StartArgs {
    HANDLE mapping, collector;
    uint32_t seconds;
  };
  inline LONG64 read64(volatile LONG64 &value) { return InterlockedCompareExchange64(&value, 0, 0); }
} // namespace native_output
