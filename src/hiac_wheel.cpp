// Weapon wheel (the game's own apron radial menu, state 301) beyond Extreme Rules. OFF by default. WWE2K14_HIAC_WHEEL: 1 = Hell in a Cell only
// (the launcher's existing tick box), 2 = also the owner's list of ordinary match types (docs/tasks/WHEEL2-report.md).
// Game-side hooks only, no renderer code. docs/tasks/HIAC-wheel-report.md. "N:line" = generated/wwe2k14_recomp.N.cpp.
//
// Why it is absent in retail (read in code):
//   rules[row][56] = 0x80 locks column 56 off; sub_827D9A38 (45:65796) is the gate.
//   sub_825CBED8 (28:64795) tests G+24 (TLC) and G+20 (cell) before the wheel branch and gives short fixed lists; the refill sub_825CC428
//   (28:65393) does the same. Those two bytes are hidden during the build or the wheel opens with the short list.
//
// What this does, only when on, only for exhibition (G+18576 == 0):
//   1. after the apply copy sub_822CB218 (3:67552), G+56 = 2: "wheel on" the way Iron Man has it. Not 1: 1 also means "this is Extreme
//      Rules" (sub_827D9310, 45:64706). The rule table and the match description M stay untouched; only the live copy G changes.
//   2. cell and TLC only: around the two list builders, G+20 and G+24 read 0 so the code reaches the wheel branch. One mutex around both
//      hooks (no overlap can lose the byte) and the values restored are the ones saved at apply, never re-read.
// Mode 2 decides by an allow list of rule rows AND a deny test on the live flags: a row not on the list stays stock, and a listed row whose
// flags were changed by a Ring Type setting (backstage 2, cage 9, chamber 41, inferno 59, no-objects 4 and 6, elimination 16) stays stock too.
// Off: each hook is one test of a cached flag, then the original.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
constexpr uint32_t kG = 0x8376AB44;  // live match rules (45:64369)
// Rule rows of mode 2. Filled from logged runs, see docs/tasks/WHEEL2-report.md.
// Ladder and TLC rows removed 11-09 (owner): GitHub #110 crash reproduced in a Ladder match (6:48 in): sub_82378E80 writes into
// (obj+268)+0xBC with +268 NULL; that link comes from sub_8234DF88 -> sub_82359F30, which allocates 352 bytes (sub_82250050) -
// likely the game heap running out after many pulled ladders/tables. Ladder 55/56/57/58/76 and TLC 72/73/74/75 stay stock.
constexpr uint8_t kAllowRows[] = {
    0, 33, 68,                       // One on One: Normal, I Quit, Table (54 Iron Man, 59 LMS, 67 Submission are already on in the table)
    3, 9, 10, 12, 4, 69,             // Two on Two: Tag Team; Elimination Tag 9, 10, 12; Tornado Tag + Table
    13, 70,                          // Triple Threat: Normal, Table
    14, 71,                          // Fatal 4-Way: Normal, Table
    8, 11,                           // 6-Man: Tag Team, Elimination Tag
    15, 16, 17, 18, 19,              // Handicap: 1 v 2, 1 v 3, 2 v 3
    83, 89, 90, 91, 92, 95,          // Special Referee, also used by Title Match
    44, 45, 46, 47, 48,              // Falls Count Anywhere (43 is already on in the table)
};
constexpr uint32_t kDenyCols[] = {2, 4, 6, 9, 16, 41, 59};

int Mode() {
  static const int mode = [] {
    const char* e = std::getenv("WWE2K14_HIAC_WHEEL");
    const int v = e && e[0] == '1' ? 1 : e && e[0] == '2' ? 2 : 0;
    if (v) REXLOG_INFO("[hiac-wheel] WWE2K14_HIAC_WHEEL={}: apron weapon wheel allowed in exhibition {}", v, v == 1 ? "Hell in a Cell" : "Hell in a Cell and listed match types");
    return v;
  }();
  return mode;
}
bool RowAllowed(uint32_t row) {
  for (const uint8_t r : kAllowRows)
    if (r == row) return true;
  return false;
}
std::atomic<uint32_t> g_on_this_match{0};  // the switch fired for the current G
std::atomic<uint32_t> g_hide{0};           // the fixed-list bytes to hide: bit 0 cell, bit 1 TLC
uint8_t g_saved20 = 0, g_saved24 = 0;      // their real values, taken at apply
std::recursive_mutex g_list_mutex;
int g_list_depth = 0;                      // under g_list_mutex

// Runs one list builder with the fixed-list bytes hidden, under one lock, restoring the values saved at apply.
template <typename F>
void WithListBytesHidden(uint8_t* base, F&& original) {
  std::lock_guard<std::recursive_mutex> lock(g_list_mutex);
  const uint32_t hide = g_hide.load(std::memory_order_relaxed);
  if (g_list_depth++ == 0) {
    if (hide & 1) PPC_STORE_U8(kG + 20, 0);
    if (hide & 2) PPC_STORE_U8(kG + 24, 0);
  }
  original();
  if (--g_list_depth == 0) {
    if (hide & 1) PPC_STORE_U8(kG + 20, g_saved20);
    if (hide & 2) PPC_STORE_U8(kG + 24, g_saved24);
  }
}
bool HideActive(uint8_t* base) {
  return g_hide.load(std::memory_order_relaxed) != 0 && g_on_this_match.load(std::memory_order_relaxed) && PPC_LOAD_U8(kG + 56) == 2;
}
}  // namespace

extern "C" {
PPC_FUNC(__imp__sub_822CB218);
PPC_FUNC(sub_822CB218);
PPC_FUNC(__imp__sub_825CBED8);
PPC_FUNC(sub_825CBED8);
PPC_FUNC(__imp__sub_825CC428);
PPC_FUNC(sub_825CC428);
}

// Apply: G's first 64 bytes come from M+12 here (3:67632-67642). Runs for every match, so a later match gets its own table values back.
extern "C" PPC_FUNC(sub_822CB218) {
  __imp__sub_822CB218(ctx, base);
  const int on = Mode();
  if (!on) return;
  const uint32_t row = PPC_LOAD_U8(kG + 0);
  const uint8_t cell = PPC_LOAD_U8(kG + 20), tlc = PPC_LOAD_U8(kG + 24);
  const uint32_t mode = PPC_LOAD_U32(kG + 18576);
  const uint8_t before = PPC_LOAD_U8(kG + 56);
  bool denied = false;
  for (const uint32_t c : kDenyCols) denied = denied || PPC_LOAD_U8(kG + c) != 0;
  const bool allowed = on == 2 && !denied && (cell != 0 || RowAllowed(row));
  const bool fire = mode == 0 && before == 0 && (allowed || (on == 1 && cell != 0));
  g_on_this_match.store(fire ? 1 : 0, std::memory_order_relaxed);
  if (fire) {
    g_saved20 = cell;
    g_saved24 = tlc;
    PPC_STORE_U8(kG + 56, 2);
  }
  g_hide.store(fire ? (cell ? 1u : 0u) | (tlc ? 2u : 0u) : 0u, std::memory_order_relaxed);
  REXLOG_INFO("[hiac-wheel] apply row={} wrestlers={} mode={} cell={} tlc={} tables={} ladder={} cage={} chamber={} inferno={} c2={} c4={} c6={} c16={} listed={} denied={} col56 {} -> {}", row,
              PPC_LOAD_U8(kG + 34), mode, cell, tlc, PPC_LOAD_U8(kG + 18), PPC_LOAD_U8(kG + 19), PPC_LOAD_U8(kG + 9), PPC_LOAD_U8(kG + 41), PPC_LOAD_U8(kG + 59), PPC_LOAD_U8(kG + 2), PPC_LOAD_U8(kG + 4),
              PPC_LOAD_U8(kG + 6), PPC_LOAD_U8(kG + 16), RowAllowed(row) ? 1 : 0, denied ? 1 : 0, before, PPC_LOAD_U8(kG + 56));
}

// List builder (28:64454) and refill (28:65214).
extern "C" PPC_FUNC(sub_825CBED8) {
  if (!Mode() || !HideActive(base)) {
    __imp__sub_825CBED8(ctx, base);
    return;
  }
  const uint32_t list = ctx.r3.u32;
  WithListBytesHidden(base, [&] { __imp__sub_825CBED8(ctx, base); });
  REXLOG_INFO("[hiac-wheel] list built: {} items", PPC_LOAD_U32(list + 0));
}
extern "C" PPC_FUNC(sub_825CC428) {
  if (!Mode() || !HideActive(base)) {
    __imp__sub_825CC428(ctx, base);
    return;
  }
  WithListBytesHidden(base, [&] { __imp__sub_825CC428(ctx, base); });
}

// GitHub #122: enlarge only the in-match kind-1 event arena. The caller LR and
// size jointly identify sub_82593FE8's large pool result; entrance arenas use a
// different caller/size and remain unchanged. This is a guest heap allocation,
// so an unsatisfied request returns null instead of crossing into its neighbor.
namespace {
constexpr uint32_t kPool122RetailBytes = 27'577'549;
constexpr uint32_t kPool122GrowthBytes = 6u * 1024u * 1024u;
constexpr uint32_t kPool122CreateCaller = 0x8259473Cu;
constexpr uint32_t kPool122BackingCaller = 0x823B0A68u;
constexpr uint32_t kPool122LoaderCaller = 0x823D1BB4u;
constexpr uint32_t kPool122PostprocessCaller = 0x823D1C80u;

struct Pool122GrowthAttempt {
  bool active = false;
  uint32_t retail_bytes = 0;
  uint32_t enlarged_bytes = 0;
};
thread_local Pool122GrowthAttempt g_pool122_growth_attempt;

struct Pool122FailedEvent {
  bool active = false;
  uint32_t owner = 0;
  uint32_t saved_mode = 0;
};
thread_local Pool122FailedEvent g_pool122_failed_event;
}  // namespace

extern "C" {
PPC_FUNC(__imp__sub_823B09E0);
PPC_FUNC(sub_823B09E0);
PPC_FUNC(__imp__sub_823D0C60);
PPC_FUNC(sub_823D0C60);
PPC_FUNC(__imp__sub_82C39040);
PPC_FUNC(sub_82C39040);
PPC_FUNC(__imp__sub_823D0E08);
PPC_FUNC(sub_823D0E08);
}

extern "C" PPC_FUNC(sub_823B09E0) {
  // Only with the weapon wheel option on: heap43_grow.cpp makes room for the +6 MiB only then (same switches).
  static const bool grow_on = [] {
    const char* off = std::getenv("WWE2K14_HEAP43_GROW");
    return Mode() != 0 && !(off && off[0] == '0');
  }();
  if (!grow_on || ctx.lr != kPool122CreateCaller || ctx.r5.u32 != 1 || ctx.r4.u32 != kPool122RetailBytes) {
    __imp__sub_823B09E0(ctx, base);
    return;
  }

  const Pool122GrowthAttempt previous = g_pool122_growth_attempt;
  g_pool122_growth_attempt = {true, ctx.r4.u32, ctx.r4.u32 + kPool122GrowthBytes};
  ctx.r4.u32 = g_pool122_growth_attempt.enlarged_bytes;
  REXLOG_INFO("[pool122] in-match kind-1 pool request {} -> {} bytes (+{} MiB)",
              kPool122RetailBytes, ctx.r4.u32, kPool122GrowthBytes / (1024u * 1024u));
  __imp__sub_823B09E0(ctx, base);
  g_pool122_growth_attempt = previous;
}

extern "C" PPC_FUNC(sub_823D0C60) {
  const bool enlarged_request = g_pool122_growth_attempt.active && ctx.lr == kPool122BackingCaller &&
                                ctx.r7.u32 == g_pool122_growth_attempt.enlarged_bytes;
  if (!enlarged_request) {
    __imp__sub_823D0C60(ctx, base);
    return;
  }

  const uint64_t descriptor = ctx.r3.u64;
  const uint64_t arg4 = ctx.r4.u64;
  const uint64_t arg5 = ctx.r5.u64;
  const uint64_t arg6 = ctx.r6.u64;
  const uint32_t caller = ctx.lr;
  __imp__sub_823D0C60(ctx, base);
  if (ctx.r3.u32 != 0) return;

  REXLOG_WARN("[pool122] enlarged backing allocation failed for descriptor={:08X}, retrying retail size {}",
              static_cast<uint32_t>(descriptor), g_pool122_growth_attempt.retail_bytes);
  ctx.r3.u64 = descriptor;
  ctx.r4.u64 = arg4;
  ctx.r5.u64 = arg5;
  ctx.r6.u64 = arg6;
  ctx.r7.u32 = g_pool122_growth_attempt.retail_bytes;
  ctx.lr = caller;
  __imp__sub_823D0C60(ctx, base);
  if (ctx.r3.u32 != 0) {
    REXLOG_INFO("[pool122] retail backing retry succeeded descriptor={:08X}", static_cast<uint32_t>(descriptor));
  } else {
    REXLOG_ERROR("[pool122] retail backing retry also failed descriptor={:08X}", static_cast<uint32_t>(descriptor));
  }
}

extern "C" PPC_FUNC(sub_82C39040) {
  if (ctx.lr == kPool122LoaderCaller && ctx.r4.u32 == 0) {
    // Do not enter the decompressor/copy path with a null destination. Also
    // make the parent loader take its no-metadata branch and stop this event's
    // remaining rows; its generated code otherwise reads the destination again.
    g_pool122_failed_event.active = true;
    g_pool122_failed_event.owner = ctx.r29.u32;
    g_pool122_failed_event.saved_mode = g_pool122_failed_event.owner
        ? PPC_LOAD_U32(g_pool122_failed_event.owner + 68) : 0;
    if (g_pool122_failed_event.owner)
      PPC_STORE_U32(g_pool122_failed_event.owner + 68, 0);
    ctx.r23.u32 = ctx.r31.u32 + 1;
    REXLOG_ERROR("[pool122] event load failed cleanly: null destination archive={:08X} key={:08X} index={} stop_after_row={}",
                 ctx.r3.u32, ctx.r6.u32, ctx.r31.u32, ctx.r23.u32);
    ctx.r3.u32 = 0;
    return;
  }
  __imp__sub_82C39040(ctx, base);
}

extern "C" PPC_FUNC(sub_823D0E08) {
  if (g_pool122_failed_event.active && ctx.lr == kPool122PostprocessCaller && ctx.r4.u32 == 0) {
    if (g_pool122_failed_event.owner)
      PPC_STORE_U32(g_pool122_failed_event.owner + 68, g_pool122_failed_event.saved_mode);
    g_pool122_failed_event = {};
    ctx.r3.u32 = 0;
    return;
  }
  __imp__sub_823D0E08(ctx, base);
}

// GitHub #110 (2026-11-12): weapons set in a corner freeze the wrestlers in wheel matches. The corner-weapon code asks
// sub_827D9310 ("is this Extreme Rules?" = G+56 == 1) before treating an object as a corner weapon; with the wheel on,
// G+56 is 2 and the answer is no, so e.g. sub_8256DF78 (25:55646) reports "not a corner weapon" for types 8990/8991 and
// the whip/grapple waits forever. Fix: only while OUR switch fired for this match (G+56 == 2), answer "yes" to the calls
// from the corner-object code below (return addresses of their 'bl 0x827d9310'; Qwen triage + Claude check, out/qwen/
// Q-dbg110-*). Every other caller (rules, DQ, presentation) keeps the real answer. sub_8274F438 is left out on purpose:
// one of its blocks needs ER together with a NOT-ER predicate and would become reachable.
namespace {
constexpr uint32_t kCornerSites[] = {
    0x8256DF9C,  // sub_8256DF78: is this object a corner weapon (types 8990/8991)
    0x82508880,  // sub_82508838: corner-slot object release path
    0x82508AC0,  // sub_82508A60: corner interaction id set (21100/21101)
    0x8250E52C,  // sub_8250E508: corner pair table scan (ER table +19440)
    0x8250E5DC,  // sub_8250E5C0: corner pair table entry (ER table +19440)
    0x82697C94,  // sub_82697B88: weapon id list check (8990, 15089, 15090, 15901-15903, 14625)
    0x82695864,  // sub_826957F0: object field 96 = 3 in ER
    0x82695930,  // sub_826958E0: object event handler 14001/14003
    0x8278C54C,  // sub_8278C520: object state 245 gate
    0x8278C360,  // sub_8278C338: object state write +1016
    0x825DA3AC,  // sub_825DA380: object state 245 handler
};
std::atomic<uint32_t> g_corner_logged{0};
}  // namespace

extern "C" {
PPC_FUNC(__imp__sub_827D9310);
PPC_FUNC(sub_827D9310);
}

extern "C" PPC_FUNC(sub_827D9310) {
  if (ctx.r3.u32 == kG && g_on_this_match.load(std::memory_order_relaxed) && PPC_LOAD_U8(kG + 56) == 2) {
    const uint32_t site = static_cast<uint32_t>(ctx.lr);
    for (uint32_t s : kCornerSites) {
      if (s == site) {
        if (g_corner_logged.fetch_add(1) < 12)
          REXLOG_INFO("[hiac-wheel] corner weapon check from {:08X}: answered as Extreme Rules (#110)", site);
        ctx.r3.u64 = 1;
        return;
      }
    }
  }
  __imp__sub_827D9310(ctx, base);
}
