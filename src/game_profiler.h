// TEST-only continuous per-thread profiler (lane all-1104, owner 2026-11-04: in-match slowdown hunt on the 780M laptop).
// Off unless TEST-PROFILE.txt exists next to the exe (or WWE2K14_PROFILE=1). Windows only; elsewhere a no-op.
// Once per second it logs "[prof]" lines: CPU time of every thread of the process (GetThreadTimes) and, for threads
// that were busy in the previous second, what they were running (leaf PC + exe return-address candidates, sampled
// every 20 ms with the hitch catcher's suspend discipline). Map exe offsets with out/symbols/<build>/ publics.
#pragma once

namespace wwe2k14::prof {

void Start();  // idempotent; called from the first guest present

}  // namespace wwe2k14::prof
