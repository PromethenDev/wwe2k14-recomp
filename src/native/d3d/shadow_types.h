// Stage 2 (decision 0006) shared POD types. INTEGRATOR-OWNED, frozen at step 0
// (docs/reports/20261014-d3d-stage2-plan.md section 3). Changes go through the integrator.
#pragma once

#include <cstdint>

namespace nr::backend {
enum class TargetFormat : uint32_t;  // opaque; full definition in src/native/nr/nr_backend.h
}

namespace wwe2k14::d3d {

// Event kinds produced by capture (WP9) on the game thread and consumed by the worker.
enum class EvKind : uint8_t { kDraw, kDrawUP, kClear, kResolve, kSetRT, kSetDepth, kBeginTiling, kEndTiling,
                              kShaderLoad, kAluLoad, kSwap, kDeviceReset, kFullSnapshot };
inline constexpr uint32_t kEvKindCount = uint32_t(EvKind::kFullSnapshot) + 1;
inline constexpr const char* kEvKindNames[kEvKindCount] = {"draw", "drawup", "clear", "resolve", "setrt", "setdepth",
    "begintile", "endtile", "shaderload", "aluload", "swap", "reset", "fullsnap"};

// Device-relative dword range; `count_dw` dwords of data follow the header block list.
struct DeltaBlock { uint16_t offset_dw; uint16_t count_dw; };

struct EvHeader {
  uint32_t seq;            // game-thread order, gap-free
  uint32_t frame;          // game Swap count
  EvKind kind; uint8_t emitter; uint16_t n_blocks;   // n_blocks DeltaBlocks + data follow
  uint32_t payload_bytes;
  uint32_t packet_addr;    // physical header address of the emitted draw packet (WP3 method), 0xFFFFFFFF if none
  uint32_t args[10];       // r4..r10 + f1 bits + 2 stack args, per kind (documented per kind in capture.h)
};

struct SurfaceDesc {
  uint32_t object, surface_info, color_or_depth_info, hiz;
  uint32_t width, height;
  uint8_t samples; bool depth; nr::backend::TargetFormat format;
  uint8_t guest_samples = 0;  // card S3-10: guest sample count when the host count was capped (WWE2K14_D3D_AA_CAP), else 0
};

struct ResolveDesc {
  uint32_t src_object, dest_texture, flags; int32_t src_rect[4]; int32_t dst_point[2];
  uint32_t level, slice; uint32_t rb_copy[12];   // 0x2318..0x2323 after the worker body
  bool clear_color, clear_depth; float clear_rgba[4]; float clear_z; uint32_t clear_stencil;
  bool has_dst_point;  // card S3-17b: the game passed pDestPoint (dst_point valid)
};

// Identity of the microcode the GPU executes for one stage (filled by WP5-impl, shader_identity.h).
struct ShaderKey {
  uint64_t fnv = 0;        // FNV-1a-64 of the guest-order microcode bytes = shader manifest key (same value for vs/ps)
  uint32_t object = 0;     // guest address of the bound D3D shader object it was associated with (0 = none)
  uint32_t code_addr = 0;  // PHYSICAL address emitted in the IM_LOAD packet (guest view: 0xA0000000 + code_addr)
  uint32_t bytes = 0;      // microcode length in bytes
  uint8_t stage = 0;       // 0 VS, 1 PS
  // WP5-impl extras
  uint32_t guest_addr = 0;     // effective guest address from the object's sub-block record (0 = not matched)
  uint8_t obj_span_match = 0;  // 1 = packet span == span computed from the bound object's record
  uint8_t immediate = 0;       // 1 = loaded by IM_LOAD_IMMEDIATE (patched copy inline in the ring; code_addr = ring address)
};

}  // namespace wwe2k14::d3d
