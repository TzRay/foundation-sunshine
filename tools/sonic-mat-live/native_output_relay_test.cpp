#include "native_output_relay.h"
#include <cstdio>

static void check(bool okay, const char *why) { require(okay, why); }
template<class F> static void rejects(F action, const char *why) {
  bool failed = false;
  try { action(); } catch (const std::runtime_error &) { failed = true; }
  check(failed, why);
}
static void publish_burst() {
  Shared pipe;
  pipe.ready = pipe.playing = true;
  NativeOutputRelay relay;
  std::vector<Cycle> expected(12), received;
  for (size_t n = 0; n < expected.size(); ++n) {
    for (size_t i = 0; i < expected[n].bytes.size(); ++i) expected[n].bytes[i] = BYTE(i * 17 + n);
  }
  LARGE_INTEGER hz {};
  QueryPerformanceFrequency(&hz);
  pipe.max_cycle_age_ticks = uint64_t(hz.QuadPart) * Shared::max_native_age_ms / 1000;
  const auto publish = [&](size_t n) {
    expected[n].qpc = Qpc();
    relay.publish(pipe, expected[n]);
  };
  // The fifth cycle must be retained without a reader wait, even before any
  // consumer exists. The consumer then resumes after a short scheduling pause.
  for (size_t n = 0; n < 5; ++n) publish(n);
  check(!pipe.lease_failed && pipe.queue.size() == 5, "five-cycle burst must preserve the lease");
  std::thread consumer([&] {
    Sleep(20);
    std::unique_lock lock(pipe.mutex);
    while (true) {
      pipe.changed.wait(lock, [&] { return pipe.ended || !pipe.queue.empty(); });
      if (pipe.queue.empty()) break;
      received.push_back(pipe.queue.front());
      pipe.queue.pop_front();
      pipe.changed.notify_all();
    }
  });
  std::exception_ptr error;
  try {
    {
      std::unique_lock lock(pipe.mutex);
      check(pipe.changed.wait_for(lock, std::chrono::seconds(2), [&] { return received.size() >= 5; }), "delayed consumer must resume");
    }
    for (size_t n = 5; n < expected.size(); ++n) publish(n);
    check(!pipe.lease_failed, "resumed consumer must retain the same lease");
  } catch (...) { error = std::current_exception(); }
  {
    std::lock_guard lock(pipe.mutex);
    pipe.ended = true;
    pipe.changed.notify_all();
  }
  consumer.join();
  if (error) std::rethrow_exception(error);
  check(received.size() == expected.size(), "burst must deliver every cycle");
  for (size_t n = 0; n < expected.size(); ++n)
    check(received[n].qpc == expected[n].qpc && received[n].bytes == expected[n].bytes, "burst must retain bytes, order and native timestamps");
}
static void publish_blocked_or_closed() {
  NativeOutputRelay relay;
  Cycle cycle {};
  cycle.qpc = Qpc();
  Shared blocked;
  blocked.ready = blocked.playing = true;
  blocked.queue.resize(Shared::max_lease_cycles);
  const auto began = GetTickCount64();
  relay.publish(blocked, cycle);
  const auto waited = GetTickCount64() - began;
  check(waited < 50 && blocked.lease_failed && !blocked.playing && !blocked.failed && blocked.ready &&
    blocked.queue.size() == Shared::max_lease_cycles, "overload must invalidate only the lease without blocking or dropping queued audio");
  ++cycle.qpc;
  relay.publish(blocked, cycle);
  check(blocked.cycles == 2 && !blocked.failed, "native source must continue after delivery overload");

  Shared closed;
  closed.ready = true;
  relay.publish(closed, cycle);
  check(closed.queue.empty(), "a closed lease must not receive the waiting cycle");
}
static void publish_age_budget() {
  LARGE_INTEGER hz {};
  QueryPerformanceFrequency(&hz);
  const auto limit = uint64_t(hz.QuadPart) * Shared::max_native_age_ms / 1000;
  NativeOutputRelay relay;
  for (const bool queued : {false, true}) {
    Shared pipe;
    pipe.ready = pipe.playing = true;
    pipe.max_cycle_age_ticks = limit;
    Cycle cycle {};
    cycle.qpc = Qpc() - limit - hz.QuadPart / 1000;
    if (queued) {
      pipe.queue.push_back(cycle);
      cycle.qpc = Qpc();
    }
    relay.publish(pipe, cycle);
    check(pipe.lease_failed && !pipe.failed && pipe.ready && pipe.queue.size() == size_t(queued),
      "old incoming/queued cycles must expire the lease before the capacity limit");
  }
}
int main() {
  try {
    sonic_live::MatRetry retry;
    for (const auto expected : {1000, 2000, 4000, 5000, 5000})
      check(retry.next_delay() == expected, "retry backoff and cap");
    retry.reset();
    check(retry.next_delay() == 1000, "healthy source resets backoff");
    check(sonic_live::mat_exit_code(true, true, true, true, true, 1) == sonic_live::mat_retry_exit, "clean transient source can retry");
    for (unsigned mask = 0; mask < 15; ++mask)
      check(sonic_live::mat_exit_code(mask & 1, true, mask & 2, mask & 4, mask & 8, 1) == 1, "unsafe or unclassified exit must not retry");
    check(sonic_live::mat_exit_code(true, false, true, false, true, 1) == sonic_live::mat_retry_exit,
      "known device invalidation before helper startup needs no hook restoration");
    check(sonic_live::mat_exit_code(false, false, true, false, true, 1) == 1 &&
      sonic_live::mat_exit_code(true, false, true, false, false, 1) == 1 &&
      sonic_live::mat_exit_code(true, true, true, false, true, 1) == 1,
      "unknown startup failure, failed route restoration, and unclean started helper remain nonretryable");
    publish_burst();
    publish_blocked_or_closed();
    publish_age_budget();
    std::array<BYTE, spatial_audio::carrier_bytes> bytes;
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = BYTE(i * 17 + 23);
    const BYTE preamble[] { 0x72, 0xf8, 0x1f, 0x4e, 0x16, 0, 0x62, 0xed };
    memcpy(bytes.data(), preamble, sizeof(preamble));
    std::span<const BYTE> first(bytes.data(), spatial_audio::process_bytes), second(bytes.data() + spatial_audio::process_bytes, spatial_audio::process_bytes);
    native_output::Record record { 0, 100, 123, 456, 480, 30720, 30720, S_OK, 1, 3 };
    auto advance = [&] { ++record.sequence; record.qpc += 10; };
    NativeOutputRelay exact;
    check(!exact.accept(record, first), "first half must remain pending");
    advance();
    const auto cycle = exact.accept(record, second);
    check(cycle && cycle->bytes == bytes && cycle->qpc == 110 && exact.partial_bytes() == 0 && exact.skipped_bytes() == 0, "exact bytes and native completion clock");
    advance();
    rejects([&] { exact.accept(record, second); }, "must not resync after damage");
    NativeOutputRelay partial;
    record.sequence = 0;
    check(!partial.accept(record, second) && partial.skipped_bytes() == 30720, "omit only initial partial half");
    advance();
    check(!partial.accept(record, first), "aligned first half");
    advance();
    check(partial.accept(record, second)->bytes == bytes, "partial start keeps first complete cycle intact");
    advance();
    ++record.wrapper;
    rejects([&] { partial.accept(record, first); }, "never concatenate different native renderers");
    NativeOutputRelay mismatch;
    record.sequence = 1;
    rejects([&] { mismatch.accept(record, first); }, "reject lost callback");
    NativeOutputRelay unbound;
    record.sequence = 0;
    record.endpoint_slot = 0;
    rejects([&] { unbound.accept(record, first); }, "unbound diagnostics cannot become live source");
    puts("PASS native relay: delayed-consumer burst with 12 exact cycles, capacity/age limits, startup retry classification, exact bytes/clock, alignment, renderer/sequence and endpoint binding");
    return 0;
  } catch (const std::exception &e) { printf("FAILED %s\n", e.what()); return 1; }
}
