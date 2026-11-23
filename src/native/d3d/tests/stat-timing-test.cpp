// C1-T3 host test: stat_timing.h schedule. Build: see tools note in docs/tasks/C1-T3.md. Run with env set by the test.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#define REXLOG_INFO(...) ((void)0)
#include "../stat_timing.h"
using namespace wwe2k14::d3d::stat;
int main() {
  // env: WWE2K14_D3D_TIMING_EVERY=8 WWE2K14_D3D_TIMING_AB=600 (set by the runner)
  assert(Every() == 8 && AbBlock() == 600);
  int on_even = 0, on_odd = 0;
  for (uint32_t f = 0; f < 600; ++f) { on_even += On(f); assert(Scale(f) == 1); }
  for (uint32_t f = 600; f < 1200; ++f) { on_odd += On(f); assert(Scale(f) == 8); }
  assert(on_even == 600 && on_odd == 75);
  // scaled sum of a constant 1.0 per frame equals the true sum in both blocks
  uint64_t sum = 0; for (uint32_t f = 600; f < 1200; ++f) if (On(f)) sum += Scale(f);
  assert(sum == 600);
  std::puts("stat-timing-test PASS");
}
