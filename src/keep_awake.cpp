// GitHub #49: Windows does not count controller input as user activity, so the display blanked and the PC went to
// sleep during long matches. While the game window exists the main thread asks Windows to keep both awake; the
// request ends when the game exits (ES_CONTINUOUS alone clears it, and Windows also drops it with the thread).

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace wwe2k14 {

void KeepDisplayAwake(bool on) {
#if defined(_WIN32) && !defined(REX_UWP)  // Xbox: not app-family; the console stays awake in games
  SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
#else
  (void)on;
#endif
}

}  // namespace wwe2k14
