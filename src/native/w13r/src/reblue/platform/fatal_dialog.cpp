/**
 * @file    platform/fatal_dialog.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/fatal_dialog.h"

#include "core/encoding.h"
#include "core/logging.h"

#include <SDL3/SDL.h>

#include <string>

#if defined(_WIN32)
#include "core/windows_lean.h"

namespace bd::platform {
namespace {

// A borderless-fullscreen game window covers the screen and holds foreground,
// so a plain MessageBox renders behind it and never grabs focus. Force this
// process's visible top-level windows down first. SW_FORCEMINIMIZE minimizes
// even a wedged window thread, and ShowWindowAsync never blocks on it.
BOOL CALLBACK MinimizeOwnTopLevelWindow(HWND hwnd, LPARAM) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) &&
      GetWindow(hwnd, GW_OWNER) == nullptr) {
    ShowWindowAsync(hwnd, SW_FORCEMINIMIZE);
  }
  return TRUE;
}

void ShowModal(std::string_view title, std::string_view body, bool warning) {
  EnumWindows(MinimizeOwnTopLevelWindow, 0);
  MessageBoxW(nullptr, bd::Utf8ToWide(body).c_str(),
              bd::Utf8ToWide(title).c_str(),
              MB_OK | (warning ? MB_ICONWARNING : MB_ICONERROR) | MB_TOPMOST |
                  MB_SETFOREGROUND);
}

} // namespace
} // namespace bd::platform

#elif defined(__APPLE__)

namespace bd::platform {
namespace {

void ShowModal(std::string_view title, std::string_view body, bool warning) {
  if (!SDL_IsMainThread()) {
    BD_WARN("dialog suppressed (not on the main thread, AppKit alerts "
            "are main-thread only) - see the logged message above");
    return;
  }
  const std::string t(title);
  const std::string b(body);
  SDL_ShowSimpleMessageBox(warning ? SDL_MESSAGEBOX_WARNING
                                   : SDL_MESSAGEBOX_ERROR,
                           t.c_str(), b.c_str(), nullptr);
}

} // namespace
} // namespace bd::platform

#else

namespace bd::platform {
namespace {

void ShowModal(std::string_view title, std::string_view body, bool warning) {
  // SDL walks its bootstrap list for a message box, and the X11 entry stores
  // through controls.window ahead of the null check meant to guard it
  // (SDL_x11messagebox.c), so a display it cannot open SIGSEGVs and eats the
  // error being reported. Pinning the driver keeps the walk off the others.
  const bool owned = !SDL_WasInit(SDL_INIT_VIDEO);
  if (owned && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    BD_WARN("no usable dialog backend ({}), the message above is log-only",
            SDL_GetError());
    return;
  }
  if (const char *driver = SDL_GetCurrentVideoDriver())
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, driver);

  const std::string t(title);
  const std::string b(body);
  SDL_ShowSimpleMessageBox(warning ? SDL_MESSAGEBOX_WARNING
                                   : SDL_MESSAGEBOX_ERROR,
                           t.c_str(), b.c_str(), nullptr);

  if (owned)
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

} // namespace
} // namespace bd::platform

#endif

namespace bd::platform {
namespace {

constexpr int kDeclineButtonId = 0;
constexpr int kAcceptButtonId = 1;

bool ShowChoice(std::string_view title, std::string_view body,
                std::string_view accept, std::string_view decline,
                SDL_MessageBoxFlags flags, rex::ui::Window *parent) {
#if defined(__APPLE__)
  if (!SDL_IsMainThread()) {
    BD_WARN("dialog suppressed (not on the main thread, AppKit alerts "
            "are main-thread only)");
    return false;
  }
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
  const bool owned = !SDL_WasInit(SDL_INIT_VIDEO);
  if (owned && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    BD_WARN("no usable dialog backend ({})", SDL_GetError());
    return false;
  }
  if (const char *driver = SDL_GetCurrentVideoDriver())
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, driver);
#endif

  // The SDK exposes no way to convert an rex::ui::Window to its SDL_Window,
  // so a non-null parent means "fetch the SDK's single SDL window", the same
  // way native_window.cpp resolves it off Windows.
  SDL_Window *sdl_parent = nullptr;
  if (parent) {
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    sdl_parent = (windows && count > 0) ? windows[0] : nullptr;
    SDL_free(windows);
  }

  const std::string t(title);
  const std::string b(body);
  const std::string yes(accept);
  const std::string no(decline);

  const SDL_MessageBoxButtonData buttons[] = {
      {0, kAcceptButtonId, yes.c_str()},
      {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT |
           SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
       kDeclineButtonId, no.c_str()},
  };
  const SDL_MessageBoxData data{
      flags,   sdl_parent, t.c_str(), b.c_str(),
      2,       buttons,    nullptr,
  };

  int button_id = kDeclineButtonId;
  const bool shown = SDL_ShowMessageBox(&data, &button_id);

#if !defined(_WIN32) && !defined(__APPLE__)
  if (owned)
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
#endif

  if (!shown) {
    BD_WARN("dialog failed to show ({})", SDL_GetError());
    return false;
  }
  return button_id == kAcceptButtonId;
}

} // namespace

void ShowFatalError(std::string_view title, std::string_view body) {
  BD_ERROR("Fatal: {} - {}", title, body);
  ShowModal(title, body, false);
}

void ShowWarning(std::string_view title, std::string_view body) {
  BD_WARN("{} - {}", title, body);
  ShowModal(title, body, true);
}

bool ShowFatalErrorWithAction(std::string_view title, std::string_view body,
                              std::string_view action,
                              rex::ui::Window *parent) {
  BD_ERROR("Fatal: {} - {}", title, body);
  return ShowChoice(title, body, action, "Quit", SDL_MESSAGEBOX_ERROR, parent);
}

} // namespace bd::platform
