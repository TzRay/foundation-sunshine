#pragma once
#include "../../third-party/moonlight-common-c/src/MatAudioRecovery.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace spatial_transport {
  // The audio thread owns teardown/restart. The control thread only validates
  // messages and moves one reliable response into its existing send loop.
  class MatAudioRecoveryV1 {
    std::mutex mutex_;
    std::condition_variable changed_;
    uint64_t current_epoch_ = 0;
    bool used_ = false, ready_ = false, failed_ = false;
    MAT_AUDIO_RECOVERY active_ {};
    std::optional<MAT_AUDIO_RECOVERY> outbound_;

  public:
    std::atomic<bool> requested { false };
    std::atomic<bool> suspended { false };

    void initialize(uint64_t epoch) { current_epoch_ = epoch; }
    bool receive(const MAT_AUDIO_RECOVERY &m) {
      std::lock_guard lock(mutex_);
      if (failed_) return false;
      if (m.verb == MAT_RECOVERY_REQUEST) {
        if (!used_ && m.previousEpoch == current_epoch_) {
          requested.store(true);
        }
        else if (used_ && m.previousEpoch == active_.previousEpoch) {
          // Both ends may discover the same fault. Preserve the one profile.
          if (!ready_) outbound_ = active_;
        }
        else if (used_ && m.previousEpoch == current_epoch_) {
          failed_ = true;
          requested.store(true);
        }
        else return false;
      }
      else if (m.verb == MAT_RECOVERY_READY && used_ && MatRecoveryMatchesProfile(&active_, &m)) {
        if (ready_ && active_.clientPort != m.clientPort) return false;
        active_.clientPort = m.clientPort;
        ready_ = true;
        if (active_.verb == MAT_RECOVERY_START) outbound_ = active_;
      }
      else if (m.verb == MAT_RECOVERY_FAIL &&
               (m.previousEpoch == current_epoch_ || (used_ && m.previousEpoch == active_.previousEpoch))) {
        failed_ = true;
        requested.store(true);
      }
      else return false;
      changed_.notify_all();
      return true;
    }
    bool begin(const MAT_AUDIO_RECOVERY &profile) {
      std::lock_guard lock(mutex_);
      if (used_ || failed_) return false;
      used_ = true;
      ready_ = false;
      active_ = profile;
      active_.verb = MAT_RECOVERY_PREPARE;
      outbound_ = active_;
      requested.store(true);
      return true;
    }
    template<class Cancel>
    bool wait_ready(std::chrono::steady_clock::time_point deadline, Cancel cancelled) {
      using namespace std::chrono_literals;
      std::unique_lock lock(mutex_);
      while (!ready_ && !failed_ && !cancelled() && std::chrono::steady_clock::now() < deadline)
        changed_.wait_for(lock, 10ms);
      return ready_ && !failed_ && !cancelled();
    }
    bool start() {
      std::lock_guard lock(mutex_);
      if (!ready_ || failed_) return false;
      active_.verb = MAT_RECOVERY_START;
      outbound_ = active_;
      return true;
    }
    bool resumed() {
      std::lock_guard lock(mutex_);
      if (failed_) return false;
      current_epoch_ = active_.newEpoch;
      requested.store(false);
      return true;
    }
    void fail(uint32_t reason) {
      std::lock_guard lock(mutex_);
      failed_ = true;
      MAT_AUDIO_RECOVERY m = active_;
      m.verb = MAT_RECOVERY_FAIL;
      m.generation = 1;
      if (!m.previousEpoch) m.previousEpoch = current_epoch_;
      m.reason = reason;
      outbound_ = m;
      changed_.notify_all();
    }
    std::optional<MAT_AUDIO_RECOVERY> take_outbound() {
      std::lock_guard lock(mutex_);
      auto result = outbound_;
      outbound_.reset();
      return result;
    }
  };

  // v1 remains wire/behavior compatible. v2 is owned by the same audio thread;
  // control callbacks only validate a message and wake that owner.
  class MatAudioRecovery: public MatAudioRecoveryV1 {
    enum class Phase { running, preparing, starting, confirming, playing, failed, blocked };
    unsigned version_ = 1, attempts_ = 0;
    std::mutex mutex_;
    std::condition_variable changed_;
    Phase phase_ = Phase::running;
    uint64_t installed_epoch_ = 0, issued_epoch_ = 0;
    uint32_t issued_ssrc_ = 0, installed_generation_ = 0;
    MAT_AUDIO_RECOVERY active_ {};
    std::optional<MAT_AUDIO_RECOVERY> outbound_;
    std::chrono::steady_clock::time_point deadline_ {};

    void fail_locked(uint32_t reason) {
      if (phase_ == Phase::blocked) return;
      if (phase_ == Phase::failed && reason != MAT_RECOVERY_BLOCKED) return;
      phase_ = reason == MAT_RECOVERY_BLOCKED ? Phase::blocked : Phase::failed;
      requested.store(true);
      suspended.store(true);
      if (active_.generation) {
        auto m = active_;
        m.verb = MAT_RECOVERY_FAIL;
        m.reason = reason;
        outbound_ = m;
      }
      changed_.notify_all();
    }

  public:
    void initialize(uint64_t epoch, unsigned version = 1, uint32_t ssrc = 0) {
      version_ = version;
      if (version != 2) return MatAudioRecoveryV1::initialize(epoch);
      installed_epoch_ = issued_epoch_ = epoch;
      issued_ssrc_ = ssrc;
    }
    bool receive(const MAT_AUDIO_RECOVERY &m) {
      if (MatRecoveryVersion(&m) != version_) return false;
      if (version_ != 2) return MatAudioRecoveryV1::receive(m);
      std::lock_guard lock(mutex_);
      if (m.verb == MAT_RECOVERY_REQUEST) {
        // Reports identify installed media, not the last healthy media. A
        // report during preparation is merged into that existing attempt.
        if (m.generation != installed_generation_ || m.previousEpoch != installed_epoch_) return false;
        if (phase_ == Phase::blocked) return true;
        if (m.reason == MAT_RECOVERY_BLOCKED) fail_locked(m.reason);
        else if (phase_ == Phase::running || phase_ == Phase::playing || phase_ == Phase::confirming)
          fail_locked(MAT_RECOVERY_RETRYABLE);
        return true;
      }
      if (!active_.generation || !MatRecoveryMatchesProfile(&active_, &m)) return false;
      // Blocked can upgrade an already failed attempt; it cannot revive it.
      if (m.verb == MAT_RECOVERY_FAIL) {
        if (active_.clientPort && m.clientPort && active_.clientPort != m.clientPort) return false;
        fail_locked(m.reason);
        return true;
      }
      if (phase_ == Phase::failed || phase_ == Phase::blocked) return false;
      if (m.verb == MAT_RECOVERY_READY) {
        if (!m.clientPort || (active_.clientPort && active_.clientPort != m.clientPort)) return false;
        if (phase_ == Phase::preparing) active_.clientPort = m.clientPort;
        else if (phase_ == Phase::starting || phase_ == Phase::confirming || phase_ == Phase::playing) {
          auto reply = active_; reply.verb = MAT_RECOVERY_START; outbound_ = reply;
        }
        else return false;
      }
      else if (m.clientPort == active_.clientPort && m.verb == MAT_RECOVERY_PLAYING &&
               (phase_ == Phase::confirming || phase_ == Phase::playing)) {
        phase_ = Phase::playing;
      }
      else if (m.clientPort == active_.clientPort && m.verb == MAT_RECOVERY_STABLE && phase_ == Phase::playing) {
        attempts_ = 0;
      }
      else return false;
      changed_.notify_all();
      return true;
    }
    bool begin(MAT_AUDIO_RECOVERY &profile) {
      if (version_ != 2) return MatAudioRecoveryV1::begin(profile);
      std::lock_guard lock(mutex_);
      if (phase_ == Phase::blocked || phase_ == Phase::preparing || phase_ == Phase::starting ||
          active_.generation == UINT32_MAX || issued_epoch_ == INT64_MAX || issued_ssrc_ == UINT32_MAX) return false;
      profile.version = 2;
      profile.generation = active_.generation + 1;
      // Issued identities are never reused, including attempts that fail before
      // READY. previousEpoch names the previous issued profile, not health.
      profile.previousEpoch = issued_epoch_;
      profile.newEpoch = ++issued_epoch_;
      profile.ssrc = ++issued_ssrc_;
      profile.reason = MAT_RECOVERY_RETRYABLE;
      active_ = profile;
      active_.verb = MAT_RECOVERY_PREPARE;
      outbound_ = active_;
      phase_ = Phase::preparing;
      if (attempts_ < 5) ++attempts_;
      deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(MAT_RECOVERY_TIMEOUT_MS);
      requested.store(true);
      suspended.store(true);
      return true;
    }
    template<class Cancel>
    bool wait_ready(std::chrono::steady_clock::time_point deadline, Cancel cancelled) {
      if (version_ != 2) return MatAudioRecoveryV1::wait_ready(deadline, cancelled);
      std::unique_lock lock(mutex_);
      while (phase_ == Phase::preparing && !active_.clientPort && !cancelled() && std::chrono::steady_clock::now() < deadline)
        changed_.wait_for(lock, std::chrono::milliseconds(10));
      return phase_ == Phase::preparing && active_.clientPort && !cancelled();
    }
    bool start() {
      if (version_ != 2) return MatAudioRecoveryV1::start();
      std::lock_guard lock(mutex_);
      if (phase_ != Phase::preparing || !active_.clientPort) return false;
      phase_ = Phase::starting;
      auto m = active_; m.verb = MAT_RECOVERY_START; outbound_ = m;
      return true;
    }
    bool resumed() {
      if (version_ != 2) return MatAudioRecoveryV1::resumed();
      std::lock_guard lock(mutex_);
      if (phase_ != Phase::starting) return false;
      installed_epoch_ = active_.newEpoch;
      installed_generation_ = active_.generation;
      phase_ = Phase::confirming;
      requested.store(false);
      return true;
    }
    void poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
      if (version_ != 2) return;
      std::lock_guard lock(mutex_);
      if (phase_ == Phase::confirming && now >= deadline_)
        fail_locked(MAT_RECOVERY_RETRYABLE);
    }
    void fail(uint32_t reason) {
      if (version_ != 2) return MatAudioRecoveryV1::fail(reason);
      std::lock_guard lock(mutex_);
      fail_locked(reason);
    }
    bool blocked() {
      std::lock_guard lock(mutex_);
      return version_ == 2 && phase_ == Phase::blocked;
    }
    template<class Cancel>
    bool wait_retry(Cancel cancelled) {
      std::unique_lock lock(mutex_);
      auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(MatRecoveryCooldownMs(attempts_));
      while (!cancelled() && (phase_ == Phase::blocked || std::chrono::steady_clock::now() < until))
        changed_.wait_for(lock, std::chrono::milliseconds(10));
      return !cancelled() && phase_ != Phase::blocked;
    }
    std::optional<MAT_AUDIO_RECOVERY> take_outbound() {
      if (version_ != 2) return MatAudioRecoveryV1::take_outbound();
      std::lock_guard lock(mutex_);
      auto result = outbound_; outbound_.reset(); return result;
    }
  };
}
