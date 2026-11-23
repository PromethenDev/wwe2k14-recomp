// TEST BUILDS ONLY (compiled only with WWE2K14_TEST_TOOLS, CMakeLists.txt), off unless WWE2K14_TEST_MOVELOG is set.
// Ported 11-15 from lane bray1/p15r1 src/bray_test.cpp (first hook only) to prove whether a 2K15 move reaches its pair phases
// (docs/tasks/P15LOOP1-launch.md). WWE2K14_TEST_MOVELOG=ID,ID: one line per distinct (move id, sub) the game asks move
// parameters for (sub_8225B280: r4 = move id, r5 = sub/phase), and every call for a watched id (repeats folded for 2 s).
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
const std::set<uint32_t>& Watch() {
  static const std::set<uint32_t> w = [] {
    std::set<uint32_t> s;
    if (const char* e = std::getenv("WWE2K14_TEST_MOVELOG")) {
      for (const char* p = e; *p;) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(p, &end, 10);
        if (end == p) break;
        s.insert(uint32_t(v));
        p = *end ? end + 1 : end;
      }
      s.insert(0xffffffffu);  // non-empty = on, even with no ids listed
      REXLOG_INFO("[mvlog] on, {} watched ids (test only)", s.size() - 1);
    }
    return s;
  }();
  return w;
}
std::mutex g_mu;
std::set<uint64_t> g_seen;
}  // namespace

extern "C" PPC_FUNC(__imp__sub_8225B280);
extern "C" PPC_FUNC(sub_8225B280) {
  const uint32_t id = ctx.r4.u32, sub = ctx.r5.u32, lr = uint32_t(ctx.lr);
  __imp__sub_8225B280(ctx, base);
  if (Watch().empty()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_seen.size() < 6000 && g_seen.insert((uint64_t(id) << 32) | sub).second)
    REXLOG_INFO("[mvlog] act id={} sub={} row={} lr=0x{:08X}", id, sub, ctx.r3.u32 ? 1 : 0, lr);
  if (Watch().count(id)) {
    static uint64_t last_key = 0;
    static auto last_t = std::chrono::steady_clock::time_point{};
    static uint32_t folded = 0;
    const uint64_t key = (uint64_t(id) << 40) ^ (uint64_t(sub & 0xff) << 32) ^ lr;
    const auto now = std::chrono::steady_clock::now();
    if (key != last_key || now - last_t > std::chrono::seconds(2)) {
      REXLOG_INFO("[mvlog2] id={} sub={} lr=0x{:08X} folded={}", id, sub, lr, folded);
      last_key = key; last_t = now; folded = 0;
    } else {
      ++folded;
    }
  }
}
