#define NOMINMAX
#include <windows.h>
#include "mat_endpoint.h"
#include <iostream>

int main() {
  using sonic_live::choose_mat_endpoint;
  if (!choose_mat_endpoint({}, L"old", L"realtek").empty()) return 1;
  if (choose_mat_endpoint({L"new"}, L"old", L"realtek") != L"new") return 2;
  if (choose_mat_endpoint({L"a", L"b"}, L"b", L"a") != L"b") return 3;
  if (choose_mat_endpoint({L"a", L"b"}, L"removed", L"b") != L"b") return 4;
  if (choose_mat_endpoint({L"b", L"a"}, L"removed", L"realtek") != L"a") return 5;
  WAVEFORMATEXTENSIBLE format {};
  format.Format = { WAVE_FORMAT_EXTENSIBLE, 8, 192000, 3072000, 16, 16, 22 };
  for (const auto &subtype : { KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP,
                              KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT20,
                              KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT21 }) {
    format.SubFormat = subtype;
    if (!sonic_live::is_mat_device_format(format)) return 6;
  }
  format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
  if (sonic_live::is_mat_device_format(format)) return 7;
  format.SubFormat = KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP;
  format.Format.nChannels = 2;
  if (sonic_live::is_mat_device_format(format)) return 8;
  std::cout << "MAT endpoint selection passed\n";
}
