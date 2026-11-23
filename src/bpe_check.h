// Bounded native decoder for "BPE " streams (GitHub #125), replacing the guest decoder's unbounded walk. Pure C++, no SDK types, so a host test can include it
// (tools/test-bpe-check). "N:line" = generated/wwe2k14_recomp.N.cpp.
//
// Stream format, as the game's decoder sub_82C45148 (81.cpp:641-937) reads it:
//   header 16 bytes: "BPE " (checked by the #125 caller, 80.cpp:45089-45113), u32 @4, u32 LE @8 = payload length
//   (blob length = 16 + it; the decoder never reads it), u32 LE @12 = output size (81.cpp:655-677).
//   If size == 0 the decoder returns at once (81.cpp:679-680). Otherwise, while output < size (81.cpp:924-926):
//   one block =
//     pair table (loc_82C45228..452C4): left[i] = i for all i (81.cpp:697-753, reset per block), c = 0; repeat
//       b = byte; if b >= 0x80 { c += b - 127; if c == 256 end of table; count = 1 } else count = b + 1;
//       count times { left[c] = byte; if left[c] != c then right[c] = byte; c++ }   until c == 256
//     data count: u16 LE (loc_82C452CC);
//     count data bytes (loc_82C452F0..45344): byte x expands to x if left[x] == x, else to expand(left[x]) then
//       expand(right[x]) (explicit stack at r1+544; right[x] is pushed). The output pointer is NOT bounded inside a
//       block: only after it (loc_82C45348).
// Decode() walks the same bytes, never reads at or past in_len, never writes at or past size, and reports
// kOk only for a stream the game's decoder would decode to exactly `size` bytes without reading past in_len.
// Refused (status != kOk, or output != size):
//   - a pair-table run that would write left[c] for c >= 256 (decoder: past its 256-byte table, and the c == 256
//     exit is then never hit);
//   - a pair cycle (expansion never ends; decoder: stack overruns its frame or output overruns its range);
//   - output that would pass size inside a block, or a stream that ends before size bytes.
#pragma once
#include <cstdint>

namespace bpe_check {

enum class Status : uint32_t {
  kOk = 0,
  kHeaderShort,     // in_len < 16
  kTruncated,       // a read would be at or past in_len
  kTableOverflow,   // pair table index would pass 255
  kPairCycle,       // a used byte's expansion never ends
  kOutputOverrun,   // a block ends past size
};

inline const char* StatusName(Status s) {
  switch (s) {
    case Status::kOk: return "ok";
    case Status::kHeaderShort: return "header-short";
    case Status::kTruncated: return "truncated";
    case Status::kTableOverflow: return "table-overflow";
    case Status::kPairCycle: return "pair-cycle";
    case Status::kOutputOverrun: return "output-overrun";
  }
  return "?";
}

struct Result {
  Status status = Status::kOk;
  uint32_t consumed = 0;  // bytes read, header included (offset one past the last byte read)
  uint64_t output = 0;    // output bytes the decoder would write (saturated)
  uint32_t blocks = 0;
};

// Header words (little-endian bytes). `hdr` must point at 16 readable bytes.
inline uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Blob length 16 + payload word, or 0 when that overflows 32 bits.
inline uint32_t InputLength(uint32_t payload_word) {
  return payload_word > 0xFFFFFFFFu - 16u ? 0u : payload_word + 16u;
}

// Bounded native decode: the same stream walk as sub_82C45148 (81.cpp:641-937), but reads are capped at in_len and
// writes at size. Writes only out[0, size). status kOk <=> the decoder would read only [0, in_len) and write exactly
// size bytes, and then out[0, size) holds exactly what it writes. On any other status out[] holds partial output
// (decode into scratch and copy only on kOk if the target must stay untouched).
// Pair stack: 256 entries. Without a cycle the depth is at most 255 (each stacked entry belongs to a distinct
// non-literal ancestor), so a full stack = a left-cycle (kPairCycle); a right-cycle produces output forever and
// hits the size cap (kOutputOverrun). The game's stack (3648 entries, r1+544..r1+4192) would overrun its frame in
// the first case and its output range in the second.
inline Result Decode(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t size) {
  Result r;
  if (in_len < 16) { r.status = Status::kHeaderShort; return r; }
  uint32_t pos = 16, w = 0;
  uint8_t left[256], right[256], stk[256];
  while (w < size) {  // 81.cpp:677-680 / 924-926
    for (int i = 0; i < 256; ++i) left[i] = uint8_t(i);
    uint32_t c = 0;
    for (;;) {  // pair table, loc_82C45228..452C4
      if (pos >= in_len) { r.status = Status::kTruncated; goto done; }
      const uint32_t b = in[pos++];
      uint32_t count;
      if (b >= 0x80) {
        c += b - 127;
        if (c == 256) break;
        count = 1;
      } else {
        count = b + 1;
      }
      if (c + count > 256) { r.status = Status::kTableOverflow; goto done; }
      for (; count; --count, ++c) {
        if (pos >= in_len) { r.status = Status::kTruncated; goto done; }
        const uint8_t v = in[pos++];
        left[c] = v;
        if (v != c) {
          if (pos >= in_len) { r.status = Status::kTruncated; goto done; }
          right[c] = in[pos++];
        }
      }
      if (c == 256) break;
    }
    if (in_len - pos < 2) { r.status = Status::kTruncated; goto done; }
    uint32_t n = uint32_t(in[pos]) | (uint32_t(in[pos + 1]) << 8);  // loc_82C452CC
    pos += 2;
    if (n > in_len - pos) { r.status = Status::kTruncated; goto done; }  // every data byte is one input byte
    for (; n; --n) {  // loc_82C452F0..45344
      uint32_t x = in[pos++];
      uint32_t sp = 0;
      for (;;) {
        const uint32_t l = left[x];
        if (l == x) {
          if (w >= size) { r.status = Status::kOutputOverrun; goto done; }
          out[w++] = uint8_t(x);
          if (sp == 0) break;
          x = stk[--sp];
        } else {
          if (sp == 256) { r.status = Status::kPairCycle; goto done; }
          stk[sp++] = right[x];
          x = l;
        }
      }
    }
    ++r.blocks;
  }
done:
  r.consumed = pos;
  r.output = w;
  return r;
}

// The hook's decision after it has checked the header, the output range and that [in, in+in_len) is readable.
// has_entry: the caller is the PACH entry site (80.cpp:45116, lr 0x82C39124) and entry_size = its ctx.r28
// (sub_82C38E48's stored entry size, 80.cpp:45083-45101 `mr r28,r3`). Decodes into `scratch` (size bytes); the
// caller copies scratch to the guest output only when refusal == kNone, so a refused call writes nothing.
enum class Refusal : uint32_t { kNone = 0, kLengthOverflow, kLengthOverEntry, kDecode };

inline const char* RefusalName(Refusal f) {
  switch (f) {
    case Refusal::kNone: return "none";
    case Refusal::kLengthOverflow: return "length-overflow";
    case Refusal::kLengthOverEntry: return "length-over-entry";
    case Refusal::kDecode: return "decode";
  }
  return "?";
}

struct Decision {
  Refusal refusal = Refusal::kNone;
  Result decode;
};

inline Decision DecodeChecked(const uint8_t* in, uint32_t in_len, uint32_t size, bool has_entry, uint32_t entry_size,
                              uint8_t* scratch) {
  Decision d;
  if (in_len < 16) { d.refusal = Refusal::kLengthOverflow; return d; }
  if (has_entry && in_len > entry_size) { d.refusal = Refusal::kLengthOverEntry; return d; }
  d.decode = Decode(in, in_len, scratch, size);
  if (d.decode.status != Status::kOk || d.decode.output != size) d.refusal = Refusal::kDecode;
  return d;
}

}  // namespace bpe_check
