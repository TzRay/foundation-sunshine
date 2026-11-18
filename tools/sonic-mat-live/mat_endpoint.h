#pragma once
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace sonic_live {
  inline bool is_mat_device_format(const WAVEFORMATEXTENSIBLE &format) {
    const auto &wave = format.Format;
    return wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE && wave.cbSize >= 22 &&
           wave.nChannels == 8 && wave.nSamplesPerSec == 192000 &&
           wave.wBitsPerSample == 16 && wave.nBlockAlign == 16 &&
           (format.SubFormat == KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP ||
            format.SubFormat == KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT20 ||
            format.SubFormat == KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT21);
  }

  inline bool configured_mat(IMMDevice *device) {
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return false;
    PROPVARIANT value {};
    bool result = false;
    // PKEY_AudioEngine_DeviceFormat (mmdeviceapi.h); not exported by uuid.lib.
    constexpr PROPERTYKEY deviceFormatKey { { 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } }, 0 };
    if (SUCCEEDED(properties->GetValue(deviceFormatKey, &value)) &&
        value.vt == VT_BLOB && value.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
      WAVEFORMATEXTENSIBLE format {};
      std::memcpy(&format, value.blob.pBlobData, sizeof(format));
      result = is_mat_device_format(format);
    }
    PropVariantClear(&value);
    return result;
  }

  // Query only: never open an exclusive stream or change spatial sound settings.
  inline bool supports_mat(IMMDevice *device) {
    Microsoft::WRL::ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return false;
    WAVEFORMATEXTENSIBLE_IEC61937 format {};
    auto &wave = format.FormatExt.Format;
    wave.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.nChannels = 8;
    wave.nSamplesPerSec = 192000;
    wave.nAvgBytesPerSec = 3072000;
    wave.nBlockAlign = 16;
    wave.wBitsPerSample = 16;
    wave.cbSize = sizeof(format) - sizeof(WAVEFORMATEX);
    format.FormatExt.Samples.wValidBitsPerSample = 16;
    format.FormatExt.dwChannelMask = KSAUDIO_SPEAKER_7POINT1;
    format.dwEncodedSamplesPerSec = 48000;
    format.dwEncodedChannelCount = 8;
    // MAT 1.0/TrueHD alone does not establish support for live PCM/object MAT.
    for (const auto &subtype : { KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT21,
                                KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT20 }) {
      format.FormatExt.SubFormat = subtype;
      if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wave, nullptr) == S_OK) return true;
    }
    return false;
  }

  inline std::wstring choose_mat_endpoint(std::vector<std::wstring> candidates,
                                         const std::wstring &current, const std::wstring &default_id) {
    if (candidates.empty()) return {};
    for (const auto &preferred : { current, default_id }) {
      if (std::find(candidates.begin(), candidates.end(), preferred) != candidates.end()) return preferred;
    }
    return *std::min_element(candidates.begin(), candidates.end());
  }
}
