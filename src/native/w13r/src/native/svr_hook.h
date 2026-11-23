// SvR 2008: native renderer hooks that observe the game's D3D calls.
//
// SvR's D3D library stays in charge of every D3D object and all device state: it runs unmodified
// on the null GPU, exactly as in the playable build. A hook runs the game's own function and hands
// the same arguments to the renderer, which mirrors what it sees on the host GPU.
//
// re:Blue replaces these functions outright, which suits Blue Dragon (its resources live in
// re:Blue's own heap). In SvR the objects are the game's: a shader made by the renderer and later
// freed by the game's own Release corrupts the guest heap, and skipped bookkeeping (fences,
// reference counts, device shadows) desynchronises the D3D library.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

#include "recorder.h"

namespace svr {

// WWE 2K15 bring-up switch (cvar svr_hooks_off, native/wwe13_native_stubs.cpp): a comma list of
// hook names (D3DDevice_Swap, ...) whose renderer side is skipped; the game's own D3D function
// always runs. Read once per hook.
bool HookEnabled(const char *name);

// The argument registers of a guest call: r3-r10 (integers and pointers), f1-f8 (floats), and r1
// for arguments passed on the stack (still in the caller's frame after the call returns).
inline void CopyArgs(PPCContext &dst, const PPCContext &src) {
  dst.r1 = src.r1;
  dst.r2 = src.r2;
  dst.r13 = src.r13;
  dst.r3 = src.r3;
  dst.r4 = src.r4;
  dst.r5 = src.r5;
  dst.r6 = src.r6;
  dst.r7 = src.r7;
  dst.r8 = src.r8;
  dst.r9 = src.r9;
  dst.r10 = src.r10;
  dst.f1 = src.f1;
  dst.f2 = src.f2;
  dst.f3 = src.f3;
  dst.f4 = src.f4;
  dst.f5 = src.f5;
  dst.f6 = src.f6;
  dst.f7 = src.f7;
  dst.f8 = src.f8;
}

// Live hook CPU profile (wwe13_hook_profile, default off): time in the game's own D3D function
// (the guest side: device shadows + GPU packets for the emulated GPU) and in the renderer's side,
// per hook, logged every 600 frames as [hook-cpu]. Costs three clock reads per hooked call.
struct HookProf {
  const char *name;
  std::atomic<uint64_t> guest_ns{0}, host_ns{0}, calls{0};
  HookProf *next = nullptr;
};
HookProf *RegisterHookProf(const char *name);
extern bool g_hook_profile;
inline uint64_t HookNow() {
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
}
// Call once per frame (render thread): refreshes g_hook_profile and logs the totals.
void HookProfileTick(uint64_t frame);

// Guest write probe (wwe13_guest_write_probe, default off): which bytes of the D3D device
// (r3 of every D3DDevice_* call, its 0x5FB0 bytes) the game's own function changes, per hook,
// sampled every 8th call. Logged as merged offset ranges ([guest-writes]) when the set grows.
// Tells what a hook could stop running without the renderer losing state it reads back.
extern bool g_guest_probe;
void GuestProbeBegin(const char *name, uint32_t device, const uint8_t *base);
void GuestProbeEnd(const char *name, uint32_t device, const uint8_t *base);

}  // namespace svr

namespace svr::recorder {
// Replay entry for a BEFORE/AFTER hook: re-translate the recorded registers into the
// function's typed arguments exactly as the hook's own HostToGuestFunction call does.
// return_value is ignored (the renderer side of these hooks drops it).
template <auto Fn>
void ReplayCall(PPCContext& args, uint32_t /*return_value*/, uint8_t* base) {
  rex::ppc::HostToGuestFunction<Fn>(args, base);
}
}  // namespace svr::recorder

// The game's function, then the renderer's (with the same arguments; its return value is dropped,
// the game's stays in r3).
#define SVR_HOOK_AFTER(subroutine, function)                     \
  REX_EXTERN(__imp__##subroutine);                               \
  extern "C" REX_FUNC(subroutine) {                              \
    PPCContext svr_args;                                         \
    ::svr::CopyArgs(svr_args, ctx);                              \
    static ::svr::HookProf *const svr_prof = ::svr::RegisterHookProf(#subroutine); \
    const uint64_t svr_t0 = ::svr::g_hook_profile ? ::svr::HookNow() : 0; \
    if (::svr::g_guest_probe)                                    \
      ::svr::GuestProbeBegin(#subroutine, svr_args.r3.u32, base); \
    __imp__##subroutine(ctx, base);                              \
    if (::svr::g_guest_probe)                                    \
      ::svr::GuestProbeEnd(#subroutine, svr_args.r3.u32, base);  \
    const uint64_t svr_t1 = svr_t0 ? ::svr::HookNow() : 0;       \
    ::svr::recorder::RecordHook(#subroutine, svr_args, ctx.r3.u32, false); \
    static const bool svr_on = ::svr::HookEnabled(#subroutine);  \
    if (svr_on)                                                  \
      rex::ppc::HostToGuestFunction<function>(svr_args, base);   \
    if (svr_t0) {                                                \
      svr_prof->guest_ns.fetch_add(svr_t1 - svr_t0, std::memory_order_relaxed); \
      svr_prof->host_ns.fetch_add(::svr::HookNow() - svr_t1, std::memory_order_relaxed); \
      svr_prof->calls.fetch_add(1, std::memory_order_relaxed);   \
    }                                                            \
  }                                                              \
  static const bool _svr_reg_##subroutine =                       \
      ::svr::recorder::RegisterHook(#subroutine,                 \
                                    &::svr::recorder::ReplayCall<function>, \
                                    ::svr::recorder::HookKind::kCall) != nullptr

// The renderer's function (reading the state the call is about to consume), then the game's.
#define SVR_HOOK_BEFORE(subroutine, function)                    \
  REX_EXTERN(__imp__##subroutine);                               \
  extern "C" REX_FUNC(subroutine) {                              \
    PPCContext svr_args;                                         \
    ::svr::CopyArgs(svr_args, ctx);                              \
    const uint32_t svr_device = ctx.r3.u32; /* the renderer's call leaves its result in svr_args.r3 */ \
    static ::svr::HookProf *const svr_prof = ::svr::RegisterHookProf(#subroutine); \
    const uint64_t svr_t0 = ::svr::g_hook_profile ? ::svr::HookNow() : 0; \
    ::svr::recorder::RecordHook(#subroutine, svr_args, ctx.r3.u32, false); \
    static const bool svr_on = ::svr::HookEnabled(#subroutine);  \
    if (svr_on)                                                  \
      rex::ppc::HostToGuestFunction<function>(svr_args, base);   \
    const uint64_t svr_t1 = svr_t0 ? ::svr::HookNow() : 0;       \
    if (::svr::g_guest_probe)                                    \
      ::svr::GuestProbeBegin(#subroutine, svr_device, base);     \
    __imp__##subroutine(ctx, base);                              \
    if (::svr::g_guest_probe)                                    \
      ::svr::GuestProbeEnd(#subroutine, svr_device, base);       \
    if (svr_t0) {                                                \
      svr_prof->host_ns.fetch_add(svr_t1 - svr_t0, std::memory_order_relaxed); \
      svr_prof->guest_ns.fetch_add(::svr::HookNow() - svr_t1, std::memory_order_relaxed); \
      svr_prof->calls.fetch_add(1, std::memory_order_relaxed);   \
    }                                                            \
  }                                                              \
  static const bool _svr_reg_##subroutine =                       \
      ::svr::recorder::RegisterHook(#subroutine,                 \
                                    &::svr::recorder::ReplayCall<function>, \
                                    ::svr::recorder::HookKind::kCall) != nullptr

// The game's function, then the renderer's with the arguments and the game's return value (r3).
#define SVR_HOOK_RESULT(subroutine, function)                    \
  REX_EXTERN(__imp__##subroutine);                               \
  extern "C" REX_FUNC(subroutine) {                              \
    PPCContext svr_args;                                         \
    ::svr::CopyArgs(svr_args, ctx);                              \
    static ::svr::HookProf *const svr_prof = ::svr::RegisterHookProf(#subroutine); \
    const uint64_t svr_t0 = ::svr::g_hook_profile ? ::svr::HookNow() : 0; \
    if (::svr::g_guest_probe)                                    \
      ::svr::GuestProbeBegin(#subroutine, svr_args.r3.u32, base); \
    __imp__##subroutine(ctx, base);                              \
    if (::svr::g_guest_probe)                                    \
      ::svr::GuestProbeEnd(#subroutine, svr_args.r3.u32, base);  \
    const uint64_t svr_t1 = svr_t0 ? ::svr::HookNow() : 0;       \
    ::svr::recorder::RecordHook(#subroutine, svr_args, ctx.r3.u32, true); \
    static const bool svr_on = ::svr::HookEnabled(#subroutine);  \
    if (svr_on)                                                  \
      function(svr_args, ctx.r3.u32, base);                      \
    if (svr_t0) {                                                \
      svr_prof->guest_ns.fetch_add(svr_t1 - svr_t0, std::memory_order_relaxed); \
      svr_prof->host_ns.fetch_add(::svr::HookNow() - svr_t1, std::memory_order_relaxed); \
      svr_prof->calls.fetch_add(1, std::memory_order_relaxed);   \
    }                                                            \
  }                                                              \
  static const bool _svr_reg_##subroutine =                       \
      ::svr::recorder::RegisterHook(#subroutine,                 \
                                    function,                   \
                                    ::svr::recorder::HookKind::kResult) != nullptr
