// RR1 (docs/tasks/RR-record-replay.md): live guest-memory recording for the D3D side-file. Format and dedupe:
// rr_mem_core.h. Off (one relaxed load per call) unless the side-file records (WWE2K14_D3D_SIDEFILE_OUT) with memory on.
//
// Every worker-side reader of guest memory calls NoteRead / NotePhys with the exact range it is about to read; the
// worker loop calls SetCurrent before each event; the texture write-watch callback calls NoteInvalidate.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace rex::memory {
class Memory;
}

namespace wwe2k14::d3d::rr {

namespace detail {
extern std::atomic<bool> g_active;  // recording memory AND the worker's current event lies inside the window
}

// Worker loop, before dispatching an event.
void SetCurrent(uint32_t seq, uint32_t frame);
inline bool Active() { return detail::g_active.load(std::memory_order_relaxed); }

void NoteReadSlow(uint32_t guest_addr, uint64_t len);
void NotePhysSlow(uint32_t phys, uint64_t len);
// A read of [guest_addr, guest_addr + len) through the guest virtual base (0xA0000000+ = physical views).
inline void NoteRead(uint32_t guest_addr, uint64_t len) {
  if (Active()) NoteReadSlow(guest_addr, len);
}
// A read of physical [phys, phys + len) (Memory::TranslatePhysical).
inline void NotePhys(uint32_t phys, uint64_t len) {
  if (Active()) NotePhysSlow(phys, len);
}
// A read through a host pointer into guest memory (physical or virtual mapping; other pointers are ignored).
void NoteHostSlow(const uint8_t* host, uint64_t len);
inline void NoteHost(const uint8_t* host, uint64_t len) {
  if (Active()) NoteHostSlow(host, len);
}
// The physical write-watch fired for [start, start + length) (any thread).
void NoteInvalidate(uint32_t start, uint32_t length);

// Counters for the end-of-recording log line.
struct Stats {
  uint64_t reads, pages_written, pages_unchanged, pages_unmapped, invalidations, bytes;
};
Stats GetStats();

// RR2 offline replay: the same read hooks check the replay image instead of recording. `covered(space, page)` says
// whether a recorded page was applied there; reads touching other pages are counted (the recording missed a read site
// or a page). Turns the hooks on for the rest of the process.
// RR4 desk check (replay process, standalone Memory): the window-open physical snapshot skips no-access pages without
// faulting. True = PASS; *report = one line.
bool SnapshotSelfTest(rex::memory::Memory* mem, std::string* report);

using CoveredFn = bool (*)(uint32_t space, uint32_t page);
void BeginReplayCheck(const uint8_t* phys_base, CoveredFn covered);
struct ReplayCheckStats {
  uint64_t reads, uncovered_reads, uncovered_pages;
};
ReplayCheckStats GetReplayCheckStats();

}  // namespace wwe2k14::d3d::rr
