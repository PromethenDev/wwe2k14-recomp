// Add-on offer licence bit for pack positions 32+ (select-screen cap, 2026-11-15; docs/tasks/SELCAP-report-20261115.md).
// "N:line" = generated/wwe2k14_recomp.N.cpp.
//
// sub_828216C0 (48:9856) reads one 0x40-byte catalog.dlc offer into a 68-byte item: +0 offer id (file +2), +36 type
// (file +4), +52 pack id (file +6), +60 file +10; the file's +8 field (position inside the pack) only becomes the
// licence bit item +56 = 1 << position (48:9944, slw: 0 for positions 32-63). sub_82821D18 (48:10783) later ANDs that
// bit with the owning package's licence mask and sets item +64 = 1 (locked, not listed) when the result is 0
// (48:10859-10873). So every offer at pack position 32-63 is locked although the package licence is 0xFFFFFFFF -
// the "32 offers per pack" limit seen in game (PORT-roster-plan.md:70).
//
// Fix: when the computed bit is 0, give the offer bit 31 (the same bit position 31 already uses, which is listed).
// Stock offers all sit at positions < 32, so they are unchanged; only imported offers past 31 are affected.
#include <cstdint>
#include <cstdlib>

#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

extern "C" {
PPC_FUNC(__imp__sub_828216C0);
PPC_FUNC(sub_828216C0);
}

extern "C" PPC_FUNC(sub_828216C0) {
  const uint32_t item = ctx.r3.u32;
  __imp__sub_828216C0(ctx, base);
  // WWE2K14_DLC_OFFER_MASK=0: old behaviour (offers past position 31 stay locked), for an A/B of the SELCAP load hang.
  static const bool on = [] { const char* e = std::getenv("WWE2K14_DLC_OFFER_MASK"); return !(e && *e == '0'); }();
  if (on && item != 0 && PPC_LOAD_U32(item + 56) == 0) PPC_STORE_U32(item + 56, 0x80000000u);
}

// Log once per licence pass (sub_82821D18: r4 = package/catalog object, iterated with sub_82818078 count /
// sub_82818080 item): how many offers ended locked (+64 != 0) and how many sit past position 31. Lets a run show the
// fix without a select-screen look.
extern "C" {
PPC_FUNC(__imp__sub_82821D18);
PPC_FUNC(sub_82821D18);
PPC_FUNC(sub_82818078);
PPC_FUNC(sub_82818080);
}

#include <rex/logging.h>

extern "C" PPC_FUNC(sub_82821D18) {
  const uint32_t pkg = ctx.r4.u32;
  __imp__sub_82821D18(ctx, base);
  if (pkg == 0) return;
  PPCContext c = ctx;
  c.r3.u64 = pkg;
  sub_82818078(c, base);
  const int32_t n = c.r3.s32;
  uint32_t locked = 0, pack = 0;
  for (int32_t i = 0; i < n; ++i) {
    c.r3.u64 = pkg;
    c.r4.u64 = static_cast<uint32_t>(i);
    sub_82818080(c, base);
    const uint32_t it = c.r3.u32;
    if (it == 0) continue;
    pack = PPC_LOAD_U32(it + 52);
    if (PPC_LOAD_U32(it + 64) != 0) ++locked;
  }
  REXLOG_INFO("[dlc-offers] pack={} offers={} locked={}", pack, n, locked);
}
