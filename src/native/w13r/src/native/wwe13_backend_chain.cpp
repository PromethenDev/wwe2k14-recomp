// WWE '13 PC: the graphics API chain when the native renderer can't create its device (from WWE '12
// PC, native/wwe12_vulkan_error.cpp).
//
// With gpu_backend = "any" (AUTO, the default) the next build is tried: wwe13_native.exe (Vulkan)
// starts wwe13_d3d12.exe, which starts wwe13_d3d11.exe, with the same arguments. Only when the last
// one fails too (or is missing, or the player chose an API) does the player get the message.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwchar>
#include <filesystem>
#include <string>

#include <rex/cvar.h>

bool Wwe13TryNextBackend() {
  // gpu_backend is the SDK's cvar (rex_app.cpp): "any", "vulkan", "d3d12" or "d3d11".
  const std::string backend = rex::cvar::GetFlagByName("gpu_backend");
  if (!backend.empty() && backend != "any")
    return false;
  wchar_t self[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, self, MAX_PATH))
    return false;
  const std::filesystem::path exe(self);
  const std::wstring name = exe.filename().wstring();
  // The next exe of the chain that is there: a package may come without one of them.
  const bool vulkan = _wcsicmp(name.c_str(), L"wwe13_native.exe") == 0;
  if (!vulkan && _wcsicmp(name.c_str(), L"wwe13_d3d12.exe") != 0)
    return false;
  std::filesystem::path target;
  std::error_code ec;
  for (const wchar_t *next : {L"wwe13_d3d12.exe", L"wwe13_d3d11.exe"}) {
    if (!vulkan && _wcsicmp(next, L"wwe13_d3d12.exe") == 0)
      continue;  // (D3D12 failed: D3D11 only)
    if (std::filesystem::exists(exe.parent_path() / next, ec)) {
      target = exe.parent_path() / next;
      break;
    }
  }
  if (target.empty())
    return false;
  // Same arguments: the program name is replaced, the rest of the command line kept.
  std::wstring args;
  if (const wchar_t *cmd = GetCommandLineW()) {
    const wchar_t *p = cmd;
    if (*p == L'"') {
      p = std::wcschr(p + 1, L'"');
      p = p ? p + 1 : cmd + std::wcslen(cmd);
    } else {
      while (*p && *p != L' ' && *p != L'\t')
        ++p;
    }
    args = p;
  }
  std::wstring line = L"\"" + target.wstring() + L"\"" + args;
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  const std::wstring dir = exe.parent_path().wstring();
  if (!CreateProcessW(target.wstring().c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      dir.c_str(), &si, &pi))
    return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}
#else
bool Wwe13TryNextBackend() { return false; }
#endif
