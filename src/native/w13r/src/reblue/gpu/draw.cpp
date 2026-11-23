/**
 * @file    gpu/draw.cpp
 * @brief   What every draw flushes: the guest render state read, the PSO
 *          lookup, and the constant buffer uploads.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"
#if defined(SVR_NATIVE_RENDERER)
#include "svr_resources.h"
#endif

#include <chrono>
#include <cstddef>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <cstring>
#include <mutex>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/format.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_recorder.h"

#include <rex/cvar.h>

#include "gpu/resources.h"
#include "gpu/shaders/shader_cache.h"

#if defined(SVR_NATIVE_RENDERER)
// Graphics option (0.9): depth of field off. The blur is mixed in by the final composite pixel
// shaders (16 of them, one per scene type: also bloom, star and the fade), which lerp the sharp
// scene towards the bokeh one by a factor built from g_f4DOFParam (c20) and g_f4DOFParam2 (c21).
// With both zero the factor is 0 in every one of them (checked in the recompiled HLSL): the picture
// stays sharp and everything else the shader does is kept.
// Test only (2026-11-20, GPU-bound check): never a setting. Changes the picture.
REXCVAR_DEFINE_BOOL(wwe13_diag_gpu_cut, false, "WWE13",
                    "Diagnostic: draws cover only the top-left quarter of their scissor (GPU pixel "
                    "work cut, CPU work unchanged) to tell GPU-bound from CPU-bound; breaks the picture");
REXCVAR_DEFINE_BOOL(wwe12_dof, true, "WWE12",
                    "Depth of field (the background blur in entrances and replays). Off = always sharp.");

namespace {
bool IsDepthOfFieldShader(const bd::gpu::GuestShader *ps) {
  if (!ps || !ps->shaderCacheEntry)
    return false;
  static constexpr u64 kComposites[] = {
      0x0FEC5CECDBA5810Dull, 0xF60E5E4F317563D5ull, 0x9EE429D2C805B600ull, 0xA2D494A6044918DCull,
      0x200B458668E107EFull, 0x927957AA999DA68Eull, 0xA156ED8CA3079353ull, 0xA3D5790C4FA7D782ull,
      0x1B0935BE876A3DA1ull, 0x551EAADB825246D5ull, 0x42A8F6505CC9B6D9ull, 0xF181B25EF875B4EEull,
      0x330EFE242327CDB4ull, 0xB1FF80392ED034E5ull, 0x88BCA1C21E6C4F26ull, 0x9AB82F4C09F85285ull};
  const u64 h = ps->shaderCacheEntry->hash;
  for (u64 k : kComposites)
    if (k == h)
      return true;
  return false;
}
}  // namespace
#endif

namespace bd::gpu {
extern std::atomic<u64> g_render_thread_pso_ns;  // pso_recorder.cpp
namespace {

// BD g_renderStateCache: DWORD array indexed by D3DRS byte offset, written
// only by bdSetRenderState.
constexpr u32 kRenderStateCacheVa = 0x82DBE1A8;

// Guest D3DRENDERSTATETYPE byte offsets. Only the fields
// ReadDeviceRenderState consumes are modeled.
struct BdRenderStateCache {
  u8 pad_0000[40];            // +0x00
  be_u32 zEnable;             // +0x28 (40)  D3DRS_ZENABLE
  be_u32 zFunc;               // +0x2C (44)  D3DRS_ZFUNC
  be_u32 zWriteEnable;        // +0x30 (48)  D3DRS_ZWRITEENABLE
  be_u32 fillMode;            // +0x34 (52)  D3DRS_FILLMODE
  be_u32 cullMode;            // +0x38 (56)  D3DRS_CULLMODE
  u8 pad_003C[48];            // +0x3C
  be_u32 stencilEnable;       // +0x6C (108) D3DRS_STENCILENABLE
  be_u32 twoSidedStencilMode; // +0x70 (112) D3DRS_TWOSIDEDSTENCILMODE
  be_u32 stencilFail;         // +0x74 (116) D3DRS_STENCILFAIL
  be_u32 stencilZFail;        // +0x78 (120) D3DRS_STENCILZFAIL
  be_u32 stencilPass;         // +0x7C (124) D3DRS_STENCILPASS
  be_u32 stencilFunc;         // +0x80 (128) D3DRS_STENCILFUNC
  be_u32 stencilRef;          // +0x84 (132) D3DRS_STENCILREF
  // Cache 0 means never-set and must read as the X360 runtime default
  // 0xFFFFFFFF (BD never writes STENCILMASK/STENCILWRITEMASK).
  be_u32 stencilMask;      // +0x88 (136) D3DRS_STENCILMASK
  be_u32 stencilWriteMask; // +0x8C (140) D3DRS_STENCILWRITEMASK
  u8 pad_0090[212 - 144];  // +0x90
  be_u32 colorWriteEnable; // +0xD4 (212) D3DRS_COLORWRITEENABLE
};
static_assert(offsetof(BdRenderStateCache, zEnable) == 40);
static_assert(offsetof(BdRenderStateCache, zFunc) == 44);
static_assert(offsetof(BdRenderStateCache, zWriteEnable) == 48);
static_assert(offsetof(BdRenderStateCache, fillMode) == 52);
static_assert(offsetof(BdRenderStateCache, cullMode) == 56);
static_assert(offsetof(BdRenderStateCache, stencilEnable) == 108);
static_assert(offsetof(BdRenderStateCache, twoSidedStencilMode) == 112);
static_assert(offsetof(BdRenderStateCache, stencilFail) == 116);
static_assert(offsetof(BdRenderStateCache, stencilZFail) == 120);
static_assert(offsetof(BdRenderStateCache, stencilPass) == 124);
static_assert(offsetof(BdRenderStateCache, stencilFunc) == 128);
static_assert(offsetof(BdRenderStateCache, stencilRef) == 132);
static_assert(offsetof(BdRenderStateCache, stencilMask) == 136);
static_assert(offsetof(BdRenderStateCache, stencilWriteMask) == 140);
static_assert(offsetof(BdRenderStateCache, colorWriteEnable) == 212);

// Blend state comes from the Xenos register shadow, where the guest SDK folds
// AlphaBlendEnable and friends together across many call sites.
// RB_BLENDCONTROL0: COLOR_SRCBLEND[4:0] COLOR_COMB_FCN[7:5]
// COLOR_DESTBLEND[12:8] ALPHA_SRCBLEND[20:16] ALPHA_COMB_FCN[23:21]
// ALPHA_DESTBLEND[28:24], raw D3DBLEND/D3DBLENDOP. RB_COLORCONTROL bit 31 is
// the alpha master enable.
#if defined(SVR_NATIVE_RENDERER)
// SvR (2008 and 2009 share this layout): render state comes from the Xenos register values SvR's
// D3D library keeps in the device, one dword per register, in the blocks its draw functions flush
// (2008: sub_825AB3E8 -> sub_8223BDD8 / sub_8223D740): 0x2000.. at +0x2880 (16 regs), 0x2080.. at +0x28C0 (3: window
// offset, window scissor TL/BR), 0x2100.. at +0x28CC (21), 0x2180.. at +0x2920 (5), 0x2200.. at
// +0x2934 (12), 0x2280.. at +0x2964 (21), 0x2300.. at +0x29B8 (38), 0x2380.. at +0x2A50 (8). The
// SetRenderState_* setters write them immediately, so they are current when the SVR_HOOK_BEFORE
// draw hook runs. Xbox 360 D3D enums are the hardware encodings, which is what the Convert*
// helpers take.
u32 SvrRegisterOffset(u32 reg) {
  struct Block {
    u32 first;
    u32 count;
    u32 offset;
  };
  static constexpr Block kBlocks[] = {
      {0x2000, 16, 0x2880}, {0x2080, 3, 0x28C0},  {0x2100, 21, 0x28CC}, {0x2180, 5, 0x2920},
      {0x2200, 12, 0x2934}, {0x2280, 21, 0x2964}, {0x2300, 38, 0x29B8}, {0x2380, 8, 0x2A50},
  };
  for (const Block &b : kBlocks) {
    if (reg >= b.first && reg < b.first + b.count)
      return b.offset + (reg - b.first) * 4;
  }
  return 0;
}

u32 SvrRegister(u32 device_guest, u32 reg) {
  const u32 offset = SvrRegisterOffset(reg);
  if (!offset)
    return 0;
  const auto *value = bd::mem::at<const be_u32>(device_guest + offset);
  return value ? u32(*value) : 0;
}

void ReadDeviceRenderState(VideoState &s, u32 device_guest) {
  if (!device_guest)
    return;
  bool &dirty = s.dirtyStates.pipelineState;
  PipelineState &ps = s.pipelineState;
  const auto *dev = bd::mem::at<const D3DDevice>(device_guest);

  // RB_BLENDCONTROL0. The XDK writes ONE/ZERO/ADD for both color and alpha when blending is off.
  const u32 blend = SvrRegister(device_guest, 0x2201);
  Video::SetDirtyValue(dirty, ps.alphaBlendEnable, blend != 0x00010001u);
  Video::SetDirtyValue(dirty, ps.srcBlend, ConvertBlendMode(blend & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.blendOp, ConvertBlendOp((blend >> 5) & 0x7u));
  Video::SetDirtyValue(dirty, ps.destBlend, ConvertBlendMode((blend >> 8) & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.srcBlendAlpha, ConvertBlendMode((blend >> 16) & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.blendOpAlpha, ConvertBlendOp((blend >> 21) & 0x7u));
  Video::SetDirtyValue(dirty, ps.destBlendAlpha, ConvertBlendMode((blend >> 24) & 0x1Fu));

  // RB_DEPTHCONTROL. Front-face stencil in [19:8], back face (TWOSIDEDSTENCILMODE, the CCWSTENCIL*
  // setters sub_8223E360/E390/E3C8/E400) func [22:20], fail [25:23], zpass [28:26], zfail [31:29].
  const u32 depth = SvrRegister(device_guest, 0x2200);
  Video::SetDirtyValue(dirty, ps.zEnable, (depth & 0x2u) != 0);
  Video::SetDirtyValue(dirty, ps.zWriteEnable, (depth & 0x4u) != 0);
  Video::SetDirtyValue(dirty, ps.zFunc, ConvertCompareFunc((depth >> 4) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilEnable, (depth & 0x1u) != 0);
  Video::SetDirtyValue(dirty, ps.stencilTwoSided, (depth & 0x80u) != 0);
  Video::SetDirtyValue(dirty, ps.stencilFunc, ConvertCompareFunc((depth >> 8) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilFail, ConvertStencilOp((depth >> 11) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilPass, ConvertStencilOp((depth >> 14) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilZFail, ConvertStencilOp((depth >> 17) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilFuncCCW, ConvertCompareFunc((depth >> 20) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilFailCCW, ConvertStencilOp((depth >> 23) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilPassCCW, ConvertStencilOp((depth >> 26) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilZFailCCW, ConvertStencilOp((depth >> 29) & 0x7u));

  // RB_STENCILREFMASK: ref [7:0], mask [15:8], write mask [23:16].
  const u32 stencil = SvrRegister(device_guest, 0x210D);
  Video::SetDirtyValue(dirty, ps.stencilRef, static_cast<u8>(stencil & 0xFFu));
  Video::SetDirtyValue(dirty, ps.stencilMask, static_cast<u8>((stencil >> 8) & 0xFFu));
  Video::SetDirtyValue(dirty, ps.stencilWriteMask, static_cast<u8>((stencil >> 16) & 0xFFu));

  // PA_SU_SC_MODE_CNTL: cull_front [0], cull_back [1], face [2] (0: front faces are CCW); the X360
  // D3DCULL values are exactly these bits (NONE 0, CW 2, CCW 6; sub_8223DA20). The host front face
  // follows the face bit (as Xenia's pipeline caches do): the same triangles are culled, and the
  // back-face stencil ops and SV_IsFrontFace match the hardware. Dual polygon mode with line fill
  // means wireframe.
  const u32 mode = SvrRegister(device_guest, 0x2205);
  const bool cull_front = (mode & 0x1u) != 0;
  const bool cull_back = (mode & 0x2u) != 0;
  Video::SetDirtyValue(dirty, ps.frontFace,
                       (mode & 0x4u) != 0 ? plume::RenderFrontFace::CLOCKWISE
                                          : plume::RenderFrontFace::COUNTER_CLOCKWISE);
  Video::SetDirtyValue(dirty, ps.cullMode,
                       cull_front == cull_back ? plume::RenderCullMode::NONE
                       : cull_front           ? plume::RenderCullMode::FRONT
                                              : plume::RenderCullMode::BACK);
  const bool wireframe = ((mode >> 3) & 0x3u) == 1 && ((mode >> 5) & 0x7u) == 1;
  Video::SetDirtyValue(dirty, ps.fillMode, wireframe ? plume::RenderFillMode::WIREFRAME
                                                     : plume::RenderFillMode::SOLID);

  // RB_COLOR_MASK, render target 0. A null pixel shader is a depth-only draw on Xenos (the XDK's
  // shader flush sub_8223CF10 programs RB_MODECONTROL = kDepth when device+0x318C is null): without
  // a fragment shader the host would write undefined color, so color writes are masked off.
  const bool depth_only = dev && u32(dev->pixelShader) == 0;
  Video::SetDirtyValue(dirty, ps.colorWriteEnable,
                       depth_only ? 0u : (SvrRegister(device_guest, 0x2104) & 0xFu));

  // Alpha test is fixed-function on the Xbox 360: RB_COLORCONTROL ALPHA_FUNC [2:0] (X360 D3DCMP =
  // Xenos CompareFunction) and ALPHA_TEST_ENABLE [3]; RB_ALPHA_REF is a float (the AlphaRef setter
  // sub_8223DEE0 stores ref / 255). The recompiled pixel shaders can only clip(oC0.w - threshold),
  // i.e. pass when alpha >= threshold, so each function is folded into that test. SvR's engine
  // init (sub_82471898) enables the test with ALPHAFUNC = ALWAYS.
  const u32 color_control = SvrRegister(device_guest, 0x2202);
  const u32 alpha_ref_bits = SvrRegister(device_guest, 0x210E);
  float alpha_ref;
  std::memcpy(&alpha_ref, &alpha_ref_bits, sizeof(alpha_ref));
  bool alpha_test = (color_control & 0x8u) != 0;
  float alpha_threshold = alpha_ref;
  if (alpha_test) {
    switch (color_control & 0x7u) {
    case 7: // ALWAYS
      alpha_test = false;
      break;
    case 6: // GREATEREQUAL: clip(a - ref) as is
      break;
    case 4: // GREATER; at least the smallest normal float, so ref 0 survives flush-to-zero
      alpha_threshold = std::max(std::nextafter(alpha_ref, std::numeric_limits<float>::infinity()),
                                 std::numeric_limits<float>::min());
      break;
    case 0: // NEVER
      alpha_threshold = std::numeric_limits<float>::infinity();
      break;
    default: { // LESS, EQUAL, LESSEQUAL, NOTEQUAL: not expressible as clip(a - t)
      static std::atomic<u32> s_warned{0};
      if (s_warned.fetch_add(1, std::memory_order_relaxed) < 4)
        BD_WARN("[svr] alpha func {} not supported by the recompiled alpha test; drawing "
                "without it",
                color_control & 0x7u);
      alpha_test = false;
      break;
    }
    }
  }
  Video::SetAlphaTestMode(alpha_test);
  Video::SetAlphaThreshold(alpha_threshold);
}

// The window scissor SvR's D3D sends with each draw: SetScissorRect (sub_8225A500) folds the
// viewport and, when D3DRS_SCISSORTESTENABLE is set, the scissor rect into PA_SC_WINDOW_SCISSOR
// TL/BR (registers 0x2081/0x2082): x in [14:0], y in [30:16], BR exclusive. Applied after the
// viewport flush (which resets the host scissor to the viewport).
void SvrApplyScissor(VideoState &s, u32 device_guest) {
  if (!s.command_list)
    return;
  Video::FlushViewport();
  const u32 tl = SvrRegister(device_guest, 0x2081);
  const u32 br = SvrRegister(device_guest, 0x2082);
  if (tl == 0 && br == 0)
    return;  // never programmed yet
  // The bound host target is this many times the guest's size.
  const auto *dev = bd::mem::at<const D3DDevice>(device_guest);
  const float k = dev ? SvrSurfaceScale(u32(dev->renderTargetShadow[0])) : SvrRenderScale();
  const auto sc = [k](i32 v) { return i32(float(v) * k + 0.5f); };
  const i32 left = i32(tl & 0x7FFFu);
  const i32 top = i32((tl >> 16) & 0x7FFFu);
  const i32 right = std::max(left, i32(br & 0x7FFFu));
  const i32 bottom = std::max(top, i32((br >> 16) & 0x7FFFu));
  plume::RenderRect rc{sc(left), sc(top), sc(right), sc(bottom)};
  if (REXCVAR_GET(wwe13_diag_gpu_cut)) {
    rc.right = rc.left + std::max(1, (rc.right - rc.left) / 2);
    rc.bottom = rc.top + std::max(1, (rc.bottom - rc.top) / 2);
  }
  s.command_list->setScissors(&rc, 1);
}
#else
void ReadDeviceRenderState(VideoState &s, u32 device_guest) {
  const auto *dev = bd::mem::at<const D3DDevice>(device_guest);
  if (!dev)
    return;
  const u32 blend = dev->rbBlendControl0;
  const u32 color_control = dev->rbColorControl;

  bool &dirty = s.dirtyStates.pipelineState;
  PipelineState &ps = s.pipelineState;

  Video::SetDirtyValue(dirty, ps.alphaBlendEnable,
                       (color_control & 0x80000000u) != 0);
  Video::SetDirtyValue(dirty, ps.srcBlend, ConvertBlendMode(blend & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.blendOp, ConvertBlendOp((blend >> 5) & 0x7u));
  Video::SetDirtyValue(dirty, ps.destBlend,
                       ConvertBlendMode((blend >> 8) & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.srcBlendAlpha,
                       ConvertBlendMode((blend >> 16) & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.blendOpAlpha,
                       ConvertBlendOp((blend >> 21) & 0x7u));
  Video::SetDirtyValue(dirty, ps.destBlendAlpha,
                       ConvertBlendMode((blend >> 24) & 0x1Fu));

  // Depth/cull/fill/color-write come from BD's own render state cache, not the
  // register shadow, whose enable bits BD suppresses behind regs[3046]==0.
  const auto *rs = bd::mem::at<const BdRenderStateCache>(kRenderStateCacheVa);
  if (rs) {
    Video::SetDirtyValue(dirty, ps.zEnable, rs->zEnable != 0u);
    // bdEngineInit seeds the cache from device getters reblue never fills, so
    // an unwritten slot reads 0 rather than its runtime default, and ZFUNC 0 is
    // D3DCMP_NEVER. Take the X360 defaults for the depth pair until it is set.
    const u32 z_func = rs->zFunc;
    Video::SetDirtyValue(dirty, ps.zFunc,
                         z_func ? ConvertCompareFunc(z_func)
                                : plume::RenderComparisonFunction::LESS_EQUAL);
    Video::SetDirtyValue(dirty, ps.zWriteEnable,
                         z_func == 0u || rs->zWriteEnable != 0u);
    Video::SetDirtyValue(dirty, ps.cullMode, ConvertCullMode(rs->cullMode));
    // The debug wireframe toggle (Shift+F3) and the Visual prim recorder both
    // reach the host only here: they bracket their draws in
    // bdSetRenderState(D3DRS_FILLMODE) with no other host-visible signal.
    Video::SetDirtyValue(dirty, ps.fillMode, ConvertFillMode(rs->fillMode));
    Video::SetDirtyValue(dirty, ps.colorWriteEnable,
                         rs->colorWriteEnable & 0xFu);

    auto mask_or_default = [](be_u32 v) -> u8 {
      return v ? static_cast<u8>(v & 0xFFu) : 0xFFu;
    };
    Video::SetDirtyValue(dirty, ps.stencilEnable, rs->stencilEnable != 0u);
    Video::SetDirtyValue(dirty, ps.stencilTwoSided,
                         rs->twoSidedStencilMode != 0u);
    Video::SetDirtyValue(dirty, ps.stencilFail,
                         ConvertStencilOp(rs->stencilFail));
    Video::SetDirtyValue(dirty, ps.stencilZFail,
                         ConvertStencilOp(rs->stencilZFail));
    Video::SetDirtyValue(dirty, ps.stencilPass,
                         ConvertStencilOp(rs->stencilPass));
    Video::SetDirtyValue(dirty, ps.stencilFunc,
                         ConvertCompareFunc(rs->stencilFunc));
    Video::SetDirtyValue(dirty, ps.stencilRef,
                         static_cast<u8>(rs->stencilRef & 0xFFu));
    Video::SetDirtyValue(dirty, ps.stencilMask,
                         mask_or_default(rs->stencilMask));
    Video::SetDirtyValue(dirty, ps.stencilWriteMask,
                         mask_or_default(rs->stencilWriteMask));
  }
}
#endif  // SVR_NATIVE_RENDERER
} // namespace

bool Video::FlushRenderState(u32 device_guest) {
  std::lock_guard lock(state().mutex);
  return FlushRenderStateLocked(device_guest);
}

extern bool g_draw_profile;
extern std::atomic<u64> g_flush_prof_ns[5];  // pipeline, vs consts, ps consts, shared consts, rest
std::atomic<u64> g_flush_prof_ns[5]{};
namespace {
inline u64 FlushNow() { return u64(std::chrono::steady_clock::now().time_since_epoch().count()); }
}  // namespace

bool Video::FlushRenderStateLocked(u32 device_guest) {
  const u64 fp0 = g_draw_profile ? FlushNow() : 0;
  u64 fp_mark = fp0;
  const auto fp = [&](int i) {
    if (!fp0) return;
    const u64 now = FlushNow();
    g_flush_prof_ns[i].fetch_add(now - fp_mark, std::memory_order_relaxed);
    fp_mark = now;
  };
  auto &s = state();
  // A confirmed device-removed event is terminal: stop recording so the render
  // thread cannot race the fatal dialog into a crash.
  if (DeviceIsLost())
    return false;
  if (!s.command_list_open)
    return false;
  if (!s.draw_framebuffer_bound)
    return false;
  // CPU zone: a GPU zone here would add two GPU timestamps per draw.
  BD_CPU_ZONE("FlushRenderState");

  // Fold the Set*-hook mirrors into the pipelineState the PSO lookup reads.
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexShader,
                s.vertex_shader);
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.pixelShader,
                s.pixel_shader);
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexDeclaration,
                s.vertex_declaration);

  for (u32 i = 0; i < 16; ++i) {
    SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexStrides[i],
                  static_cast<u8>(s.input_slots[i].stride));
  }

  // The PSO's formats must match the framebuffer BindDrawFramebuffer attached,
  // so they come from the same ResolveEffectiveTargets pair it binds.
  {
    GuestTexture *rt = nullptr;
    GuestTexture *ds = nullptr;
    ResolveEffectiveTargets(s, rt, ds);
    const auto rt_format = rt ? rt->format : plume::RenderFormat::UNKNOWN;
    const auto ds_format = ds ? ds->format : plume::RenderFormat::UNKNOWN;
    SetDirtyValue(s.dirtyStates.pipelineState,
                  s.pipelineState.renderTargetFormat, rt_format);
    SetDirtyValue(s.dirtyStates.pipelineState,
                  s.pipelineState.depthStencilFormat, ds_format);
  }

  // RB_DEPTHCONTROL + RB_BLENDCONTROL0 + RB_STENCILREFMASK + the color control
  // alpha enable bit. BD inlines its render state writes at LTCG-eligible sites
  // and routes the rest through bdSetRenderState's dispatch table, so the
  // register shadow merges both.
  ReadDeviceRenderState(s, device_guest);
#if defined(SVR_NATIVE_RENDERER)
  SvrApplyScissor(s, device_guest);
  // A guest pixel shader with no host shader would draw with no fragment shader (undefined color):
  // drop the draw (a null guest pixel shader is depth-only, see ReadDeviceRenderState).
  if (!s.pipelineState.pixelShader) {
    const auto *dev = bd::mem::at<const D3DDevice>(device_guest);
    if (dev && u32(dev->pixelShader) != 0) {
      static std::atomic<u32> s_dropped{0};
      const u32 k = s_dropped.fetch_add(1, std::memory_order_relaxed);
      if (k < 8 || (k & (k - 1)) == 0)
        BD_WARN("[svr] draw #{} dropped: guest pixel shader 0x{:08X} has no host shader", k,
                u32(dev->pixelShader));
      return false;
    }
  }
#endif

  // Anything missing here means the engine has not wired the pipeline up yet.
  if (!s.pipelineState.vertexShader || !s.pipelineState.vertexDeclaration) {
    u32 n;
    if (DiagShouldLog(3, s.render_target, &n)) {
      BD_DEV_WARN("[draw-diag] #{} draw dropped: vs={} decl={} ps={} rt={}x{}", n,
             static_cast<void *>(s.pipelineState.vertexShader),
             static_cast<void *>(s.pipelineState.vertexDeclaration),
             static_cast<void *>(s.pipelineState.pixelShader),
             s.render_target ? s.render_target->width : 0,
             s.render_target ? s.render_target->height : 0);
    }
    return false;
  }

  // D3D12 retains the bound pipeline across draws in a command list, so a clean
  // pipelineState can skip both the cache lookup and the bind.
  // BeginCommandList force-dirties this on every command list reset.
  if (s.dirtyStates.pipelineState) {
    PipelineState lookup = s.pipelineState;
    SanitizePipelineState(lookup);
    bool built = false;
    const auto build_start = std::chrono::steady_clock::now();
    auto *pso = GetOrCreatePipeline(lookup, &built);
    if (built)
      g_render_thread_pso_ns.fetch_add(
          u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - build_start)
                  .count()),
          std::memory_order_relaxed);
    if (!pso) {
      u32 n;
      if (DiagShouldLog(4, s.render_target, &n)) {
        BD_DEV_WARN("[draw-diag] #{} draw dropped: PSO build failed (vs={} ps={} "
               "rt={}x{} fmt={})",
               n, static_cast<void *>(s.pipelineState.vertexShader),
               static_cast<void *>(s.pipelineState.pixelShader),
               s.render_target ? s.render_target->width : 0,
               s.render_target ? s.render_target->height : 0,
               u32(s.pipelineState.renderTargetFormat));
      }
      return false;
    }
    // 'built' means this draw compiled the PSO synchronously, so neither
    // residual nor predictor covered it. Warns once per pipeline, and
    // REBLUE_PSO_CAP builds also capture it for the residual/template tooling.
    RecordPipelineState(lookup, CurrentRenderPassId(), built);
    s.command_list->setPipeline(pso);
    s.current_pso = pso;
  } else if (!s.current_pso) {
    // Clean dirty bits but no PSO bound: the first draw after a command list
    // reset that lost the force-dirty.
    return false;
  }

  // The Set*ShaderConstant wrappers dirty-track these, so clean means the bound
  // constants are still live and the 4 KiB byte swap upload can be skipped.
  // Vulkan push offsets 0/8/16 follow the guest PushConstants member order
  // emitted by the recompiler.
  fp(0);  // everything up to here: state fold, render state, scissor, pipeline lookup/bind
  if (device_guest) {
#if defined(SVR_NATIVE_RENDERER)
    // SvR 2008: its engine also writes constants straight into the device (inline XDK
    // setters), which no hook sees, so upload both stages every draw until those writers
    // are covered.
    s.dirtyStates.vertexShaderConstants = true;
    s.dirtyStates.pixelShaderConstants = true;
#endif
    if (s.dirtyStates.vertexShaderConstants) {
      auto vs_alloc = UploadVertexShaderConstants(device_guest);
      if (vs_alloc.size) {
#if defined(REBLUE_D3D12)
        s.command_list->setGraphicsRootDescriptor(vs_alloc.ref, 0);
#else
        s.command_list->setGraphicsPushConstants(
            kGuestPushConstantRangeIndex, &vs_alloc.gpuAddress, 0, sizeof(u64));
#endif
      }
    }

    fp(1);
    if (s.dirtyStates.pixelShaderConstants) {
#if defined(SVR_NATIVE_RENDERER)
      const bool zero_dof = !REXCVAR_GET(wwe12_dof) && IsDepthOfFieldShader(s.pixel_shader);
#else
      const bool zero_dof = false;
#endif
      auto ps_alloc = UploadPixelShaderConstants(device_guest, zero_dof);
      if (ps_alloc.size) {
#if defined(REBLUE_D3D12)
        s.command_list->setGraphicsRootDescriptor(ps_alloc.ref, 1);
#else
        s.command_list->setGraphicsPushConstants(kGuestPushConstantRangeIndex,
                                                 &ps_alloc.gpuAddress,
                                                 sizeof(u64), sizeof(u64));
#endif
      }
    }

    // SharedConstants rebuilds from live guest state every draw: the sampler
    // fetch constants are written by unhooked recompiled code, so there is no
    // dirty signal. The upload is skipped internally when the built block is
    // byte-identical to the one already bound on this list.
    fp(2);
    auto sc_alloc = UploadSharedConstants(device_guest);
    if (sc_alloc.size) {
#if defined(REBLUE_D3D12)
      s.command_list->setGraphicsRootDescriptor(sc_alloc.ref, 2);
#else
      s.command_list->setGraphicsPushConstants(kGuestPushConstantRangeIndex,
                                               &sc_alloc.gpuAddress,
                                               2 * sizeof(u64), sizeof(u64));
#endif
    }
  }

  fp(3);
  // Lens flare occlusion count: the counter UAV (root descriptor 3 on D3D12,
  // set 4 on Vulkan) for the sun test quad draw bracketed by D3DQuery_Issue
  // BEGIN/END. The pipeline cache swaps occlusion_count_ps in while counting.
  if (s.occlusion_counting) {
    const u32 slot = s.frame.load(std::memory_order_relaxed);
    if (s.occlusion_counter[slot]) {
#if defined(REBLUE_D3D11)
      // (no UAV yet on D3D11: Occlusion::Begin never counts there)
#elif defined(REBLUE_D3D12)
      s.command_list->setGraphicsRootDescriptor(
          s.occlusion_counter[slot]->at(0), 3);
#else
      s.command_list->setGraphicsDescriptorSet(
          s.occlusion_descriptor_set[slot].get(), kOcclusionDescriptorSetIndex);
#endif
    }
  }

  // Clean state is first=255, last=0, so 'first <= last' skips the call when
  // nothing changed. BeginCommandList force-dirties the full range every
  // command list reset: D3D12 IA bindings do not survive begin().
  if (s.dirtyStates.vertexStreamFirst <= s.dirtyStates.vertexStreamLast) {
    const u32 first = s.dirtyStates.vertexStreamFirst;
    const u32 count = u32{s.dirtyStates.vertexStreamLast} - first + 1u;
    s.command_list->setVertexBuffers(first, s.vertex_views + first, count,
                                     s.input_slots + first);
  }

  // Re-binds only when SetIndices changed buffer/size/format, or
  // BeginCommandList force-dirtied after a command list reset.
  if (s.dirtyStates.indices && s.index_view.buffer.ref != nullptr) {
    s.command_list->setIndexBuffer(&s.index_view);
  }

  s.dirtyStates = DirtyStates(false);
  fp(4);
  return true;
}

} // namespace bd::gpu
