// BPE decompressor guard (GitHub #125: WM24 Shawn Michaels vs John Cena, 30 Years of WrestleMania). Always on.
// "N:line" = generated/wwe2k14_recomp.N.cpp. Root-cause report: docs/tasks/GH125-rootcause.md.
//
// Crash: sub_82C45148 (81.cpp:641-936) is the "BPE " decompressor: r3 = source, r4 = destination. Output end =
// r4 + the little-endian word at source bytes 12-15 (81.cpp:665-677) and the decode loop runs while the write
// pointer r5 < end (81.cpp:924-926). The input pointer r6 is never bounded (Q4-C1d): when the stream ends early,
// r6 runs past the input end and #125 read guest 0x714F0000 (host fault 0x1714F0000).
// The header DOES carry the input length: bytes 8-11 (LE) = payload length, blob length = 16 + it (true for all
// 9963 retail blobs checked on the host, tools/test-bpe-check); the decoder itself never reads those bytes.
// The hook replaces the guest decoder with ONE bounded native decode (src/bpe_check.h Decode, same stream walk as
// 81.cpp:641-936; output byte-identical to the recompiled guest decoder on all 9963 retail blobs checked on the
// host, tools/test-bpe-check). The guest decoder is no longer called. Before anything is written:
//   1. the 16-byte header is readable guest memory (the hook's own reads must not fault on the bad input);
//   2. [r4, r4+size) is writable;
//   3. in_len = 16 + payload word; [src, src+in_len) is readable;
//   4. at the PACH entry call site (lr 0x82C39124, 80.cpp:45116-45117) in_len <= ctx.r28 = the entry's stored size
//      (sub_82C38E48, 80.cpp:45083 + `mr r28,r3`; r28 is non-volatile and unchanged up to the call);
//   5. the decode, into a host scratch buffer, reads only [src, src+in_len) and yields exactly `size` bytes.
// Only then is scratch copied to [r4, r4+size): the guest decoder's only writes outside its own stack frame
// (stores via r5 at 81.cpp:890; every other store is r1-relative: the 4192-byte frame's tables/stack and
// __savegprlr_26's register spill, dead after return). Scratch + copy measured faster than validate-then-decode
// (ch139: 660 ms vs 861 ms; guest decoder alone 1107 ms).
// Return value: success r3 = r4 + size (r3 is set to dst + size at 81.cpp:678 and never changed before
// loc_82C45350); refusal and size 0: r3 = r4 (the size-0 exit, 81.cpp:678-682 -> 929-934), nothing written.
// Volatile registers r4-r12/ctr/cr/xer are left as on entry (the guest leaves scratch values; callers do not read
// them: 80.cpp:45116-45119 does `li r3,1` right after). Non-volatile registers and r1 are unchanged, as after the
// guest's __restgprlr_26.
// r5 at the call is the caller's resource key (an ID, not a byte capacity: 80.cpp:45051 `mr r5,r31`; the same key
// is fed to sub_82252758 to obtain the destination, 40.cpp:2533-2539, 22.cpp:26317-26324), so there is no size>r5
// refusal. 8 callers (grep 'bl 0x82c45148'); none uses r3 right after. Only the PACH site has r28 = entry size;
// the others are bounded by the header length, the readable range and the decode.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "bpe_check.h"

namespace {
std::atomic<bool> g_logged{false};
constexpr uint32_t kPachCallReturn = 0x82C39124;  // lr of the `bl 0x82c45148` at 80.cpp:45116-45117
constexpr uint32_t kKeepScratch = 16u << 20;       // per-thread reusable scratch up to this (retail max seen 5.4 MiB)
thread_local std::vector<uint8_t> t_scratch;

// Strictest access the guest heap reports for [addr, addr+size) (size >= 1): kNoAccess when the range cannot be
// verified (no kernel state / no heap / 32-bit overflow / crosses a heap boundary) or any page in it is
// uncommitted or not protected (xmemory.h:193).
rex::memory::PageAccess RangeAccess(uint32_t addr, uint32_t size) {
  if (addr + size < addr) return rex::memory::PageAccess::kNoAccess;
  auto* ks = rex::system::kernel_state();
  auto* mem = ks ? ks->memory() : nullptr;
  auto* heap = mem ? mem->LookupHeap(addr) : nullptr;
  if (!heap) return rex::memory::PageAccess::kNoAccess;
  return heap->QueryRangeAccess(addr, addr + size - 1);
}
}

extern "C" {
PPC_FUNC(sub_82C45148);
}

extern "C" PPC_FUNC(sub_82C45148) {
  const uint32_t src = ctx.r3.u32;
  const uint32_t dst = ctx.r4.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const bool pach_site = caller == kPachCallReturn;
  const uint32_t r28 = ctx.r28.u32;
  uint32_t size = 0, in_len = 0;

  // Refusal: the guest decoder's own size-0 exit value, nothing written.
  auto refuse = [&](const char* why) {
    if (!g_logged.exchange(true))
      REXLOG_WARN("[bpe-guard] sub_82C45148 refused: {} (src {:08X}, dst {:08X}, size {:X}, in_len {:X}, r28 {:X}{}, "
                  "caller {:08X}): size-0 exit (dst), nothing written, instead of a crash (#125)",
                  why, src, dst, size, in_len, r28, pach_site ? " = entry size" : " (not the PACH site)", caller);
    ctx.r3.u64 = ctx.r4.u64;
  };

  // 1. Header (size word at bytes 12-15, 81.cpp:665-677; length word at 8-11).
  if (RangeAccess(src, 16) == rex::memory::PageAccess::kNoAccess) {
    refuse("source header not readable");
    return;
  }
  auto le32 = [&](uint32_t a) {
    return uint32_t(PPC_LOAD_U8(a)) | (uint32_t(PPC_LOAD_U8(a + 1)) << 8) | (uint32_t(PPC_LOAD_U8(a + 2)) << 16) |
           (uint32_t(PPC_LOAD_U8(a + 3)) << 24);
  };
  size = le32(src + 12);
  in_len = bpe_check::InputLength(le32(src + 8));
  if (size == 0) {  // the guest decoder returns r3 = r4 + 0 at once, touching only its stack frame (81.cpp:678-682)
    ctx.r3.u64 = ctx.r4.u64;
    return;
  }
  // 2. Output range.
  if (RangeAccess(dst, size) != rex::memory::PageAccess::kReadWrite) {
    refuse("output range not writable");
    return;
  }
  // 3. Input range from the header's own length word.
  if (in_len == 0) {
    refuse("input length word overflows");
    return;
  }
  if (RangeAccess(src, in_len) == rex::memory::PageAccess::kNoAccess) {
    refuse("input range not readable");
    return;
  }
  // 4 + 5. Entry size at the PACH site, then the bounded decode into scratch. Each range is inside one heap
  // (RangeAccess), so one raw host pointer covers it (the same mapping PPC_LOAD_U8/PPC_STORE_U8 use).
  uint8_t* scratch;
  std::unique_ptr<uint8_t[]> big;
  if (size <= kKeepScratch) {
    if (t_scratch.size() < size) t_scratch.resize(size);
    scratch = t_scratch.data();
  } else {
    big.reset(new (std::nothrow) uint8_t[size]);  // uninitialised: untouched pages cost nothing on a quick refusal
    if (!big) {
      refuse("no host memory for the decode buffer");
      return;
    }
    scratch = big.get();
  }
  const auto d = bpe_check::DecodeChecked(PPC_RAW_ADDR(src), in_len, size, pach_site, r28, scratch);
  if (d.refusal != bpe_check::Refusal::kNone) {
    if (!g_logged.load())
      REXLOG_WARN("[bpe-guard] decode detail: {} / {} (consumed {:X}, output {:X}, blocks {})",
                  bpe_check::RefusalName(d.refusal), bpe_check::StatusName(d.decode.status), d.decode.consumed,
                  d.decode.output, d.decode.blocks);
    refuse(d.refusal == bpe_check::Refusal::kLengthOverEntry ? "input length > PACH entry size"
                                                             : "stream does not decode to exactly size bytes");
    return;
  }
  std::memcpy(PPC_RAW_ADDR(dst), scratch, size);
  ctx.r3.u64 = uint64_t(size) + ctx.r4.u64;  // 81.cpp:678 `add r3,r11,r4`
}
