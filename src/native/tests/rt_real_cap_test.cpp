// V19 host test: LRU order, bound/pending protection, resolution-scaled cap, and same-frame retire bursts.
// g++ -std=c++20 -Wall -Wextra -Werror -I src/native/d3d src/native/tests/rt_real_cap_test.cpp -o rt_real_cap_test
#include <cstdio>
#include <vector>

#include "rt_real_cap_core.h"

using namespace wwe2k14::d3d::rt_real_cap;

static int g_fail = 0;
#define EXPECT(c)                                                                                 \
  do {                                                                                            \
    if (!(c)) {                                                                                   \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                   \
      ++g_fail;                                                                                   \
    }                                                                                             \
  } while (0)

int main() {
  constexpr uint64_t MB = 1ull << 20;
  const uint64_t base = 512 * MB;
  EXPECT(ScaleCapBytes(base, 600) == base);   // lower scale keeps the 720p floor
  EXPECT(ScaleCapBytes(base, 1000) == base);  // 720p
  EXPECT(ScaleCapBytes(base, 1500) == 1152 * MB);
  EXPECT(ScaleCapBytes(base, 2000) == 2048 * MB);
  EXPECT(ScaleCapBytes(base, 3000) == 4608 * MB);  // 4K internal scene

  // Oldest safe targets go first and LRU order is deterministic for equal timestamps.
  std::vector<Candidate> lru = {{3, 1000, 120, false, false}, {4, 1000, 130, false, false},
                                {5, 3000, 200, false, false}};
  Plan p = MakePlan(500, 100, 512, 10000, 3000, lru);
  EXPECT(p.fits && p.after_bytes <= 512);
  EXPECT(p.victims.size() == 2 && p.victims[0] == 3 && p.victims[1] == 4);

  // A current color/depth target and a pending backend read are never offered as victims.
  std::vector<Candidate> protected_set = {{1, 0, 300, true, false}, {2, 1000, 130, false, false},
                                           {3, 2000, 100, false, false}, {4, 0, 300, false, true}};
  p = MakePlan(600, 50, 512, 10000, 3000, protected_set);
  EXPECT(p.fits);
  EXPECT(p.victims.size() == 2 && p.victims[0] == 2 && p.victims[1] == 3);
  for (uint64_t victim : p.victims) EXPECT(victim != 1 && victim != 4);

  // A target larger than the cap, or a full cap with no safe LRU victims, is refused.
  p = MakePlan(0, 513, 512, 10000, 3000, {});
  EXPECT(!p.fits && p.victims.empty());
  p = MakePlan(600, 20, 512, 10000, 3000, {{1, 0, 500, true, false}});
  EXPECT(!p.fits && p.victims.empty());

  // Scene-change burst: 400 one-MB targets are selected together. ReleaseSurface makes them
  // pending-retire, so the 808 MB query still sees all 400 MB and the next 1 MB request appears
  // to exceed the 512 MB cap. One backend wait retires the batch; the same-frame request then fits.
  std::vector<Candidate> burst;
  burst.reserve(400);
  for (uint64_t i = 0; i < 400; ++i) burst.push_back({i + 1, 0, MB, false, false});
  const uint64_t burst_live = 808 * MB;
  const uint64_t burst_incoming = MB;
  const uint64_t burst_pending = 400 * MB;
  p = MakePlan(burst_live, burst_incoming, base, 10000, 3000, burst);
  EXPECT(p.fits && p.victims.size() == 400 && p.after_bytes == 409 * MB);
  EXPECT(PendingRetireCanSatisfy(burst_live, burst_incoming, base, burst_pending));
  EXPECT(!PendingRetireCanSatisfy(burst_live, burst_incoming, base, burst_pending, true));
  const uint64_t after_wait = burst_live - burst_pending;
  p = MakePlan(after_wait, burst_incoming, base, 10000, 3000, {});
  EXPECT(p.fits && p.victims.empty() && p.after_bytes == 409 * MB);
  EXPECT(!PendingRetireCanSatisfy(after_wait, burst_incoming, base, 0));

  // A wait cannot fix an incoming image larger than the whole cap, or a shortfall larger than
  // all bytes pending retirement.
  EXPECT(!PendingRetireCanSatisfy(0, 513 * MB, base, 1000 * MB));
  EXPECT(!PendingRetireCanSatisfy(808 * MB, MB, base, 296 * MB));

  if (g_fail) {
    std::printf("%d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("rt_real_cap_test: all passed\n");
  return 0;
}
