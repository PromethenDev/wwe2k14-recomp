// Native Clear tree (native_clear.h). Live policy: guest memory; the six guest callees run through their (hooked)
// symbols with the guest return address and r1 = the 83F0 frame (caller r1 - 528), exactly as the host test runs them.
#include "native_clear.h"
#include "native_clear_core.h"

#include <cstdlib>
#include <cstring>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "native_setters_core.h"

extern "C" {
PPC_FUNC(sub_82C7C8F0);
PPC_FUNC(sub_82CB7E50);
PPC_FUNC(sub_82CC0D18);
PPC_FUNC(sub_82CBA798);
PPC_FUNC(sub_82CAC5C0);
PPC_FUNC(sub_82CC0988);
}

namespace wwe2k14::d3d::native_clear {
namespace {

using Fn = void (*)(PPCContext&, uint8_t*);
Fn Callee(uint32_t a) {
  switch (a) {
    case 0x82C7C8F0u: return sub_82C7C8F0;
    case 0x82CB7E50u: return sub_82CB7E50;
    case 0x82CC0D18u: return sub_82CC0D18;
    case 0x82CBA798u: return sub_82CBA798;
    case 0x82CAC5C0u: return sub_82CAC5C0;
    case 0x82CC0988u: return sub_82CC0988;
    default: return nullptr;
  }
}

struct GuestM {
  PPCContext& ctx;
  uint8_t* base;
  uint32_t sp;
  uint8_t L8(uint32_t a) { return PPC_LOAD_U8(a); }
  uint16_t L16(uint32_t a) { return PPC_LOAD_U16(a); }
  uint32_t L32(uint32_t a) { return PPC_LOAD_U32(a); }
  uint64_t L64(uint32_t a) { return PPC_LOAD_U64(a); }
  void S8(uint32_t a, uint8_t v) { PPC_STORE_U8(a, v); }
  void S16(uint32_t a, uint16_t v) { PPC_STORE_U16(a, v); }
  void S32(uint32_t a, uint32_t v) { PPC_STORE_U32(a, v); }
  void S64(uint32_t a, uint64_t v) { PPC_STORE_U64(a, v); }
  uint64_t Run(uint32_t addr, uint32_t lr, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8,
               uint64_t r9, uint64_t r10) {
    ctx.r1.u64 = sp;
    ctx.r3.u64 = r3, ctx.r4.u64 = r4, ctx.r5.u64 = r5, ctx.r6.u64 = r6;
    ctx.r7.u64 = r7, ctx.r8.u64 = r8, ctx.r9.u64 = r9, ctx.r10.u64 = r10;
    ctx.lr = lr;
    if (Fn fn = Callee(addr)) fn(ctx, base);
    return ctx.r3.u64;
  }
  uint64_t Call(uint32_t addr, uint32_t lr, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8,
                uint64_t r9, uint64_t r10) {
    return Run(addr, lr, r3, r4, r5, r6, r7, r8, r9, r10);
  }
  uint64_t CallF(uint32_t addr, uint32_t lr, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8,
                 uint64_t r9, uint64_t r10, uint64_t f1_bits) {
    ctx.fpscr.disableFlushMode();
    ctx.f1.u64 = f1_bits;
    return Run(addr, lr, r3, r4, r5, r6, r7, r8, r9, r10);
  }
};

struct Config {
  int mode = 0;
  uint32_t ab = 0;
};
const Config& Cfg() {
  static const Config c = [] {
    Config k;
#if !defined(__ANDROID__) && !defined(REX_UWP)
    // Default ON on desktop since 11-19: host test 50,000 states 0 mismatches (8 planted bugs caught); smoke match
    // nclear-on-1 PASS (~29 calls/f native, picture normal); in-run A/B nclear-ab-1: all threads +0.03 ms/f, 7 of 13
    // pairs faster = no clear difference. WWE2K14_D3D_NATIVE_CLEAR=0 restores the guest bodies. Android / Xbox: guest.
    k.mode = 1;
#endif
    if (const char* e = std::getenv("WWE2K14_D3D_NATIVE_CLEAR"); e && *e) k.mode = std::atoi(e) == 1 ? 1 : 0;
    if (const char* e = std::getenv("WWE2K14_D3D_NATIVE_CLEAR_AB"); e && *e) k.ab = uint32_t(std::strtoul(e, nullptr, 10));
    if (k.mode) REXLOG_INFO("[d3d-native-clear] ON (native body) ab={}", k.ab);
    return k;
  }();
  return c;
}

}  // namespace

bool Body(PPCContext& ctx, uint8_t* base, void (*)(PPCContext&, uint8_t*), uint32_t frame) {
  const Config& cfg = Cfg();
  if (cfg.mode != 1 || !native_setters::AbNewArm(frame, cfg.ab)) return false;
  const uint32_t caller_sp = ctx.r1.u32, D = ctx.r3.u32;
  GuestM m{ctx, base, caller_sp - 528u};
  // r6 is passed whole (64-bit): the body hands it on to 82CB7E50 in r4 (host-test finding).
  Clear(m, caller_sp, D, ctx.r4.u32, ctx.r5.u32, ctx.r6.u64, ctx.r7.u32, ctx.r9.u32, ctx.f1.u64);
  ctx.r1.u64 = caller_sp;
  ctx.r3.u64 = D;  // the guest body returns r3 = D (24236-24244); one caller (82BB7CC8 area) returns it on
  return true;
}

}  // namespace wwe2k14::d3d::native_clear
