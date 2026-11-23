// Host test: clang++ -std=c++20 src/native/tests/multi-shadows-test.cpp -o /tmp/ms && /tmp/ms
#include "../../multi_shadows_core.h"
#include <cstdio>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
using namespace wwe2k14::multishadows;
int main() {
  int fails = 0;
  const uint32_t creators[] = {0x822894B8u, 0x827F8058u};
  // Every other caller seen in generated/ (cloth, character flags, blur choice, sub_827EA130...) keeps the real count, on or off.
  const uint32_t others[] = {0x827E9D94u, 0x827EA168u, 0x827EA808u, 0x827EA8A8u, 0x827F2D40u, 0x82503C64u, 0x827718E8u, 0x827719B8u, 0u};
  for (uint32_t lr : creators) {
    CHECK(IsCreatorCall(lr));
    for (int n = 0; n <= 2; ++n) CHECK(ReportedCount(lr, n, true) == n);        // one-on-one etc: untouched
    for (int n = 3; n <= 6; ++n) CHECK(ReportedCount(lr, n, true) == 2);        // 3 to 6: the game's test passes
    for (int n = 7; n <= 40; ++n) CHECK(ReportedCount(lr, n, true) == n);       // beyond the 6 slots (online branches): not forced
    for (int n = -1; n <= 40; ++n) CHECK(ReportedCount(lr, n, false) == n);     // switch off: never changed
    CHECK(ReportedCount(lr, -1, true) == -1);
  }
  for (uint32_t lr : others) {
    CHECK(!IsCreatorCall(lr));
    for (int n = -1; n <= 40; ++n) CHECK(ReportedCount(lr, n, true) == n);
  }
  // Ladder and TLC: flags G[19], G[24]
  for (int n = -1; n <= 40; ++n) {
    const bool range = n >= 3 && n <= 6;
    CHECK(ForceWithFlags(n, true, false, true) == range);
    CHECK(ForceWithFlags(n, false, true, true) == range);
    CHECK(ForceWithFlags(n, true, true, true) == range);
    CHECK(!ForceWithFlags(n, false, false, true));  // no flag: the normal path decides
    CHECK(!ForceWithFlags(n, true, true, false));   // switch off: never
  }
  std::printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails);
  return fails != 0;
}
