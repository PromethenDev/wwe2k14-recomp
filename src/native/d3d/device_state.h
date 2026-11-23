// WP2 (decision 0006): read-only decoder of the guest D3D device state at a draw.
// Method source: docs/reports/20261014-2k14-d3d-layout.md (static; every offset below cites its line there).
// Confidence per layout report: V = verified by >=2 sites, C = candidate. No offset here is runtime-verified;
// WP3 must cross-check every mirrored register against the SDK RegisterFile (decision 0006 rule).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

namespace wwe2k14::d3d {

// ---- register-mirror blocks: device offset = base + 4*(reg - first_reg) (report section 3) ----
struct MirrorBlock {
  const char* name;
  uint32_t first_reg;   // Xenos register index of dword 0
  uint32_t dev_offset;  // byte offset in the device
  uint32_t dwords;
  bool candidate;       // true = C in the report (offset/size or register meaning not verified)
};

inline constexpr uint32_t kFetchSlots = 32;  // 0x4800..0x48BF, 32 x 6 dw (report line 40, V)
inline constexpr uint32_t kMirrorBlockCount = 12;
extern const MirrorBlock kMirrorBlocks[kMirrorBlockCount];

// Offsets of the blocks (report lines 40-51).
inline constexpr uint32_t kOffFetch = 1152;      // 0x480  V (line 40)
inline constexpr uint32_t kOffVsConst = 1920;    // 0x780  V (line 41)  256 vec4
inline constexpr uint32_t kOffPsConst = 6016;    // 0x1780 V (line 42)  256 vec4
inline constexpr uint32_t kOffBool = 10112;      // 0x2780 V (line 43)  8 dw
inline constexpr uint32_t kOffLoop = 10144;      // C (line 44): base 10144 vs 10140 unresolved (index base 0 or 1)
inline constexpr uint32_t kOffRs2000 = 10368;    // 0x2880 V for 0x2000-0x2005, C rest (line 45), 19 dw
inline constexpr uint32_t kOffRs2100 = 10444;    // 0x28CC V mask, C rest (line 46), 21 dw
inline constexpr uint32_t kOffRs2180 = 10528;    // 0x2920 C (line 47), 5 dw
inline constexpr uint32_t kOffRs2200 = 10548;    // 0x2934 V block (line 48), 12 dw
inline constexpr uint32_t kOffRs2280 = 10596;    // 0x2964 C (line 49), 21 dw
inline constexpr uint32_t kOffRs2300 = 10680;    // 0x29B8 V block, C regs (line 50), 38 dw
inline constexpr uint32_t kOffRs2380 = 10832;    // 0x2A50 V block (line 51), 8 dw

// ---- dirty masks (line 59, V): five u64 at dev+0,+8,+16,+24,+32 ----
inline constexpr uint32_t kOffDirty = 0;
inline constexpr uint32_t kDirtyCount = 5;

// ---- pointer / non-mirror fields (report section 4) ----
inline constexpr uint32_t kOffPixelShader = 13068;   // 0x330C V (line 61)
inline constexpr uint32_t kOffVertexShader = 13072;  // 0x3310 V (line 62)
inline constexpr uint32_t kOffIndexBuffer = 12812;   // 0x320C V (line 63)
inline constexpr uint32_t kOffDecl = 12240;          // 0x2FD0 C (line 64); see open question: DeepSeek table calls the
                                                     // setter at this store SetStreamSource, layout report says decl.
inline constexpr uint32_t kOffStreamVb = 12836;      // +4*s, V (line 65)
inline constexpr uint32_t kOffStreamStride = 12904;  // +s, byte = (stride>>2)&0xFF, C (line 67)
inline constexpr uint32_t kOffColorRt = 12816;       // +4*i, i=0..3, V (line 69)
inline constexpr uint32_t kOffDepth = 12832;         // V (line 70)
inline constexpr uint32_t kOffTexture = 12920;       // +4*s, V (line 71)
inline constexpr uint32_t kMaxStreams = 16;          // slot count NOT in report (12904-12836=68 bytes => <=17); 16 = fetch pairs 1912-8*s (line 66)
inline constexpr uint32_t kMaxColorRt = 4;           // line 69
inline constexpr uint32_t kMaxTextureStages = 26;    // reset loop over 26 stages (line 32); 32 fetch slots exist, ptr array length C

struct DeviceState {
  uint32_t device = 0;  // guest address
  uint64_t dirty[kDirtyCount] = {};

  // Raw mirror dwords (host order). Fetch constants, [slot*6 + dword].
  uint32_t fetch[kFetchSlots * 6] = {};
  uint32_t vs_const[256 * 4] = {};
  uint32_t ps_const[256 * 4] = {};
  uint32_t bool_const[8] = {};
  uint32_t loop_const[32] = {};  // C
  uint32_t rs2000[19] = {};
  uint32_t rs2100[21] = {};
  uint32_t rs2180[5] = {};
  uint32_t rs2200[12] = {};
  uint32_t rs2280[21] = {};
  uint32_t rs2300[38] = {};
  uint32_t rs2380[8] = {};

  // Pointers (guest addresses).
  uint32_t pixel_shader = 0;
  uint32_t vertex_shader = 0;
  uint32_t index_buffer = 0;
  uint32_t decl_candidate = 0;  // C
  uint32_t stream_vb[kMaxStreams] = {};
  uint8_t stream_stride_dwords[kMaxStreams] = {};  // C; raw byte at 12904+s, i.e. stride/4
  uint32_t color_rt[kMaxColorRt] = {};
  uint32_t depth = 0;
  uint32_t texture_ptr[kMaxTextureStages] = {};
};

// Decode the device at `device` (guest address) out of host memory where guest address A lives at guest_base + A.
// Returns false only for null arguments / device == 0 / misalignment. No bounds check on guest memory.
bool ReadDeviceState(const uint8_t* guest_base, uint32_t device, DeviceState* out);

// Calls cb(xenos_register_index, value) for every dword of every mirror block (index = SDK RegisterFile index).
// Candidate blocks (loop consts) are skipped unless include_candidates. Scissor note: dwords 0x2011/0x2012 of the
// 0x2000 block hold window scissor TL/BR that the SDK keeps at 0x2081/0x2082 (report line 45, C): reported here
// as 0x2011/0x2012; WP3 must remap.
void ForEachMirroredRegister(const DeviceState& s, const std::function<void(uint32_t reg, uint32_t value)>& cb,
                             bool include_candidates = true);

#ifdef WWE2K14_D3D_SELFTEST
bool DeviceStateSelfTest();  // synthetic buffer, returns true on pass; not called at runtime
#endif

}  // namespace wwe2k14::d3d
