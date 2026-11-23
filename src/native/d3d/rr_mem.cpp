// RR1: live guest-memory recording (rr_mem.h).
#include "rr_mem.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <vector>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "capture.h"
#include "../nr/nr_backend.h"
#include "../nr/nr_shared.h"
#include "rr_mem_core.h"
#include "shader_identity.h"
#include "textures.h"
#include "sidefile.h"

namespace wwe2k14::d3d::rr {

namespace detail {
std::atomic<bool> g_active{false};
}

namespace {

constexpr size_t kFlushBytes = 1u << 20;

struct Rec {
  std::mutex mu;
  PageDedupe dedupe;
  std::vector<uint8_t> buf;
  std::atomic<uint32_t> seq{0};
  uint32_t frame = 0;
  bool started = false, was_active = false, logged_end = false;
  uint64_t reads = 0, invalidations = 0, bytes = 0;
  const uint8_t* phys_base = nullptr;
  CoveredFn covered = nullptr;  // RR2 replay check mode
  uint64_t uncovered_reads = 0;
  std::unordered_set<uint64_t> uncovered_pages;
  std::unordered_set<uint64_t> shaders_written;  // (stage << 63) ^ fnv
  uint64_t shaders = 0;
  bool snapshotted = false;
};

void DumpShadersLocked(Rec& r);

Rec& R() {
  static Rec* r = [] {
    auto* x = new Rec();  // leaked: used from atexit
    x->buf.reserve(kFlushBytes + kMemBody + 64);
    std::atexit([] {
      Rec& q = R();
      std::lock_guard<std::mutex> lk(q.mu);
      if (q.snapshotted) DumpShadersLocked(q);  // review 11-18: shaders first seen in the window, if the run ended early
      if (!q.buf.empty()) sidefile::RecordRaw(q.buf.data(), q.buf.size());
      q.buf.clear();
    });
    return x;
  }();
  return *r;
}

void FlushLocked(Rec& r) {
  if (r.buf.empty()) return;
  sidefile::RecordRaw(r.buf.data(), r.buf.size());
  r.bytes += r.buf.size();
  r.buf.clear();
}

const uint8_t* PagePtr(Rec& r, uint32_t space, uint32_t page) {
  if (space == kPhysical) {
    if (!r.phys_base) {
      auto* ks = rex::system::kernel_state();
      if (ks && ks->memory()) r.phys_base = ks->memory()->TranslatePhysical<const uint8_t*>(0);
    }
    return r.phys_base ? r.phys_base + page : nullptr;
  }
  const uint8_t* b = ShadowGuestBase();
  return b && GuestRangeOk(page, kPageBytes) ? b + page : nullptr;
}

// RR4: the shader table (captured on the game thread, not in events) into the recording; each shader once.
void DumpShadersLocked(Rec& r) {
  ShaderForEachRaw([&](uint32_t stage, uint64_t fnv, const uint8_t* raw, uint32_t bytes) {
    if (!r.shaders_written.insert((uint64_t(stage) << 63) ^ fnv).second) return;
    EncodeShd(&r.buf, stage, fnv, raw, bytes);
    ++r.shaders;
  });
}

// Host-readable [begin, end) ranges overlapping [lo, hi) (the OS view: guest protect alone may not match host protect).
std::vector<std::pair<uintptr_t, uintptr_t>> HostReadable(uintptr_t lo, uintptr_t hi) {
  std::vector<std::pair<uintptr_t, uintptr_t>> out;
#ifdef _WIN32
  for (uintptr_t a = lo; a < hi;) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const DWORD pr = mbi.Protect & 0xFF;
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && pr != PAGE_NOACCESS && pr != 0;
    if (readable) out.emplace_back(std::max(a, lo), std::min(end, hi));
    a = end;
  }
#else
  if (FILE* f = std::fopen("/proc/self/maps", "r")) {
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
      unsigned long long b = 0, e = 0;
      char perms[8] = {};
      if (std::sscanf(line, "%llx-%llx %7s", &b, &e, perms) != 3 || perms[0] != 'r') continue;
      if (e <= lo || b >= hi) continue;
      out.emplace_back(std::max<uintptr_t>(uintptr_t(b), lo), std::min<uintptr_t>(uintptr_t(e), hi));
    }
    std::fclose(f);
  }
#endif
  return out;
}

// RR4: every non-zero physical page once at window open (kPsnTag comment, rr_mem_core.h). Worker thread, r.mu held.
struct SnapResult {
  uint64_t pages = 0, unreadable = 0;
};
SnapResult SnapshotPhysicalLocked(Rec& r, rex::memory::Memory* mem) {
  SnapResult res;
  if (mem && !r.phys_base) r.phys_base = mem->TranslatePhysical<const uint8_t*>(0);
  auto* heap = mem ? mem->GetPhysicalHeap() : nullptr;
  if (!r.phys_base || !heap) return res;
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t seq = r.seq.load(std::memory_order_relaxed);
  static const uint8_t zero[kPageBytes] = {};
  const uint64_t zero_hash = PageHash(zero);
  uint64_t pages = 0, unreadable = 0;
  const uintptr_t base = reinterpret_cast<uintptr_t>(r.phys_base);
  const auto host_ok = HostReadable(base, base + kPhysSnapshotEnd);
  size_t hi_idx = 0;
  for (uint32_t p = 0; p < kPhysSnapshotEnd; p += kPageBytes) {
    const uintptr_t hp = base + p;
    while (hi_idx < host_ok.size() && host_ok[hi_idx].second <= hp) ++hi_idx;
    if (hi_idx >= host_ok.size() || hp < host_ok[hi_idx].first || hp + kPageBytes > host_ok[hi_idx].second) {
      ++unreadable;
      continue;
    }
    // RR4 run 2 crashed here: released / no-access guest pages are not host-readable (si_addr phys 0x1FC90000). Only
    // pages the guest page table marks readable are touched (the worker cannot have read the others either).
    if (heap->QueryRangeAccess(p, p + kPageBytes - 1) == rex::memory::PageAccess::kNoAccess) {
      ++unreadable;
      continue;
    }
    const uint8_t* host = r.phys_base + p;
    if (std::memcmp(host, zero, kPageBytes) == 0) {
      r.dedupe.Seed(kPhysical, p, zero_hash);
      continue;
    }
    r.dedupe.Touch(kPhysical, p, kPageBytes, [&](uint32_t, uint32_t q) { return r.phys_base + q; },
                   [&](uint32_t s, uint32_t q, const uint8_t* bytes) { EncodeMem(&r.buf, seq, s, q, bytes); });
    ++pages;
    if (r.buf.size() >= kFlushBytes) FlushLocked(r);
  }
  uint8_t marker[12];
  std::memcpy(marker, &kPsnTag, 4);
  const uint32_t four = 4;
  std::memcpy(marker + 4, &four, 4);
  std::memcpy(marker + 8, &seq, 4);
  r.buf.insert(r.buf.end(), marker, marker + 12);
  FlushLocked(r);
  REXLOG_INFO("[rr-mem] physical snapshot at seq {}: {} non-zero pages ({} MB), {} unreadable skipped, in {} ms", seq, pages,
              (pages * kPageBytes) >> 20, unreadable,
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  res.pages = pages;
  res.unreadable = unreadable;
  return res;
}

// RR5: the resolve registry (images made before the window, GPU-only) at window open. Worker thread inside its
// dispatch batch (backend lock held), r.mu held.
void SaveResolveRegistryLocked(Rec& r) {
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t saved = 0, failed = 0, bytes = 0;
  std::string first_error;
  textures::ForEachResolveEntry([&](uint32_t start, uint32_t length, const ResolveDesc& desc, uint32_t snap,
                                    const nr::backend::ResolveCensusKey& key) {
    uint32_t format = 0, w = 0, h = 0;
    std::vector<uint8_t> px;
    std::string err;
    if (!nr::backend::ReadSnapshotRaw(snap, &format, &w, &h, px, &err)) {
      if (!failed++) first_error = err;
      return;
    }
    EncodeRim(&r.buf, start, length, format, w, h, &desc, uint32_t(sizeof desc), &key, uint32_t(sizeof key), px.data(),
              px.size());
    ++saved;
    bytes += px.size();
    if (r.buf.size() >= kFlushBytes) FlushLocked(r);
  });
  FlushLocked(r);
  REXLOG_INFO("[rr-mem] resolve registry: {} images saved ({} MB), {} not saved{}{} in {} ms", saved, bytes >> 20, failed,
              failed ? ": " : "", first_error,
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

void Touch(Rec& r, uint32_t space, uint32_t addr, uint64_t len) {
  std::lock_guard<std::mutex> lk(r.mu);
  ++r.reads;
  if (r.covered) {
    bool miss = false;
    const uint64_t end = uint64_t(addr) + len;
    for (uint64_t p = addr & ~uint64_t(kPageBytes - 1); p < end; p += kPageBytes) {
      if (r.covered(space, uint32_t(p))) continue;
      miss = true;
      if (r.uncovered_pages.insert((uint64_t(space) << 32) | p).second && r.uncovered_pages.size() <= 24)
        REXLOG_WARN("[rr-play] uncovered read: {} page 0x{:08X} (read 0x{:08X} +{}) at seq {}",
                    space == kPhysical ? "physical" : "virtual", uint32_t(p), addr, len, r.seq.load());
    }
    r.uncovered_reads += miss;
    return;
  }
  const uint32_t seq = r.seq.load(std::memory_order_relaxed);
  r.dedupe.Touch(space, addr, len, [&](uint32_t s, uint32_t p) { return PagePtr(r, s, p); },
                 [&](uint32_t s, uint32_t p, const uint8_t* bytes) { EncodeMem(&r.buf, seq, s, p, bytes); });
  if (r.buf.size() >= kFlushBytes) FlushLocked(r);
}

}  // namespace

void SetCurrent(uint32_t seq, uint32_t frame) {
  static const bool enabled = [] {
    const bool on = sidefile::MemoryRecordEnabled();
    if (on) nr::shared::SetReadObserver(&NoteHost);  // texture detile / hash reads
    return on;
  }();
  if (!enabled) {
    if (Active()) R().seq.store(seq, std::memory_order_relaxed);  // RR2 replay check: log lines carry the seq
    return;
  }
  Rec& r = R();
  r.seq.store(seq, std::memory_order_relaxed);
  if (r.started && frame == r.frame) return;
  r.started = true;
  r.frame = frame;
  const bool active = sidefile::InWindow(frame);
  if (active != r.was_active) {
    std::lock_guard<std::mutex> lk(r.mu);
    DumpShadersLocked(r);  // at window open (the table so far) and close (shaders first seen inside the window)
    if (active && !r.was_active && !r.snapshotted) {
      r.snapshotted = true;
      auto* ks = rex::system::kernel_state();
      SnapshotPhysicalLocked(r, ks ? ks->memory() : nullptr);
      SaveResolveRegistryLocked(r);
    }
    FlushLocked(r);
    if (!active && r.was_active && !r.logged_end) {
      r.logged_end = true;
      REXLOG_INFO("[rr-mem] window closed at frame {}: reads={} pages_written={} unchanged={} unmapped={} distinct={} "
                  "invalidations={} shaders={} mb={}",
                  frame, r.reads, r.dedupe.written(), r.dedupe.unchanged(), r.dedupe.unmapped(), r.dedupe.distinct_pages(),
                  r.invalidations, r.shaders, r.bytes >> 20);
    }
    r.was_active = active;
  }
  detail::g_active.store(active, std::memory_order_relaxed);
}

void NoteReadSlow(uint32_t guest_addr, uint64_t len) {
  uint32_t space, addr;
  if (!len || !Normalize(guest_addr, &space, &addr)) return;
  Touch(R(), space, addr, len);
}

void NotePhysSlow(uint32_t phys, uint64_t len) {
  if (!len) return;
  Touch(R(), kPhysical, phys & 0x1FFFFFFFu, len);
}

void NoteHostSlow(const uint8_t* host, uint64_t len) {
  if (!host || !len) return;
  Rec& r = R();
  if (!r.phys_base) PagePtr(r, kPhysical, 0);  // resolves the physical base
  const uintptr_t h = reinterpret_cast<uintptr_t>(host);
  if (r.phys_base && h >= reinterpret_cast<uintptr_t>(r.phys_base) && h - reinterpret_cast<uintptr_t>(r.phys_base) < 0x20000000u) {
    Touch(r, kPhysical, uint32_t(h - reinterpret_cast<uintptr_t>(r.phys_base)), len);
    return;
  }
  const uint8_t* vb = ShadowGuestBase();
  if (vb && h >= reinterpret_cast<uintptr_t>(vb) && h - reinterpret_cast<uintptr_t>(vb) < 0x100000000ull)
    NoteReadSlow(uint32_t(h - reinterpret_cast<uintptr_t>(vb)), len);
}

void NoteInvalidate(uint32_t start, uint32_t length) {
  if (!Active()) return;
  Rec& r = R();
  std::lock_guard<std::mutex> lk(r.mu);
  ++r.invalidations;
  if (r.covered) return;
  EncodeInv(&r.buf, r.seq.load(std::memory_order_relaxed), start, length);
}

bool SnapshotSelfTest(rex::memory::Memory* mem, std::string* report) {
  // A guest physical block: 4 pages of data, the middle 2 then made no-access (as a released / protected region).
  auto* heap = mem->GetPhysicalHeap();
  uint32_t addr = 0;
  if (!heap->Alloc(16 * kPageBytes, kPageBytes, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                   rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &addr)) {
    *report = "alloc failed";
    return false;
  }
  uint8_t* host = mem->TranslatePhysical<uint8_t*>(addr);
  std::memset(host, 0x5A, 4 * kPageBytes);
  heap->Protect(addr + kPageBytes, 2 * kPageBytes, rex::memory::kMemoryProtectNoAccess);
  // Page 3: host no-access only (guest table still says readable) - must be caught by the OS-level check.
  rex::memory::Protect(host + 3 * kPageBytes, kPageBytes, rex::memory::PageAccess::kNoAccess, nullptr);
  Rec scratch;  // not the recorder: nothing reaches a file
  const SnapResult a = SnapshotPhysicalLocked(scratch, mem);
  // Expect exactly page addr (non-zero, readable); pages 1-2 (guest no-access) and 3 (host no-access) skipped.
  const bool ok = a.pages == 1 && a.unreadable >= 3;
  char line[200];
  std::snprintf(line, sizeof line, "snapshot self-test: block 0x%08X pages=%llu unreadable=%llu records=%zu bytes -> %s", addr,
                (unsigned long long)a.pages, (unsigned long long)a.unreadable, scratch.buf.size(), ok ? "PASS" : "FAIL");
  *report = line;
  rex::memory::Protect(host + 3 * kPageBytes, kPageBytes, rex::memory::PageAccess::kReadWrite, nullptr);
  heap->Protect(addr, 16 * kPageBytes, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  std::memset(host, 0, 4 * kPageBytes);
  return ok;
}

void BeginReplayCheck(const uint8_t* phys_base, CoveredFn covered) {
  Rec& r = R();
  {
    std::lock_guard<std::mutex> lk(r.mu);
    r.phys_base = phys_base;
    r.covered = covered;
  }
  nr::shared::SetReadObserver(&NoteHost);
  detail::g_active.store(true, std::memory_order_relaxed);
}

ReplayCheckStats GetReplayCheckStats() {
  Rec& r = R();
  std::lock_guard<std::mutex> lk(r.mu);
  return ReplayCheckStats{r.reads, r.uncovered_reads, uint64_t(r.uncovered_pages.size())};
}

Stats GetStats() {
  Rec& r = R();
  std::lock_guard<std::mutex> lk(r.mu);
  return Stats{r.reads, r.dedupe.written(), r.dedupe.unchanged(), r.dedupe.unmapped(), r.invalidations, r.bytes};
}

}  // namespace wwe2k14::d3d::rr
