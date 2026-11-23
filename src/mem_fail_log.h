// Caller-named allocation-failure log + below-pool lender rules (T2 MEM-NEVERFAIL, 2026-11-18). Pure C++ without the
// SDK so the rules are host-tested (src/native/tests/mem_fail_log_test.cpp); used by memory_share.cpp and
// pool_fail_log.cpp. "N:line" = generated/wwe2k14_recomp.N.cpp.
//
// Guest call chain: at an allocator hook's entry ctx.lr = the return address in the direct caller and ctx.r1 = that
// caller's stack pointer after its `stwu r1,-N(r1)`. Every non-leaf function saves its own LR at -8 of its ENTRY stack
// pointer (`mflr r12; stw r12,-8(r1)` 0:58, or inside __savegprlr_N, e.g. 82:60185 + 39 lines) and its frame's word 0
// is the back chain (= its entry stack pointer). So frame k's return address = *(back_k - 8) with back_k =
// *(back_{k-1}), back_0 = r1. Frameless leaf callers are skipped by this walk (their caller appears instead); the first
// word is always ctx.lr. Failure path only: nothing here runs while allocations succeed.
#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace wwe2k14::mem::faillog {

constexpr int kFrames = 5;
constexpr uint32_t kCodeLo = 0x82000000u;  // sanity bound for a return address (image base 0x82000000)
constexpr uint32_t kCodeHi = 0x84000000u;
constexpr uint32_t kMaxFrameBytes = 1u << 20;  // a back chain jumping more than 1 MiB is not a stack frame

struct Chain {
  uint32_t ret[kFrames] = {};
  int n = 0;
};

// load(addr) -> big-endian u32 at guest addr; readable(addr, bytes) -> true when those guest bytes may be read.
template <class Load, class Readable>
Chain Walk(uint32_t lr, uint32_t sp, Load&& load, Readable&& readable) {
  Chain c;
  c.ret[0] = lr;
  c.n = 1;
  for (int k = 1; k < kFrames; ++k) {
    if (sp == 0 || (sp & 3) || !readable(sp, 4)) break;
    const uint32_t back = load(sp);
    if (back <= sp || back - sp > kMaxFrameBytes || (back & 3) || !readable(back - 8, 4)) break;
    const uint32_t ret = load(back - 8);
    if (ret < kCodeLo || ret >= kCodeHi || (ret & 3)) break;
    c.ret[c.n++] = ret;
    sp = back;
  }
  return c;
}

// "82C58500<-82BDB0F4<-..." into out (>= 9 * kFrames bytes).
inline void FormatChain(const Chain& c, char* out, size_t cap) {
  size_t o = 0;
  out[0] = 0;
  for (int i = 0; i < c.n && o + 10 < cap; ++i)
    o += size_t(std::snprintf(out + o, cap - o, i ? "<-%08X" : "%08X", c.ret[i]));
}

// Once-per-caller registry: Hit() returns the caller's failure count (1 = first time; the caller logs on 1, 2, 4, 8 ...)
// or 0 when the registry is full (distinct callers beyond kMaxKeys are only counted in overflow()).
class Registry {
 public:
  static constexpr size_t kMaxKeys = 1024;
  uint32_t Hit(uint32_t kind, uint32_t caller) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t key = (uint64_t(kind) << 32) | caller;
    auto it = counts_.find(key);
    if (it != counts_.end()) return ++it->second;
    if (counts_.size() >= kMaxKeys) {
      ++overflow_;
      return 0;
    }
    counts_.emplace(key, 1u);
    return 1;
  }
  size_t distinct() {
    std::lock_guard<std::mutex> lk(mu_);
    return counts_.size();
  }
  uint64_t overflow() {
    std::lock_guard<std::mutex> lk(mu_);
    return overflow_;
  }

 private:
  std::mutex mu_;
  std::unordered_map<uint64_t, uint32_t> counts_;
  uint64_t overflow_ = 0;
};

inline bool LogThisHit(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }  // 1, 2, 4, 8, ...

// ---------------------------------------------------------------- below-pool lender (WWE2K14_MEM_SHARE_LOW)
// Physical memory below the lowest graphics window: the window pool is carved at boot as [0x1FC00000 - pool,
// 0x1FC00000) and the general heap block (all 56 block heaps) is a kernel allocation placed directly under it
// (sub_8225F160 0:36814 pool, 0:36848 heap block; the pool block is freed again at 0:36883), so everything below the
// lowest window that the kernel's physical heap reports free belongs to no window and no heap, and no layout rebuild
// touches it (sub_8225F480 only rewrites window descriptors). memshare-1/-2/S2-1/-2/-3: 44,032 KiB free there in all 21
// fill samples from boot to the result screen (never used by the game).
struct Span {
  uint32_t lo = 0, hi = 0;
  bool ok() const { return hi > lo; }
};
// lo[i]/hi[i] = window i descriptor +4/+8 (physical). Returns [0, lowest window start) or an empty span.
inline Span BelowPool(const uint32_t* lo, const uint32_t* hi, uint32_t n) {
  uint32_t m = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < n; ++i)
    if (hi[i] > lo[i] && lo[i] < m) m = lo[i];
  if (m == 0xFFFFFFFFu || m == 0) return Span{};
  return Span{0, m};
}

constexpr uint64_t kLowNetCap = 24ull << 20;   // below-pool loans outstanding at most
constexpr uint64_t kLowReserve = 16ull << 20;  // leave at least this much of the below-pool memory free
// free_bytes / largest = scan of the below-pool span now; net = below-pool loans outstanding.
inline const char* LowRefusal(uint64_t free_bytes, uint64_t largest, uint64_t size, uint64_t net) {
  if (net + size > kLowNetCap) return "below-pool loan cap (24 MB) reached";
  if (free_bytes < size + kLowReserve) return "below-pool memory would drop under its 16 MB reserve";
  if (largest < size) return "no free run below the window pool is large enough";
  return nullptr;
}

}  // namespace wwe2k14::mem::faillog
