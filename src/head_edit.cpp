// Superstar Heads: full body, hair and facial hair editing (owner 2026-11-09, launcher Mods page tick box). OFF by default.
// WWE2K14_HEAD_EDIT=1 turns it on. Game-side hooks only, no renderer code. Study: docs/tasks/HB1-morph-lock.md, HB3/HB4 reports.
// "N:line" = generated/wwe2k14_recomp.N.cpp.
//
// sub_83392988 (159:69590) answers "this CAW uses a Superstar Head" (*(mgr+4788) != 512). With a head the game:
//   - hides CAW menu items whose data tag is 6280 (menu.pac MFLO/0001; builder sub_833A3F90, call returns to 0x833A4020, item label
//     copied to r1+136): SKIN, HAIR, FACE, FACIAL HAIR, MAKEUP, FACEPAINT, BODY ART, HEADWEAR, NECK, TRAPS;
//   - skips body-morph regions 1 (neck) and 4 (traps) in the model morph applier sub_82306070 (5:52862, 5:53359; returns 0x82306134,
//     0x82306468).
// When on, only those answers become "no head": the four menu items below and the two applier sites. Every other caller (layer lists,
// template copies, alternate-attire flag, head-or-neck part check) keeps the game's answer. Verified on lane A (HB3, HB4) and by the
// owner on the laptop (2026-11-09). Off: one test of a cached flag, then the original.
#include <cstdint>
#include <cstdlib>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
constexpr uint32_t kMenuSite = 0x833A4020;      // sub_833A3F90, menu item filter
constexpr uint32_t kApplierSite1 = 0x82306134;  // sub_82306070, region loop 1
constexpr uint32_t kApplierSite2 = 0x82306468;  // sub_82306070, region loop 2
constexpr uint32_t kLabelOffset = 136;           // item label in the builder's stack copy (r29 = *(r1+136))

bool On() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_HEAD_EDIT");
    const bool v = e && e[0] == '1' && e[1] == '\0';
    if (v) REXLOG_INFO("[head-edit] WWE2K14_HEAD_EDIT=1: Superstar Heads can edit neck, traps, hair and facial hair");
    return v;
  }();
  return on;
}
bool UnlockedLabel(uint32_t label) {
  return label == 6485 /* NECK */ || label == 6551 /* TRAPS */ || label == 6386 /* HAIR */ || label == 6462 /* FACIAL HAIR */;
}
}  // namespace

extern "C" {
PPC_FUNC(__imp__sub_83392988);
PPC_FUNC(sub_83392988);
}

extern "C" PPC_FUNC(sub_83392988) {
  const uint32_t site = static_cast<uint32_t>(ctx.lr);
  __imp__sub_83392988(ctx, base);
  if (!On() || ctx.r3.u32 == 0) return;
  if (site == kApplierSite1 || site == kApplierSite2) {
    ctx.r3.u32 = 0;
    return;
  }
  if (site == kMenuSite) {
    const uint32_t label_address = ctx.r1.u32 + kLabelOffset;  // the builder's own stack frame, written just before this call
    if (UnlockedLabel(PPC_LOAD_U32(label_address))) ctx.r3.u32 = 0;
  }
}
