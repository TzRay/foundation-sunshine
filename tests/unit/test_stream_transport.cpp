#ifdef _WIN32
  #include "src/platform/windows/send_batch_policy.h"
#endif
#include "src/platform/send_batch.h"
#include "src/streaming/transport.h"
#include "src/streaming/audio_budget.h"
#include "src/streaming/bitrate.h"
#include "src/nvenc/rate_control.h"
#include "src/thread_safe.h"

#include <gtest/gtest.h>
#include <atomic>
#include <limits>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

TEST(StreamQueue, OverflowIsObservedWithTheRetainedItem) {
  safe::queue_t<int> queue;
  for (int value = 1; value <= 33; ++value) queue.raise(value);
  std::uint64_t dropped = 0;
  EXPECT_EQ(*queue.pop(&dropped), 33);
  EXPECT_EQ(dropped, 32);
  EXPECT_FALSE(queue.peek());
  queue.raise(34);
  EXPECT_EQ(*queue.pop(&dropped), 34);
  EXPECT_EQ(dropped, 32);
}

TEST(StreamQueue, ConcurrentReadsAndShutdownAreSynchronized) {
  safe::event_t<bool> event;
  safe::queue_t<int> queue;
  std::atomic<bool> finished { false };
  std::thread writer([&] {
    for (int i = 0; i < 10000; ++i) {
      event.raise(true);
      queue.raise(i);
    }
    event.stop();
    queue.stop();
    finished = true;
  });
  do {
    (void) event.peek();
    (void) event.running();
    (void) queue.peek();
    (void) queue.running();
  } while (!finished);
  writer.join();
  EXPECT_FALSE(event.peek());
  EXPECT_FALSE(queue.running());
  event.reset();
  EXPECT_TRUE(event.running());
  EXPECT_FALSE(event.peek());
}

TEST(StreamRecovery, OverflowDropsDependentFramesUntilAnIdr) {
  streaming::encoded_queue_recovery_t recovery;
  EXPECT_FALSE(recovery.observe(0, true).discard);
  EXPECT_FALSE(recovery.observe(0, false).discard);
  auto result = recovery.observe(32, false);
  EXPECT_TRUE(result.discard);
  EXPECT_TRUE(result.request_idr);
  EXPECT_TRUE(result.overflow);
  EXPECT_FALSE(recovery.observe(32, false).request_idr);
  EXPECT_FALSE(recovery.observe(32, true).discard);
  EXPECT_FALSE(recovery.observe(32, false).discard);
}

TEST(StreamRecovery, LostRecoveryIdrCanBeRequestedAgain) {
  streaming::encoded_queue_recovery_t recovery;
  EXPECT_TRUE(recovery.observe(0, false).request_idr);
  EXPECT_TRUE(recovery.observe(32, false).request_idr);
  EXPECT_FALSE(recovery.observe(64, true).discard);
  EXPECT_FALSE(recovery.observe(64, false).request_idr);
}

TEST(StreamRecovery, SharedQueueOverflowRecoversEachSessionIndependently) {
  streaming::encoded_queue_recovery_t first, second;
  first.observe(0, true);
  second.observe(0, true);
  EXPECT_FALSE(first.observe(32, true).discard);
  EXPECT_TRUE(second.observe(32, false).request_idr);
  EXPECT_FALSE(first.observe(32, false).discard);
  EXPECT_TRUE(second.observe(32, false).discard);
}

TEST(StreamTransport, FallbackNeverReplaysAnAcceptedPrefix) {
  std::vector<std::size_t> indices;
  const auto accepted = platf::send_datagram_suffix(4, { 2, platf::batch_send_status_e::fallback }, [&](auto i) {
    indices.push_back(i);
    return i == 3;  // One individual send fails.
  });
  EXPECT_EQ(indices, (std::vector<std::size_t> { 2, 3 }));
  EXPECT_EQ(accepted, 3);
}

TEST(StreamTransport, CongestionAndUnknownAcceptanceDoNotReplayTheBatch) {
  for (const auto status : { platf::batch_send_status_e::would_block, platf::batch_send_status_e::failed, platf::batch_send_status_e::accepted }) {
    EXPECT_EQ(platf::send_datagram_suffix(4, { 2, status }, [](auto) {
      ADD_FAILURE() << "Unexpected fallback";
      return true;
    }), 2);
  }
}

TEST(StreamTransport, BatchBytesIncludeEveryEncryptionPrefix) {
  EXPECT_EQ(streaming::video_batch_size(1408, 0, false), 46);
  EXPECT_EQ(streaming::video_batch_size(1376, 32, false), 46);
  for (const auto size : { 64U, 1024U, 1376U, 1408U, 9000U }) {
    const auto count = streaming::video_batch_size(size, 32, false);
    EXPECT_LE(count * (size + 32), 65536);
    EXPECT_LE(count, 64);
    EXPECT_LE(streaming::video_batch_size(size, 32, true), 16);
  }
  EXPECT_EQ(streaming::video_batch_size(std::numeric_limits<std::size_t>::max(), 32, false), 1);
}

TEST(StreamPacing, RequiresAnExactVersionedOptIn) {
  EXPECT_TRUE(streaming::smooth_pacing_requested("smooth-v1"));
  for (const auto value : { "", "1", "smooth-v2", "smooth-v1x" }) EXPECT_FALSE(streaming::smooth_pacing_requested(value));
}

TEST(StreamPacing, LateWakeDoesNotAccumulateCatchupBursts) {
  streaming::video_batch_pacer_t pacer;
  const auto start = streaming::video_batch_pacer_t::clock::now();
  EXPECT_EQ(pacer.deadline(start), start);
  pacer.sent(start, 16000);
  EXPECT_EQ(pacer.deadline(start), start + 160us);
  const auto late = start + 10ms;
  EXPECT_EQ(pacer.deadline(late), late);
  pacer.sent(late, 16000);
  EXPECT_EQ(pacer.deadline(late), late + 160us);
}

TEST(AudioBudget, AccountsForPcmAndFourPlusTwoAudioFec) {
  for (const auto channels : { 2, 6, 8, 12 }) {
    const auto pcm = streaming::pcm_s16_bitrate(48000, channels);
    EXPECT_EQ(pcm, channels * 768000);
    EXPECT_EQ(streaming::audio_with_fec_kbps(pcm), channels * 1152);
  }
  EXPECT_EQ(streaming::encoded_audio_bitrate(6, 0, false), 448000);
  EXPECT_EQ(streaming::encoded_audio_bitrate(6, 0, true), 384000);
  EXPECT_EQ(streaming::encoded_audio_bitrate(2, 2000000, false), 640000);
}

TEST(AudioBudget, FixedAudioReserveIsNotCappedToTwentyPercent) {
  streaming::bitrate_budget_t budget(20000, 0, 9216, 500, 20, false);
  EXPECT_EQ(budget.applied().encoder_kbps, (20000 - 9216 - 500) * 100 / 120);
  EXPECT_EQ(budget.request(9000), 20000);  // Cannot fit the negotiated PCM audio.
  EXPECT_EQ(budget.request(30000), 30000);
}

TEST(NvencRateControl, PreservesPeakRatioAndVbvDurationAcrossRateChanges) {
  EXPECT_EQ(nvenc::rescale_rate_control(30000000, 20000000, 40000000), 60000000);
  EXPECT_EQ(nvenc::rescale_rate_control(333333, 20000000, 40000000), 666666);
  EXPECT_EQ(nvenc::rescale_rate_control(166666, 20000000, 40000000), 333332);
  EXPECT_EQ(nvenc::rescale_rate_control(0, 20000000, 40000000), 0);
  EXPECT_EQ(nvenc::rescale_rate_control(123, 0, 40000000), 123);
  EXPECT_EQ(nvenc::rescale_rate_control(UINT32_MAX, 1, 800000000), UINT32_MAX);
}

#ifdef _WIN32
TEST(WindowsSendBatch, DistinguishesRejectedCallsFromCongestionAndUnknownFailure) {
  EXPECT_EQ(platf::classify_batch_send_failure(WSAEINVAL).status, platf::batch_send_status_e::fallback);
  EXPECT_EQ(platf::classify_batch_send_failure(WSAEMSGSIZE).status, platf::batch_send_status_e::fallback);
  EXPECT_EQ(platf::classify_batch_send_failure(WSAEWOULDBLOCK).status, platf::batch_send_status_e::would_block);
  EXPECT_EQ(platf::classify_batch_send_failure(WSAENOBUFS).status, platf::batch_send_status_e::failed);
  EXPECT_EQ(platf::classify_batch_send_failure(WSAECONNRESET).error, WSAECONNRESET);
}
#endif
