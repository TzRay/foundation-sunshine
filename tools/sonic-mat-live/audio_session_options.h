#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

namespace sonic_live::audio_sessions {
  inline std::wstring capture_endpoint(int argc, wchar_t **argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--list-audio-sessions") return {};
    if (argc == 4 && std::wstring_view(argv[1]) == L"--list-audio-sessions" &&
        std::wstring_view(argv[2]) == L"--capture-endpoint" && *argv[3]) return argv[3];
    throw std::invalid_argument("--list-audio-sessions [--capture-endpoint ENDPOINT_ID]");
  }

  inline std::string quote(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : text) {
      if (c == '\"' || c == '\\') { result += '\\'; result += char(c); }
      else if (c < 0x20) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
      else result += char(c);
    }
    return result + '\"';
  }
}
