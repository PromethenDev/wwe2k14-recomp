// G46 (docs/cards/G46-victims-back-crash.md, GitHub #46): transcription of the game's render-command pump drain
// sub_82BB9418 (generated/wwe2k14_recomp.76.cpp:8208-8330) with a fixed node slot and two guarded recoveries.
// Header-only so the host test (tests/g46_drain_test.cpp in lane w46c) runs the same code as the game build.
//
// Differences from the generated drain, all no-ops while the PPC ABI and the drain's own invariant hold:
//   1. After each sub_82BB8C00 call, ctx.r1 and r14..r31 are compared with their pre-call values (a callee must
//      preserve them); on a mismatch they are restored and Hooks::Drift is called.
//   2. The node pointer is re-read from the slot address fixed at frame setup (frame r1 + 80), not from ctx.r1 + 80.
//      If it is < 0x10000 it is replaced by the invariant node (Hooks::Null(.., true)): the interpreter writes the slot
//      only in op 81/82 (76:7208-7263; stores 7239, 7263), both times as bank_ptr + cursor*16, so after the call
//      slot == [pump + ([pump+36]+12)*4] + [pump+44]*16 (the drain's loop-head expression, 76:8251-8276).
//   3. If the invariant node is also < 0x10000 the drain ends as on opcode 105 (Hooks::Null(.., false)).
#pragma once
#include <cstdint>

#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace g46 {
inline uint32_t L(uint8_t* base, uint32_t a) { return PPC_LOAD_U32(a); }
inline void S(uint8_t* base, uint32_t a, uint32_t v) { PPC_STORE_U32(a, v); }

struct NonVol {
  uint64_t r1 = 0, lr = 0;
  uint64_t r[18] = {};  // r14..r31
};
#define G46_NV_LIST(c) \
  {&c.r14, &c.r15, &c.r16, &c.r17, &c.r18, &c.r19, &c.r20, &c.r21, &c.r22, &c.r23, &c.r24, &c.r25, &c.r26, &c.r27, \
   &c.r28, &c.r29, &c.r30, &c.r31}
inline void Save(const PPCContext& c, NonVol& s) {
  const PPCRegister* g[18] = G46_NV_LIST(c);
  s.r1 = c.r1.u64;
  s.lr = c.lr;
  for (int i = 0; i < 18; ++i) s.r[i] = g[i]->u64;
}
// Bit i set = r(14+i) differs.
inline uint32_t Diff(const PPCContext& c, const NonVol& s) {
  const PPCRegister* g[18] = G46_NV_LIST(c);
  uint32_t m = 0;
  for (int i = 0; i < 18; ++i) m |= (g[i]->u64 != s.r[i] ? 1u : 0u) << i;
  return m;
}
inline void Restore(PPCContext& c, const NonVol& s) {
  PPCRegister* g[18] = G46_NV_LIST(c);
  c.r1.u64 = s.r1;
  for (int i = 0; i < 18; ++i) g[i]->u64 = s.r[i];
}
#undef G46_NV_LIST

inline uint32_t InvariantNode(uint8_t* base, uint32_t pump) {
  const uint32_t bank = L(base, pump + 36);
  return L(base, pump + (bank + 12) * 4) + (L(base, pump + 44) << 4);
}

// Hooks: void SaveGpr(ctx, base) / RestGpr(ctx, base)  (__savegprlr_29 / __restgprlr_29)
//        void Interp(ctx, base)                        (sub_82BB8C00, the hooked RenderCmdInterp)
//        void Drift(ctx, base, pump, slot, frame_r1, const NonVol& pre, uint32_t nv_mask)
//        void Null(ctx, base, pump, slot, uint32_t reload, uint32_t invariant, bool recovered)
//        bool Check() / void Mismatch(base, pump, slot_value, invariant)
template <class Hooks>
void DrainFixed(PPCContext& ctx, uint8_t* base, Hooks& h) {
  // 76:8211-8219 prologue
  ctx.r12.u64 = ctx.lr;
  ctx.lr = 0x82BB9420;
  h.SaveGpr(ctx, base);
  {
    const uint32_t ea = -128 + ctx.r1.u32;
    PPC_STORE_U32(ea, ctx.r1.u32);
    ctx.r1.u32 = ea;
  }
  const uint64_t frame_r1 = ctx.r1.u64;
  const uint32_t slot = ctx.r1.u32 + 80;
  const uint32_t pump = ctx.r3.u32;
  ctx.r11.u64 = L(base, pump + 132);  // 76:8220
  ctx.r31.u64 = ctx.r3.u64;
  ctx.r29.s64 = 0;
  uint32_t idx = 0, off = 0;
  if (ctx.r11.u32 != 0) {  // 76:8226-8228 cmplwi/ble
    ctx.r30.s64 = 0;
    for (;;) {
      // loc_82BB943C 76:8230-8250: cursor := entry start; skip the entry when end == start
      const uint32_t entry = L(base, pump + 108) + off;
      const uint32_t start = L(base, entry + 16);
      const uint32_t end = L(base, entry + 12);
      S(base, pump + 44, start);
      if (end - L(base, pump + 44) != 0) {
        for (;;) {
          // loc_82BB945C 76:8251-8288
          const uint32_t bank = L(base, pump + 36);
          const uint32_t cur = L(base, pump + 44);
          const uint32_t node = L(base, pump + (bank + 12) * 4) + (cur << 4);
          S(base, slot, node);  // 76:8276 stw r9,80(r1)
          const uint32_t op = L(base, L(base, pump + (L(base, pump + 36) + 12) * 4) + (L(base, pump + 44) << 4));
          if (int32_t(op) == 105) goto done;
          if (int32_t(L(base, node)) == 104) break;
          // 76:8289-8297
          ctx.r29.u64 = idx;
          ctx.r30.u64 = off;
          ctx.r31.u64 = pump;
          NonVol pre;
          Save(ctx, pre);
          ctx.r4.s64 = ctx.r1.s64 + 80;
          ctx.r3.u64 = ctx.r31.u64;
          ctx.lr = 0x82BB94B8;
          h.Interp(ctx, base);
          // difference 1
          const uint32_t nv = Diff(ctx, pre);
          if (ctx.r1.u64 != frame_r1 || nv != 0) {
            h.Drift(ctx, base, pump, slot, frame_r1, pre, nv);
            Restore(ctx, pre);
          }
          // 76:8299-8309, difference 2/3
          uint32_t reload = L(base, slot);
          if (reload < 0x10000u) {
            const uint32_t inv = InvariantNode(base, pump);
            const bool ok = inv >= 0x10000u;
            h.Null(ctx, base, pump, slot, reload, inv, ok);
            if (!ok) goto done;
            reload = inv;
            S(base, slot, reload);
          } else if (h.Check()) {
            const uint32_t inv = InvariantNode(base, pump);
            if (inv != reload) h.Mismatch(base, pump, reload, inv);
          }
          ctx.r10.u64 = L(base, pump + 44);
          ctx.r11.u64 = L(base, reload + 8);
          ctx.r11.s64 = ctx.r11.s64 + 1;
          ctx.r10.u64 = ctx.r10.u64 + ctx.r11.u64;
          S(base, pump + 44, ctx.r10.u32);
        }
      }
      // loc_82BB94D4 76:8318-8327
      ctx.r11.u64 = L(base, pump + 132);
      ++idx;
      off += 68;
      ctx.r29.u64 = idx;
      ctx.r30.u64 = off;
      if (!(idx < ctx.r11.u32)) break;
    }
  }
done:
  // loc_82BB94E8 76:8328-8331: r29..r31 + LR come back from the frame as in the generated epilogue
  ctx.r1.u64 = frame_r1;
  ctx.r1.s64 = ctx.r1.s64 + 128;
  h.RestGpr(ctx, base);
}
}  // namespace g46
