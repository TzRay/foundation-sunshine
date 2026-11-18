#define MAT_OUTPUT_SELFTEST
#include "native_output_tap.cpp"
#include <array>
#include <cstdio>
#include <fstream>
#include <stdexcept>

static void check(bool okay, const char *message) {
  if (!okay) throw std::runtime_error(message);
}

static uint32_t fake_bytes = 30720;
static HRESULT fake_result = S_OK;
static unsigned original_calls;
static bool throw_original;
struct FakeStatus {
  void *table;
  BYTE padding[0x38];
  uint32_t index, unused;
  wchar_t *names;
};
struct FakeRenderer { void *table, *wrapper, *reserved, *parent; FakeStatus *status; };
static_assert(offsetof(FakeStatus, index) == 0x40 && offsetof(FakeStatus, names) == 0x48 && offsetof(FakeRenderer, status) == 0x20);
static Process rendered_call;
static BYTE rendered_output[native_output::max_bytes];
static uint32_t rendered_bytes;
static FakeRenderer *nested_renderer;
static Render render_call;
static bool nested, throw_render;
static unsigned render_calls;

static __declspec(noinline) void __fastcall fake_render(void *renderer, uint32_t inputs, void **input, uint32_t outputs, void **output) {
  ++render_calls;
  if (throw_render) RaiseException(0xe0150002, 0, 0, nullptr);
  if (nested_renderer && !nested) {
    nested = true;
    render_call(nested_renderer, inputs, input, outputs, output);
    nested = false;
  }
  rendered_call(static_cast<FakeRenderer *>(renderer)->wrapper, 480, sizeof(rendered_output), rendered_output, &rendered_bytes);
}

static bool render_exception_propagates(void *renderer) {
  __try { render_hook(renderer, 1, nullptr, 1, nullptr); }
  __except (GetExceptionCode() == 0xe0150002 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
  return false;
}

static __declspec(noinline) HRESULT __fastcall fake_process(void *, uint32_t, uint32_t capacity, BYTE *output, uint32_t *produced) {
  ++original_calls;
  if (throw_original) RaiseException(0xe0150001, 0, 0, nullptr);
  if (FAILED(fake_result)) return fake_result;
  if (produced) *produced = fake_bytes;
  if (output && fake_bytes <= capacity) {
    for (uint32_t i = 0; i < fake_bytes; ++i) output[i] = static_cast<BYTE>((i * 17 + 23) & 255);
    if (fake_bytes >= 8) {
      const BYTE preamble[] = { 0x72, 0xf8, 0x1f, 0x4e, 0x16, 0, 2, 0 };
      memcpy(output, preamble, sizeof(preamble));
    }
  }
  return fake_result;
}

static bool exception_propagates(void *wrapper) {
  __try { hook(wrapper, 480, 0, nullptr, nullptr); }
  __except (GetExceptionCode() == 0xe0150001 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
  return false;
}

int main(int argc, char **argv) {
  auto *ring = static_cast<native_output::Ring *>(VirtualAlloc(nullptr, sizeof(native_output::Ring), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!ring) return 1;
  try {
    void *table[8] {};
    table[7] = reinterpret_cast<void *>(fake_process);
    void **implementation = table;
    struct Wrapper { void *table; void *implementation; } wrapper { nullptr, &implementation };
    dolby_process = table[7];
    original = fake_process;
    current = ring;
    ring->magic = native_output::magic;
    ring->version = native_output::version;
    std::array<BYTE, native_output::max_bytes> output {};
    uint32_t produced = 0;
    auto run = [&] { return hook(&wrapper, 480, static_cast<uint32_t>(output.size()), output.data(), &produced); };
    check(run() == S_OK && original_calls == 1 && produced == fake_bytes && ring->write == 1, "copy successful call");
    check(ring->slots[0].record.wrapper == reinterpret_cast<uint64_t>(&wrapper) &&
          ring->slots[0].record.implementation == reinterpret_cast<uint64_t>(&implementation) &&
          !memcmp(ring->slots[0].bytes, output.data(), fake_bytes), "copy byte-for-byte and bind implementation");
    output.fill(0);
    check(ring->slots[0].bytes[0] == 0x72, "capture owns bytes before source reuse");
    fake_result = E_FAIL;
    check(hook(&wrapper, 480, 0, reinterpret_cast<BYTE *>(1), reinterpret_cast<uint32_t *>(1)) == E_FAIL && ring->failed == 1 && ring->write == 1 && !ring->writer, "preserve failed HRESULT without reading failed output");
    fake_result = S_FALSE;
    fake_bytes = 0;
    check(run() == S_FALSE && ring->write == 2 && ring->slots[1].record.produced == 0, "preserve successful empty callback");
    fake_result = S_OK;
    fake_bytes = native_output::max_bytes + 1;
    check(run() == S_OK && ring->invalid == 1 && ring->write == 2 && !ring->writer, "reject oversized copy without changing original result");
    fake_bytes = 30720;
    table[7] = nullptr;
    check(run() == S_OK && ring->ignored == 1 && ring->write == 2, "ignore other encoder implementations");
    table[7] = dolby_process;
    ring->writer = 1;
    check(run() == S_OK && ring->dropped == 1 && ring->writer == 1, "busy writer never blocks or releases another writer");
    ring->writer = 0;
    ring->write = native_output::slot_count;
    check(run() == S_OK && ring->dropped == 2 && !ring->writer, "full ring never overwrites unread output");
    ring->write = 2;
    ring->stop = 1;
    check(run() == S_OK && ring->write == 2, "stop preserves original callback without admission");
    ring->stop = 0;
    throw_original = true;
    check(exception_propagates(&wrapper) && active == 0 && ring->invalid == 2, "original exception propagates and callback releases");
    throw_original = false;

    wchar_t names[native_output::endpoint_count][native_output::endpoint_stride] {};
    wcscpy_s(names[2], L"{0.0.0.00000000}.{00000000-0000-0000-0000-000000000001}");
    wcscpy_s(names[3], L"{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}");
    endpoint_table = names[0];
    status_vtable = &names;
    FakeStatus status { status_vtable, {}, 2, 0, endpoint_table };
    FakeRenderer renderer { nullptr, &wrapper, nullptr, reinterpret_cast<void *>(1), &status };
    FakeStatus other_status = status;
    other_status.index = 3;
    FakeRenderer other = renderer;
    other.status = &other_status;
    wcscpy_s(ring->endpoint, names[2]);
    original_render = fake_render;
    rendered_call = hook;
    render_call = render_hook;
    ring->write = ring->read = 0;
    render_hook(&renderer, 1, nullptr, 1, nullptr);
    check(ring->write == 1 && ring->slots[0].record.endpoint_slot == 3 && !render_context && !active, "bind renderer status +0x20 to endpoint and wrapper");
    render_hook(&other, 1, nullptr, 1, nullptr);
    run();
    check(ring->write == 1, "ignore other endpoint and calls outside the bound renderer");
    other_status.index = native_output::endpoint_count;
    check(!endpoint_context(&other, ring->endpoint).wrapper, "reject out of bounds endpoint table index");
    other_status.index = 2;
    other_status.table = nullptr;
    check(!endpoint_context(&other, ring->endpoint).wrapper && !endpoint_context(reinterpret_cast<void *>(1), ring->endpoint).wrapper, "reject wrong status identity and unreadable renderer");
    other_status.table = status_vtable;
    other_status.index = 3;
    nested_renderer = &other;
    render_hook(&renderer, 1, nullptr, 1, nullptr);
    check(ring->write == 2 && !render_context && !active, "nested different endpoint cannot inherit outer admission");
    nested_renderer = nullptr;
    throw_render = true;
    check(render_exception_propagates(&renderer) && !render_context && !active, "renderer exception restores thread context and admission");
    throw_render = false;

    // Exercise a real detour and restoration inside this test process only.
    current = nullptr;
    BYTE before[16];
    memcpy(before, reinterpret_cast<void *>(fake_process), sizeof(before));
    check(MH_Initialize() == MH_OK && MH_CreateHook(reinterpret_cast<void *>(fake_process), reinterpret_cast<void *>(hook), reinterpret_cast<void **>(&original)) == MH_OK, "create own-process hook");
    check(MH_EnableHook(reinterpret_cast<void *>(fake_process)) == MH_OK, "enable own-process hook");
    BYTE render_before[16];
    memcpy(render_before, reinterpret_cast<void *>(fake_render), sizeof(render_before));
    check(MH_CreateHook(reinterpret_cast<void *>(fake_render), reinterpret_cast<void *>(render_hook), reinterpret_cast<void **>(&original_render)) == MH_OK &&
          MH_EnableHook(reinterpret_cast<void *>(fake_render)) == MH_OK, "enable own-process renderer hook");
    memset(ring, 0, sizeof(*ring));
    current = ring;
    fake_bytes = 61440;
    Process volatile call = fake_process;
    check(call(&wrapper, 960, static_cast<uint32_t>(output.size()), output.data(), &produced) == S_OK && ring->write == 1, "actual detour copies full cycle");
    wcscpy_s(ring->endpoint, names[2]);
    rendered_call = fake_process;
    render_call = fake_render;
    nested_renderer = &other;
    Render volatile call_renderer = fake_render;
    call_renderer(&renderer, 1, nullptr, 1, nullptr);
    check(ring->write == 2 && ring->slots[1].record.endpoint_slot == 3 && !memcmp(ring->slots[1].bytes, rendered_output, fake_bytes), "actual two detours preserve bound output and reject nested other endpoint");
    nested_renderer = nullptr;
    check(MH_DisableHook(reinterpret_cast<void *>(fake_process)) == MH_OK, "disable own-process hook");
    check(MH_DisableHook(reinterpret_cast<void *>(fake_render)) == MH_OK && !memcmp(render_before, reinterpret_cast<void *>(fake_render), sizeof(render_before)), "renderer entry restored");
    current = nullptr;
    check(active == 0 && !memcmp(before, reinterpret_cast<void *>(fake_process), sizeof(before)), "original entry restored");
    check(call(&wrapper, 960, static_cast<uint32_t>(output.size()), output.data(), &produced) == S_OK && ring->write == 2, "source runs after hook removal");
    check(MH_Uninitialize() == MH_OK, "release own-process hooks");
    if (argc == 2) {
      LARGE_INTEGER frequency {};
      QueryPerformanceFrequency(&frequency);
      native_output::FileHeader header { native_output::magic, native_output::version, 2, GetCurrentProcessId(), 0, static_cast<uint64_t>(frequency.QuadPart) };
      std::ofstream fixture(argv[1], std::ios::binary);
      fixture.write(reinterpret_cast<const char *>(&header), sizeof(header));
      fixture.write(reinterpret_cast<const char *>(&ring->slots[0].record), sizeof(native_output::Record));
      fixture.write(reinterpret_cast<const char *>(ring->slots[0].bytes), ring->slots[0].record.produced);
      fixture.close();
      check(fixture.good(), "write explicitly marked own-process fixture");
    }
    VirtualFree(ring, 0, MEM_RELEASE);
    puts("PASS native-output: byte preservation, HRESULT, empty/failure/oversize, endpoint/wrapper identity, nesting, busy/full, stop, exceptions, real two-hook detour and restore; system capture untested");
    return 0;
  }
  catch (const std::exception &e) {
    printf("FAILED %s\n", e.what());
    return 1;
  }
}
