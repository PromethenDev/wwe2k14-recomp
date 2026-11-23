// TEST-ONLY deterministic match setup (owner 2026-11-04: fast testing without menus / random scenes). Default OFF:
// inert unless the environment switches below are set (the launcher never sets them), like unlock_all.cpp; kept in
// every build because the automated test routes run the standard (TEST_TOOLS=OFF) builds.
//
// Step 1 (this file): fixed arena. sub_822CA958(M, arena, flag) is the match-description arena setter (VERIFIED
// statically in our build, generated/wwe2k14_recomp.3.cpp): it stores arena at M+532 and flag at M+537; arena id 300
// means "random" and is replaced by a value read from the object returned by sub_822BB608(M+8) before the store.
// Automated test routes keep the menu default (random), so every run picked a different arena (T1b launches 1-3:
// TLC, Extreme Rules, WrestleMania retro), which defeated scene-specific tests.
//
//   WWE2K14_TEST_SCENARIO_LOG=1   log every call: caller, requested arena, flag, arena stored at M+532 afterwards
//   WWE2K14_TEST_ARENA=<id>       every arena request becomes <id> (same code path as a player choosing that arena).
//                                 The menus call the setter twice per match: first with 300 (random), then from the
//                                 exhibition finalize (caller 0x8292900C) with the arena the menu had already picked at
//                                 random (run R1a: 300 -> then 28, BG28 loaded) - so both calls are replaced.
//                                 Id = number of the arena file PAC\BG\BG<id>.PAC (BG12 = TLC, BG16 = Extreme Rules,
//                                 BG28 / BG39 = WrestleMania arenas; T1b + R1a runs).
//

#include <cstdint>
#include <cstdlib>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
bool LogOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_TEST_SCENARIO_LOG");
    return v && v[0] == '1';
  }();
  return on;
}
int32_t FixedArena() {
  static const int32_t id = [] {
    const char* v = std::getenv("WWE2K14_TEST_ARENA");
    if (!v || !*v) return -1;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    if (end == v || n < 0 || n >= 300) {
      REXLOG_WARN("[test-scenario] WWE2K14_TEST_ARENA='{}' ignored (expected 0..299)", v);
      return -1;
    }
    REXLOG_INFO("[test-scenario] WWE2K14_TEST_ARENA={}: random arena requests use this arena (test only)", n);
    return static_cast<int32_t>(n);
  }();
  return id;
}
}  // namespace

extern "C" PPC_FUNC(__imp__sub_822CA958);
extern "C" PPC_FUNC(sub_822CA958) {
  const uint32_t m = ctx.r3.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const int32_t requested = ctx.r4.s32;
  const uint32_t flag = ctx.r5.u32;
  const int32_t fixed = FixedArena();
  if (fixed >= 0) ctx.r4.u64 = static_cast<uint64_t>(fixed);
  const int32_t passed = ctx.r4.s32;
  __imp__sub_822CA958(ctx, base);
  if (LogOn()) {
    const uint32_t stored = m ? PPC_LOAD_U32(m + 532) : 0;
    REXLOG_INFO("[test-scenario] arena set M=0x{:08X} caller=0x{:08X} requested={} passed={} flag={} stored={}", m,
                caller, requested, passed, flag & 0xFF, stored);
  }
}

// Step 2: fixed wrestlers. sub_822C9C08(M, i, v, team, ...) is the participant setter the exhibition finalize calls
// (VERIFIED statically: stores v at M+724+i*3580+56; v == 500 clears the slot). WWE2K14_TEST_WRESTLERS=a,b,... replaces v
// for participant 0,1,... so a route that accepts the menu's default pick plays the wanted wrestlers. Logged by
// WWE2K14_TEST_SCENARIO_LOG=1.
namespace {
const std::vector<uint32_t>& TestWrestlers() {
  static const std::vector<uint32_t> v = [] {
    std::vector<uint32_t> out;
    if (const char* e = std::getenv("WWE2K14_TEST_WRESTLERS")) {
      for (const char* p = e; *p;) {
        char* end = nullptr;
        const unsigned long n = std::strtoul(p, &end, 10);
        if (end == p) break;
        out.push_back(uint32_t(n));
        p = *end == ',' ? end + 1 : end;
      }
      REXLOG_INFO("[test-scenario] WWE2K14_TEST_WRESTLERS: {} slots (test only)", out.size());
    }
    return out;
  }();
  return v;
}
}  // namespace

extern "C" PPC_FUNC(__imp__sub_822C9C08);
extern "C" PPC_FUNC(sub_822C9C08) {
  const uint32_t i = ctx.r4.u32, was = ctx.r5.u32;
  const auto& w = TestWrestlers();
  if (i < w.size() && was != 500) ctx.r5.u64 = w[i];
  if (LogOn())
    REXLOG_INFO("[test-scenario] participant i={} value={} -> {} team={} a5={} a6={} a7={} a8={} caller=0x{:08X}", i, was,
                ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, uint32_t(ctx.lr));
  __imp__sub_822C9C08(ctx, base);
}
