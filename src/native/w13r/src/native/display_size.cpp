// Display facts for the native renderer's automatic internal resolution (svr_resources.cpp).
// Kept out of the renderer sources so <windows.h> doesn't meet re:Blue's headers.

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// Size of the primary display's current mode in physical pixels (not DPI-scaled).
bool SvrDisplaySize(uint32_t &w, uint32_t &h) {
#if defined(_WIN32)
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsHeight) {
    w = mode.dmPelsWidth;
    h = mode.dmPelsHeight;
    return true;
  }
#endif
  return false;
}

// Steam sets SteamDeck=1 on the Deck, and Proton passes it through to the game.
bool SvrOnSteamDeck() {
  const char *v = std::getenv("SteamDeck");
  return v && std::strcmp(v, "1") == 0;
}
