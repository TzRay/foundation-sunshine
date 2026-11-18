#include "capture_options.h"
#include "audio_session_options.h"
#include <iostream>
int
main() {
  try {
    using sonic_live::capture_limit;
    if (capture_limit(1, L"--continuous") != 0 || capture_limit(2, L"--max-seconds", L"1") != 1 || capture_limit(2, L"--max-seconds", L"60") != 60) return 1;
    for (auto text : { L"", L"0", L"61", L"-1", L"+8", L"8suffix", L" 8", L"18446744073709551616" }) {
      bool rejected = false;
      try {
        capture_limit(2, L"--max-seconds", text);
      }
      catch (...) {
        rejected = true;
      }
      if (!rejected) return 2;
    }
    for (int count : { 0, 2, 3 }) {
      bool rejected = false;
      try {
        capture_limit(count, L"--continuous", L"8");
      }
      catch (...) {
        rejected = true;
      }
      if (!rejected) return 3;
    }
    bool rejected = false;
    try {
      capture_limit(1, L"--max-seconds");
    }
    catch (...) {
      rejected = true;
    }
    if (!rejected) return 4;
    wchar_t executable[] = L"publisher", operation[] = L"--list-audio-sessions", option[] = L"--capture-endpoint", endpoint[] = L"endpoint-id";
    wchar_t *args[] = { executable, operation, option, endpoint };
    if (!sonic_live::audio_sessions::capture_endpoint(2, args).empty() ||
        sonic_live::audio_sessions::capture_endpoint(4, args) != endpoint) return 6;
    rejected = false;
    try { sonic_live::audio_sessions::capture_endpoint(3, args); } catch (...) { rejected = true; }
    if (!rejected || sonic_live::audio_sessions::quote("a\"\\\n\x01中文") != "\"a\\\"\\\\\\u000a\\u0001中文\"") return 7;
    std::cout << "PASS explicit continuous/bounded capture options, malformed bounds fail closed; nativeCapture=0\n";
    return 0;
  }
  catch (...) {
    return 5;
  }
}
