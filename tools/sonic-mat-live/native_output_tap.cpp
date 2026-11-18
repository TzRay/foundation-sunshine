#define NOMINMAX
#include "native_output_protocol.h"
#include "../../src/spatial_audio/native_mat_guard.h"
#include <MinHook.h>
#include <cstring>

namespace {
  using Process = HRESULT(__fastcall *)(void *, uint32_t, uint32_t, BYTE *, uint32_t *);
  using Render = void(__fastcall *)(void *, uint32_t, void **, uint32_t, void **);
  Process original;
  Render original_render;
  void *target;
  void *render_target;
  void *dolby_process;
  void *status_vtable;
  wchar_t *endpoint_table;
  BYTE saved[16], render_saved[16];
  volatile LONG active, running;
  void *volatile current;
  bool created, render_created;
  struct RenderContext { void *wrapper; uint32_t endpoint_slot; };
  thread_local RenderContext *render_context;

  native_output::Ring *ring_now() {
    return static_cast<native_output::Ring *>(InterlockedCompareExchangePointer(&current, nullptr, nullptr));
  }

  // Pinned audioeng Initialize stores CSpatialStatusEntry at renderer + 0x20.
  // +0x18 is its parent renderer, not the status entry. No private method call.
  RenderContext endpoint_context(void *renderer, const wchar_t *wanted) {
    __try {
      auto *p = static_cast<BYTE *>(renderer);
      auto *status = *reinterpret_cast<BYTE **>(p + 0x20);
      if (!status || *reinterpret_cast<void **>(status) != status_vtable ||
          *reinterpret_cast<wchar_t **>(status + 0x48) != endpoint_table) return {};
      const auto index = *reinterpret_cast<uint32_t *>(status + 0x40);
      if (index >= native_output::endpoint_count) return {};
      const auto *name = endpoint_table + index * native_output::endpoint_stride;
      for (uint32_t i = 0; i < native_output::endpoint_chars; ++i) {
        auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? wchar_t(c + (L'a' - L'A')) : c; };
        if (lower(name[i]) != lower(wanted[i])) return {};
        if (!wanted[i]) return { *reinterpret_cast<void **>(p + 8), index + 1 };
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return {};
  }

  void __fastcall render_hook(void *renderer, uint32_t inputs, void **input, uint32_t outputs, void **output) {
    InterlockedIncrement(&active);
    auto *ring = ring_now();
    RenderContext context {};
    if (ring && !ring->stop && ring->endpoint[0]) context = endpoint_context(renderer, ring->endpoint);
    auto *previous = render_context;
    render_context = &context;
    __try {
      original_render(renderer, inputs, input, outputs, output);
    }
    __finally {
      render_context = previous;
      InterlockedDecrement(&active);
    }
  }

  // Only observe the known Dolby implementation. Headphone/PCM renderers and
  // the separate Sunshine worker must not be mistaken for the Windows source.
  uint64_t identity(void *wrapper) {
    __try {
      auto *implementation = *reinterpret_cast<void **>(static_cast<BYTE *>(wrapper) + 8);
      auto **table = *static_cast<void ***>(implementation);
      if (table[7] == dolby_process) return reinterpret_cast<uint64_t>(implementation);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
  }

  void snapshot(native_output::Ring *ring, void *wrapper, uint64_t implementation,
    uint32_t frames, uint32_t capacity, BYTE *output, uint32_t *produced, HRESULT result, uint32_t endpoint_slot) {
    // Never wait, allocate, log, or perform file I/O on the audio callback.
    __try {
      if (FAILED(result)) {
        InterlockedIncrement64(&ring->failed);
        return; // A failed call does not promise a valid output buffer.
      }
      if (!produced || *produced > capacity || *produced > native_output::max_bytes || (*produced && !output)) {
        InterlockedIncrement64(&ring->invalid);
        return;
      }
      const auto w = native_output::read64(ring->write), r = native_output::read64(ring->read);
      if (w - r >= native_output::slot_count) {
        InterlockedIncrement64(&ring->dropped);
        return;
      }
      auto &slot = ring->slots[w % native_output::slot_count];
      LARGE_INTEGER qpc {};
      QueryPerformanceCounter(&qpc);
      slot.record = { static_cast<uint64_t>(w), static_cast<uint64_t>(qpc.QuadPart),
        reinterpret_cast<uint64_t>(wrapper), implementation, frames, capacity, *produced,
        result, GetCurrentThreadId(), endpoint_slot };
      if (*produced) memcpy(slot.bytes, output, *produced);
      InterlockedExchange64(&ring->write, w + 1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
      InterlockedIncrement64(&ring->invalid);
    }
  }

  HRESULT __fastcall hook(void *wrapper, uint32_t frames, uint32_t capacity, BYTE *output, uint32_t *produced) {
    InterlockedIncrement(&active);
    auto *ring = ring_now();
    uint64_t implementation = 0;
    uint32_t endpoint_slot = 0;
    if (ring && !ring->stop) {
      const bool bound = render_context && render_context->wrapper == wrapper && render_context->endpoint_slot;
      if (!ring->endpoint[0] || bound) {
        implementation = identity(wrapper);
        if (bound) endpoint_slot = render_context->endpoint_slot;
      }
      InterlockedIncrement64(implementation ? &ring->calls : &ring->ignored);
    }
    HRESULT result = E_UNEXPECTED;
    __try {
      result = original(wrapper, frames, capacity, output, produced);
      if (implementation) {
        if (InterlockedCompareExchange(&ring->writer, 1, 0)) InterlockedIncrement64(&ring->dropped);
        else {
          __try {
            snapshot(ring, wrapper, implementation, frames, capacity, output, produced, result, endpoint_slot);
          }
          __finally {
            InterlockedExchange(&ring->writer, 0);
          }
        }
      }
    }
    __finally {
      if (AbnormalTermination() && implementation) InterlockedIncrement64(&ring->invalid);
      InterlockedDecrement(&active);
    }
    return result;
  }

  DWORD verify_source() {
    if (!native_mat_guard::system_engine(GetCurrentProcess())) return ERROR_BAD_ENVIRONMENT;
    auto *base = reinterpret_cast<BYTE *>(GetModuleHandleW(L"audioeng.dll"));
    auto encoder = GetModuleHandleW(L"DolbyMatEnc.dll");
    auto engine = GetModuleHandleW(L"DolbyMatEngine.dll");
    if (!native_mat_guard::loaded_audioeng(reinterpret_cast<HMODULE>(base)) || !encoder || !engine) return ERROR_MOD_NOT_FOUND;
    wchar_t encoder_path[32768] {}, engine_path[32768] {};
    if (!GetModuleFileNameW(encoder, encoder_path, 32768) || !GetModuleFileNameW(engine, engine_path, 32768)) return ERROR_MOD_NOT_FOUND;
    const auto directory = std::filesystem::path(encoder_path).parent_path();
    if (directory != std::filesystem::path(engine_path).parent_path() ||
        !mat_compat::dolby_directory_shape(directory.wstring() + L"\\") ||
        !mat_compat::supported_dolby(native_mat_guard::sha256(encoder_path), native_mat_guard::sha256(engine_path))) return ERROR_REVISION_MISMATCH;
    if (memcmp(base + native_output::process_rva, native_output::process_entry, sizeof(native_output::process_entry)) ||
        memcmp(base + native_output::render_rva, native_output::render_entry, sizeof(native_output::render_entry))) return ERROR_REVISION_MISMATCH;
    target = base + native_output::process_rva;
    render_target = base + native_output::render_rva;
    status_vtable = base + native_output::status_vtable_rva;
    endpoint_table = reinterpret_cast<wchar_t *>(base + native_output::endpoint_table_rva);
    dolby_process = reinterpret_cast<BYTE *>(encoder) + native_output::dolby_process_rva;
    return 0;
  }
}

extern "C" __declspec(dllexport) DWORD WINAPI StartNativeOutput(void *input) {
  const auto args = *static_cast<native_output::StartArgs *>(input);
  if (InterlockedCompareExchange(&running, 1, 0)) {
    CloseHandle(args.mapping);
    CloseHandle(args.collector);
    return ERROR_BUSY;
  }
  auto *ring = static_cast<native_output::Ring *>(MapViewOfFile(args.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(native_output::Ring)));
  CloseHandle(args.mapping);
  if (!ring) {
    CloseHandle(args.collector);
    InterlockedExchange(&running, 0);
    return ERROR_INVALID_HANDLE;
  }
  DWORD error = 0;
  if (ring->magic != native_output::magic || ring->version != native_output::version || !args.collector || args.seconds > 60 ||
      (!args.seconds && !ring->endpoint[0]) ||
      ring->endpoint[native_output::endpoint_chars - 1]) error = ERROR_INVALID_PARAMETER;
  if (!error) {
    try { error = verify_source(); }
    catch (...) { error = ERROR_BAD_ENVIRONMENT; }
  }
  if (!error && !created) {
    HMODULE pinned {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&StartNativeOutput), &pinned)) error = GetLastError();
    auto status = error ? MH_ERROR_NOT_INITIALIZED : MH_Initialize();
    if (!error && status != MH_OK) error = 0x5000 + status;
    if (!error) {
      memcpy(saved, target, sizeof(saved));
      status = MH_CreateHook(target, reinterpret_cast<void *>(hook), reinterpret_cast<void **>(&original));
      if (status != MH_OK) error = 0x5100 + status;
      else created = true;
    }
  }
  if (!error && ring->endpoint[0] && !render_created) {
    memcpy(render_saved, render_target, sizeof(render_saved));
    auto status = MH_CreateHook(render_target, reinterpret_cast<void *>(render_hook), reinterpret_cast<void **>(&original_render));
    if (status != MH_OK) error = 0x5400 + status;
    else render_created = true;
  }
  if (!error) {
    InterlockedExchangePointer(&current, ring);
    auto status = MH_QueueEnableHook(target);
    if (status == MH_OK && ring->endpoint[0]) status = MH_QueueEnableHook(render_target);
    if (status == MH_OK) status = MH_ApplyQueued();
    if (status != MH_OK) error = 0x5200 + status;
    if (!error) {
      InterlockedExchange(&ring->status, 1);
      const auto deadline = GetTickCount64() + args.seconds * 1000;
      while (!ring->stop && (!args.seconds || GetTickCount64() < deadline) && WaitForSingleObject(args.collector, 20) == WAIT_TIMEOUT &&
             static_cast<LONG64>(GetTickCount64()) - native_output::read64(ring->heartbeat) < 5000) {}
    }
  }
  InterlockedExchange(&ring->stop, 1);
  if (created) {
    auto status = MH_DisableHook(target);
    if (status != MH_OK && status != MH_ERROR_DISABLED && !error) error = 0x5300 + status;
  }
  if (render_created) {
    auto status = MH_DisableHook(render_target);
    if (status != MH_OK && status != MH_ERROR_DISABLED && !error) error = 0x5500 + status;
  }
  InterlockedExchangePointer(&current, nullptr);
  while (InterlockedCompareExchange(&active, 0, 0)) Sleep(1);
  if (created && memcmp(target, saved, sizeof(saved))) error = ERROR_WRITE_FAULT;
  if (render_created && memcmp(render_target, render_saved, sizeof(render_saved))) error = ERROR_WRITE_FAULT;
  CloseHandle(args.collector);
  ring->error = error;
  InterlockedExchange(&ring->status, error ? -1 : 2);
  UnmapViewOfFile(ring);
  InterlockedExchange(&running, 0);
  return error;
}
