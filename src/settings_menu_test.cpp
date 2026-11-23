// G1 test hook (TEST-ONLY, default OFF): scripted pad presses that open the in-game settings menu with Back+Start,
// change options with the D-pad and A, and close it with B, so an unattended run proves the menu, the pad path and
// the live options. Builds without test tools return 0 (off).
// Merge note: listed with the other WWE2K14_NO_TEST_TOOLS sources in CMakeLists.txt (out/lanes/ovl1/CMakeLists-merge.patch).
#include "settings_menu_test.h"

#include <chrono>
#include <cstdlib>

namespace wwe2k14::menu {
namespace {
const auto g_process_start = std::chrono::steady_clock::now();
const auto g_process_start_wall = std::chrono::system_clock::now();

struct Press {
  double at, length;
  uint16_t buttons;
};
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kStartBack = 0x0030, kA = 0x1000, kB = 0x2000;
// Menu rows: 0 Crowd, 1 Shadows, 2 FPS counter, 3 Lower CPU use, 4 Sync frames to monitor, 5 Stretch to fill.
constexpr Press kScript[] = {
    {0.0, 0.30, kStartBack},                                     // open
    {2.0, 0.15, kDown}, {2.5, 0.15, kUp},                        // nav cursor on row 0
    {3.0, 0.15, kA},                                             // Crowd off
    {4.0, 0.15, kDown}, {4.5, 0.15, kA},                         // Shadows off
    {5.0, 0.15, kDown}, {5.5, 0.15, kDown}, {6.0, 0.15, kA},     // Lower CPU use flipped
    {6.5, 0.15, kDown}, {7.0, 0.15, kA},                         // Sync flipped
    {7.5, 0.15, kDown}, {8.0, 0.15, kA},                         // Stretch flipped
    {20.0, 0.15, kUp}, {20.5, 0.15, kA},                         // Sync back
    {21.0, 0.15, kUp}, {21.5, 0.15, kUp},                        // past Lower CPU use (stays flipped: saved on close)
    {22.0, 0.15, kUp}, {22.5, 0.15, kA},                         // Shadows back on
    {23.0, 0.15, kUp}, {23.5, 0.15, kA},                         // Crowd back on
    {24.5, 0.15, kDown}, {25.0, 0.15, kDown}, {25.5, 0.15, kDown},
    {26.0, 0.15, kDown}, {26.5, 0.15, kDown}, {27.0, 0.15, kA},  // down to row 5: Stretch back
};
// Close, by cycle (review-fix check ovl1-r5): cycle % 3 == 0 B tap, == 1 B held 300 ms, == 2 keyboard Esc sent by
// the harness at 30.0 s (the script presses nothing; if the menu is still open at 40.0 s, a B tap closes it).
constexpr Press kCloseTap = {30.0, 0.15, kB}, kCloseHold = {30.0, 0.30, kB}, kFallback = {40.0, 0.15, kB};
}  // namespace

double ProcessStartEpochSeconds() {
  return std::chrono::duration<double>(g_process_start_wall.time_since_epoch()).count();
}

double SecondsSinceProcessStart() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_process_start).count();
}

uint16_t TestScriptButtonsAt(double t, int period, int start, bool menu_open) {
  if (period <= 0 || t < start) return 0;
  const int cycle = int((t - start) / period);
  const double c = t - start - period * double(cycle);
  uint16_t b = 0;
  auto in = [c](const Press& p) { return c >= p.at && c < p.at + p.length; };
  for (const Press& p : kScript)
    if (in(p)) b |= p.buttons;
  switch (cycle % 3) {
    case 0: if (in(kCloseTap)) b |= kCloseTap.buttons; break;
    case 1: if (in(kCloseHold)) b |= kCloseHold.buttons; break;
    default: if (menu_open && in(kFallback)) b |= kFallback.buttons; break;
  }
  return b;
}

int TestCloseMode(double t, int period, int start) {
  return period <= 0 || t < start ? -1 : int((t - start) / period) % 3;
}

#ifdef WWE2K14_NO_TEST_TOOLS
int TestAutoOpenPeriodSeconds() { return 0; }
int TestAutoOpenStartSeconds() { return 0; }
uint16_t TestScriptButtons(double, bool) { return 0; }
#else
int TestAutoOpenPeriodSeconds() {
  static const int v = [] {
    const char* e = std::getenv("WWE2K14_TEST_OVERLAY_AUTOOPEN");
    const long p = e ? std::strtol(e, nullptr, 10) : 0;
    return p >= 35 && p <= 3600 ? int(p) : 0;
  }();
  return v;
}
int TestAutoOpenStartSeconds() {
  static const int v = [] {
    const char* e = std::getenv("WWE2K14_TEST_OVERLAY_START_S");
    const long s = e ? std::strtol(e, nullptr, 10) : 90;
    return s >= 0 && s <= 3600 ? int(s) : 90;
  }();
  return v;
}
uint16_t TestScriptButtons(double t, bool menu_open) {
  const int period = TestAutoOpenPeriodSeconds();
  return period ? TestScriptButtonsAt(t, period, TestAutoOpenStartSeconds(), menu_open) : 0;
}
#endif

}  // namespace wwe2k14::menu
