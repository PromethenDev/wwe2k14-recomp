// WP9 (decision 0006 stage 2): the pure, dependency-free core of the capture -> worker event format.
// Game side: `Shadow` (a copy of the device mirror as last sent) turns "what changed" into DeltaBlocks.
// Worker side: `Image` (the reconstructed device dwords + the set of overlaid dwords) applies them again and
// updates a DeviceState. Header-only so tools/d3d/capture-stress.cpp can exercise exactly the production code.
//
// Block semantics (see capture.h for the record layout):
//   * "mirror" blocks   = the device mirror's own memory at capture time (host byte order), sent only where it
//                         differs from what the worker already has (diff against `Shadow`).
//   * "overlay" blocks  = register writes that did NOT come from the mirror: the loaded blocks of
//                         LOAD_ALU_CONSTANT packets (and, optionally, inline constant writes) the emitter put in the
//                         ring. Applied after the mirror blocks of the same record, remembered in the `ovl` set,
//                         and replaced by the next mirror block that covers the same dword.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "device_state.h"
#include "shadow_types.h"

namespace wwe2k14::d3d::cap {

inline constexpr uint32_t kImageDwords = 3280;  // device bytes [0, 13120): covers every field the reader decodes

struct Section {
  const char* name;
  uint32_t off, n;  // device dword offset, dword count
};
// Always diffed against the shadow at every record (cheap: ~420 dwords). Offsets from device_state.h.
inline constexpr Section kAlways[] = {
    {"fetch", kOffFetch / 4, kFetchSlots * 6},
    {"boolloop", kOffBool / 4, 40},  // bool (8) + loop (32)
    {"rs", kOffRs2000 / 4, 124},     // rs2000 .. rs2380, contiguous
    {"decl", kOffDecl / 4, 1},
    {"index", kOffIndexBuffer / 4, 1},
    {"rt_ds_vb", kOffColorRt / 4, 21},  // 4 colour RT + depth + 16 stream VB pointers
    {"stride", kOffStreamStride / 4, 4},
    {"tex", kOffTexture / 4, kMaxTextureStages},
    {"shaders", kOffPixelShader / 4, 2},  // PS then VS
};
inline constexpr uint32_t kNumAlways = sizeof(kAlways) / sizeof(kAlways[0]);
// Diffed only where the dirty masks (dev+0 VS, dev+8 PS) say so (8 KB each way): bit 63-i covers dwords [16 i, 16 i + 16).
inline constexpr Section kAluVs = {"vs_const", kOffVsConst / 4, 1024};
inline constexpr Section kAluPs = {"ps_const", kOffPsConst / 4, 1024};
inline constexpr uint32_t kRbCopyOff = kOffRs2300 / 4 + (0x2318 - 0x2300);  // RB_COPY_* 0x2318..0x2323 (12 dwords)
inline constexpr uint32_t kRbCopyN = 12;
inline constexpr uint32_t kOffRs2300Bytes = 10680;  // dev+10680: the block the register-emit helper is passed (r6)

// Card DE-3T: the guest D3D's own pending-upload tags dev+16 / dev+24 / dev+32 (t[0..2]) and the always-section dwords
// each bit stands for. Taken from the flush dispatcher 82CB20C0 (generated/wwe2k14_recomp.85.cpp:7966-8320; Begin 82CB4E10
// and Draw 82CB52D0 repeat it): register group k of a run is uploaded when bit top_bit - k is set (82CC0D18 / 82CC0F58 take
// the leading-zero count as the index), from device dword dw + per * k.
struct TagGroup {
  uint8_t word, top_bit, nbits, per;
  uint16_t dw;
};
inline constexpr TagGroup kTagGroups[] = {
    {0, 57, 16, 1, 10368 / 4},  // 0x2000..0x200F
    {0, 41, 21, 1, 10444 / 4},  // 0x2100..
    {0, 16, 5, 1, 10528 / 4},   // 0x2180..
    {0, 11, 12, 1, 10548 / 4},  // 0x2200..
    {1, 31, 32, 6, 1152 / 4},   // fetch slot s at bit 31 - s (82CC0F58)
    {1, 54, 21, 1, 10596 / 4},  // 0x2280..
    {2, 37, 38, 1, 10680 / 4},  // 0x2300..
    {2, 45, 8, 1, 10832 / 4},   // 0x2380..
    {2, 56, 1, 40, 10112 / 4},  // bool + loop (0x4900.., 40 registers)
};
// Read at every tag-guided scan: never tagged (pointers, decl, index, stride, rs 0x2010..0x2012), written inside a
// dispatch after the Pre scan without a tag in memory (82CC1E48: 10372, 10528..10536, 10580; 82CC0790: 10560), or written
// by the resolve worker outside the tag contract (RB_COPY 10776..10823).
inline constexpr Section kTagAlways[] = {
    {"rs2001", 10372 / 4, 1},   {"rs2010", 10432 / 4, 3},  {"rs2180", 10528 / 4, 3}, {"rs2203", 10560 / 4, 1},
    {"rs2208", 10580 / 4, 1},   {"rb_copy", 10776 / 4, 12}, {"decl", kOffDecl / 4, 1}, {"index", kOffIndexBuffer / 4, 1},
    {"rt_ds_vb", kOffColorRt / 4, 21}, {"stride", kOffStreamStride / 4, 4}, {"tex", kOffTexture / 4, kMaxTextureStages},
    {"shaders", kOffPixelShader / 4, 2},
};
inline constexpr uint32_t kNumTagAlways = sizeof(kTagAlways) / sizeof(kTagAlways[0]);

// F2 step 1 (tag_report.h): with WWE2K14_D3D_TAG_REPORT=1 a trusted scan reads only these residual always dwords (written
// by guest code with no hook, or by an unidentified object: 12240) plus the writer-reported dwords below.
inline constexpr Section kTagResidual[] = {
    {"rs2001", 10372 / 4, 1}, {"rs2010", 10432 / 4, 3}, {"rs2203", 10560 / 4, 1}, {"rs2208", 10580 / 4, 1},
    {"decl", kOffDecl / 4, 1},
};
inline constexpr uint32_t kNumTagResidual = sizeof(kTagResidual) / sizeof(kTagResidual[0]);
// Writer-reported always dwords: bit i stands for device dword kRepDw(i). Runs of consecutive bits are consecutive dwords.
struct RepRun {
  uint16_t bit, dw, n;
};
inline constexpr RepRun kRepRuns[] = {
    {0, 10528 / 4, 3},                 // rs2180..2182 (ShaderFlush)
    {3, 10776 / 4, 12},                // RB_COPY (ResolveWorker)
    {15, kOffIndexBuffer / 4, 22},     // index, color RT[4], depth, stream VB[16] (contiguous)
    {37, kOffStreamStride / 4, 4 + kMaxTextureStages},  // stride bytes (4 dw) + stage textures (contiguous)
    {67, kOffPixelShader / 4, 2},      // PS, VS
};
inline constexpr uint32_t kRepBits = 69;
inline constexpr uint32_t kRepBitRs2180 = 0, kRepBitRbCopy = 3, kRepBitIndex = 15, kRepBitRt = 16, kRepBitVb = 21,
                          kRepBitStride = 37, kRepBitTex = 41, kRepBitShaders = 67;
static_assert(kOffIndexBuffer / 4 + 1 == kOffColorRt / 4 && kOffStreamStride / 4 + 4 == kOffTexture / 4 &&
                  kOffPixelShader / 4 + 1 == kOffVertexShader / 4,
              "reported runs must be contiguous");
// Calls f(dw, n) once per run of consecutive set bits (bit runs never cross a kRepRuns boundary into a gap).
template <class F>
inline void ForEachReported(uint64_t lo, uint64_t hi, F&& f) {
  for (const RepRun& r : kRepRuns) {
    uint32_t i = 0;
    while (i < r.n) {
      const uint32_t b = r.bit + i;
      const bool set = b < 64 ? ((lo >> b) & 1) : ((hi >> (b - 64)) & 1);
      if (!set) { ++i; continue; }
      uint32_t j = i + 1;
      while (j < r.n) {
        const uint32_t bj = r.bit + j;
        if (!(bj < 64 ? ((lo >> bj) & 1) : ((hi >> (bj - 64)) & 1))) break;
        ++j;
      }
      f(uint32_t(r.dw) + i, j - i);
      i = j;
    }
  }
}

// Calls f(off_dw, n_dw) once per run of consecutive set bits of every tag group.
template <class F>
inline void ForEachTagRun(const uint64_t t[3], F&& f) {
  for (const TagGroup& g : kTagGroups) {
    uint64_t x = t[g.word] << (63 - g.top_bit);  // register 0 of the group at bit 63
    x &= ~(~0ull >> g.nbits);
    uint32_t k = 0;
    while (x) {
      const uint32_t z = uint32_t(__builtin_clzll(x));
      x <<= z;
      k += z;
      const uint64_t inv = ~x;
      const uint32_t len = inv ? uint32_t(__builtin_clzll(inv)) : 64u;
      f(uint32_t(g.dw) + g.per * k, g.per * len);
      x = len >= 64 ? 0 : x << len;
      k += len;
    }
  }
}

inline uint32_t Bswap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint32_t LoadRaw(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// Device dword offset of a Xenos register that the mirror holds, false otherwise (scissor 0x2011/0x2012 excluded:
// the mirror stores them in a different format, see device_state.cpp).
inline bool RegToDevDw(uint32_t reg, uint32_t* off_dw) {
  for (uint32_t i = 0; i < kMirrorBlockCount; ++i) {
    const MirrorBlock& b = kMirrorBlocks[i];
    if (reg >= b.first_reg && reg < b.first_reg + b.dwords) {
      const uint32_t k = reg - b.first_reg;
      if (i == 5 && (k == 17 || k == 18)) return false;
      *off_dw = b.dev_offset / 4 + k;
      return true;
    }
  }
  return false;
}

// Growing list of blocks + data, fixed capacity (no allocation on the game thread).
struct Builder {
  static constexpr uint32_t kMaxBlocks = 2048, kMaxDw = 8192;
  DeltaBlock blocks[kMaxBlocks];
  uint32_t data[kMaxDw];
  uint32_t nb = 0, nd = 0;
  bool overflow = false;
  void Clear() { nb = nd = 0; overflow = false; }
  bool Empty() const { return nb == 0; }
  // Append one host-order dword for device dword `off`; extends the last block when contiguous.
  void Put(uint32_t off, uint32_t v) {
    if (nb && uint32_t(blocks[nb - 1].offset_dw) + blocks[nb - 1].count_dw == off && blocks[nb - 1].count_dw < 0xFFFF) {
      if (nd >= kMaxDw) { overflow = true; return; }
      data[nd++] = v;
      ++blocks[nb - 1].count_dw;
      return;
    }
    if (nb >= kMaxBlocks || nd >= kMaxDw) { overflow = true; return; }
    blocks[nb++] = DeltaBlock{uint16_t(off), 1};
    data[nd++] = v;
  }
};

// Game-thread copy of the mirror as the worker last knows it. `valid` = 0 forces the next scan of a dword to send it.
class Shadow {
 public:
  Shadow() { Reset(); }
  void Reset() {
    std::memset(valid_, 0, sizeof valid_);
    std::memset(ovl_, 0, sizeof ovl_);
  }
  bool Overlaid(uint32_t i) const { return ovl_[i] != 0; }

  // Diff [off, off + n) of the device bytes `dev` (big-endian guest memory) against the shadow; append the changed
  // dwords (host order) to `b` and update the shadow. Gaps of one unchanged dword are bridged (a block header costs
  // the same as a dword) unless that dword is overlaid. Returns the number of dwords that differed.
  uint32_t ScanDiff(const uint8_t* dev, uint32_t off, uint32_t n, Builder* b) {
    if (off + n <= kImageDwords) {  // fast path: whole range valid and byte-equal to the live memory
      uint8_t all = 1;
      for (uint32_t i = off; i < off + n; ++i) all &= valid_[i];
      if (all && std::memcmp(raw_ + off, dev + 4 * size_t(off), 4 * size_t(n)) == 0) return 0;
    }
    uint32_t changed = 0;
    int64_t last_put = -2;
    for (uint32_t i = off; i < off + n && i < kImageDwords; ++i) {
      const uint32_t raw = LoadRaw(dev + 4 * size_t(i));
      if (valid_[i] && raw == raw_[i]) continue;
      if (last_put == int64_t(i) - 2) {
        const uint32_t g = i - 1;
        if (!ovl_[g]) {
          const uint32_t graw = LoadRaw(dev + 4 * size_t(g));
          raw_[g] = graw;
          valid_[g] = 1;
          b->Put(g, Bswap32(graw));
        }
      }
      raw_[i] = raw;
      valid_[i] = 1;
      ovl_[i] = 0;
      b->Put(i, Bswap32(raw));
      last_put = i;
      ++changed;
    }
    return changed;
  }
  // Snapshot: every dword of the range regardless of the shadow, except overlaid ones (those stay as they are).
  void ScanForce(const uint8_t* dev, uint32_t off, uint32_t n, Builder* b) {
    for (uint32_t i = off; i < off + n && i < kImageDwords; ++i) {
      if (ovl_[i]) continue;
      const uint32_t raw = LoadRaw(dev + 4 * size_t(i));
      raw_[i] = raw;
      valid_[i] = 1;
      b->Put(i, Bswap32(raw));
    }
  }
  // Counts dwords of [off, off + n) that differ from the shadow without touching it (dirty-mask audit).
  uint32_t CountDiff(const uint8_t* dev, uint32_t off, uint32_t n, uint32_t* first) const {
    uint32_t c = 0;
    for (uint32_t i = off; i < off + n && i < kImageDwords; ++i) {
      const uint32_t raw = LoadRaw(dev + 4 * size_t(i));
      if ((!valid_[i] || raw != raw_[i]) && !ovl_[i]) {
        if (!c && first) *first = i;
        ++c;
      }
    }
    return c;
  }
  // Card DE-3T CHECK: would ScanDiff send dword i (not valid or not byte-equal to the live memory)? Read-only.
  bool WouldSend(const uint8_t* dev, uint32_t i) const {
    return i < kImageDwords && (!valid_[i] || LoadRaw(dev + 4 * size_t(i)) != raw_[i]);
  }
  // Register writes that did not come from the mirror: send as overlay, remember them. The next scan of these
  // dwords sends the mirror value again (valid = 0), which is what ends the overlay.
  void Overlay(uint32_t off, const uint32_t* host_vals, uint32_t n, Builder* ob) {
    for (uint32_t k = 0; k < n; ++k) {
      const uint32_t i = off + k;
      if (i >= kImageDwords) break;
      ovl_[i] = 1;
      valid_[i] = 0;
      ob->Put(i, host_vals[k]);
    }
  }

 private:
  uint32_t raw_[kImageDwords];
  uint8_t valid_[kImageDwords];
  uint8_t ovl_[kImageDwords];
};

// Copies host-order dwords into the DeviceState fields that device dwords [off, off + n) belong to.
// Dwords outside the decoded fields (dirty masks, gaps, unknown fields) are ignored.
inline void ApplyDwordsToState(DeviceState* s, uint32_t off, const uint32_t* v, uint32_t n) {
  struct Range { uint32_t start, count; uint32_t member; };
  static const Range kRanges[] = {
      {kOffFetch / 4, kFetchSlots * 6, uint32_t(offsetof(DeviceState, fetch))},
      {kOffVsConst / 4, 1024, uint32_t(offsetof(DeviceState, vs_const))},
      {kOffPsConst / 4, 1024, uint32_t(offsetof(DeviceState, ps_const))},
      {kOffBool / 4, 8, uint32_t(offsetof(DeviceState, bool_const))},
      {kOffLoop / 4, 32, uint32_t(offsetof(DeviceState, loop_const))},
      {kOffRs2000 / 4, 19, uint32_t(offsetof(DeviceState, rs2000))},
      {kOffRs2100 / 4, 21, uint32_t(offsetof(DeviceState, rs2100))},
      {kOffRs2180 / 4, 5, uint32_t(offsetof(DeviceState, rs2180))},
      {kOffRs2200 / 4, 12, uint32_t(offsetof(DeviceState, rs2200))},
      {kOffRs2280 / 4, 21, uint32_t(offsetof(DeviceState, rs2280))},
      {kOffRs2300 / 4, 38, uint32_t(offsetof(DeviceState, rs2300))},
      {kOffRs2380 / 4, 8, uint32_t(offsetof(DeviceState, rs2380))},
      {kOffDecl / 4, 1, uint32_t(offsetof(DeviceState, decl_candidate))},
      {kOffIndexBuffer / 4, 1, uint32_t(offsetof(DeviceState, index_buffer))},
      {kOffColorRt / 4, kMaxColorRt, uint32_t(offsetof(DeviceState, color_rt))},
      {kOffDepth / 4, 1, uint32_t(offsetof(DeviceState, depth))},
      {kOffStreamVb / 4, kMaxStreams, uint32_t(offsetof(DeviceState, stream_vb))},
      {kOffStreamStride / 4, kMaxStreams / 4, 0xFFFFFFFFu},  // byte fields, handled below
      {kOffTexture / 4, kMaxTextureStages, uint32_t(offsetof(DeviceState, texture_ptr))},
      {kOffPixelShader / 4, 1, uint32_t(offsetof(DeviceState, pixel_shader))},
      {kOffVertexShader / 4, 1, uint32_t(offsetof(DeviceState, vertex_shader))},
  };
  uint32_t pos = off;
  const uint32_t end = off + n;
  while (pos < end) {
    bool hit = false;
    for (const Range& r : kRanges) {
      if (pos < r.start || pos >= r.start + r.count) continue;
      const uint32_t take = (end - pos < r.start + r.count - pos) ? end - pos : r.start + r.count - pos;
      const uint32_t* src = v + (pos - off);
      if (r.member == 0xFFFFFFFFu) {
        for (uint32_t k = 0; k < take; ++k)
          for (uint32_t b = 0; b < 4; ++b)
            s->stream_stride_dwords[(pos - r.start + k) * 4 + b] = uint8_t(src[k] >> (24 - 8 * b));
      } else {
        std::memcpy(reinterpret_cast<uint8_t*>(s) + r.member + 4 * size_t(pos - r.start), src, 4 * size_t(take));
      }
      pos += take;
      hit = true;
      break;
    }
    if (!hit) ++pos;  // not a decoded field
  }
}

// Worker-side reconstruction: the device dwords as the capture side believes them, plus the overlay set.
class Image {
 public:
  Image() { Reset(); }
  void Reset() {
    std::memset(img_, 0, sizeof img_);
    std::memset(ovl_, 0, sizeof ovl_);
  }
  bool Overlaid(uint32_t i) const { return ovl_[i] != 0; }
  const uint32_t* dwords() const { return img_; }
  // `payload` = DeltaBlock[n_blocks] then the data dwords. The first n_mirror blocks are mirror blocks, the rest
  // overlay. Updates `st` too (when given). Returns false (and applies nothing further) on a malformed block.
  bool Apply(const uint8_t* payload, uint32_t n_blocks, uint32_t n_mirror, DeviceState* st) {
    const DeltaBlock* blk = reinterpret_cast<const DeltaBlock*>(payload);
    const uint8_t* dp = payload + sizeof(DeltaBlock) * size_t(n_blocks);
    for (uint32_t i = 0; i < n_blocks; ++i) {
      const DeltaBlock b = blk[i];
      if (uint32_t(b.offset_dw) + b.count_dw > kImageDwords) return false;
      uint32_t tmp[256];
      uint32_t done = 0;
      while (done < b.count_dw) {
        const uint32_t take = (b.count_dw - done > 256) ? 256 : b.count_dw - done;
        std::memcpy(tmp, dp + 4 * size_t(done), 4 * size_t(take));
        const uint32_t o = b.offset_dw + done;
        std::memcpy(img_ + o, tmp, 4 * size_t(take));
        std::memset(ovl_ + o, i >= n_mirror ? 1 : 0, take);
        if (st) ApplyDwordsToState(st, o, tmp, take);
        done += take;
      }
      dp += 4 * size_t(b.count_dw);
    }
    return true;
  }
  // Compares a snapshot (blocks of the live mirror) with the reconstruction; overlaid dwords are skipped. Counts the
  // mismatching dwords, reports the first `max_report` as (offset_dw, ours, live) and, when `heal` is set, copies the
  // snapshot over them (also into `st`).
  struct Mismatch { uint32_t off, ours, live; };
  uint32_t Compare(const uint8_t* payload, uint32_t n_blocks, DeviceState* st, bool heal, Mismatch* rep,
                   uint32_t max_report, uint32_t* n_reported) {
    const DeltaBlock* blk = reinterpret_cast<const DeltaBlock*>(payload);
    const uint32_t* dp = reinterpret_cast<const uint32_t*>(payload + sizeof(DeltaBlock) * size_t(n_blocks));
    uint32_t bad = 0, nrep = 0;
    for (uint32_t i = 0; i < n_blocks; ++i) {
      const DeltaBlock b = blk[i];
      if (uint32_t(b.offset_dw) + b.count_dw > kImageDwords) break;
      for (uint32_t k = 0; k < b.count_dw; ++k) {
        const uint32_t o = b.offset_dw + k, live = dp[k];
        if (ovl_[o] || img_[o] == live) continue;
        ++bad;
        if (rep && nrep < max_report) rep[nrep++] = Mismatch{o, img_[o], live};
        if (heal) {
          img_[o] = live;
          if (st) ApplyDwordsToState(st, o, &live, 1);
        }
      }
      dp += b.count_dw;
    }
    if (n_reported) *n_reported = nrep;
    return bad;
  }

 private:
  uint32_t img_[kImageDwords];
  uint8_t ovl_[kImageDwords];
};

// Which section a device dword belongs to (diagnostics).
inline const char* SectionName(uint32_t off) {
  for (const Section& s : kAlways)
    if (off >= s.off && off < s.off + s.n) return s.name;
  if (off >= kAluVs.off && off < kAluVs.off + kAluVs.n) return kAluVs.name;
  if (off >= kAluPs.off && off < kAluPs.off + kAluPs.n) return kAluPs.name;
  return "other";
}

}  // namespace wwe2k14::d3d::cap
