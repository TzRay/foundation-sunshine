// Opt-in real-endpoint check. Plays one second of quiet per-channel test tones,
// then verifies the loopback clock continues for one second without that stream.
#include "bed_source.h"
#include <array>

int
wmain(int argc, wchar_t **argv) {
  try {
    if (argc != 3 || std::wstring_view(argv[1]) != L"--endpoint") {
      puts("bed-source-probe --endpoint ENDPOINT_ID|default (one second test tones, one second idle)");
      return 2;
    }
    using namespace sonic_live;
    using Microsoft::WRL::ComPtr;
    BedSource source(argv[2]);
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> endpoint;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> writer;
    bed_hr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "PROBE_ENUMERATOR");
    bed_hr(enumerator->GetDevice(source.endpoint().c_str(), &endpoint), "PROBE_ENDPOINT");
    bed_hr(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "PROBE_CLIENT");
    WAVEFORMATEX *format = nullptr;
    bed_hr(client->GetMixFormat(&format), "PROBE_FORMAT");
    struct FreeFormat {
      WAVEFORMATEX *value;
      ~FreeFormat() { CoTaskMemFree(value); }
    } free_format { format };
    bed_hr(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr), "PROBE_RENDER_OPEN");
    bed_hr(client->GetService(IID_PPV_ARGS(&writer)), "PROBE_RENDER_SERVICE");
    UINT32 capacity = 0;
    bed_hr(client->GetBufferSize(&capacity), "PROBE_RENDER_CAPACITY");
    uint64_t generated = 0;
    // Keep LFE inside its passband. A 770 Hz tone verifies loopback but is
    // correctly attenuated by the native encoder's LFE filter.
    const auto hz = [&](uint32_t lane) {
      return lane == 3 && (source.mask() & SPEAKER_LOW_FREQUENCY) ? 80. : 440. + 110. * lane;
    };
    const auto fill = [&] {
      UINT32 padding = 0;
      bed_hr(client->GetCurrentPadding(&padding), "PROBE_RENDER_PADDING");
      const auto frames = capacity - padding;
      if (!frames) return;
      BYTE *bytes = nullptr;
      bed_hr(writer->GetBuffer(frames, &bytes), "PROBE_RENDER_BUFFER");
      auto *pcm = reinterpret_cast<float *>(bytes);
      for (uint32_t frame = 0; frame < frames; ++frame)
        for (uint32_t lane = 0; lane < source.channels(); ++lane)
          pcm[size_t(frame) * source.channels() + lane] = float(.05 * std::sin(6.283185307179586 * hz(lane) * double(generated + frame) / 48000.));
      generated += frames;
      bed_hr(writer->ReleaseBuffer(frames, 0), "PROBE_RENDER_RELEASE");
    };
    fill();
    bed_hr(client->Start(), "PROBE_RENDER_START");
    const auto began = receiver::qpc(), frequency = receiver::hz();
    BedBlock block;
    std::array<double, 8> re {}, im {};
    uint64_t active_frames = 0, idle_blocks = 0, blocks = 0;
    bool stopped = false;
    while (receiver::qpc() - began < 2 * frequency) {
      const auto elapsed = receiver::qpc() - began;
      if (elapsed < frequency)
        fill();
      else if (!stopped) {
        bed_hr(client->Stop(), "PROBE_RENDER_STOP");
        stopped = true;
      }
      if (!source.next(block)) continue;
      if (elapsed < frequency) {
        for (uint32_t frame = 0; frame < 480; ++frame)
          for (uint32_t lane = 0; lane < source.channels(); ++lane) {
            const auto phase = 6.283185307179586 * hz(lane) * double(active_frames + frame) / 48000.;
            const auto value = block.pcm[size_t(frame) * source.channels() + lane];
            re[lane] += value * std::cos(phase);
            im[lane] += value * std::sin(phase);
          }
        active_frames += 480;
      }
      else if (elapsed > frequency * 12 / 10)
        ++idle_blocks;
      ++blocks;
    }
    source.stop();
    if (active_frames < 24000 || idle_blocks < 40) throw std::runtime_error("Loopback tone/idle coverage incomplete");
    for (uint32_t lane = 0; lane < source.channels(); ++lane) {
      const auto amplitude = 2 * std::hypot(re[lane], im[lane]) / double(active_frames);
      printf("BED_SOURCE_TONE lane=%u hz=%g amplitude=%g\n", lane, hz(lane), amplitude);
      if (amplitude < .002) throw std::runtime_error("Expected per-channel loopback test tone missing");
    }
    printf("BED_SOURCE_PROBE_PASS channels=%u mask=0x%x blocks=%llu idleBlocks=%llu clipped=%llu nativeEncoderTested=0\n",
      source.channels(), source.mask(), blocks, idle_blocks, source.clipped());
    return 0;
  }
  catch (const std::exception &e) {
    fprintf(stderr, "BED_SOURCE_PROBE_FAILED %s\n", e.what());
    return 1;
  }
}
