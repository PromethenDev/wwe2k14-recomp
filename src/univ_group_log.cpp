// Universe scene-group log (2026-11-14, owner goal "deeper Universe"): LOG ONLY, game behaviour unchanged.
// "N:line" = generated/wwe2k14_recomp.N.cpp.
//
// sub_82B9CC68 (75:19185) = the primary story-group chooser: r3 = match/rivalry object, r4 = slot index 0-2 (stored at
// context +296 by sub_82B9A930); it returns the wanted group (story_id / 100 + 1, -1 = none; sub_82B9A9C0). Its main
// caller is sub_82B86068 (return address 0x82B8617C), which passes the group to the story picker sub_82B85BE8 (whose
// pick is logged by univ_fresh_stories.cpp as "[univ-story] pick"). Source: out/univ/luna/L5-report.md (claims re-checked
// against 74:45739-45745 on 2026-11-14).
//
// Why: before any variety change to the group choice, a season of these lines tells which groups each slot/caller
// produces and how often, so alternatives are only ever chosen among groups the game itself uses for that slot.
#include <atomic>
#include <cstdint>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

extern "C" {
PPC_FUNC(__imp__sub_82B9CC68);
PPC_FUNC(sub_82B9CC68);
}

namespace {
std::atomic<uint32_t> g_calls{0};
}

extern "C" PPC_FUNC(sub_82B9CC68) {
  const uint32_t site = static_cast<uint32_t>(ctx.lr);
  const uint32_t object = ctx.r3.u32;
  const uint32_t slot = ctx.r4.u32;
  __imp__sub_82B9CC68(ctx, base);
  const int32_t group = ctx.r3.s32;
  const uint32_t n = ++g_calls;
  REXLOG_INFO("[univ-group] call #{} site={:08X} object={:08X} slot={} group={}", n, site, object, slot, group);
}
