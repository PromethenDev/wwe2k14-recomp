// P25-DIAG12 host test (no game, no GPU): g++ -std=c++20 -O1 -g -fsanitize=address,undefined
//   -I src/native/diag src/native/diag/tests/p25-model-alloc-test.cpp -o p25-model-alloc-test && ./p25-model-alloc-test
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "p25_guest_read.h"
#include "p25_model_alloc_core.h"

using namespace wwe2k14::p25diag;

namespace {
struct FakeGuest {
  std::unordered_map<uint32_t, uint32_t> words;
  std::unordered_map<uint32_t, GuestReadStatus> forced;

  GuestRead32 Read(uint32_t address) const {
    auto f = forced.find(address);
    if (f != forced.end()) return GuestRead32{f->second, 0};
    auto it = words.find(address);
    if (it == words.end()) return GuestRead32{GuestReadStatus::kUnmapped, 0};  // never zero-fill absent memory
    return GuestRead32{GuestReadStatus::kOk, it->second};
  }
};

struct FakeReg {
  uint32_t u32 = 0;
};
struct FakeCtx {
  FakeReg r3, r4, r5, r6;
  uint64_t lr = 0;
};

FakeGuest PachMemory() {
  FakeGuest m;
  m.words[0x1000] = 0x2000;      // obj+0 -> header
  m.words[0x1004] = 0x300;       // obj+4 -> data base offset
  m.words[0x2000] = kPachMagic;  // 'PACH'
  m.words[0x2004] = 2;           // count
  m.words[0x2008] = 0;           // row 0 id
  m.words[0x200C] = 0x10;        // row 0 data offset
  m.words[0x2010] = 0xABCD;      // row 0 size
  return m;
}

FakeGuest HeapMemory() {
  FakeGuest m;
  m.words[kHeapTablePtr] = 0x83000;
  const uint32_t rec = 0x83000 + 13 * kHeapEntrySize;
  m.words[rec + 0] = 0x14000;  // start
  m.words[rec + 4] = 0x140A0;  // end: 0xA0 = 160 bytes = 10 blocks
  m.words[0x14008] = 1;        // free-list head index
  m.words[0x14010] = 6;        // block 1: end index 6 -> payload (6-1-1) blocks = 64 bytes
  m.words[0x14018] = 3;        // next free block
  m.words[0x14030] = 8;        // block 3: end index 8 -> payload (8-3-1) blocks = 64 bytes
  m.words[0x14038] = 0;        // end of list
  return m;
}
}  // namespace

int main() {
  // OS fault-safe read: normal host memory, an unmapped-page read, and a read that crosses into an unmapped page.
  {
    uint32_t word = 0x11223344;
    uint32_t out = 0;
    assert(OsReadBe32(&word, &out));
    assert(out == __builtin_bswap32(word));
    assert(OsLastReadErrno() == 0);
#if defined(__linux__)
    const long page_size = ::sysconf(_SC_PAGESIZE);
    assert(page_size > 0);
    uint8_t* pages = static_cast<uint8_t*>(
        ::mmap(nullptr, size_t(page_size) * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(pages != MAP_FAILED);
    pages[0] = 0x11;
    pages[1] = 0x22;
    pages[2] = 0x33;
    pages[3] = 0x44;
    out = 0;
    assert(OsReadBe32(pages, &out) && out == 0x11223344);
    assert(OsLastReadErrno() == 0);
    assert(::mprotect(pages + page_size, size_t(page_size), PROT_NONE) == 0);
    out = 0;
    assert(!OsReadBe32(pages + page_size, &out));        // unmapped page
    assert(OsLastReadErrno() != 0);
    assert(!OsReadBe32(pages + page_size - 2, &out));    // 4-byte read crosses into the unmapped page
    assert(OsLastReadErrno() != 0);
    assert(::munmap(pages, size_t(page_size) * 2) == 0);
#endif
  }

  // SafeGuestRead32: boundary first, then the mapping gate, then the OS read gate. No callback on a failed gate.
  {
    int readable_calls = 0, read_calls = 0;
    auto readable_yes = [&](uint32_t, uint32_t) {
      ++readable_calls;
      return true;
    };
    auto readable_no = [&](uint32_t, uint32_t) {
      ++readable_calls;
      return false;
    };
    auto read_ok = [&](uintptr_t, uint32_t* v) {
      ++read_calls;
      *v = 0x12345678;
      return true;
    };
    auto read_fail = [&](uintptr_t, uint32_t*) {
      ++read_calls;
      return false;
    };
    assert(SafeGuestRead32(0xFFFFFFFEu, 0, readable_yes, read_ok).status == GuestReadStatus::kBoundary);
    assert(readable_calls == 0 && read_calls == 0);
    assert(SafeGuestRead32(0x1000u, 0, readable_no, read_ok).status == GuestReadStatus::kUnmapped);
    assert(readable_calls == 1 && read_calls == 0);
    assert(SafeGuestRead32(0x1000u, 0, readable_yes, read_fail).status == GuestReadStatus::kFault);
    assert(readable_calls == 2 && read_calls == 1);
    const GuestRead32 ok = SafeGuestRead32(0x1000u, 0, readable_yes, read_ok);
    assert(ok.status == GuestReadStatus::kOk && ok.value == 0x12345678);
    assert(readable_calls == 3 && read_calls == 2);
  }

  // PACH row 0 parsing, including every unreadable field and address wrap.
  {
    FakeGuest m = PachMemory();
    const PachInfo p = ParsePach(0x1000, [&](uint32_t a) { return m.Read(a); });
    assert(p.stop == GuestReadStatus::kOk && p.obj_readable && p.header_readable && p.magic_readable &&
           p.count_readable && p.data_base_readable && p.leaf0_readable);
    assert(p.is_pach && p.header == 0x2000 && p.magic == kPachMagic && p.count == 2 && p.data_base_offset == 0x300);
    assert(p.leaf0_id == 0 && p.leaf0_offset == 0x10 && p.leaf0_size == 0xABCD);

    FakeGuest no_obj;  // absent object
    const PachInfo u = ParsePach(0x1000, [&](uint32_t a) { return no_obj.Read(a); });
    assert(u.stop == GuestReadStatus::kUnmapped && std::string(u.stop_where) == "obj" && !u.obj_readable);

    FakeGuest no_header;
    no_header.words[0x1000] = 0x2000;
    const PachInfo h = ParsePach(0x1000, [&](uint32_t a) { return no_header.Read(a); });
    assert(h.stop == GuestReadStatus::kUnmapped && std::string(h.stop_where) == "header" && h.obj_readable &&
           h.header_readable && !h.magic_readable);

    FakeGuest fault_magic = PachMemory();
    fault_magic.forced[0x2000] = GuestReadStatus::kFault;
    const PachInfo f = ParsePach(0x1000, [&](uint32_t a) { return fault_magic.Read(a); });
    assert(f.stop == GuestReadStatus::kFault && std::string(f.stop_where) == "header" && f.obj_readable &&
           f.header_readable && !f.magic_readable);

    FakeGuest fault_count = PachMemory();
    fault_count.forced[0x2004] = GuestReadStatus::kFault;
    const PachInfo fc = ParsePach(0x1000, [&](uint32_t a) { return fault_count.Read(a); });
    assert(fc.stop == GuestReadStatus::kFault && std::string(fc.stop_where) == "count" && fc.magic_readable &&
           !fc.count_readable);

    FakeGuest wrapped = PachMemory();
    wrapped.words[0x1000] = 0xFFFFFFFC;
    wrapped.words[0xFFFFFFFC] = kPachMagic;
    const PachInfo w = ParsePach(0x1000, [&](uint32_t a) { return wrapped.Read(a); });
    assert(w.stop == GuestReadStatus::kBoundary && std::string(w.stop_where) == "count-addr" && w.magic_readable &&
           !w.count_readable);

    FakeGuest data_wrap = PachMemory();
    data_wrap.words[0xFFFFFFFC] = 0x2000;  // object pointer itself is at the top of the 32-bit space
    const PachInfo dw = ParsePach(0xFFFFFFFC, [&](uint32_t a) { return data_wrap.Read(a); });
    assert(dw.stop == GuestReadStatus::kBoundary && std::string(dw.stop_where) == "data-base-addr" &&
           dw.obj_readable && dw.header_readable && dw.magic_readable && dw.count_readable);

    FakeGuest count0 = PachMemory();
    count0.words[0x2004] = 0;
    const PachInfo z = ParsePach(0x1000, [&](uint32_t a) { return count0.Read(a); });
    assert(z.stop == GuestReadStatus::kOk && z.count_readable && z.count == 0 && !z.leaf0_readable);

    FakeGuest no_leaf_size = PachMemory();
    no_leaf_size.forced[0x2010] = GuestReadStatus::kFault;
    const PachInfo ns = ParsePach(0x1000, [&](uint32_t a) { return no_leaf_size.Read(a); });
    assert(ns.stop == GuestReadStatus::kFault && std::string(ns.stop_where) == "leaf0-size" && ns.count_readable &&
           ns.data_base_readable && !ns.leaf0_readable);

    FakeGuest no_leaf_offset;
    no_leaf_offset = PachMemory();
    no_leaf_offset.forced[0x200C] = GuestReadStatus::kUnmapped;
    const PachInfo no = ParsePach(0x1000, [&](uint32_t a) { return no_leaf_offset.Read(a); });
    assert(no.stop == GuestReadStatus::kUnmapped && std::string(no.stop_where) == "leaf0-offset" &&
           !no.leaf0_readable);
  }

  // Heap record + free-list walk, including unreadable table/record/head/block/next and corrupt ranges.
  {
    FakeGuest m = HeapMemory();
    const HeapInfo h = WalkHeap(13, [&](uint32_t a) { return m.Read(a); });
    assert(h.stop == GuestReadStatus::kOk && h.walk_ok && h.table_readable && h.record_readable && h.head_readable);
    assert(h.heap_id == 13 && h.start == 0x14000 && h.end == 0x140A0 && h.total == 160 && h.head == 1);
    assert(h.free_blocks == 2 && h.free_bytes == 128 && h.largest_free == 64);

    FakeGuest no_table;
    const HeapInfo t = WalkHeap(13, [&](uint32_t a) { return no_table.Read(a); });
    assert(t.stop == GuestReadStatus::kUnmapped && std::string(t.stop_where) == "table" && !t.table_readable);

    FakeGuest fault_table;
    fault_table.forced[kHeapTablePtr] = GuestReadStatus::kFault;
    const HeapInfo ft = WalkHeap(13, [&](uint32_t a) { return fault_table.Read(a); });
    assert(ft.stop == GuestReadStatus::kFault && std::string(ft.stop_where) == "table" && !ft.table_readable);

    FakeGuest no_record;
    no_record.words[kHeapTablePtr] = 0x83000;
    const HeapInfo nr = WalkHeap(13, [&](uint32_t a) { return no_record.Read(a); });
    assert(nr.stop == GuestReadStatus::kUnmapped && std::string(nr.stop_where) == "record-start" &&
           nr.table_readable && !nr.record_readable);

    FakeGuest no_record_end = HeapMemory();
    no_record_end.forced[0x83000 + 13 * kHeapEntrySize + 4] = GuestReadStatus::kUnmapped;
    const HeapInfo ne = WalkHeap(13, [&](uint32_t a) { return no_record_end.Read(a); });
    assert(ne.stop == GuestReadStatus::kUnmapped && std::string(ne.stop_where) == "record-end" &&
           ne.table_readable && !ne.record_readable && ne.start == 0x14000);

    FakeGuest no_head = HeapMemory();
    no_head.words.erase(0x14008);
    const HeapInfo nh = WalkHeap(13, [&](uint32_t a) { return no_head.Read(a); });
    assert(nh.stop == GuestReadStatus::kUnmapped && std::string(nh.stop_where) == "head" && nh.record_readable &&
           !nh.head_readable);

    FakeGuest no_block = HeapMemory();
    no_block.words.erase(0x14010);
    const HeapInfo nb = WalkHeap(13, [&](uint32_t a) { return no_block.Read(a); });
    assert(nb.stop == GuestReadStatus::kUnmapped && std::string(nb.stop_where) == "block" && nb.head_readable &&
           !nb.walk_ok && nb.free_blocks == 0);

    FakeGuest fault_block = HeapMemory();
    fault_block.forced[0x14010] = GuestReadStatus::kFault;
    const HeapInfo fb = WalkHeap(13, [&](uint32_t a) { return fault_block.Read(a); });
    assert(fb.stop == GuestReadStatus::kFault && std::string(fb.stop_where) == "block" && fb.head_readable &&
           !fb.walk_ok && fb.free_blocks == 0);

    FakeGuest fault_next = HeapMemory();
    fault_next.forced[0x14018] = GuestReadStatus::kFault;
    const HeapInfo fn = WalkHeap(13, [&](uint32_t a) { return fault_next.Read(a); });
    assert(fn.stop == GuestReadStatus::kFault && std::string(fn.stop_where) == "next" && !fn.walk_ok &&
           fn.free_blocks == 1 && fn.free_bytes == 64);

    FakeGuest cycle = HeapMemory();
    cycle.words[0x14018] = 1;
    const HeapInfo cy = WalkHeap(13, [&](uint32_t a) { return cycle.Read(a); });
    assert(cy.stop == GuestReadStatus::kBoundary && std::string(cy.stop_where) == "cycle" && !cy.walk_ok);

    FakeGuest bad_index = HeapMemory();
    bad_index.words[0x14008] = 20;
    const HeapInfo bi = WalkHeap(13, [&](uint32_t a) { return bad_index.Read(a); });
    assert(bi.stop == GuestReadStatus::kBoundary && std::string(bi.stop_where) == "index");

    FakeGuest bad_block = HeapMemory();
    bad_block.words[0x14010] = 20;
    const HeapInfo bb = WalkHeap(13, [&](uint32_t a) { return bad_block.Read(a); });
    assert(bb.stop == GuestReadStatus::kBoundary && std::string(bb.stop_where) == "block-range");

    FakeGuest record_wrap;
    record_wrap.words[kHeapTablePtr] = 0xFFFFFF00;
    const HeapInfo rw = WalkHeap(55, [&](uint32_t a) { return record_wrap.Read(a); });
    assert(rw.stop == GuestReadStatus::kBoundary && std::string(rw.stop_where) == "record-addr" &&
           !rw.record_readable);

    uint32_t add_out = 0;
    assert(!AddU32(0xFFFFFFFCu, 4, &add_out));
    assert(!AddMulU32(0xFFFFFFF0u, 2, 16, &add_out));
  }

  // Call-through contract: disabled = original only, no callbacks, no argument snapshot side effects; enabled = original
  // exactly once and after() still sees the entry arguments even if the original changed the context.
  {
    FakeCtx ctx;
    ctx.r3.u32 = 7;
    ctx.r4.u32 = 8;
    ctx.r5.u32 = 9;
    ctx.r6.u32 = 10;
    ctx.lr = 0x1234;
    int calls = 0;
    bool before = false, after = false;
    InvokeOriginal(
        ctx, [&] { ++calls; ctx.r3.u32 = 99; }, false, [&](const SavedArgs&, FakeCtx&) { before = true; },
        [&](const SavedArgs&, FakeCtx&) { after = true; });
    assert(calls == 1 && !before && !after && ctx.r3.u32 == 99 && ctx.r4.u32 == 8 && ctx.lr == 0x1234);

    FakeCtx ctx2;
    ctx2.r3.u32 = 7;
    ctx2.r4.u32 = 8;
    ctx2.r5.u32 = 9;
    ctx2.r6.u32 = 10;
    ctx2.lr = 0x1234;
    calls = 0;
    before = after = false;
    bool saved_ok = false, post_ok = false;
    InvokeOriginal(
        ctx2, [&] { ++calls; ctx2.r3.u32 = 99; ctx2.lr = 0x9999; }, true,
        [&](const SavedArgs& s, FakeCtx& c) {
          before = true;
          saved_ok = s.r3 == 7 && s.r4 == 8 && s.r5 == 9 && s.r6 == 10 && s.lr == 0x1234 && c.r3.u32 == 7;
        },
        [&](const SavedArgs& s, FakeCtx& c) {
          after = true;
          post_ok = s.r3 == 7 && s.r4 == 8 && s.r5 == 9 && s.r6 == 10 && s.lr == 0x1234 && c.r3.u32 == 99;
        });
    assert(calls == 1 && before && after && saved_ok && post_ok);

    const SavedArgs snap = SaveArgs(ctx2);
    assert(snap.r3 == 99 && snap.r4 == 8 && snap.r5 == 9 && snap.r6 == 10 && snap.lr == 0x9999);
  }

  std::printf("p25-model-alloc-test PASS\n");
  return 0;
}
