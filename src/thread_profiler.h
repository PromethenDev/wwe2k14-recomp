// Ported Linux host-thread sampler. Environment names and WWE13TP1 output are
// retained for compatibility with the shared SDK/WWE 13 profiling tools.
#pragma once

namespace wwe2k14 {

void StartThreadProfilerFromEnv();
void StopThreadProfiler();

}  // namespace wwe2k14
