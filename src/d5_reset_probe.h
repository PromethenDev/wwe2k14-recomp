// Shared state for the D5 reset-race probe (docs/cards/D5-pump-delay-test.md, H-D5a).
// Diagnostic only, default off; see src/d5_reset_probe.cpp for the full note.
//
// The drain window is marked by the sub_82BB9418 override in src/cage_hang_probe.cpp and
// read by the reset-path overrides in src/d5_reset_probe.cpp, so both sides live here.
#pragma once

#include <atomic>
#include <cstdint>

// 1 while the pump drain (sub_82BB9418) runs on g_d5_drain_tid, else 0. Only written
// when WWE2K14_DIAG_D5_RESET=1; never touched otherwise.
extern std::atomic<int> g_d5_drain_active;
extern std::atomic<uint32_t> g_d5_drain_tid;
extern std::atomic<uint32_t> g_d5_drain_count;  // drain entries (nested entries included)

namespace d5probe {

// WWE2K14_DIAG_D5_RESET=1, read once. Off: the overrides only call __imp__ (one branch).
// Also true when WWE2K14_D5_SERIALIZE=1 (the drain window must be tracked for the serialisation).
bool Enabled();
// Card D5 fix candidate (WWE2K14_D5_SERIALIZE=1): a reset path entered on another thread while the pump drain runs
// waits (bounded, 100 ms) until that drain has finished.
bool Serialize();

// Host thread id of the calling thread (SYS_gettid on Linux, same style as
// src/thread_profiler.cpp:87; std::hash<std::thread::id> elsewhere).
uint32_t HostThreadId();

// Logs the 30 s summary line when 30 s have passed since the last one. Called from the
// gated overrides; no-op cost when the gate is off is not paid (callers check Enabled()).
void MaybeLogSummary();

}  // namespace d5probe
