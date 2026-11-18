#pragma once
#include "../../third-party/moonlight-common-c/src/MatLiveCrypto.h"
#include "../../third-party/moonlight-common-c/src/MatLiveProfile.h"
#include "../spatial_audio/native_mat_pipe_source.h"
#include "mat_fragmenter.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <openssl/rand.h>
#include <string>
#include <thread>

namespace spatial_transport {
  struct LiveDiagnostics {
    const char *stage = "prepare";
    uint64_t cycles = 0, group = 0, samples = 0;
    uint64_t read_max_us = 0, late_max_us = 0, group_max_us = 0;
    uint64_t over_10ms = 0, over_60ms = 0;
  };
  struct LiveSession {
    std::shared_ptr<void> reservation;
    LI_MAT_LIVE_PROFILE profile {};
    std::unique_ptr<spatial_audio::EncodedAudioSource> source;
    std::atomic<bool> started { false };
    bool recovery_supported = false;
    unsigned recovery_selected = 0;
    uint32_t ssrc = 0;
    std::function<bool()> source_cancelled;
    LiveDiagnostics diagnostics;
    std::function<void(const LiveDiagnostics &)> report;
  };
  inline std::shared_ptr<LiveSession>
  prepare_live(const std::string &pipe, const std::string &producer, bool try_recovery = false,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max(),
    const std::function<bool()> &cancelled = {}) {
#if defined(_WIN32)
    if (pipe.empty() && producer.empty()) return {};
    spatial_audio::require(!pipe.empty() && (!producer.empty() || pipe == spatial_audio::mat_auto_pipe), "Live MAT source identity required");
    auto source = std::make_unique<spatial_audio::NativeMatPipeSource>();
    if (cancelled) source->cancel_when(cancelled);
    const auto wait = deadline == std::chrono::steady_clock::time_point::max() ? 0 :
      (std::max)(int64_t(0), std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    source->prepare(pipe, producer, try_recovery, static_cast<DWORD>(wait), deadline, false);
    source->cancel_when([] { return false; });
    auto result = std::make_shared<LiveSession>();
    result->recovery_supported = source->supports_restart();
    uint64_t epoch = 0;
    spatial_audio::require(RAND_bytes(reinterpret_cast<unsigned char *>(&epoch), sizeof(epoch)) == 1, "MAT live epoch entropy");
    result->profile.epoch = (epoch & INT64_MAX) | 1;
    result->profile.domain = 1;
    uint32_t ssrc = 0;
    spatial_audio::require(RAND_bytes(reinterpret_cast<unsigned char *>(&ssrc), sizeof(ssrc)) == 1, "MAT SSRC entropy");
    result->ssrc = (ssrc & 0x7fffffff) | 1;
    result->source = std::move(source);
    return result;
#else
    (void) pipe;
    (void) producer;
    (void) try_recovery;
    (void) deadline;
    (void) cancelled;
    return {};
#endif
  }
  inline std::string
  describe_live(const LiveSession &s) {
    return "a=x-ss-audio.mat-live:1\r\na=x-ss-audio.mat-live-carrier:" MAT_CARRIER
           "\r\na=x-ss-audio.mat-live-clock:48000\r\na=x-ss-audio.mat-live-crypto:" MAT_LIVE_CRYPTO
           "\r\na=x-ss-audio.mat-live-epoch:" +
           std::to_string(s.profile.epoch) +
           "\r\na=x-ss-audio.mat-live-domain:" + std::to_string(s.profile.domain) + "\r\n" +
           (s.recovery_supported ? "a=x-ss-audio.mat-live-recovery:1\r\na=x-ss-audio.mat-live-recovery-v2:1\r\n" : "");
  }
  inline bool
  accept_live(const LiveSession *s, const std::string &version, const std::string &carrier, const std::string &clock,
    const std::string &crypto, const std::string &epoch, const std::string &domain) {
    return s && version == "1" && MatLiveProfileConfirm(&s->profile, MAT_LIVE_NAME, carrier.c_str(), clock.c_str(), crypto.c_str(), epoch.c_str(), domain.c_str());
  }
  // Source lifetime is the stream lifetime. No file/count/SHA or source-sample catch-up.
  // Every packet is independently authenticated using a 64-bit counter before RTP/FEC.
  template <class Emit, class Wait, class Cancel>
  uint64_t
  live_session(LiveSession &session, const uint8_t *key, size_t key_size, Emit emit, Wait wait, Cancel cancelled) {
    using spatial_audio::require;
    using Clock = std::chrono::steady_clock;
    require(session.source && !session.started.exchange(true), "Live MAT source lease already used");
#if defined(_WIN32)
    if (auto pipe = dynamic_cast<spatial_audio::NativeMatPipeSource *>(session.source.get()))
      pipe->cancel_when(session.source_cancelled ? session.source_cancelled : std::function<bool()>(cancelled));
#endif
    struct Stop {
      spatial_audio::EncodedAudioSource &source;
      bool keep;
      ~Stop() { if (!keep) source.stop(); }
    } stop { *session.source, session.recovery_selected != 0 };
    const auto descriptor = session.source->descriptor();
    require(descriptor.version == 1 && descriptor.format == spatial_audio::Format::mat_iec61937 &&
              descriptor.clock == spatial_audio::Clock::carrier_48000 && descriptor.rate == 48000,
      "Live MAT source format/clock unsupported");
    const auto source_failed = [&](const char *message) {
      const auto detail = session.source->error();
      throw std::runtime_error(detail.empty() ? message : std::string(message) + ": " + detail);
    };
    if (!session.source->start()) source_failed("Live MAT source could not start");
    MAT_LIVE_CIPHER cipher {};
    require(MatLiveCipherInit(&cipher, key, key_size, session.profile.epoch, session.profile.domain), "Live MAT key");
    MAT_LIVE_CRYPTO_CONTEXT encryptor {};
    struct Wipe {
      MAT_LIVE_CIPHER &c;
      MAT_LIVE_CRYPTO_CONTEXT &context;
      ~Wipe() { MatLiveContextDestroy(&context); MatLiveWipe(&c, sizeof(c)); }
    } wipe { cipher, encryptor };
    require(MatLiveContextInit(&encryptor, &cipher, 1), "Live MAT encryption context");
    uint64_t packet = 0, cycles = 0;
    uint32_t hash = 2166136261u;
    const auto send = [&](const uint8_t *plain) {
      require(!cancelled(), "Live MAT cancelled");
      require(packet < uint64_t(INT64_MAX), "Live MAT packet counter overflow");
      uint8_t wire[MAT_LIVE_PACKET];
      require(MatLiveSealWithContext(&encryptor, packet++, plain, wire), "Live MAT packet encryption");
      emit(wire, sizeof(wire));
    };
    uint8_t control[MAT_WIRE_PACKET];
    mat_wire_make(control, MAT_WIRE_SYNC, session.profile.epoch, session.profile.domain, 0, 0, 0, nullptr);
    for (unsigned i = 0; i < 4; i++) send(control);
    auto last = Clock::now();
    auto next_report = last + std::chrono::seconds(5);
    auto &diagnostics = session.diagnostics;
    spatial_audio::Frame frame;
    while (!cancelled()) {
      diagnostics.stage = "source-read";
      const auto read_start = Clock::now();
      auto status = session.source->read(frame);
      diagnostics.read_max_us = (std::max)(diagnostics.read_max_us,
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - read_start).count()));
      if (status == spatial_audio::Read::ended) break;
      if (status == spatial_audio::Read::failed) source_failed("Native MAT source failed");
      if (status == spatial_audio::Read::pending) {
        require(Clock::now() - last < std::chrono::seconds(2), "Live MAT source stalled");
        wait(std::chrono::milliseconds(1));
        continue;
      }
      require(status == spatial_audio::Read::frame && frame.sequence == cycles && frame.sample_start == cycles * 960 &&
                frame.format == spatial_audio::Format::mat_iec61937 && frame.clock == spatial_audio::Clock::carrier_48000 &&
                frame.sample_rate == 48000 && frame.duration_frames == 960,
        "Live MAT source descriptor/sequence/clock");
      spatial_audio::validate_carrier(frame.bytes);
      require(cycles < uint64_t(INT64_MAX) / 20000000, "Live MAT cycle counter overflow");
      // Pace the 15 four-packet FEC data groups across the source cycle. Per-
      // fragment timers leave only 333us after the last fragment, so timer
      // overshoot plus encryption/hash/I/O can make every 20ms cycle run late
      // and gradually fill the live source queue. Group pacing matches the
      // sender's four-packet credit window and leaves the final group interval
      // for that work. Keep each cycle tied to source availability.
      const auto cycle_start = Clock::now();
      frame.epoch = session.profile.epoch;
      frame.domain = session.profile.domain;
      unsigned index = 0;
      auto group_start = cycle_start;
      fragment(frame, [&](const uint8_t *data, size_t) {
        diagnostics.stage = "pacing";
        const auto fragment_index = index++;
        const auto group_fragment = (fragment_index / 4) * 4;
        diagnostics.group = fragment_index / 4;
        auto target = cycle_start + std::chrono::microseconds(uint64_t(group_fragment) * 20000 / MAT_WIRE_FRAGMENTS);
        while (Clock::now() < target) {
          require(!cancelled(), "Live MAT cancelled");
          wait(target - Clock::now());
        }
        const auto now = Clock::now();
        const auto late_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - target).count());
        diagnostics.late_max_us = (std::max)(diagnostics.late_max_us, late_us);
        if (fragment_index % 4 == 0) {
          group_start = now;
          ++diagnostics.samples;
          diagnostics.over_10ms += late_us >= 10000;
          diagnostics.over_60ms += late_us >= 60000;
        }
        if (late_us >= 60000) throw std::runtime_error("Live MAT missed deadline; lateUs=" + std::to_string(late_us) +
          " cycle=" + std::to_string(cycles) + " group=" + std::to_string(diagnostics.group));
        diagnostics.stage = "encrypt-send";
        send(data);
        if (fragment_index % 4 == 3) diagnostics.group_max_us = (std::max)(diagnostics.group_max_us,
          uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - group_start).count()));
      });
      hash = mat_hash_update(hash, frame.bytes.data(), frame.bytes.size());
      ++cycles;
      diagnostics.cycles = cycles;
      last = Clock::now();
      if (last >= next_report) {
        if (session.report) session.report(diagnostics);
        diagnostics.samples = diagnostics.read_max_us = diagnostics.late_max_us = diagnostics.group_max_us = 0;
        diagnostics.over_10ms = diagnostics.over_60ms = 0;
        next_report = last + std::chrono::seconds(5);
      }
    }
    require(!cancelled() && cycles > 0, "Live MAT cancelled/empty; no successful EOS");
    mat_wire_make(control, MAT_WIRE_END, session.profile.epoch, session.profile.domain, cycles, 0, hash, nullptr);
    for (unsigned i = 0; i < 4; i++) send(control);
    return cycles;
  }
}  // namespace spatial_transport
