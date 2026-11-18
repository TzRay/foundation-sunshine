#pragma once
#include "bed_input.h"
#include "bed_packet_clock.h"
#include <algorithm>
#include <audioclient.h>
#include <cstdio>
#include <deque>
#include <mmdeviceapi.h>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace sonic_live {
  inline void
  bed_hr(HRESULT hr, const char *operation) {
    if (FAILED(hr)) {
      fprintf(stderr, "BED_WASAPI operation=%s HRESULT=0x%08x\n", operation, unsigned(hr));
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) throw BedDeviceInvalidated(operation);
      throw std::runtime_error(operation);
    }
  }
  struct BedBlock {
    std::vector<float> pcm;
    int64_t firstQpc = 0, lastQpc = 0;
    uint64_t sourceFrame = 0;
  };
  class BedSource {
    template <class T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct Apartment {
      Apartment() { bed_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "BED_COM_INIT"); }
      ~Apartment() { CoUninitialize(); }
    } apartment_;
    struct Event {
      HANDLE value = nullptr;
      ~Event() {
        if (value) CloseHandle(value);
      }
    } event_;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> endpoint_;
    ComPtr<IAudioClient> capture_, keepalive_;
    ComPtr<IAudioCaptureClient> reader_;
    ComPtr<IAudioRenderClient> silence_;
    std::wstring endpoint_id_;
    uint32_t channels_ = 0, mask_ = 0, render_capacity_ = 0;
    bool capture_started_ = false, render_started_ = false, seen_packet_ = false;
    uint64_t last_device_ = 0, last_timestamp_ = 0, source_frames_ = 0;
    uint64_t queued_frames_ = 0, clipped_ = 0, packets_ = 0, silent_frames_ = 0;
    struct Packet {
      std::vector<float> pcm;
      uint64_t device;
      int64_t qpc;
      uint32_t offset = 0, frames;
    };
    std::deque<Packet> pending_;

    static int64_t
    qpc_ticks(uint64_t time100ns) {
      const auto frequency = receiver::hz();
      return int64_t(time100ns / 10000000) * frequency + int64_t(time100ns % 10000000) * frequency / 10000000;
    }
    void
    keep_silent_render_active() {
      UINT32 padding = 0;
      bed_hr(keepalive_->GetCurrentPadding(&padding), "BED_RENDER_PADDING");
      if (padding > render_capacity_) throw std::runtime_error("BED_RENDER_BOUNDS");
      const auto available = render_capacity_ - padding;
      if (!available) return;
      BYTE *bytes = nullptr;
      bed_hr(silence_->GetBuffer(available, &bytes), "BED_RENDER_BUFFER");
      bed_hr(silence_->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT), "BED_RENDER_RELEASE");
    }
    void
    collect() {
      UINT32 frames = 0;
      for (;;) {
        bed_hr(reader_->GetNextPacketSize(&frames), "BED_NEXT_PACKET");
        if (!frames) break;
        BYTE *bytes = nullptr;
        DWORD flags = 0;
        UINT64 position = 0, timestamp = 0;
        bed_hr(reader_->GetBuffer(&bytes, &frames, &flags, &position, &timestamp), "BED_CAPTURE_BUFFER");
        struct Release {
          IAudioCaptureClient *reader;
          uint32_t frames;
          ~Release() {
            if (reader) reader->ReleaseBuffer(frames);
          }
        } release { reader_.Get(), frames };
        if (packets_ < 3 || (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY))
          printf("BED_PACKET index=%llu position=%llu previousPosition=%llu frames=%u flags=0x%x qpc100ns=%llu previousQpc100ns=%llu\n",
            packets_, position, last_device_, frames, flags, timestamp, last_timestamp_);
        // Some spatial endpoints report a raw device-position stride different
        // from the 48k PCM packet size. Do not guess a rescaling from nChannels
        // or GetFrequency (which describes IAudioClock::GetPosition units).
        // Use the capture API's discontinuity flag, ordered timestamps and the
        // actual returned PCM frame count; retain raw positions for diagnosis.
        check_bed_packet_clock(seen_packet_, flags, position, timestamp, last_device_, last_timestamp_);
        if (queued_frames_ + frames > 9600) throw std::runtime_error("BED_CAPTURE_BACKPRESSURE");
        Packet packet;
        packet.frames = frames;
        packet.device = position;
        packet.qpc = qpc_ticks(timestamp);
        packet.pcm.resize(size_t(frames) * channels_);
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        if (!silent) {
          if (!bytes) throw std::runtime_error("BED_NULL_PCM");
          const auto *input = reinterpret_cast<const float *>(bytes);
          for (size_t n = 0; n < packet.pcm.size(); ++n) {
            const float value = input[n];
            if (!std::isfinite(value)) throw std::runtime_error("BED_NONFINITE_PCM");
            if (std::abs(value) > 1) ++clipped_;
            packet.pcm[n] = std::clamp(value, -1.f, 1.f);
          }
        }
        else
          silent_frames_ += frames;
        bed_hr(reader_->ReleaseBuffer(frames), "BED_CAPTURE_RELEASE");
        release.reader = nullptr;
        last_device_ = position;
        last_timestamp_ = timestamp;
        seen_packet_ = true;
        queued_frames_ += frames;
        ++packets_;
        pending_.push_back(std::move(packet));
      }
    }

  public:
    explicit BedSource(const std::wstring &id, bool start = true) {
      bed_hr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_)), "BED_ENUMERATOR");
      if (id == L"default")
        bed_hr(enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint_), "BED_DEFAULT_ENDPOINT");
      else
        bed_hr(enumerator_->GetDevice(id.c_str(), &endpoint_), "BED_ENDPOINT_MISSING");
      LPWSTR identity = nullptr;
      bed_hr(endpoint_->GetId(&identity), "BED_ENDPOINT_ID");
      endpoint_id_ = identity;
      CoTaskMemFree(identity);
      bed_hr(endpoint_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &capture_), "BED_CAPTURE_ACTIVATE");
      WAVEFORMATEX *format = nullptr;
      bed_hr(capture_->GetMixFormat(&format), "BED_MIX_FORMAT");
      struct FreeFormat {
        WAVEFORMATEX *value;
        ~FreeFormat() { CoTaskMemFree(value); }
      } free_format { format };
      if (format->nSamplesPerSec != 48000) throw std::runtime_error("BED_FORMAT_NOT_48K");
      if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE || format->cbSize < 22 ||
          reinterpret_cast<WAVEFORMATEXTENSIBLE *>(format)->SubFormat != KSDATAFORMAT_SUBTYPE_IEEE_FLOAT || format->wBitsPerSample != 32)
        throw std::runtime_error("BED_FORMAT_NOT_FLOAT32");
      channels_ = format->nChannels;
      mask_ = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(format)->dwChannelMask;
      if (!((channels_ == 2 && mask_ == 0x3) || (channels_ == 6 && (mask_ == 0x3f || mask_ == 0x60f)) ||
            (channels_ == 8 && mask_ == 0x63f)) ||
          format->nBlockAlign != channels_ * sizeof(float))
        throw std::runtime_error("BED_CHANNELS_UNSUPPORTED");
      printf("BED_SOURCE endpoint=%ls channels=%u rate=48000 mask=0x%x captureStarted=%u\n",
        endpoint_id_.c_str(), channels_, mask_, unsigned(start));
      if (!start) return;
      event_.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!event_.value) throw std::runtime_error("BED_CAPTURE_EVENT");
      bed_hr(capture_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
               1000000, 0, format, nullptr),
        "BED_LOOPBACK_OPEN");
      bed_hr(capture_->SetEventHandle(event_.value), "BED_CAPTURE_SET_EVENT");
      bed_hr(capture_->GetService(IID_PPV_ARGS(&reader_)), "BED_CAPTURE_SERVICE");
      ComPtr<IAudioClock> capture_clock;
      UINT64 clock_frequency = 0;
      bed_hr(capture_->GetService(IID_PPV_ARGS(&capture_clock)), "BED_CAPTURE_CLOCK");
      bed_hr(capture_clock->GetFrequency(&clock_frequency), "BED_CAPTURE_CLOCK_RATE");
      printf("BED_CLOCK frequency=%llu mixRate=48000\n", clock_frequency);
      // A silent render stream keeps the endpoint's capture clock running even
      // when no application is playing. Gaps are never inferred to be silence.
      bed_hr(endpoint_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &keepalive_), "BED_RENDER_ACTIVATE");
      bed_hr(keepalive_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr), "BED_RENDER_OPEN");
      bed_hr(keepalive_->GetBufferSize(&render_capacity_), "BED_RENDER_CAPACITY");
      bed_hr(keepalive_->GetService(IID_PPV_ARGS(&silence_)), "BED_RENDER_SERVICE");
      keep_silent_render_active();
      bed_hr(keepalive_->Start(), "BED_RENDER_START");
      render_started_ = true;
      bed_hr(capture_->Start(), "BED_CAPTURE_START");
      capture_started_ = true;
    }
    ~BedSource() { stop(); }
    void
    stop() {
      if (capture_started_) {
        capture_->Stop();
        capture_started_ = false;
      }
      if (render_started_) {
        keepalive_->Stop();
        render_started_ = false;
      }
    }
    uint32_t
    channels() const { return channels_; }
    uint32_t
    mask() const { return mask_; }
    const std::wstring &
    endpoint() const { return endpoint_id_; }
    uint64_t
    clipped() const { return clipped_; }
    uint64_t
    packets() const { return packets_; }
    uint64_t
    silent_frames() const { return silent_frames_; }
    uint64_t
    buffered_frames() const { return queued_frames_; }
    bool
    next(BedBlock &block) {
      if (!capture_started_) throw std::runtime_error("BED_SOURCE_NOT_RUNNING");
      keep_silent_render_active();
      collect();
      if (queued_frames_ < receiver::frames) {
        if (WaitForSingleObject(event_.value, 20) == WAIT_FAILED) throw std::runtime_error("BED_CAPTURE_WAIT");
        keep_silent_render_active();
        collect();
      }
      if (queued_frames_ < receiver::frames) return false;
      block.pcm.resize(size_t(receiver::frames) * channels_);
      block.sourceFrame = source_frames_;
      block.firstQpc = pending_.front().qpc + int64_t(pending_.front().offset) * receiver::hz() / 48000;
      uint32_t written = 0;
      while (written < receiver::frames) {
        auto &packet = pending_.front();
        const auto take = std::min(receiver::frames - written, packet.frames - packet.offset);
        std::copy_n(packet.pcm.data() + size_t(packet.offset) * channels_, size_t(take) * channels_,
          block.pcm.data() + size_t(written) * channels_);
        packet.offset += take;
        block.lastQpc = packet.qpc + int64_t(packet.offset - 1) * receiver::hz() / 48000;
        written += take;
        queued_frames_ -= take;
        if (packet.offset == packet.frames) pending_.pop_front();
      }
      source_frames_ += receiver::frames;
      return true;
    }
  };
}  // namespace sonic_live
