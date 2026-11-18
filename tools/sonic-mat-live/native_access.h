#pragma once
#include "native_session.h"
#include <io.h>
#include <share.h>

inline void
enable_native_access() {
  HANDLE token = nullptr;
  require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token), "Open capture token");
  TOKEN_ELEVATION elevation {};
  DWORD bytes = 0;
  TOKEN_PRIVILEGES privilege {};
  privilege.PrivilegeCount = 1;
  privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  bool enabled = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) && elevation.TokenIsElevated &&
                 LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &privilege.Privileges[0].Luid);
  if (enabled) {
    SetLastError(ERROR_SUCCESS);
    enabled = AdjustTokenPrivileges(token, FALSE, &privilege, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
  }
  const auto error = GetLastError();
  CloseHandle(token);
  printf("NATIVE_ACCESS elevated=%u debugPrivilege=%u\n", elevation.TokenIsElevated, unsigned(enabled));
  SetLastError(error);
  require(enabled, "Administrator token with SeDebugPrivilege required; no injection attempted");
}

inline void
redirect_native_log(const fs::path &path) {
  if (path.empty()) return;
  require(!fs::exists(path), "Fresh capture log required");
  FILE *log = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
  require(log != nullptr, "Open capture log");
  const auto duplicated = _dup2(_fileno(log), _fileno(stdout));
  fclose(log);
  require(duplicated == 0, "Redirect capture output");
  require(_dup2(_fileno(stdout), _fileno(stderr)) == 0, "Redirect capture diagnostics");
  setvbuf(stdout, nullptr, _IONBF, 0);
}
