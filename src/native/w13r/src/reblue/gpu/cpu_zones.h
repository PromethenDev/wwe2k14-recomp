// Where the render thread's time goes, per frame, in the [perf] log line (present.cpp).
// The game's draws, the constants, geometry and textures they need, and Present. Testers' logs
// then tell what to speed up on their phones (a profiler is out of their reach). Zones nest
// (constants are inside draws): each one is a total of its own. Android only: the PC version
// has Tracy (core/profiling.h) and no need for it in players' logs.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace bd::gpu::zones {

enum Id { kDraws, kConstants, kGeometry, kTextures, kPresent, kCount };

inline std::atomic<uint64_t> g_ns[kCount];

struct Scope {
  Id id;
  std::chrono::steady_clock::time_point t0;
  explicit Scope(Id i) : id(i), t0(std::chrono::steady_clock::now()) {}
  ~Scope() {
    g_ns[id].fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - t0).count()),
                       std::memory_order_relaxed);
  }
};

}  // namespace bd::gpu::zones

#if defined(__ANDROID__)
#define WWE12_CPU_ZONE(id) ::bd::gpu::zones::Scope wwe12_cpu_zone_##id(::bd::gpu::zones::id)
#else
#define WWE12_CPU_ZONE(id) ((void)0)
#endif
