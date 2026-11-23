// TEMPORARY test-only diagnostic for GitHub #122 (owner 2026-11-10): log the event-scene arena pool. Remove when #122 is settled.
// Built only with WWE2K14_TEST_TOOLS (CMakeLists.txt); inert unless WWE2K14_TEST_POOL_DIAG=1.
//
// Static facts (Q4-C1b/C1c, checked by Claude; "N:line" = generated/wwe2k14_recomp.N.cpp):
//   sub_823D1D48 (12:17384, reached only through a pointer table) jumps to sub_823D19A0 when *(this+60) is 3..7.
//   sub_823D19A0 (12:16849) takes a buffer from the arena at P = *(this+52): new = *(P+8) + align16(size); *(P+8) = new
//   (always); result = *(P+0) + old if new <= *(P+4), else 0 - unchecked - then calls the loader sub_82C39040 with it as the
//   destination (12:17148). sub_82C39040 (80:44985) copies or decompresses into its r4; with r4 = 0 that is #122's fault at 0x60.
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
bool On() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_TEST_POOL_DIAG");
    const bool v = e && e[0] == '1';
    if (v) REXLOG_INFO("[pool-diag] WWE2K14_TEST_POOL_DIAG=1: logging the event-scene arena (#122, test only)");
    return v;
  }();
  return on;
}
std::atomic<uint32_t> g_lines{0};
constexpr uint32_t kMaxLines = 4000;
}  // namespace

extern "C" {
PPC_FUNC(__imp__sub_823D19A0);
PPC_FUNC(sub_823D19A0);
}

extern "C" PPC_FUNC(sub_823D19A0) {
  if (!On()) { __imp__sub_823D19A0(ctx, base); return; }
  const uint32_t self = ctx.r3.u32;
  const uint32_t pool = self ? PPC_LOAD_U32(self + 52) : 0;
  const uint32_t state = self ? PPC_LOAD_U32(self + 60) : 0;
  const uint32_t pbase = pool ? PPC_LOAD_U32(pool + 0) : 0, cap = pool ? PPC_LOAD_U32(pool + 4) : 0;
  const uint32_t before = pool ? PPC_LOAD_U32(pool + 8) : 0;
  __imp__sub_823D19A0(ctx, base);
  const uint32_t after = pool ? PPC_LOAD_U32(pool + 8) : 0;
  if (g_lines.fetch_add(1) < kMaxLines)
    REXLOG_INFO("[pool-diag] event load this={:08X} state={} pool={:08X} base={:08X} cap={} used {} -> {} (+{}) free_after={} ret={}",
                self, state, pool, pbase, cap, before, after, after - before, static_cast<int64_t>(cap) - after, ctx.r3.u32);
}

// TEMPORARY #128 diagnostic (owner 2026-11-10): sub_822C1C68 (3:44806) maps wrestler id -> Universe slot from the u16 table at
// 0x835DDF90 (512 = none). With WWE2K14_TEST_UNIV_DIAG=1, the first call logs every id 0..499 whose slot is 512 (compact
// ranges), and each distinct id later answered 512 (first 64). Test builds only, like the pool log above.
extern "C" {
PPC_FUNC(__imp__sub_822C1C68);
PPC_FUNC(sub_822C1C68);
}
namespace {
bool UnivOn() {
  static const bool on = [] { const char* e = std::getenv("WWE2K14_TEST_UNIV_DIAG"); return e && e[0] == '1'; }();
  return on;
}
}  // namespace
extern "C" PPC_FUNC(sub_822C1C68) {
  if (!UnivOn()) { __imp__sub_822C1C68(ctx, base); return; }
  static std::atomic<bool> dumped{false};
  if (!dumped.exchange(true)) {
    std::string none; int count = 0, start = -1;
    for (int id = 0; id <= 500; ++id) {
      const bool missing = id < 500 && PPC_LOAD_U16(0x835DDF90u + 2u * id) == 512;
      if (missing) { ++count; if (start < 0) start = id; }
      else if (start >= 0) { none += (none.empty() ? "" : ",") + std::to_string(start) + (id - 1 > start ? "-" + std::to_string(id - 1) : ""); start = -1; }
    }
    REXLOG_INFO("[univ-diag] slot table at first lookup: {} of 500 ids have no Universe slot: {}", count, none);
  }
  const uint32_t id = ctx.r3.u32;
  __imp__sub_822C1C68(ctx, base);
  static std::atomic<uint32_t> logged{0};
  if (ctx.r3.u32 == 512 && id < 500 && logged.fetch_add(1) < 64)
    REXLOG_INFO("[univ-diag] id {} has no Universe slot (caller {:08X})", id, static_cast<uint32_t>(ctx.lr));
}
