// G1 test hook (TEST-ONLY, default OFF; settings_menu_test.cpp). Builds without test tools return 0 / off.
#pragma once

#include <cstdint>

namespace wwe2k14::menu {

int TestAutoOpenPeriodSeconds();  // WWE2K14_TEST_OVERLAY_AUTOOPEN=<seconds>, 0 = off
int TestAutoOpenStartSeconds();   // WWE2K14_TEST_OVERLAY_START_S (default 90)
// Scripted pad buttons (XINPUT bits) for pad 0 at `t` seconds after process start: every PERIOD from START the
// script presses Back+Start (open), walks the menu with the D-pad and A (Crowd, Shadows, Lower CPU use, Sync, Stretch
// off/flipped), puts them back except Lower CPU use, then B (close, which saves). 0 when off or between presses.
uint16_t TestScriptButtons(double t, bool menu_open);
// Same script as a pure function, for the host test. Close by cycle % 3: B tap, B held 300 ms, keyboard Esc from the
// harness at 30 s (fallback B tap at 40 s only if the menu is still open).
uint16_t TestScriptButtonsAt(double t, int period, int start, bool menu_open);
int TestCloseMode(double t, int period, int start);  // -1 before start, else cycle % 3
double SecondsSinceProcessStart();
double ProcessStartEpochSeconds();  // wall clock of the script's t = 0 (for the harness's key injector)

}  // namespace wwe2k14::menu
