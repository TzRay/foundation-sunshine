#pragma once
#include "../../src/spatial_audio/object_input_protocol.h"
#include <array>
#include <bit>
#include <cmath>
#include <memory>
#include <mmreg.h>
#include <span>
#include <stdexcept>

namespace sonic_live {
  struct BedChannel {
    uint32_t wave, type;
    const char *name;
  };
  // WAVEFORMATEXTENSIBLE and AudioObjectType have different bit assignments.
  inline constexpr std::array<BedChannel, 12> bed_channels { { { SPEAKER_FRONT_LEFT, 0x2, "FL" }, { SPEAKER_FRONT_RIGHT, 0x4, "FR" },
    { SPEAKER_FRONT_CENTER, 0x8, "FC" }, { SPEAKER_LOW_FREQUENCY, 0x10, "LFE" },
    { SPEAKER_SIDE_LEFT, 0x20, "SL" }, { SPEAKER_SIDE_RIGHT, 0x40, "SR" },
    { SPEAKER_BACK_LEFT, 0x80, "BL" }, { SPEAKER_BACK_RIGHT, 0x100, "BR" },
    { SPEAKER_TOP_FRONT_LEFT, 0x200, "TFL" }, { SPEAKER_TOP_FRONT_RIGHT, 0x400, "TFR" },
    { SPEAKER_TOP_BACK_LEFT, 0x800, "TBL" }, { SPEAKER_TOP_BACK_RIGHT, 0x1000, "TBR" } } };
  inline constexpr uint32_t bed_wave_mask = 0x2d63f;
  static_assert([] {
    uint32_t wave = 0, types = 0;
    for (const auto &channel : bed_channels) {
      if ((wave & channel.wave) || (types & channel.type)) return false;
      wave |= channel.wave;
      types |= channel.type;
    }
    return wave == bed_wave_mask && types == receiver::bed714Mask;
  }());
  enum class BedSubmission { full,
    subset };

  // Fixed-layout PCM, 480 interleaved float frames per call. Full mode submits
  // the entire declared 7.1.4 bed, with absent input channels explicitly silent.
  // Subset mode separately tests the native encoder's sparse-input behavior.
  class BedInput {
    uint32_t mask_, index_ = 0;
    BedSubmission submission_;
    bool ended_ = false;
    std::unique_ptr<receiver::Slot>
    packet(bool end, int64_t first, int64_t last) {
      if (ended_ || index_ >= UINT32_MAX - 100 || first <= 0 || last < first)
        throw std::runtime_error("Invalid/ended bed source sequence or clock");
      auto result = std::make_unique<receiver::Slot>();
      auto &header = result->header;
      header.index = index_;
      header.frames = receiver::frames;
      header.kind = end ? 2 : 1;
      header.firstQpc = first;
      header.lastQpc = last;
      for (size_t n = 0; n < bed_channels.size(); ++n) {
        const auto &channel = bed_channels[n];
        if (submission_ == BedSubmission::subset && !(mask_ & channel.wave)) continue;
        auto &call = result->calls[header.calls++];
        call.id = 0x100 + static_cast<uint32_t>(n);
        call.type = channel.type;
        call.frames = receiver::frames;
        call.flags = end ? 8 : (index_ ? receiver::updateFlags(call.type) : receiver::createFlags(call.type));
      }
      return result;
    }

  public:
    BedInput(uint32_t wave_mask, BedSubmission submission):
        mask_(wave_mask), submission_(submission) {
      if (!mask_ || (mask_ & ~bed_wave_mask)) throw std::runtime_error("Unsupported bed channel mask");
    }
    uint32_t
    channels() const { return std::popcount(mask_); }
    uint32_t
    submitted_channels() const { return submission_ == BedSubmission::full ? 12 : channels(); }
    std::unique_ptr<receiver::Slot>
    data(std::span<const float> pcm, int64_t first, int64_t last) {
      if (pcm.size() != size_t(channels()) * receiver::frames)
        throw std::runtime_error("Bed input must contain 480 complete interleaved frames");
      for (float value : pcm)
        if (!std::isfinite(value) || std::abs(value) > 1)
          throw std::runtime_error("Bed PCM must be finite and normalized");
      auto result = packet(false, first, last);
      for (uint32_t n = 0; n < result->header.calls; ++n) {
        auto &call = result->calls[n];
        const auto wave = bed_channels[size_t(call.id - 0x100)].wave;
        if (!(mask_ & wave)) continue;
        const auto input = std::popcount(mask_ & (wave - 1));
        for (uint32_t frame = 0; frame < receiver::frames; ++frame)
          call.pcm[frame] = pcm[size_t(frame) * channels() + input];
      }
      ++index_;
      return result;
    }
    std::unique_ptr<receiver::Slot>
    finish(int64_t qpc) {
      if (!index_) throw std::runtime_error("Cannot end an empty bed source");
      auto result = packet(true, qpc, qpc);
      ended_ = true;
      return result;
    }
  };
}  // namespace sonic_live
