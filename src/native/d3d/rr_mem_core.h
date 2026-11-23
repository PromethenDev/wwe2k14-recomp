// RR1 (docs/tasks/RR-record-replay.md): guest-memory part of a self-contained D3D recording. Header-only, no SDK
// dependencies (host test: src/native/d3d/tests/rr-mem-test.cpp).
//
// The live worker reports every guest read it makes (rr_mem.h NoteRead). The recorder writes each 4 KB page the first
// time it is read inside the recording window and again only when its content changed since it was last written
// (content hash). Replay rebuilds a sparse guest memory: before replaying event `seq` it applies every page record whose
// seq <= `seq` (later records overwrite earlier ones) and every invalidation record with seq < `seq`.
//
// Spaces: kVirtual = guest virtual address below 0xA0000000 (heaps / image); kPhysical = physical address
// (addr & 0x1FFFFFFF) for the 0xA0000000 / 0xC0000000 / 0xE0000000 views and for TranslatePhysical reads, which all
// alias the same physical memory.
//
// Side-file records (sidefile.h version 2; little endian):
//   "MEM1" u32 tag | u32 body_bytes = 16 + 4096 | u32 seq | u32 space | u32 page_addr | u32 reserved | 4096 bytes
//   "INV1" u32 tag | u32 body_bytes = 16        | u32 seq | u32 start (physical) | u32 length | u32 reserved
#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace wwe2k14::d3d::rr {

constexpr uint32_t kPageBytes = 4096;
constexpr uint32_t kMemTag = 0x314D454Du;  // "MEM1"
constexpr uint32_t kInvTag = 0x31564E49u;  // "INV1"
// RR4 finding: shader microcode is captured on the game thread into shader_identity's table (not via events), mostly
// at load time before any window; the recording carries the table once: body = stage, bytes, fnv lo, fnv hi + raw code.
constexpr uint32_t kShdTag = 0x31444853u;  // "SHD1"
// RR4 finding 2: a cold replay reads resources the live worker had cached before the window (texture/vertex caches skip
// the read), so the recording opens with every non-zero physical page (MEM1 records) followed by "PSN1" (body: seq):
// from that seq the whole physical space is covered (unwritten pages are zero in both). 0x1FFF0000+ is the SDK's
// no-access guard and is skipped.
constexpr uint32_t kPsnTag = 0x314E5350u;  // "PSN1"
// RR5 (finding 3: replay served ~24 of live's ~65 resolve binds/frame): images resolved before the window live only in
// the renderer's resolve registry. "RIM1" = one registry entry at window open: body = start, length, format (backend
// RenderFormat), w, h, desc_bytes, key_bytes, reserved, then the registration (ResolveDesc), census key, tight pixels.
constexpr uint32_t kRimTag = 0x314D4952u;  // "RIM1"
constexpr uint32_t kPhysSnapshotEnd = 0x1FFF0000u;
constexpr uint32_t kMemBody = 16 + kPageBytes;
constexpr uint32_t kInvBody = 16;
enum Space : uint32_t { kVirtual = 0, kPhysical = 1 };

// Guest address as a worker reader uses it (host = guest base + address) -> (space, address in that space). false: not
// a recordable address. The SDK maps the 0xE0000000 view at physical + 0x1000 (xmemory.cpp map_info: target
// 0x100001000), so base + 0xE0000000 + x holds physical x + 0x1000; 0xA0000000 / 0xC0000000 map physical 1:1.
inline bool Normalize(uint32_t guest_addr, uint32_t* space, uint32_t* addr) {
  if (guest_addr >= 0xE0000000u) {
    *space = kPhysical;
    *addr = (guest_addr - 0xE0000000u + 0x1000u) & 0x1FFFFFFFu;
  } else if (guest_addr >= 0xA0000000u) {
    *space = kPhysical;
    *addr = guest_addr & 0x1FFFFFFFu;
  } else {
    if (guest_addr < 0x1000u) return false;
    *space = kVirtual;
    *addr = guest_addr;
  }
  return true;
}

// FNV-1a 64 over 8-byte words (page content identity; collisions only cost a missed rewrite of a changed page).
inline uint64_t PageHash(const uint8_t* p) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < kPageBytes; i += 8) {
    uint64_t v;
    std::memcpy(&v, p + i, 8);
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

// Which pages of a read must be written. `page_ptr(space, page_addr)` returns the host pointer of the page start (or
// null: unmapped, skipped and counted); `emit(space, page_addr, bytes)` writes one page record.
class PageDedupe {
 public:
  template <class PagePtr, class Emit>
  void Touch(uint32_t space, uint32_t addr, uint64_t len, PagePtr&& page_ptr, Emit&& emit) {
    if (!len) return;
    const uint64_t end = uint64_t(addr) + len;
    const uint64_t limit = space == kPhysical ? 0x20000000ull : 0xA0000000ull;
    for (uint64_t p = addr & ~uint64_t(kPageBytes - 1); p < end && p < limit; p += kPageBytes) {
      const uint8_t* host = page_ptr(space, uint32_t(p));
      if (!host) {
        ++unmapped_;
        continue;
      }
      const uint64_t key = (uint64_t(space) << 32) | p;
      const uint64_t h = PageHash(host);
      auto [it, inserted] = last_.try_emplace(key, h);
      if (!inserted && it->second == h) {
        ++unchanged_;
        continue;
      }
      it->second = h;
      ++written_;
      emit(space, uint32_t(p), host);
    }
  }
  // Marks a page as known with content hash `h` without writing it (RR4 snapshot: all-zero pages the replay image
  // already holds).
  void Seed(uint32_t space, uint32_t page_addr, uint64_t h) { last_[(uint64_t(space) << 32) | page_addr] = h; }
  void Reset() { last_.clear(); written_ = unchanged_ = unmapped_ = 0; }
  uint64_t written() const { return written_; }
  uint64_t unchanged() const { return unchanged_; }
  uint64_t unmapped() const { return unmapped_; }
  size_t distinct_pages() const { return last_.size(); }

 private:
  std::unordered_map<uint64_t, uint64_t> last_;
  uint64_t written_ = 0, unchanged_ = 0, unmapped_ = 0;
};

inline void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline uint32_t Get32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// Full record (tag + length + body) into `out`.
inline void EncodeMem(std::vector<uint8_t>* out, uint32_t seq, uint32_t space, uint32_t page_addr, const uint8_t* bytes) {
  const size_t at = out->size();
  out->resize(at + 8 + kMemBody);
  uint8_t* o = out->data() + at;
  Put32(o, kMemTag);
  Put32(o + 4, kMemBody);
  Put32(o + 8, seq);
  Put32(o + 12, space);
  Put32(o + 16, page_addr);
  Put32(o + 20, 0);
  std::memcpy(o + 24, bytes, kPageBytes);
}
inline void EncodeInv(std::vector<uint8_t>* out, uint32_t seq, uint32_t start, uint32_t length) {
  const size_t at = out->size();
  out->resize(at + 8 + kInvBody);
  uint8_t* o = out->data() + at;
  Put32(o, kInvTag);
  Put32(o + 4, kInvBody);
  Put32(o + 8, seq);
  Put32(o + 12, start);
  Put32(o + 16, length);
  Put32(o + 20, 0);
}

inline void EncodeShd(std::vector<uint8_t>* out, uint32_t stage, uint64_t fnv, const uint8_t* raw, uint32_t bytes) {
  const size_t at = out->size();
  out->resize(at + 8 + 16 + bytes);
  uint8_t* o = out->data() + at;
  Put32(o, kShdTag);
  Put32(o + 4, 16 + bytes);
  Put32(o + 8, stage);
  Put32(o + 12, bytes);
  Put32(o + 16, uint32_t(fnv));
  Put32(o + 20, uint32_t(fnv >> 32));
  std::memcpy(o + 24, raw, bytes);
}

inline void EncodeRim(std::vector<uint8_t>* out, uint32_t start, uint32_t length, uint32_t format, uint32_t w, uint32_t h,
                      const void* desc, uint32_t desc_bytes, const void* key, uint32_t key_bytes, const uint8_t* px,
                      size_t px_bytes) {
  const size_t body = 32 + size_t(desc_bytes) + key_bytes + px_bytes;
  const size_t at = out->size();
  out->resize(at + 8 + body);
  uint8_t* o = out->data() + at;
  const uint32_t words[10] = {kRimTag, uint32_t(body), start, length, format, w, h, desc_bytes, key_bytes, 0};
  std::memcpy(o, words, sizeof words);
  std::memcpy(o + 40, desc, desc_bytes);
  std::memcpy(o + 40 + desc_bytes, key, key_bytes);
  std::memcpy(o + 40 + desc_bytes + key_bytes, px, px_bytes);
}

// Decoded records (pointers into the loaded blob).
struct MemRecord {
  uint32_t seq, space, page_addr;
  const uint8_t* bytes;
};
struct InvRecord {
  uint32_t seq, start, length;
};
struct ShdRecord {
  uint32_t stage, bytes;
  uint64_t fnv;
  const uint8_t* raw;  // guest order
};
struct RimRecord {
  uint32_t start, length, format, w, h, desc_bytes, key_bytes;
  const uint8_t* desc;
  const uint8_t* key;
  const uint8_t* px;
  uint64_t px_bytes;
};
inline bool DecodeRim(const uint8_t* body, uint32_t body_bytes, RimRecord* r) {
  if (body_bytes < 32) return false;
  r->start = Get32(body), r->length = Get32(body + 4), r->format = Get32(body + 8);
  r->w = Get32(body + 12), r->h = Get32(body + 16), r->desc_bytes = Get32(body + 20), r->key_bytes = Get32(body + 24);
  if (uint64_t(32) + r->desc_bytes + r->key_bytes > body_bytes) return false;
  r->desc = body + 32;
  r->key = r->desc + r->desc_bytes;
  r->px = r->key + r->key_bytes;
  r->px_bytes = body_bytes - 32 - r->desc_bytes - r->key_bytes;
  return r->w && r->h && r->px_bytes % (uint64_t(r->w) * r->h) == 0;
}
inline bool DecodeShd(const uint8_t* body, uint32_t body_bytes, ShdRecord* r) {
  if (body_bytes < 16) return false;
  r->stage = Get32(body);
  r->bytes = Get32(body + 4);
  r->fnv = uint64_t(Get32(body + 8)) | (uint64_t(Get32(body + 12)) << 32);
  r->raw = body + 16;
  return r->stage <= 1 && r->bytes == body_bytes - 16 && (r->bytes & 3) == 0;
}
// body = record body (after tag + length). false: wrong size.
inline bool DecodeMem(const uint8_t* body, uint32_t body_bytes, MemRecord* r) {
  if (body_bytes != kMemBody) return false;
  r->seq = Get32(body);
  r->space = Get32(body + 4);
  r->page_addr = Get32(body + 8);
  r->bytes = body + 16;
  return r->space <= kPhysical && (r->page_addr & (kPageBytes - 1)) == 0;
}
inline bool DecodeInv(const uint8_t* body, uint32_t body_bytes, InvRecord* r) {
  if (body_bytes != kInvBody) return false;
  r->seq = Get32(body);
  r->start = Get32(body + 4);
  r->length = Get32(body + 8);
  return true;
}

// Replay-side cursor: applies records in seq order up to an event. Records must be sorted by seq (stable: the recording
// order within one seq is kept). `write_page(space, page_addr, bytes)` and `invalidate(start, length)` do the work.
class Applier {
 public:
  Applier(const std::vector<MemRecord>* mem, const std::vector<InvRecord>* inv) : mem_(mem), inv_(inv) {}
  // Before replaying event `seq`: pages with record seq <= seq, invalidations with record seq < seq.
  template <class WritePage, class Invalidate>
  void Before(uint32_t seq, WritePage&& write_page, Invalidate&& invalidate) {
    while (inv_i_ < inv_->size() && (*inv_)[inv_i_].seq < seq) {
      const InvRecord& r = (*inv_)[inv_i_++];
      invalidate(r.start, r.length);
    }
    while (mem_i_ < mem_->size() && (*mem_)[mem_i_].seq <= seq) {
      const MemRecord& r = (*mem_)[mem_i_++];
      write_page(r.space, r.page_addr, r.bytes);
    }
  }
  size_t mem_applied() const { return mem_i_; }
  size_t inv_applied() const { return inv_i_; }

 private:
  const std::vector<MemRecord>* mem_;
  const std::vector<InvRecord>* inv_;
  size_t mem_i_ = 0, inv_i_ = 0;
};

}  // namespace wwe2k14::d3d::rr
