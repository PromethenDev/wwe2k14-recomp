// WWE 2K14 Havok-cloth handshake race fix.
//
// The HavokClothThread (guest thread entry 0x8251ED58) is a persistent worker:
// it loops waiting on the manual-reset "StartCloth" event (+0x68), runs one
// job, sets the manual-reset "EndCloth" event (+0x6c), and waits again. The
// game's begin() (0x8251E6D0) and end() (0x8251E8D0) drive it:
//
//   begin(): [obj+0x64] = 1; NtSetEvent(StartCloth); NtClearEvent(EndCloth);
//   end():   if ([obj+0x58] > 0 && [obj+0x64] != 0) {
//              [obj+0x64] = 0; NtWaitForSingleObjectEx(EndCloth, INFINITE);
//              NtClearEvent(EndCloth); }
//
// On the Xbox 360 the worker cannot complete a job between begin()'s
// SetEvent(StartCloth) and ClearEvent(EndCloth), so clearing EndCloth after
// signalling StartCloth is harmless. On the recompiled build the worker runs
// on another host core: it wakes on StartCloth, clears it, runs the job and
// sets EndCloth all before begin() reaches ClearEvent(EndCloth), which then
// wipes the completion signal. end()'s infinite wait for EndCloth never
// returns, the worker is left waiting on StartCloth, and the game freezes
// forever during the scene/cloth teardown that runs after the Undertaker's
// titantron entrance.
//
// Evidence (logs/ut-instr1/game.log, instrumented runtime; t200373 = game
// main thread, t200399 = HavokClothThread):
//   09:31:51.533 t200373 set   StartCloth
//   09:31:51.534 t200399 set   EndCloth
//   09:31:51.534 t200399 wait-begin StartCloth
//   09:31:51.534 t200373 clear EndCloth      <- wipes the completion signal
//   09:31:51.534 t200373 wait-begin EndCloth <- never returns; run hung
//
// Fix: clear EndCloth BEFORE signalling StartCloth. The worker can then only
// set EndCloth after the job that StartCloth triggers, so the completion
// signal cannot be lost. This is a strong PPC_FUNC override of begin(); the
// generated implementation is a weak alias and is replaced at link time (same
// mechanism as src/present_interval.cpp).

#include <cstdint>

#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace {
// Guest addresses from generated/wwe2k14_recomp.23.cpp:8555-8605
// (PPC_FUNC_IMPL(__imp__sub_8251E6D0)).
constexpr uint32_t kClothSingleton = 0x83763088;
constexpr uint32_t kRunFlag = 100;  // +0x64
constexpr uint32_t kStartEvent = 104;  // +0x68, "StartCloth"
constexpr uint32_t kEndEvent = 108;  // +0x6c, "EndCloth"
}  // namespace

extern "C" PPC_FUNC(__imp__sub_822622E0);  // NtSetEvent(handle)
extern "C" PPC_FUNC(__imp__sub_82C929E0);  // NtClearEvent(handle)

extern "C" PPC_FUNC(sub_8251E6D0) {
  const uint32_t object = PPC_LOAD_U32(kClothSingleton);
  if (object == 0) {
    return;
  }
  const uint32_t start_event = PPC_LOAD_U32(object + kStartEvent);
  if (start_event == 0) {
    return;
  }
  PPC_STORE_U32(object + kRunFlag, 1);

  // Clear the previous completion signal first, then ask the worker to run.
  // ctx.lr values match the guest return addresses the original begin() used
  // (0x8251E710 after the ClearEvent call, 0x8251E708 after the SetEvent call).
  ctx.r3.u32 = PPC_LOAD_U32(object + kEndEvent);
  ctx.lr = 0x8251E710;
  __imp__sub_82C929E0(ctx, base);
  ctx.r3.u32 = PPC_LOAD_U32(object + kStartEvent);
  ctx.lr = 0x8251E708;
  __imp__sub_822622E0(ctx, base);
}

// Second begin() variant, 0x8251E728 (generated/wwe2k14_recomp.23.cpp:8608, reached through the function table): the
// object comes in r3 instead of the singleton, it also stores [obj+0x10] = 1, and after the handshake it calls
// 0x824FB140(obj) and 0x82C3F1B0(obj, 0). Same set-StartCloth-then-clear-EndCloth order, so the same lost completion
// signal (out/night/20261031/jobrace/report.md section 1). Override: identical except EndCloth is cleared first.
// Owner 2026-10-31 hypothesis: the freezes happen on integrated GPUs (780M start hang, UHD 630 LOW_POWER runs stuck).
extern "C" PPC_FUNC(sub_822622E0);
extern "C" PPC_FUNC(sub_82C929E0);
extern "C" PPC_FUNC(sub_824FB140);
extern "C" PPC_FUNC(sub_82C3F1B0);

extern "C" PPC_FUNC(sub_8251E728) {
  // Prologue as generated: save lr and r31, 96-byte frame.
  PPC_STORE_U32(ctx.r1.u32 + -8, static_cast<uint32_t>(ctx.lr));
  PPC_STORE_U64(ctx.r1.u32 + -16, ctx.r31.u64);
  const uint32_t frame = ctx.r1.u32 - 96;
  PPC_STORE_U32(frame, ctx.r1.u32);
  ctx.r1.u32 = frame;
  ctx.r31.u64 = ctx.r3.u64;
  const uint32_t object = ctx.r31.u32;
  PPC_STORE_U32(object + 16, 1);
  if (PPC_LOAD_U32(object + kStartEvent) != 0) {
    PPC_STORE_U32(object + kRunFlag, 1);
    // Clear the previous completion signal first, then ask the worker to run (guest return addresses kept).
    ctx.r3.u64 = PPC_LOAD_U32(object + kEndEvent);
    ctx.lr = 0x8251E760;
    sub_82C929E0(ctx, base);
    ctx.r3.u64 = PPC_LOAD_U32(object + kStartEvent);
    ctx.lr = 0x8251E758;
    sub_822622E0(ctx, base);
  }
  ctx.r3.u64 = ctx.r31.u64;
  ctx.lr = 0x8251E768;
  sub_824FB140(ctx, base);
  ctx.r4.s64 = 0;
  ctx.r3.u64 = ctx.r31.u64;
  ctx.lr = 0x8251E774;
  sub_82C3F1B0(ctx, base);
  // Epilogue as generated.
  ctx.r1.s64 = ctx.r1.s64 + 96;
  ctx.lr = PPC_LOAD_U32(ctx.r1.u32 + -8);
  ctx.r31.u64 = PPC_LOAD_U64(ctx.r1.u32 + -16);
}
