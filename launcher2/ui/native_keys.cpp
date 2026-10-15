#include "native_keys.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <X11/Xlib.h>
#include <X11/keysym.h>
#endif

namespace wwe2k14::launcher::ui {

bool ShiftDownAtLaunch(SDL_Window* window) {
  if ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0) return true;
#ifdef _WIN32
  return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#elif defined(__linux__)
  if (!window) return false;
  const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
  Display* display = static_cast<Display*>(
      SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
  if (!display) return false;
  char keymap[32]{};
  if (!XQueryKeymap(display, keymap)) return false;
  const KeyCode left = XKeysymToKeycode(display, XK_Shift_L);
  const KeyCode right = XKeysymToKeycode(display, XK_Shift_R);
  const auto down = [&keymap](KeyCode key) {
    if (key == 0) return false;
    return (static_cast<unsigned char>(keymap[key >> 3]) & (1U << (key & 7))) != 0;
  };
  return down(left) || down(right);
#else
  (void)window;
  return false;
#endif
}

}  // namespace wwe2k14::launcher::ui
