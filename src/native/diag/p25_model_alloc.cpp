// P25-DIAG12: TEST-ONLY, read-only diagnostic for the p25 ch104 match-load crash (docs/tasks/P25-DIAG12.md).
// Built only with WWE2K14_TEST_TOOLS and inert unless WWE2K14_TEST_P25_MODEL_ALLOC=1. It changes no allocator result,
// no loader branch and no memory: every override calls the original body exactly once with the same context.
//
// RUN13 evidence (out/port25/luna/p25-run13/evidence/p25-alloc-lines.txt): the PACH probe faulted at obj, the units
// allocator hook did not fire, and the null memcpy still happened for leaf 0. That means the leaf-0 bytes went through
// the OTHER branch at loc_82721F98: sub_82C31A20() != 0 -> sub_82C31518(r3 = BYTES). This file now logs both allocator
// branches, the branch selector itself, and the OS read errno.
//
// Every guest read here is checked: 32-bit boundary, the emulator's mapping/protection query, then an OS fault-safe
// host read (process_vm_readv / ReadProcessMemory). A failed read stops that parser/walk and is logged as a status; it
// never becomes a usable zero and never becomes a guest fault. The original guest function still sees its own memory.
//
// Verified call sites (generated/wwe2k14_recomp.40.cpp / .80.cpp):
//   sub_82721FF8 -> sub_82721DA8  return 0x82722074 (only caller)
//   sub_82721DA8 -> sub_82296290  return 0x82721F6C (loc_82721F60, only when this+104 == 0)
//   sub_82721DA8 -> sub_82C31A20  return 0x82721F84 (branch selector)
//   sub_82721DA8 -> sub_82C31518  return 0x82721F9C (branch != 0, r3 = BYTES)
//   sub_82721DA8 -> sub_82C32DE8  return 0x82721FAC (branch == 0, r3 = 16-byte units)
//   sub_82721DA8 -> sub_82C391F0  return 0x82721FD0 (the call that memcpy's into the allocation)
//   sub_82C32DE8 -> sub_82C31100  return 0x82C32E00 (the allocator's current-heap lookup)
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "../../memory_share.h"
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "p25_guest_read.h"
#include "p25_model_alloc_core.h"

extern "C" {
PPC_FUNC(__imp__sub_82721DA8);
PPC_FUNC(sub_82721DA8);
PPC_FUNC(__imp__sub_82296290);
PPC_FUNC(sub_82296290);
PPC_FUNC(__imp__sub_82C31A20);
PPC_FUNC(sub_82C31A20);
PPC_FUNC(__imp__sub_82C31518);
PPC_FUNC(__imp__sub_82C31100);
PPC_FUNC(__imp__sub_82C32DE8);
PPC_FUNC(__imp__sub_82C391F0);
PPC_FUNC(sub_82C391F0);
PPC_FUNC(__imp__sub_82C38A90);
PPC_FUNC(sub_82C38A90);
PPC_FUNC(__imp__sub_82C39290);
PPC_FUNC(sub_82C39290);
}

namespace {
using namespace wwe2k14::p25diag;

bool On() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_TEST_P25_MODEL_ALLOC");
    const bool v = e && e[0] == '1' && e[1] == '\0';
    if (v) REXLOG_INFO("[p25-alloc] WWE2K14_TEST_P25_MODEL_ALLOC=1: read-only ch104 loader allocation diagnostic");
    return v;
  }();
  return on;
}

// The guest range must be in one heap, committed and at least readable (xmemory.h QueryRangeAccess). This is the
// mapping/protection gate; the OS read below is the fault gate.
bool HostRangeReadable(uint32_t guest, uint32_t size) {
  if (size == 0) return false;
  const uint64_t end = uint64_t(guest) + size - 1;
  if (end > 0xFFFFFFFFu) return false;
  auto* ks = rex::system::kernel_state();
  auto* mem = ks ? ks->memory() : nullptr;
  auto* heap = mem ? mem->LookupHeap(guest) : nullptr;
  if (!heap) return false;
  return heap->QueryRangeAccess(guest, uint32_t(end)) != rex::memory::PageAccess::kNoAccess;
}

GuestRead32 ReadGuest32(uint8_t* base, uint32_t guest) {
  if (guest > 0xFFFFFFFCu) return GuestRead32{GuestReadStatus::kBoundary, 0};
  const uintptr_t host = reinterpret_cast<uintptr_t>(PPC_RAW_ADDR(guest));
  return SafeGuestRead32(guest, host,
                         [](uint32_t a, uint32_t s) { return HostRangeReadable(a, s); },
                         [](uintptr_t h, uint32_t* out) {
                           return OsReadBe32(reinterpret_cast<const void*>(h), out);
                         });
}

std::string HexOrDash(bool ok, uint32_t value) {
  if (!ok) return "--------";
  char b[9];
  std::snprintf(b, sizeof b, "%08X", value);
  return std::string(b);
}

std::string DecOrQ(bool ok, uint64_t value) { return ok ? std::to_string(value) : std::string("??"); }

// Per-thread state for the one loader frame. sub_82721DA8 is called from one site; the nested hooks only act while it runs.
thread_local bool t_in_loader = false;
thread_local uint32_t t_this = 0;
thread_local uint32_t t_arg = 0;
thread_local uint32_t t_this96 = 0;
thread_local bool t_this96_ok = false;
thread_local uint32_t t_job = 0;
thread_local bool t_job_ok = false;
thread_local uint32_t t_this104 = 0;
thread_local bool t_this104_ok = false;
thread_local uint32_t t_leaf0_size = 0;
thread_local bool t_leaf0_called = false;
thread_local uint32_t t_branch_result = 0;
thread_local bool t_branch_known = false;
thread_local uint32_t t_alloc_units = 0;
thread_local uint32_t t_alloc_bytes = 0;
thread_local uint32_t t_alloc_lr = 0;
thread_local uint32_t t_heap_id = 0;
thread_local bool t_heap_id_valid = false;

std::atomic<uint32_t> g_enter{0}, g_enter_flush{0};
std::atomic<uint32_t> g_branch{0}, g_branch_flush{0};
std::atomic<uint32_t> g_alloc{0}, g_alloc_fail{0}, g_alloc_flush{0};
std::atomic<uint32_t> g_memcpy{0}, g_memcpy_null{0}, g_memcpy_flush{0};

// The log sink's normal flush level is err, so INFO lines can be lost if the process dies immediately after them.
// Flush the bounded critical lines only; the counters keep the cost bounded even if the loader runs many times.
void FlushFirst(std::atomic<uint32_t>& counter, uint32_t cap) {
  if (counter.fetch_add(1, std::memory_order_relaxed) < cap) rex::FlushLogging();
}

// Common allocator log for both branches. request_bytes is the caller's byte request (bytes branch) or the rounded
// units*16 (units branch); units_est is the 16-byte-unit estimate (exact on the units branch). The heap walk is a
// read-only snapshot.
void LogAlloc(const char* mode, uint32_t caller, uint32_t request_bytes, uint32_t units, uint32_t result, bool failed,
              uint8_t* base) {
  const bool log = failed ? (g_alloc_fail.fetch_add(1, std::memory_order_relaxed) < 16)
                          : (g_alloc.fetch_add(1, std::memory_order_relaxed) < 64);
  if (!log) return;
  HeapInfo h{};
  if (t_heap_id_valid) {
    const auto read32 = [base](uint32_t a) { return ReadGuest32(base, a); };
    h = WalkHeap(t_heap_id, read32);
  }
  const std::string span =
      h.record_readable ? (HexOrDash(true, h.start) + "-" + HexOrDash(true, h.end)) : std::string("--------");
  const std::string heap = h.table_readable ? HexOrDash(true, h.table) : std::string("--------");
  const std::string free = (h.free_blocks || h.walk_ok) ? std::to_string(h.free_bytes) : std::string("??");
  const std::string largest = (h.free_blocks || h.walk_ok) ? std::to_string(h.largest_free) : std::string("??");
  const std::string heap_id = t_heap_id_valid ? std::to_string(t_heap_id) : std::string("??");
  REXLOG_INFO(
      "[p25-alloc] alloc mode={} caller={:08X} this={:08X} arg={:08X} this104={} this104_ok={} leaf0_size={} "
      "leaf0_called={} branch={} branch_ok={} request_bytes={} units_est={} result={:08X} heap_id={} heap={} table_ok={} "
      "record_ok={} span={} total={} head={} head_ok={} free={} largest_free={} free_blocks={} walk_ok={} stop={}@{} "
      "errno={}",
      mode, caller, t_this, t_arg, HexOrDash(t_this104_ok, t_this104), t_this104_ok, t_leaf0_size, t_leaf0_called,
      DecOrQ(t_branch_known, t_branch_result), t_branch_known, request_bytes, units, result, heap_id, heap,
      h.table_readable, h.record_readable, span, DecOrQ(h.record_readable, h.total), HexOrDash(h.head_readable, h.head),
      h.head_readable, free, largest, h.free_blocks, h.walk_ok, GuestReadName(h.stop), h.stop_where, OsLastReadErrno());
  if (failed) {
    rex::FlushLogging();
  } else {
    FlushFirst(g_alloc_flush, 32);
  }
}
}  // namespace

// sub_82721FF8 -> sub_82721DA8: log the branch selector and the PACH identity before the loader decides its path.
extern "C" PPC_FUNC(sub_82721DA8) {
  const bool hit = On() && uint32_t(ctx.lr) == kCallerSub82721FF8;
  InvokeOriginal(
      ctx, [&] { __imp__sub_82721DA8(ctx, base); }, hit,
      [&](const SavedArgs& s, PPCContext&) {
        t_in_loader = true;
        t_heap_id_valid = false;
        t_this = s.r3;
        t_arg = s.r4;
        t_this96 = 0;
        t_this96_ok = false;
        t_job = 0;
        t_job_ok = false;
        t_this104 = 0;
        t_this104_ok = false;
        t_leaf0_size = 0;
        t_leaf0_called = false;
        t_branch_result = 0;
        t_branch_known = false;
        t_alloc_units = 0;
        t_alloc_bytes = 0;
        t_alloc_lr = 0;

        uint32_t addr = 0;
        if (s.r3 && AddU32(s.r3, 96, &addr)) {
          const GuestRead32 r = ReadGuest32(base, addr);
          if (r.status == GuestReadStatus::kOk) {
            t_this96 = r.value;
            t_this96_ok = true;
            if (t_this96) {
              const GuestRead32 j = ReadGuest32(base, t_this96);
              if (j.status == GuestReadStatus::kOk) {
                t_job = j.value;
                t_job_ok = true;
              }
            }
          }
        }
        if (s.r3 && AddU32(s.r3, 104, &addr)) {
          const GuestRead32 r = ReadGuest32(base, addr);
          if (r.status == GuestReadStatus::kOk) {
            t_this104 = r.value;
            t_this104_ok = true;
          }
        }

        const auto read32 = [base](uint32_t a) { return ReadGuest32(base, a); };
        const PachInfo p = ParsePach(s.r4, read32);
        if (g_enter.fetch_add(1, std::memory_order_relaxed) < 64) {
          REXLOG_INFO(
              "[p25-alloc] enter this={:08X} arg={:08X} this96={} this96_ok={} job={} job_ok={} this104={} "
              "this104_ok={} pach={} hdr={} hdr_ok={} magic={} magic_ok={} count={} count_ok={} data_base_off={} "
              "data_base_ok={} leaf0={} leaf0_ok={} read_stop={}@{} errno={} caller={:08X}",
              s.r3, s.r4, HexOrDash(t_this96_ok, t_this96), t_this96_ok, HexOrDash(t_job_ok, t_job), t_job_ok,
              HexOrDash(t_this104_ok, t_this104), t_this104_ok, p.is_pach, HexOrDash(p.header_readable, p.header),
              p.header_readable, HexOrDash(p.magic_readable, p.magic), p.magic_readable,
              DecOrQ(p.count_readable, p.count), p.count_readable,
              HexOrDash(p.data_base_readable, p.data_base_offset), p.data_base_readable,
              p.leaf0_readable ? (std::to_string(p.leaf0_id) + "/" + std::to_string(p.leaf0_offset) + "/" +
                                  std::to_string(p.leaf0_size))
                               : std::string("unread"),
              p.leaf0_readable, GuestReadName(p.stop), p.stop_where, OsLastReadErrno(), uint32_t(s.lr));
          FlushFirst(g_enter_flush, 32);
        }
      },
      [&](const SavedArgs&, PPCContext&) {
        t_in_loader = false;
        t_heap_id_valid = false;
      });
}

// loc_82721F60 -> sub_82296290(arg, 0): the exact stored leaf-0 size the loader uses as the allocation request.
extern "C" PPC_FUNC(sub_82296290) {
  const bool hit = On() && t_in_loader && uint32_t(ctx.lr) == kCallerLeafSize;
  InvokeOriginal(
      ctx, [&] { __imp__sub_82296290(ctx, base); }, hit, [](const SavedArgs&, PPCContext&) {},
      [&](const SavedArgs&, PPCContext& c) {
        t_leaf0_size = c.r3.u32;
        t_leaf0_called = true;
      });
}

// loc_82721F84 -> sub_82C31A20: the branch selector. result 0 = units branch (sub_82C32DE8); otherwise the bytes branch
// (sub_82C31518). This line is emitted even if neither allocator hook fires.
extern "C" PPC_FUNC(sub_82C31A20) {
  const bool hit = On() && t_in_loader && uint32_t(ctx.lr) == kCallerBranch;
  InvokeOriginal(
      ctx, [&] { __imp__sub_82C31A20(ctx, base); }, hit, [](const SavedArgs&, PPCContext&) {},
      [&](const SavedArgs&, PPCContext& c) {
        t_branch_result = c.r3.u32;
        t_branch_known = true;
        if (g_branch.fetch_add(1, std::memory_order_relaxed) < 64) {
          REXLOG_INFO(
              "[p25-alloc] branch caller={:08X} result={} path={} this104={} this104_ok={} last_errno={}",
              kCallerBranch, t_branch_result,
              t_branch_result ? "bytes(sub_82C31518)" : "units(sub_82C32DE8)",
              HexOrDash(t_this104_ok, t_this104), t_this104_ok, OsLastReadErrno());
          FlushFirst(g_branch_flush, 32);
        }
      });
}

// loc_82721F98 -> sub_82C31518: the non-zero branch. r3 is the BYTE request, not 16-byte units.
static PPC_FUNC(P25_sub_82C31518) {
  const bool hit = On() && t_in_loader;  // P25-VB17 v4: any loader-frame allocation (RUN18 decode path)
  InvokeOriginal(
      ctx, [&] { __imp__sub_82C31518(ctx, base); }, hit,
      [&](const SavedArgs& s, PPCContext& c) {
        t_heap_id_valid = false;
        t_alloc_bytes = s.r3;
        t_alloc_lr = uint32_t(s.lr);
        const PPCContext saved_ctx = c;
        __imp__sub_82C31100(c, base);
        t_heap_id = c.r3.u32;
        t_heap_id_valid = true;
        c = saved_ctx;
      },
      [&](const SavedArgs&, PPCContext& c) {
        const uint32_t result = c.r3.u32;
        const uint32_t bytes = t_alloc_bytes;
        const uint32_t units = (bytes + 15u) >> 4;
        LogAlloc("bytes", t_alloc_lr, bytes, units, result, result == 0, base);
        t_heap_id_valid = false;
      });
}

// loc_82721FA0 -> sub_82C32DE8: the zero branch. r3 is the request in 16-byte units.
static PPC_FUNC(P25_sub_82C32DE8) {
  const bool hit = On() && t_in_loader;  // P25-VB17 v4: any loader-frame allocation (RUN18 decode path)
  InvokeOriginal(
      ctx, [&] { __imp__sub_82C32DE8(ctx, base); }, hit,
      [&](const SavedArgs& s, PPCContext& c) {
        t_heap_id_valid = false;
        t_alloc_units = s.r3;
        t_alloc_lr = uint32_t(s.lr);
        // Ask the allocator's own current-heap answer, then put the complete context back before the original runs.
        // __imp__ is the generated body; src/pool_fail_log.cpp owns the strong sub_82C31100 symbol (no second override).
        const PPCContext saved_ctx = c;
        __imp__sub_82C31100(c, base);
        t_heap_id = c.r3.u32;
        t_heap_id_valid = true;
        c = saved_ctx;
      },
      [&](const SavedArgs&, PPCContext& c) {
        const uint32_t result = c.r3.u32;
        const uint32_t units = t_alloc_units;
        const uint32_t bytes = units << 4;
        LogAlloc("units", t_alloc_lr, bytes, units, result, result == 0, base);
        t_heap_id_valid = false;
      });
}

// sub_82721DA8 -> sub_82C391F0(parser, dest, leaf_index, size): the call that reaches the crashing memcpy.
extern "C" PPC_FUNC(sub_82C391F0) {
  const bool hit = On() && t_in_loader && uint32_t(ctx.lr) == kCallerMemcpy;
  InvokeOriginal(
      ctx, [&] { __imp__sub_82C391F0(ctx, base); }, hit,
      [&](const SavedArgs& s, PPCContext&) {
        const bool null_dest = s.r4 == 0;
        const bool log = null_dest ? (g_memcpy_null.fetch_add(1, std::memory_order_relaxed) < 16)
                                   : (g_memcpy.fetch_add(1, std::memory_order_relaxed) < 64);
        if (log) {
          REXLOG_INFO(
              "[p25-alloc] pre-memcpy caller={:08X} arg={:08X} dest={:08X} leaf_index={:08X} size={} dest_null={} "
              "this104={} this104_ok={} leaf0_size={} leaf0_called={} branch={} branch_ok={}",
              uint32_t(s.lr), s.r3, s.r4, s.r5, s.r6, null_dest, HexOrDash(t_this104_ok, t_this104), t_this104_ok,
              t_leaf0_size, t_leaf0_called, DecOrQ(t_branch_known, t_branch_result), t_branch_known);
          if (null_dest) {
            rex::FlushLogging();
          } else {
            FlushFirst(g_memcpy_flush, 32);
          }
        }
      },
      [](const SavedArgs&, PPCContext&) {});
}

// P25-VB17 v4 (RUN18: SIGSEGV 0x100000070 in the this+104 != 0 decode path, sub_82721DA8 -> sub_82C2B250 -> memcpy):
// sub_82721DA8 -> sub_82C38A90 (return 0x82721E28) gives the decode size; -> sub_82C39290 (return 0x82721E44) returns
// the decode buffer r26 used without a check. Log both (bounded); the allocator hooks above now log any loader-frame
// allocation with its heap snapshot.
namespace {
thread_local uint32_t t_decode_size = 0;
std::atomic<uint32_t> g_decode_lines{0};
}  // namespace

extern "C" PPC_FUNC(sub_82C38A90) {
  const bool hit = On() && t_in_loader && uint32_t(ctx.lr) == 0x82721E28u;
  __imp__sub_82C38A90(ctx, base);
  if (hit) t_decode_size = ctx.r3.u32;
}

extern "C" PPC_FUNC(sub_82C39290) {
  const bool hit = On() && t_in_loader && uint32_t(ctx.lr) == 0x82721E44u;
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  __imp__sub_82C39290(ctx, base);
  if (!hit) return;
  const uint32_t result = ctx.r3.u32;
  if (result == 0 || g_decode_lines.fetch_add(1, std::memory_order_relaxed) < 64) {
    REXLOG_INFO("[p25-alloc] decode-buffer this={:08X} obj={:08X} r4={:08X} tag={:08X} decode_size={} result={:08X} null={}",
                t_this, r3, r4, r5, t_decode_size, result, result == 0);
    if (result == 0) rex::FlushLogging();
  }
}

// memory_share.cpp owns the sub_82C31518 / sub_82C32DE8 symbols; it calls these (then the generated body) instead.
namespace {
struct P25Register {
  P25Register() {
    wwe2k14::mem::g_inner_new = P25_sub_82C31518;
    wwe2k14::mem::g_inner_small = P25_sub_82C32DE8;
  }
} g_p25_register;
}  // namespace
