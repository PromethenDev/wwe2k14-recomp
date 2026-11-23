/**
 * @file    core/threading.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause - see LICENSE
 */
#include "core/threading.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "core/logging.h"

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/system/xthread.h>
#include <rex/types.h>

#if defined(_WIN32)
#include "core/windows_lean.h"
#include <timeapi.h>
#else
#include <sys/resource.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

// WWE '12: the game's threads wait in short guest sleeps (KeDelayExecutionThread), most of all
// while loading. Without a 1 ms timer period Windows rounds every one of them up to its 15.6 ms
// tick. Blue Dragon raised the period from its Sleep hook; nothing in WWE '12 reaches that hook, so
// the app can raise it at startup (Wwe12App::OnPreSetup). Off by default: measured on 2026-10-12,
// match loading took the same ~12 s with and without it (the game's loading waits are by design).
REXCVAR_DEFINE_BOOL(wwe12_precise_timer, false, "WWE12",
                    "Ask Windows for 1 ms timer precision, so the game's short waits (loading) "
                    "aren't stretched to 15.6 ms.");

namespace {
std::once_flag g_timer_init;

#if defined(__x86_64__) || defined(_M_X64)
inline void CpuPause() { _mm_pause(); }
#else
inline void CpuPause() { std::this_thread::yield(); }
#endif
} // namespace

namespace bd {

void EnableHighResTimer() {
  std::call_once(g_timer_init, [] {
#if defined(_WIN32)
    timeBeginPeriod(1);
#endif
  });
}

void DisableHighResTimer() {
#if defined(_WIN32)
  timeEndPeriod(1);
#endif
}

void DemoteThreadToBackground() {
#if defined(_WIN32)
  ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#else
  // who=0 is the calling THREAD on Linux, not the process.
  ::setpriority(PRIO_PROCESS, 0, 10);
#endif
}

void TerminateProcessNow(int exit_code) {
#if defined(_WIN32)
  ::TerminateProcess(::GetCurrentProcess(),
                     static_cast<unsigned int>(exit_code));
  // TerminateProcess can return before the caller halts, so spin and nothing
  // runs past here.
  for (;;) {
    CpuPause();
  }
#else
  std::_Exit(exit_code);
#endif
}

} // namespace bd

// PPC kernel bypass for Sleep.
u32 Sleep_hook(u32 ms) {
  bd::EnableHighResTimer();

  if (ms == 0) {
    std::this_thread::yield();
    return 0;
  }

  auto target =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(u32(ms));

  if (ms >= 2) {
    std::this_thread::sleep_for(std::chrono::milliseconds(u32(ms)) -
                                std::chrono::microseconds(1500));
  } else {
    std::this_thread::yield();
  }

  while (std::chrono::steady_clock::now() < target)
    CpuPause();

  return 0;
}
#if !defined(WWE2K14_W13R)  // w13r: SDK 0.7.3 has no rex_ kernel-export hook names; 2K14 keeps its own sleep fix
REX_HOOK(rex_Sleep, Sleep_hook);
#endif

// PPC kernel bypass for NtSuspendThread.
u32 NtSuspendThread_hook(u32 handle) {
  auto thread =
      REX_KERNEL_OBJECTS()->LookupObject<rex::system::XThread>(handle);
  if (thread)
    thread->Suspend();
  return 0;
}
#if !defined(WWE2K14_W13R)  // w13r: SDK 0.7.3 has no rex_ kernel-export hook names; 2K14 keeps its own sleep fix
REX_HOOK(rex_NtSuspendThread, NtSuspendThread_hook);
#endif

// PPC kernel bypass for ResumeThread.
u32 ResumeThread_hook(u32 handle) {
  auto thread =
      REX_KERNEL_OBJECTS()->LookupObject<rex::system::XThread>(handle);
  if (thread)
    thread->Resume();
  return 0;
}
#if !defined(WWE2K14_W13R)  // w13r: SDK 0.7.3 has no rex_ kernel-export hook names; 2K14 keeps its own sleep fix
REX_HOOK(rex_ResumeThread, ResumeThread_hook);
#endif
