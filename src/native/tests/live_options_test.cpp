// G1: host test for the live crowd/shadows options and their per-frame latch (native/d3d/live_options.h).
// Build: g++ -std=c++20 -I src src/native/tests/live_options_test.cpp -o /tmp/live_options_test -pthread
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "native/d3d/live_options.h"

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
  std::printf("check=%s result=%s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) ++failures;
}
}  // namespace

int main() {
  namespace live = wwe2k14::d3d::live;
  setenv("WWE2K14_D3D_CROWD", "0", 1);  // launcher "None"
  unsetenv("WWE2K14_D3D_SHADOWS");      // launcher default: on
  Check(live::Get().crowd.load() == 0 && live::Get().shadows.load(), "start_values_from_env");

  live::FrameLatch latch;
  bool changed = true;
  Check(latch.Update(10, &changed) && !changed && latch.crowd == 0 && latch.shadows, "first_latch_not_a_change");
  live::SetCrowd(1);
  live::SetShadows(false);
  Check(!latch.Update(10, &changed) && latch.crowd == 0 && latch.shadows, "same_frame_keeps_old_values");
  Check(latch.Update(11, &changed) && changed && latch.crowd == 1 && !latch.shadows, "next_frame_takes_new_values");
  Check(latch.Update(12, &changed) && !changed, "no_change_without_menu");
  live::SetCrowd(7);
  Check(latch.Update(13, &changed) && changed && latch.crowd == 1, "invalid_crowd_becomes_full");
  live::SetCrowd(2);
  Check(latch.Update(14, &changed) && latch.crowd == 2, "half_crowd_kept");

  // Review fix 4: launcher Half (2) survives a menu off/on; Full stays Full.
  live::SetCrowd(2);
  live::SetCrowdEnabled(false);
  Check(live::Get().crowd.load() == 0, "crowd_off");
  live::SetCrowdEnabled(true);
  Check(live::Get().crowd.load() == 2, "half_restored_not_full");
  live::SetCrowd(1);
  live::SetCrowdEnabled(false);
  live::SetCrowdEnabled(true);
  Check(live::Get().crowd.load() == 1, "full_restored");

  // Concurrency: one writer flips the options while 4 draw threads latch per frame; each latched pair must be one
  // the writer stored (crowd 0 <-> shadows off, crowd 1 <-> shadows on), checked loosely: values stay in range.
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      live::FrameLatch l;
      bool c = false;
      for (uint32_t f = 0; !stop.load(); ++f) {
        l.Update(f, &c);
        if (l.crowd != 0 && l.crowd != 1 && l.crowd != 2) bad.fetch_add(1);
      }
    });
  }
  for (int i = 0; i < 200000; ++i) {
    live::SetCrowd(i & 1);
    live::SetShadows((i & 1) != 0);
  }
  stop.store(true);
  for (auto& t : readers) t.join();
  Check(bad.load() == 0, "concurrent_latch_values_in_range");
  std::printf("live_options_test %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
