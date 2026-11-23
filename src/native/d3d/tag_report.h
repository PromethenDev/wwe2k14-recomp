// F2 step 1 (docs/tasks/F2-scan-removal.md, 11-19): writer-reported always dwords.
//
// A tag-trusted capture scan used to re-read all 76 dwords of cap::kTagAlways. 69 of them are written only by D3D
// functions we hook (static store scan of generated/, 11-19, recorded in docs/NOW.md): each hook below marks the dwords
// its body may write, at entry and at exit (a scan nested inside the body cannot lose a mark), from any thread (atomic),
// whether the native or the guest body runs. A trusted scan then reads the 7 residual always dwords + the marked ones.
// Switch WWE2K14_D3D_TAG_REPORT=1 (default 0). Verified by WWE2K14_D3D_TAG_SCAN=check (0 misses expected).
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

#include "capture_core.h"

namespace wwe2k14::d3d {

bool TagReportOn();
void TagReportMark(uint64_t lo, uint64_t hi);

struct TagReportBits {
  uint64_t lo = 0, hi = 0;
  void Add(uint32_t first_bit, uint32_t n) {
    for (uint32_t b = first_bit; b < first_bit + n && b < cap::kRepBits; ++b) {
      if (b < 64) lo |= 1ull << b;
      else hi |= 1ull << (b - 64);
    }
  }
};

// Which class-a always dwords a hooked D3D function may write (bit layout: cap::kRepDw). Arguments are read at entry.
inline TagReportBits TagReportFor(uint32_t addr, const PPCContext& ctx) {
  TagReportBits r;
  switch (addr) {
    case 0x82CAC430: {  // SetTexture(dev, stage, tex): dev+12920 + 4*stage (stwx)
      const uint32_t s = ctx.r4.u32;
      if (s < 26) r.Add(cap::kRepBitTex + s, 1);
      else r.Add(cap::kRepBitTex, 26);
      break;
    }
    case 0x82CAECB8: {  // SetStreamSource(dev, stream, vb, ...): dev+12836 + 4*stream (stwx), stride byte dev+12904 + stream
      const uint32_t s = ctx.r4.u32;
      if (s < 16) {
        r.Add(cap::kRepBitVb + s, 1);
        r.Add(cap::kRepBitStride + s / 4, 1);
      } else {
        r.Add(cap::kRepBitVb, 16);
        r.Add(cap::kRepBitStride, 4);
      }
      break;
    }
    case 0x82CAEE60: r.Add(cap::kRepBitIndex, 1); break;                // SetIndices: dev+12812
    case 0x82CB42A0: r.Add(cap::kRepBitShaders, 1); break;              // SetPixelShader: dev+13068
    case 0x82CB44A8: r.Add(cap::kRepBitShaders + 1, 1); break;          // SetVertexShader: dev+13072
    case 0x82CB4E10: r.Add(cap::kRepBitStride, 4); break;               // BeginVertices: stride bytes dev+12904..
    case 0x82CC1E48: r.Add(cap::kRepBitRs2180, 3); break;               // ShaderFlush: dev+10528..10536
    case 0x82CAF5E8:                                                    // SetRenderTarget: RT array (stwx)
    case 0x82CAF978:                                                    // SetDepthStencil: dev+12832
    case 0x82CAFC50:                                                    // ApplyRTDepth: dev+12816..12832
    case 0x82CB8BA8:                                                    // Clear tree (defensive: calls RT/DS helpers)
      r.Add(cap::kRepBitRt, 5);
      break;
    case 0x82CB2320:  // ResolveWorker (+ its single-caller helpers 82CB1A50 / 82CB1FE8): RB_COPY + RT array (stwx)
    case 0x82CB3478:  // ResolvePublic (calls the worker)
      r.Add(cap::kRepBitRbCopy, 12);
      r.Add(cap::kRepBitRt, 5);
      break;
    default: break;
  }
  return r;
}

// Constructed first in every hook wrapper (hooks.cpp), before any native fast path returns.
struct TagReportScope {
  TagReportBits bits;
  TagReportScope(uint32_t addr, const PPCContext& ctx) {
    bits = TagReportFor(addr, ctx);  // addr is a constant in each wrapper: folds to nothing for unmapped hooks
    if (!(bits.lo | bits.hi)) return;
    if (!TagReportOn()) {
      bits = {};
      return;
    }
    TagReportMark(bits.lo, bits.hi);
  }
  ~TagReportScope() {
    if (bits.lo | bits.hi) TagReportMark(bits.lo, bits.hi);
  }
  TagReportScope(const TagReportScope&) = delete;
  TagReportScope& operator=(const TagReportScope&) = delete;
};

}  // namespace wwe2k14::d3d
