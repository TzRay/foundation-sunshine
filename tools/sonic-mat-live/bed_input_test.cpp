#include "bed_input.h"
#include "bed_packet_clock.h"
#include <cstdio>
#include <limits>
#include <vector>

namespace {
  void
  check(bool value) {
    if (!value) throw std::runtime_error("Bed input assertion failed");
  }
  template <class F>
  void
  rejects(F action) {
    bool rejected = false;
    try {
      action();
    }
    catch (const std::runtime_error &) {
      rejected = true;
    }
    check(rejected);
  }
}  // namespace
int
main() {
  try {
    using namespace sonic_live;
    // Start-to-start monotonicity is insufficient after a reopen: the next
    // block can start inside the previous 480-frame interval and replay PCM.
    BedReopenAlignment alignment;
    check(alignment.accept(1000, 1479));
    alignment.reopened();
    check(!alignment.accept(900, 1379));
    check(!alignment.accept(1200, 1679));
    check(!alignment.accept(1479, 1958));
    check(alignment.accept(1480, 1959));
    check(alignment.accept(1960, 2439));
    alignment.reopened();
    check(!alignment.accept(2300, 2779));
    check(alignment.accept(2780, 3259));
    // The first WASAPI packet may establish a discontinuous baseline. A later
    // gap must end this epoch; invalid/reversed clocks must remain hard errors.
    check_bed_packet_clock(false, AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY, 0, 100, 0, 0);
    check_bed_packet_clock(true, AUDCLNT_BUFFERFLAGS_SILENT, 1920, 200, 0, 100);
    bool restart = false;
    try {
      check_bed_packet_clock(true, AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY, 513414720, 266123466460,
                            513413760, 266123266460);
    }
    catch (const BedSourceRestart &) { restart = true; }
    check(restart);
    for (const auto flags : { DWORD(0), DWORD(AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR), DWORD(AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) }) {
      bool hard_error = false;
      try { check_bed_packet_clock(true, flags, 10, 99, 20, 100); }
      catch (const BedSourceRestart &) { throw std::runtime_error("Invalid clock incorrectly allowed a source restart"); }
      catch (const std::runtime_error &) { hard_error = true; }
      check(hard_error);
    }
    rejects([] { check_bed_packet_clock(false, 0, 0, 0, 0, 0); });
    struct Layout {
      uint32_t mask;
      std::array<int, 12> input;
    };
    // Independent WAVE interleaving oracle, indexed FL/FR/FC/LFE/SL/SR/BL/BR/height.
    constexpr Layout layouts[] {
      { 0x3, { 0, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 } },
      { 0x60f, { 0, 1, 2, 3, 4, 5, -1, -1, -1, -1, -1, -1 } },
      { 0x3f, { 0, 1, 2, 3, -1, -1, 4, 5, -1, -1, -1, -1 } },
      { 0x63f, { 0, 1, 2, 3, 6, 7, 4, 5, -1, -1, -1, -1 } },
      { 0x2d63f, { 0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11 } }
    };
    for (const auto &layout : layouts) {
      const auto mask = layout.mask;
      for (auto mode : { BedSubmission::full, BedSubmission::subset }) {
        BedInput bed(mask, mode);
        std::vector<float> pcm(size_t(bed.channels()) * receiver::frames);
        for (uint32_t frame = 0; frame < receiver::frames; ++frame)
          for (uint32_t lane = 0; lane < bed.channels(); ++lane)
            pcm[size_t(frame) * bed.channels() + lane] = float(lane + 1) / 32.f + float(frame) / 48000.f;
        for (uint32_t block = 0; block < 2; ++block) {
          auto packet = bed.data(pcm, 100 + block, 101 + block);
          check(packet->header.index == block && packet->header.kind == 1 && packet->header.frames == 480);
          check(packet->header.calls == bed.submitted_channels());
          uint32_t seen = 0;
          for (uint32_t n = 0; n < packet->header.calls; ++n) {
            const auto &call = packet->calls[n];
            const auto channel = call.id - 0x100;
            check(channel < 12 && call.type == (1u << (channel + 1)) && call.frames == 480);
            check(call.flags == (block ? 2u : 3u));
            check(call.contentType == 0);
            check(call.xyz[0] == 0 && call.xyz[1] == 0 && call.xyz[2] == 0);
            check(!(seen & call.type));
            seen |= call.type;
            const auto lane = layout.input[channel];
            for (uint32_t frame = 0; frame < receiver::frames; ++frame)
              check(call.pcm[frame] == (lane >= 0 ? pcm[size_t(frame) * bed.channels() + lane] : 0.f));
          }
          check(mode != BedSubmission::full || seen == receiver::bed714Mask);
        }
        auto end = bed.finish(500);
        check(end->header.kind == 2 && end->header.index == 2 && end->header.calls == bed.submitted_channels());
        for (uint32_t n = 0; n < end->header.calls; ++n) {
          check(end->calls[n].flags == 8);
          check(end->calls[n].contentType == 0);
          for (float sample : end->calls[n].pcm) check(sample == 0);
        }
        rejects([&] { bed.data(pcm, 501, 502); });
      }
    }
    rejects([] { BedInput invalid(0xff, BedSubmission::full); });
    BedInput bed(3, BedSubmission::full);
    rejects([&] { bed.finish(1); });
    std::vector<float> pcm(960);
    rejects([&] { bed.data(std::span(pcm).first(959), 1, 2); });
    pcm[0] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { bed.data(pcm, 1, 2); });
    pcm[0] = 1.01f;
    rejects([&] { bed.data(pcm, 1, 2); });
    pcm[0] = 0;
    auto first = bed.data(pcm, 1, 2);
    check(first->header.index == 0 && first->calls[0].flags == 3);
    puts("PASS bed masks, interleaved mapping, silent full bed, lifecycle, and invalid PCM; nativeEncoderTested=0");
    return 0;
  }
  catch (const std::exception &e) {
    fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
