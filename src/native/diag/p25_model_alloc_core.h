// P25-DIAG12 host-testable core (docs/tasks/P25-DIAG12.md). Read-only parsing of the verified loader data structures.
// No raw guest dereference and no zero-on-failed-read: every read goes through a caller-supplied read32 that returns a
// status, and every parser/walk stops with the partial context it actually read.
//
// Verified layouts (generated/wwe2k14_recomp.40.cpp, .80.cpp, .2.cpp):
//   sub_82721DA8: this+96 -> job pointer, this+104 = branch selector; the zero branch reaches loc_82721FA0.
//   sub_82296290(arg, 0) = PACH row 0 stored size (0 if the object is not a PACH).
//   PACH: obj+0 -> header; header+0 = 'PACH'; header+4 = count (BE); rows at header+8, 12 bytes:
//         row+0 id, row+4 data offset, row+8 size; data base = obj+4 + header (sub_82C38D80).
//   Heap record table: pointer at 0x83930034, 56 records * 36 bytes: +0 start, +4 end (H1b report, #122/#125 code).
//   Free list: head index at start+8; block address = start + index*16; block+0 low 28 bits = end index;
//         block+8 = next free index (sub_82C32DE8 first-fit walk).
#pragma once

#include <cstdint>

namespace wwe2k14::p25diag {

inline constexpr uint32_t kCallerSub82721FF8 = 0x82722074u;  // bl sub_82721DA8 in sub_82721FF8 (only caller)
inline constexpr uint32_t kCallerLeafSize = 0x82721F6Cu;      // bl sub_82296290 at loc_82721F60
inline constexpr uint32_t kCallerAlloc = 0x82721FACu;         // bl sub_82C32DE8 at loc_82721FA0 (units)
inline constexpr uint32_t kCallerAllocBytes = 0x82721F9Cu;    // bl sub_82C31518 at loc_82721F98 (bytes)
inline constexpr uint32_t kCallerBranch = 0x82721F84u;        // bl sub_82C31A20 branch selector
inline constexpr uint32_t kCallerMemcpy = 0x82721FD0u;        // bl sub_82C391F0 after the allocation
inline constexpr uint32_t kCallerAllocHeap = 0x82C32E00u;     // bl sub_82C31100 inside sub_82C32DE8

inline constexpr uint32_t kHeapTablePtr = 0x83930034u;        // pointer to the 56-record heap table
inline constexpr uint32_t kHeapEntrySize = 36u;
inline constexpr uint32_t kHeapCount = 56u;
inline constexpr uint32_t kMaxFreeWalk = 100000u;            // hard bound for the read-only free-list walk
inline constexpr uint32_t kPachMagic = 0x50414348u;          // 'PACH'

enum class GuestReadStatus { kOk, kBoundary, kUnmapped, kFault };

inline const char* GuestReadName(GuestReadStatus s) {
  switch (s) {
    case GuestReadStatus::kOk: return "ok";
    case GuestReadStatus::kBoundary: return "boundary";
    case GuestReadStatus::kUnmapped: return "unmapped";
    case GuestReadStatus::kFault: return "fault";
  }
  return "?";
}

struct GuestRead32 {
  GuestReadStatus status = GuestReadStatus::kUnmapped;
  uint32_t value = 0;
};

// Boundary check first, then the caller's mapping/protection query, then the caller's OS fault-safe host read. A failed
// read never yields a usable zero: the status is kBoundary/kUnmapped/kFault and the value stays 0 only as unused storage.
template <class Readable, class ReadHost>
GuestRead32 SafeGuestRead32(uint32_t guest, uintptr_t host, Readable readable, ReadHost read_host) {
  if (guest > 0xFFFFFFFCu) return GuestRead32{GuestReadStatus::kBoundary, 0};
  if (!readable(guest, 4)) return GuestRead32{GuestReadStatus::kUnmapped, 0};
  uint32_t value = 0;
  if (!read_host(host, &value)) return GuestRead32{GuestReadStatus::kFault, 0};
  return GuestRead32{GuestReadStatus::kOk, value};
}

inline bool AddU32(uint32_t a, uint32_t b, uint32_t* out) {
  const uint32_t v = a + b;
  if (v < a) return false;
  *out = v;
  return true;
}

inline bool AddMulU32(uint32_t base, uint32_t index, uint32_t stride, uint32_t* out) {
  const uint64_t v = uint64_t(base) + uint64_t(index) * stride;
  if (v > 0xFFFFFFFFu) return false;
  *out = uint32_t(v);
  return true;
}

struct PachInfo {
  bool obj_readable = false;
  bool header_readable = false;
  bool magic_readable = false;
  bool count_readable = false;
  bool data_base_readable = false;
  bool leaf0_readable = false;
  bool is_pach = false;
  uint32_t header = 0;
  uint32_t magic = 0;
  uint32_t count = 0;
  uint32_t data_base_offset = 0;
  uint32_t leaf0_id = 0;
  uint32_t leaf0_offset = 0;
  uint32_t leaf0_size = 0;
  GuestReadStatus stop = GuestReadStatus::kOk;
  const char* stop_where = "ok";
};

template <class Read32>
PachInfo ParsePach(uint32_t obj, Read32 read32) {
  PachInfo out{};
  if (obj == 0) {
    out.stop = GuestReadStatus::kBoundary;
    out.stop_where = "obj-null";
    return out;
  }
  GuestRead32 r = read32(obj);
  if (r.status != GuestReadStatus::kOk) {
    out.stop = r.status;
    out.stop_where = "obj";
    return out;
  }
  out.obj_readable = true;
  out.header_readable = true;
  out.header = r.value;

  r = read32(out.header);
  if (r.status != GuestReadStatus::kOk) {
    out.stop = r.status;
    out.stop_where = "header";
    return out;
  }
  out.magic_readable = true;
  out.magic = r.value;
  out.is_pach = out.magic == kPachMagic;
  if (!out.is_pach) return out;

  uint32_t a = 0;
  if (!AddU32(out.header, 4, &a)) {
    out.stop = GuestReadStatus::kBoundary;
    out.stop_where = "count-addr";
    return out;
  }
  r = read32(a);
  if (r.status != GuestReadStatus::kOk) {
    out.stop = r.status;
    out.stop_where = "count";
    return out;
  }
  out.count_readable = true;
  out.count = r.value;

  if (!AddU32(obj, 4, &a)) {
    out.stop = GuestReadStatus::kBoundary;
    out.stop_where = "data-base-addr";
    return out;
  }
  r = read32(a);
  if (r.status != GuestReadStatus::kOk) {
    out.stop = r.status;
    out.stop_where = "data-base";
    return out;
  }
  out.data_base_readable = true;
  out.data_base_offset = r.value;
  if (out.count == 0) return out;

  if (!AddU32(out.header, 8, &a)) {
    out.stop = GuestReadStatus::kBoundary;
    out.stop_where = "leaf0-id-addr";
    return out;
  }
  r = read32(a);
  if (r.status != GuestReadStatus::kOk) {
    out.stop = r.status;
    out.stop_where = "leaf0-id";
    return out;
  }
  out.leaf0_id = r.value;

  if (!AddU32(out.header, 12, &a)) {
    out.stop = GuestReadStatus::kBoundary;
    out.stop_where = "leaf0-offset-addr";
    return out;
  }
  r = read32(a);
  if (r.status != GuestReadStatus::kOk) {
    out.stop = r.status;
    out.stop_where = "leaf0-offset";
    return out;
  }
  out.leaf0_offset = r.value;

  if (!AddU32(out.header, 16, &a)) {
    out.stop = GuestReadStatus::kBoundary;
    out.stop_where = "leaf0-size-addr";
    return out;
  }
  r = read32(a);
  if (r.status != GuestReadStatus::kOk) {
    out.stop = r.status;
    out.stop_where = "leaf0-size";
    return out;
  }
  out.leaf0_readable = true;
  out.leaf0_size = r.value;
  return out;
}

struct HeapInfo {
  bool table_readable = false;
  bool record_readable = false;
  bool head_readable = false;
  uint32_t heap_id = 0;
  uint32_t table = 0;
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t head = 0;
  uint64_t total = 0;
  uint64_t free_bytes = 0;
  uint64_t largest_free = 0;
  uint32_t free_blocks = 0;
  bool walk_ok = false;
  GuestReadStatus stop = GuestReadStatus::kOk;
  const char* stop_where = "ok";
};

template <class Read32>
HeapInfo WalkHeap(uint32_t heap_id, Read32 read32) {
  HeapInfo out{};
  out.heap_id = heap_id;
  if (heap_id >= kHeapCount) {
    out.stop = GuestReadStatus::kBoundary;
    out.stop_where = "heap-id";
    return out;
  }
  GuestRead32 r = read32(kHeapTablePtr);
  if (r.status != GuestReadStatus::kOk) {
    out.stop = r.status;
    out.stop_where = "table";
    return out;
  }
  out.table_readable = true;
  out.table = r.value;

  uint32_t rec = 0;
  if (!AddMulU32(out.table, heap_id, kHeapEntrySize, &rec)) {
    out.stop = GuestReadStatus::kBoundary;
    out.stop_where = "record-addr";
    return out;
  }
  r = read32(rec);
  if (r.status != GuestReadStatus::kOk) {
    out.stop = r.status;
    out.stop_where = "record-start";
    return out;
  }
  out.start = r.value;
  uint32_t end_addr = 0;
  if (!AddU32(rec, 4, &end_addr)) {
    out.stop = GuestReadStatus::kBoundary;
    out.stop_where = "record-end-addr";
    return out;
  }
  r = read32(end_addr);
  if (r.status != GuestReadStatus::kOk) {
    out.stop = r.status;
    out.stop_where = "record-end";
    return out;
  }
  out.end = r.value;
  if (out.end <= out.start) {
    out.stop = GuestReadStatus::kBoundary;
    out.stop_where = "record-range";
    return out;
  }
  out.record_readable = true;
  out.total = uint64_t(out.end) - out.start;
  const uint32_t total_blocks = uint32_t(out.total >> 4);

  uint32_t head_addr = 0;
  if (!AddU32(out.start, 8, &head_addr)) {
    out.stop = GuestReadStatus::kBoundary;
    out.stop_where = "head-addr";
    return out;
  }
  r = read32(head_addr);
  if (r.status != GuestReadStatus::kOk) {
    out.stop = r.status;
    out.stop_where = "head";
    return out;
  }
  out.head_readable = true;
  out.head = r.value;

  uint32_t idx = out.head;
  uint32_t n = 0;
  for (;;) {
    if (idx == 0) {
      out.walk_ok = true;
      return out;
    }
    if (idx >= total_blocks) {
      out.stop = GuestReadStatus::kBoundary;
      out.stop_where = "index";
      return out;
    }
    if (++n > kMaxFreeWalk) {
      out.stop = GuestReadStatus::kBoundary;
      out.stop_where = "cap";
      return out;
    }
    uint32_t block = 0;
    if (!AddMulU32(out.start, idx, 16u, &block)) {
      out.stop = GuestReadStatus::kBoundary;
      out.stop_where = "block-addr";
      return out;
    }
    r = read32(block);
    if (r.status != GuestReadStatus::kOk) {
      out.stop = r.status;
      out.stop_where = "block";
      return out;
    }
    const uint32_t end_idx = r.value & 0x0FFFFFFFu;
    if (end_idx <= idx || end_idx > total_blocks) {
      out.stop = GuestReadStatus::kBoundary;
      out.stop_where = "block-range";
      return out;
    }
    const uint32_t payload_blocks = end_idx - idx - 1u;
    const uint64_t bytes = uint64_t(payload_blocks) << 4;
    out.free_bytes += bytes;
    if (bytes > out.largest_free) out.largest_free = bytes;
    ++out.free_blocks;

    uint32_t next_addr = 0;
    if (!AddU32(block, 8, &next_addr)) {
      out.stop = GuestReadStatus::kBoundary;
      out.stop_where = "next-addr";
      return out;
    }
    r = read32(next_addr);
    if (r.status != GuestReadStatus::kOk) {
      out.stop = r.status;
      out.stop_where = "next";
      return out;
    }
    const uint32_t next = r.value;
    if (next == idx) {
      out.stop = GuestReadStatus::kBoundary;
      out.stop_where = "cycle";
      return out;
    }
    idx = next;
  }
}

struct SavedArgs {
  uint32_t r3 = 0;
  uint32_t r4 = 0;
  uint32_t r5 = 0;
  uint32_t r6 = 0;
  uint64_t lr = 0;
};

template <class Ctx>
SavedArgs SaveArgs(const Ctx& ctx) {
  return SavedArgs{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.lr};
}

// Calls the original exactly once. Disabled: original only (no register snapshot, no callbacks). Enabled: snapshots the
// entry arguments, runs before(saved), calls the original, then runs after(saved). The original is never skipped or repeated;
// the snapshot lets the after log report the exact request even if the original changed the context.
template <class Ctx, class Original, class Before, class After>
void InvokeOriginal(Ctx& ctx, Original original, bool logging, Before before, After after) {
  if (!logging) {
    original();
    return;
  }
  const SavedArgs saved = SaveArgs(ctx);
  before(saved, ctx);
  original();
  after(saved, ctx);
}

}  // namespace wwe2k14::p25diag
