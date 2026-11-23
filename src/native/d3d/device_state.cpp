#include "device_state.h"

#include <cstring>
#include <vector>

namespace wwe2k14::d3d {

const MirrorBlock kMirrorBlocks[kMirrorBlockCount] = {
    {"fetch", 0x4800, kOffFetch, kFetchSlots * 6, false},
    {"vs_const", 0x4000, kOffVsConst, 256 * 4, false},
    {"ps_const", 0x4400, kOffPsConst, 256 * 4, false},
    {"bool", 0x4900, kOffBool, 8, false},
    {"loop", 0x4908, kOffLoop, 32, true},
    {"rs2000", 0x2000, kOffRs2000, 19, false},  // regs 0x2000-0x2005 V, rest C
    {"rs2100", 0x2100, kOffRs2100, 21, false},
    {"rs2180", 0x2180, kOffRs2180, 5, true},
    {"rs2200", 0x2200, kOffRs2200, 12, false},
    {"rs2280", 0x2280, kOffRs2280, 21, true},
    {"rs2300", 0x2300, kOffRs2300, 38, false},
    {"rs2380", 0x2380, kOffRs2380, 8, false},
};

namespace {
inline uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }
void ReadDw(const uint8_t* dev, uint32_t off, uint32_t* dst, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) dst[i] = Be32(dev + off + 4 * i);
}
const uint32_t* BlockData(const DeviceState& s, uint32_t i) {
  switch (i) {
    case 0: return s.fetch;
    case 1: return s.vs_const;
    case 2: return s.ps_const;
    case 3: return s.bool_const;
    case 4: return s.loop_const;
    case 5: return s.rs2000;
    case 6: return s.rs2100;
    case 7: return s.rs2180;
    case 8: return s.rs2200;
    case 9: return s.rs2280;
    case 10: return s.rs2300;
    default: return s.rs2380;
  }
}
}  // namespace

bool ReadDeviceState(const uint8_t* guest_base, uint32_t device, DeviceState* out) {
  if (!guest_base || !out || device == 0 || (device & 3)) return false;
  const uint8_t* d = guest_base + device;
  out->device = device;
  for (uint32_t i = 0; i < kDirtyCount; ++i) out->dirty[i] = Be64(d + kOffDirty + 8 * i);
  for (uint32_t i = 0; i < kMirrorBlockCount; ++i)
    ReadDw(d, kMirrorBlocks[i].dev_offset, const_cast<uint32_t*>(BlockData(*out, i)), kMirrorBlocks[i].dwords);
  out->pixel_shader = Be32(d + kOffPixelShader);
  out->vertex_shader = Be32(d + kOffVertexShader);
  out->index_buffer = Be32(d + kOffIndexBuffer);
  out->decl_candidate = Be32(d + kOffDecl);
  for (uint32_t s = 0; s < kMaxStreams; ++s) {
    out->stream_vb[s] = Be32(d + kOffStreamVb + 4 * s);
    out->stream_stride_dwords[s] = d[kOffStreamStride + s];
  }
  for (uint32_t i = 0; i < kMaxColorRt; ++i) out->color_rt[i] = Be32(d + kOffColorRt + 4 * i);
  out->depth = Be32(d + kOffDepth);
  for (uint32_t s = 0; s < kMaxTextureStages; ++s) out->texture_ptr[s] = Be32(d + kOffTexture + 4 * s);
  return true;
}

void ForEachMirroredRegister(const DeviceState& s, const std::function<void(uint32_t, uint32_t)>& cb,
                             bool include_candidates) {
  for (uint32_t i = 0; i < kMirrorBlockCount; ++i) {
    const MirrorBlock& b = kMirrorBlocks[i];
    if (b.candidate && !include_candidates) continue;
    const uint32_t* data = BlockData(s, i);
    for (uint32_t k = 0; k < b.dwords; ++k) {
      // Window scissor: mirror dwords 17/18 of the 0x2000 block (dev+10436/10440, written by the D3D scissor setter
      // 0x82CAEBxx as (y<<16|x) with 15-bit fields) reach the GPU through the inline writer 0x82CAC5C0 as
      // PA_SC_WINDOW_SCISSOR_TL/BR = SDK 0x2081/0x2082 with 14-bit fields (0x3FFF masks, recomp.84.cpp 68173-68460).
      if (i == 5 && (k == 17 || k == 18)) {
        const uint32_t v = data[k];
        cb(k == 17 ? 0x2081u : 0x2082u, (v & 0x3FFFu) | (v & 0x3FFF0000u));
        continue;
      }
      cb(b.first_reg + k, data[k]);
    }
  }
}

#ifdef WWE2K14_D3D_SELFTEST
bool DeviceStateSelfTest() {
  constexpr uint32_t kDev = 0x1000;
  std::vector<uint8_t> mem(kDev + 0x4000, 0);
  auto put32 = [&](uint32_t off, uint32_t v) {
    uint8_t* p = mem.data() + kDev + off;
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
  };
  put32(0, 0x80000000u); put32(4, 0x00000001u);  // dirty[0] = 0x8000000000000001
  put32(kOffFetch + 4 * (5 * 6 + 2), 0xAABBCCDDu);  // fetch slot 5 dw 2 -> reg 0x4800+32
  put32(kOffVsConst + 16 * 7 + 4, 0x3F800000u);     // vs c7.y -> reg 0x4000+29
  put32(kOffRs2200 + 4 * 5, 0x00000006u);           // 0x2205
  put32(kOffPixelShader, 0x11223344u);
  put32(kOffStreamVb + 4 * 2, 0xA0000000u);
  mem[kDev + kOffStreamStride + 2] = 8;
  put32(kOffTexture + 4 * 3, 0x55667788u);
  put32(kOffColorRt + 4 * 1, 0x99u);
  put32(kOffDepth, 0x77u);
  DeviceState s;
  if (!ReadDeviceState(mem.data(), kDev, &s)) return false;
  bool ok = s.dirty[0] == 0x8000000000000001ull && s.fetch[32] == 0xAABBCCDDu && s.vs_const[29] == 0x3F800000u &&
            s.rs2200[5] == 6 && s.pixel_shader == 0x11223344u && s.stream_vb[2] == 0xA0000000u &&
            s.stream_stride_dwords[2] == 8 && s.texture_ptr[3] == 0x55667788u && s.color_rt[1] == 0x99u &&
            s.depth == 0x77u;
  uint32_t hits = 0, total = 0;
  ForEachMirroredRegister(s, [&](uint32_t reg, uint32_t v) {
    ++total;
    if ((reg == 0x4820 && v == 0xAABBCCDDu) || (reg == 0x401D && v == 0x3F800000u) || (reg == 0x2205 && v == 6)) ++hits;
  });
  return ok && hits == 3 && total == 192 + 1024 + 1024 + 8 + 32 + 19 + 21 + 5 + 12 + 21 + 38 + 8;
}
#endif

}  // namespace wwe2k14::d3d
