// Heap #43 grow (GitHub #122: Hell in a Cell with the weapon wheel runs heap #43 full: 0.68 MB free vs 8.3 MB
// without the wheel). sub_827DDC78(r3 = out, r4 = table, r5 = pair count) fills a 56-entry big-endian u32 array:
// out[id] = size in KiB. The in-match layout calls it with r4 == 0x83625BF8, r5 == 46 (generated 47.cpp:9706-9718);
// in that table id 43 = 39424 KiB (40,370,176 bytes). Heap #55 is the leftover (15.6 MB, never used in 23 test
// traces), so growing #43 takes from #55. This hook bumps id 43 by 8192 KiB when the game writes that table.
// Active only when WWE2K14_HIAC_WHEEL is 1 or 2 (the player's weapon wheel option); WWE2K14_HEAP43_GROW=0
// switches it off.
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
constexpr uint32_t kMatchLayoutTable = 0x83625BF8;
constexpr uint32_t kGrowKiB = 8192;

bool Enabled() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_HIAC_WHEEL");
    if (!(e && (e[0] == '1' || e[0] == '2'))) return false;
    const char* off = std::getenv("WWE2K14_HEAP43_GROW");
    return !(off && off[0] == '0');
  }();
  return on;
}

std::atomic<uint32_t> g_logs{0};
}

extern "C" {
PPC_FUNC(__imp__sub_827DDC78);
PPC_FUNC(sub_827DDC78);
}

extern "C" PPC_FUNC(sub_827DDC78) {
  const uint32_t out = ctx.r3.u32;   // r3/r4 are volatile: save before the call
  const uint32_t table = ctx.r4.u32;
  __imp__sub_827DDC78(ctx, base);
  if (Enabled() && table == kMatchLayoutTable) {
    const uint32_t v = PPC_LOAD_U32(out + 43 * 4);
    if (v == 39424) {  // exact retail value, otherwise leave it
      PPC_STORE_U32(out + 43 * 4, v + kGrowKiB);
      if (g_logs.fetch_add(1) < 4)
        REXLOG_INFO("[heap43-grow] heap 43 {} -> {} KiB", v, v + kGrowKiB);
    }
  }
}
