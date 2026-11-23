#include "../nr/nr_draw_profile.h"
// WP7 (decision 0006 stage 2): DeviceState -> nr::backend::Draw. Worker thread.
//
// ONE state source: the D3D device blocks + draw arguments (RegView), the device pointer fields (DeviceState), the SDK
// Shader analysed from the microcode WP5-impl captured, and the guest memory those objects point to (index buffer,
// vertex streams). Nothing from the SDK register file or the nr frontend is read.
//
// Adapted from nr_frontend.cpp Frontend::Draw (state decode: shader lookup / float24 variants, primitive, vertex fetch
// + indices, constants, viewport, depth/stencil/blend, textures); the pieces that are input-neutral are CALLED from
// nr_shared (ShaderSet, VertexFormat, SamplerFromFetch, SwapDwords) and the SDK draw_util helpers. Left out on purpose
// (packet-renderer-only EDRAM/ownership and copy-out behaviour): UpdateRenderTargets/ownership, held transfers, clear
// replay records, scene-once, P6 internal resolution and P17 fold. D3D VBC is separately implemented below; draws carry
// the full D3D-captured constant blocks.
#include "../load_lock_core.h"
#include "draw.h"
#include "crowd_thin.h"
#include "../nr/fix_alt_knobs.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <map>
#include <set>
#include <atomic>
#include <mutex>
#include <xxhash.h>
#include <rex/memory.h>

#include <rex/graphics/draw_observer.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include "shader_identity.h"
#include "capture.h"
#include "dirty_translate.h"
#include "glitch_detectors.h"
#include "late_upload.h"
#include "targets.h"
#include "present.h"
#include "textures.h"
#include "stat_timing.h"  // C1-T3
#include "gate.h"
#include "worker.h"
#include "weakpc_options.h"
#include "live_options.h"
#include "native_efa4_schedule.h"
#include "texture_quality.h"
#include "wide_core.h"
#include "wide_aspect.h"
#include "ultrawide_probe.h"

#ifdef WWE2K14_HAS_NR
#include "../nr/nr_backend.h"
#include "../nr/nr_shared.h"
#include "const_used_copy.h"
#include "rr_mem.h"
#endif

// Defined in the SDK's Vulkan pipeline cache (no public declaration); same declaration as nr_frontend.cpp.
#if defined(REX_UWP)
// Xbox: the D3D12-only SDK has no Vulkan pipeline cache, so the switch lives here (same name, default and meaning).
REXCVAR_DEFINE_BOOL(vulkan_float24_depth_pixel_rate, false, "GPU",
                    "Shade at pixel rate while converting float24 depth (native renderer; Xbox copy of the SDK cvar)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#else
REXCVAR_DECLARE(bool, vulkan_float24_depth_pixel_rate);
#endif

namespace wwe2k14::d3d {
namespace {
uint32_t g_scene_zdraws = 0;  // card S3-39
uint32_t g_load_zdraws = 0;   // card N4e: depth-writing draws this frame, any surface
uint32_t g_wide_frame = UINT32_MAX;
wide_core::FrameDrawStats g_wide_frame_stats{};
uint32_t g_wide_fullscreen_target = 0;
}  // namespace
namespace {  // card S3-31 census (defined at the end of this file)
uint32_t CensusEvery();
void CensusNote(const DrawSide& side, bool pg, uint64_t ps_key, bool zwrite);
uint32_t MvCensusEvery();  // U0b (defined at the end of this file)
void MvCensusNote(const DrawSide& side, uint64_t vs_key, const uint8_t* vs_constants, const uint64_t* vs_usage,
                  bool zwrite);
}  // namespace

namespace {
const uint8_t* g_guest_base = nullptr;
}
void SetGuestBase(const uint8_t* guest_base) { g_guest_base = guest_base; }

#ifdef WWE2K14_HAS_NR

namespace {

using namespace rex::graphics;
namespace nb = nr::backend;
namespace sh = nr::shared;
using sh::ShaderEntry;
using sh::VFetch;

constexpr uint32_t kPhysView = 0xA0000000u;  // guest view of physical memory
constexpr uint32_t kPhysMask = 0x1FFFFFFFu;

// V17d lane-only crowd pre-translation reuse. A hit is allowed only for the immediately following indexed draw in the
// same frame, with the same D3D device state except VS float constants and identical draw arguments. Clean write-watch
// generations prove unchanged physical inputs; dirty/unavailable watches fall back to byte comparison.
constexpr uint64_t kCrowdFastPsFnv = 0xF5C9EF1CFA228C6Eull;

struct CrowdFastSource {
  uint32_t address = 0, bytes = 0, endian = 0;
  uint64_t generation_sum = 0;
  bool generation_valid = false;
  std::vector<uint8_t> raw;
};
struct CrowdFastLookup {  // V17d: one vertex/index cache lookup whose side effects are accounted without a map walk
  nr::shared::VbCache::Key key;
  uint32_t phys = 0, bytes = 0, id = 0, cache_bytes = 0;
  bool index = false;
  bool dynamic = false;
  int result = 0;
};
struct CrowdFastCache {
  bool valid = false;
  std::vector<CrowdFastLookup> lookups;
  uint32_t frame = 0, seq = 0;
  uint8_t emitter = 0;
  uint32_t args[10] = {};
  uint64_t vs_fnv = 0, ps_fnv = 0;
  DeviceState state{};
  DrawInfo draw_info{};
  uint32_t index_object = 0;
  std::vector<uint8_t> index_object_raw;
  CrowdFastSource index_data;
  std::vector<CrowdFastSource> vertex_sources;
  std::shared_ptr<const nb::Draw> draw;
  DrawSide side;
  uint64_t vertex_bytes = 0, index_bytes = 0;
};
struct CrowdFastStats {
  uint64_t target_draws = 0, misses = 0, fast_hits = 0, checks = 0, mismatches = 0;
  uint64_t input_misses = 0, geometry_misses = 0, cache_misses = 0;
};
CrowdFastCache& CrowdFast() {
  static CrowdFastCache* c = new CrowdFastCache;
  return *c;
}
CrowdFastStats g_crowd_fast_stats;
void CopyCrowdFastDraw(nb::Draw& dst, const std::shared_ptr<const nb::Draw>& payload, const uint8_t* vs_constants,
                       const uint8_t* ps_constants, int const_used_mode, uint32_t ordinal);
void AccountCrowdFastLookups(nr::shared::VbCache& vbc, const std::vector<CrowdFastLookup>& lookups);

struct CrowdFastProfile {
  bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_CROWD_FAST_PROFILE");
    return e && std::strcmp(e, "1") == 0;
  }();
  uint64_t ns[4] = {}, calls[4] = {};
  uint32_t last_logged_frame = UINT32_MAX;
};
CrowdFastProfile& CrowdFastProfiler() {
  static CrowdFastProfile* p = new CrowdFastProfile;
  return *p;
}
inline uint64_t CrowdFastClock() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline uint64_t CrowdFastProfileStart() {
  return CrowdFastProfiler().on ? CrowdFastClock() : 0;
}
inline void CrowdFastProfileAdd(size_t stage, uint64_t start) {
  CrowdFastProfile& p = CrowdFastProfiler();
  if (p.on && start) {
    p.ns[stage] += CrowdFastClock() - start;
    ++p.calls[stage];
  }
}
void LogCrowdFastProfile(uint32_t frame) {
  CrowdFastProfile& p = CrowdFastProfiler();
  if (!p.on || !frame || frame % 300 != 0 || p.last_logged_frame == frame) return;
  REXLOG_INFO("[d3d-crowd-fast-profile] frame={} input_match_ns/call={} vbc_gate_ns/call={} draw_copy_ns/call={} check_save_ns/call={} calls={}/{}/{}/{}",
              frame, p.calls[0] ? p.ns[0] / p.calls[0] : 0, p.calls[1] ? p.ns[1] / p.calls[1] : 0,
              p.calls[2] ? p.ns[2] / p.calls[2] : 0, p.calls[3] ? p.ns[3] / p.calls[3] : 0,
              p.calls[0], p.calls[1], p.calls[2], p.calls[3]);
  p.ns[0] = p.ns[1] = p.ns[2] = p.ns[3] = 0;
  p.calls[0] = p.calls[1] = p.calls[2] = p.calls[3] = 0;
  p.last_logged_frame = frame;
}

struct CrowdFastConfig {
  int mode = 0;  // WWE2K14_D3D_CROWD_FAST: 0 off, 1 fast, 2 full-translate CHECK
  uint32_t block_frames = 0;  // alternating legacy / fast blocks, only with mode 1 and DTR mode 1
};
uint32_t ParseCrowdFastFrames(const char* text, bool* invalid) {
  if (invalid) *invalid = false;
  if (!text || !*text) return 0;
  uint64_t value = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') {
      if (invalid) *invalid = true;
      return 0;
    }
    value = value * 10 + uint64_t(*p - '0');
    if (value > 1000000) {
      if (invalid) *invalid = true;
      return 0;
    }
  }
  return uint32_t(value);
}
struct ConstUsedConfig {
  int mode = 0;  // WWE2K14_D3D_CONST_USED: 0 legacy, 1 sparse, 2 sparse + full-copy CHECK
  uint32_t block_frames = 0;
  bool guard = false;        // WWE2K14_D3D_CONST_USED_GUARD: default off, 0 restores V17e unguarded selection
  bool bounds_diag = false;  // WWE2K14_D3D_CONST_USED_DIAG: first sparse read-mask miss
  bool pad_zero = false;     // WWE2K14_D3D_CONST_USED_PAD_ZERO: zero every sparse hole before copying
};
const ConstUsedConfig& ConstUsedOptions() {
  static const ConstUsedConfig config = [] {
    ConstUsedConfig out;
    const char* mode = std::getenv("WWE2K14_D3D_CONST_USED");
    // ALL-1104 TEST build: const-used (sparse constant copy) on by default with the audited-key guard;
    // WWE2K14_D3D_CONST_USED=0 turns it off, WWE2K14_D3D_CONST_USED_GUARD=0 removes the guard.
    if (!mode || !*mode || std::strcmp(mode, "1") == 0) out.mode = 1;
    else if (std::strcmp(mode, "2") == 0) out.mode = 2;
    else if (std::strcmp(mode, "0") != 0)
      REXLOG_WARN("[d3d-const-used] invalid WWE2K14_D3D_CONST_USED='{}' (expected 0, 1, or 2); disabled", mode);

    const char* guard = std::getenv("WWE2K14_D3D_CONST_USED_GUARD");
    out.guard = !guard || !*guard || std::strcmp(guard, "1") == 0;
    if (guard && *guard && std::strcmp(guard, "0") != 0 && std::strcmp(guard, "1") != 0)
      REXLOG_WARN("[d3d-const-used] invalid WWE2K14_D3D_CONST_USED_GUARD='{}' (expected 0 or 1); disabled",
                  guard);

    const char* bounds_diag = std::getenv("WWE2K14_D3D_CONST_USED_DIAG");
    out.bounds_diag = bounds_diag && std::strcmp(bounds_diag, "1") == 0;
    if (bounds_diag && *bounds_diag && std::strcmp(bounds_diag, "0") != 0 && !out.bounds_diag)
      REXLOG_WARN("[d3d-const-used] invalid WWE2K14_D3D_CONST_USED_DIAG='{}' (expected 0 or 1); disabled",
                  bounds_diag);

    const char* pad_zero = std::getenv("WWE2K14_D3D_CONST_USED_PAD_ZERO");
    out.pad_zero = pad_zero && std::strcmp(pad_zero, "1") == 0;
    if (pad_zero && *pad_zero && std::strcmp(pad_zero, "0") != 0 && !out.pad_zero)
      REXLOG_WARN("[d3d-const-used] invalid WWE2K14_D3D_CONST_USED_PAD_ZERO='{}' (expected 0 or 1); disabled",
                  pad_zero);

    const char* schedule = std::getenv("WWE2K14_D3D_CONST_USED_AB");
    bool invalid = false;
    out.block_frames = ParseCrowdFastFrames(schedule, &invalid);
    if (invalid || (schedule && *schedule && out.block_frames == 0)) {
      REXLOG_WARN("[d3d-const-used] invalid WWE2K14_D3D_CONST_USED_AB='{}' (expected 1..1000000); disabled",
                  schedule ? schedule : "");
      out.block_frames = 0;
    }
    if (out.block_frames && out.mode == 0) {
      REXLOG_WARN("[d3d-const-used] WWE2K14_D3D_CONST_USED_AB needs mode 1 or 2; schedule disabled");
      out.block_frames = 0;
    }
    if (out.guard || out.bounds_diag || out.pad_zero)
      REXLOG_INFO("[d3d-const-used] guard={} bounds_diag={} pad_zero={} (active only while sparse copy is enabled)",
                  out.guard ? 1 : 0, out.bounds_diag ? 1 : 0, out.pad_zero ? 1 : 0);
    return out;
  }();
  return config;
}
const char* ConstUsedModeName(int mode) { return mode == 2 ? "CHECK" : mode == 1 ? "used" : "legacy"; }
void CheckConstUsedReadCoverage(const char* stage, uint64_t key, uint32_t ordinal,
                                const uint64_t* shader_usage, bool dynamic_addressing,
                                const uint64_t* copied_usage) {
  const const_used::CoverageResult coverage =
      const_used::CheckReadCoverage(shader_usage, dynamic_addressing, copied_usage);
  if (coverage.first_missing_register < 0) return;
  static std::atomic<bool> first_miss_reported{false};
  bool expected = false;
  if (!first_miss_reported.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
  REXLOG_ERROR("[d3d-const-used-bounds] first miss stage={} key={:016X} draw_ordinal={} register=c{} "
               "copied_count={} rows_copied={} dynamic_addressing={}",
               stage, key, ordinal, uint32_t(coverage.first_missing_register), coverage.copied_registers,
               const_used::DescribeRows(copied_usage), dynamic_addressing ? 1 : 0);
}
void LogConstUsedStats(const char* tag, uint32_t frame, int mode) {
  const nb::ConstUsedStats stats = nb::ConstUsedTakeStats();
  REXLOG_INFO("{} frame={} mode={} copied_regs={} cleared_regs={} used_row_mismatches={} batch_hash_diffs={} crowd_decision_diffs={}",
              tag, frame, ConstUsedModeName(mode), stats.copied_registers, stats.cleared_registers,
              stats.used_row_mismatches, stats.batch_hash_differences, stats.crowd_decision_differences);
}
int ConstUsedModeForFrame(uint32_t frame) {
  const ConstUsedConfig& config = ConstUsedOptions();
  const uint32_t block = config.block_frames ? frame / config.block_frames : 0;
  const int mode = config.block_frames ? ((block & 1u) ? config.mode : 0) : config.mode;
  static uint32_t last_block = UINT32_MAX;
  static uint32_t last_periodic_frame = UINT32_MAX;
  if (config.block_frames && block != last_block) {
    if (last_block != UINT32_MAX) {
      nb::ConstUsedFrameBoundary();
      LogConstUsedStats("[d3d-const-used-ab] completed", frame, (last_block & 1u) ? config.mode : 0);
    }
    REXLOG_INFO("[d3d-const-used-ab] block={} frames=[{}, {}) mode={} ({})", block,
                uint64_t(block) * config.block_frames, uint64_t(block + 1) * config.block_frames,
                mode, ConstUsedModeName(mode));
    last_block = block;
  } else if (config.mode && !config.block_frames && frame && frame % 300 == 0 && frame != last_periodic_frame) {
    LogConstUsedStats("[d3d-const-used]", frame, mode);
    last_periodic_frame = frame;
  }
  return mode;
}
const CrowdFastConfig& CrowdFastOptions() {
  static const CrowdFastConfig config = [] {
    CrowdFastConfig out;
    const char* mode = std::getenv("WWE2K14_D3D_CROWD_FAST");
    if (mode && std::strcmp(mode, "1") == 0) out.mode = 1;
    else if (mode && std::strcmp(mode, "2") == 0) out.mode = 2;
    else if (mode && *mode && std::strcmp(mode, "0") != 0)
      REXLOG_WARN("[d3d-crowd-fast] invalid WWE2K14_D3D_CROWD_FAST='{}' (expected 0, 1, or 2); disabled", mode);

    const char* schedule = std::getenv("WWE2K14_D3D_CROWD_FAST_AB");
    bool invalid = false;
    out.block_frames = ParseCrowdFastFrames(schedule, &invalid);
    if (invalid) {
      REXLOG_WARN("[d3d-crowd-fast] invalid WWE2K14_D3D_CROWD_FAST_AB='{}' (expected 1..1000000); disabled",
                  schedule ? schedule : "");
      out.block_frames = 0;
    }
    if (out.block_frames && (out.mode != 1 || dtr::Mode() != 1)) {
      REXLOG_WARN("[d3d-crowd-fast] WWE2K14_D3D_CROWD_FAST_AB requires CROWD_FAST=1 and DIRTY_TRANSLATE=1; schedule disabled");
      out.block_frames = 0;
    }
    if (out.mode && dtr::Mode() != 1) {
      REXLOG_WARN("[d3d-crowd-fast] CHECK/fast modes require WWE2K14_D3D_DIRTY_TRANSLATE=1; disabled");
      out.mode = 0;
      out.block_frames = 0;
    }
    const char* census = std::getenv("WWE2K14_D3D_CROWD_TRANSLATE_CENSUS");
    if (out.mode && census && std::strcmp(census, "1") == 0) {
      REXLOG_WARN("[d3d-crowd-fast] disabled while WWE2K14_D3D_CROWD_TRANSLATE_CENSUS=1");
      out.mode = 0;
      out.block_frames = 0;
    }
    return out;
  }();
  return config;
}
int CrowdFastModeForFrame(uint32_t frame, int dtr_mode) {
  const CrowdFastConfig& config = CrowdFastOptions();
  if (config.mode && frame && frame % 300 == 0 && CrowdFastProfiler().on) LogCrowdFastProfile(frame);
  int mode = config.mode;
  uint32_t block = 0;
  if (config.block_frames) {
    block = frame / config.block_frames;
    mode = (block & 1u) ? 1 : 0;  // A is legacy; B is crowd fast-translate
    if (dtr_mode != 1) mode = 0;
  } else if (mode && dtr_mode != 1) {
    mode = 0;
  }
  static uint32_t last_block = UINT32_MAX;
  static int last_mode = -1;
  if (config.block_frames && block != last_block) {
    if (last_block != UINT32_MAX)
      REXLOG_INFO("[d3d-crowd-fast] block {} summary target={} misses={} fast_hits={} checks={} mismatches={} input_misses={} geometry_misses={}",
                  last_block, g_crowd_fast_stats.target_draws, g_crowd_fast_stats.misses, g_crowd_fast_stats.fast_hits,
                  g_crowd_fast_stats.checks, g_crowd_fast_stats.mismatches, g_crowd_fast_stats.input_misses,
                  g_crowd_fast_stats.geometry_misses);
    REXLOG_INFO("[d3d-crowd-fast-ab] block={} frames=[{}, {}) mode={} ({}) DTR={}", block,
                uint64_t(block) * config.block_frames, uint64_t(block + 1) * config.block_frames, mode,
                mode ? "crowd-fast" : "legacy", dtr_mode);
    last_block = block;
    last_mode = mode;
    CrowdFast().valid = false;
    g_crowd_fast_stats = CrowdFastStats{};
  } else if (mode != last_mode) {
    if (last_mode >= 0) CrowdFast().valid = false;
    else REXLOG_INFO("[d3d-crowd-fast] mode={} ({}) DTR={}", mode,
                     mode == 2 ? "CHECK" : mode == 1 ? "fast" : "disabled", dtr_mode);
    last_mode = mode;
  }
  return mode;
}

bool SameDeviceStateExceptVsConstants(const DeviceState& a, const DeviceState& b) {
  return a.device == b.device &&
         std::memcmp(a.fetch, b.fetch, sizeof(a.fetch)) == 0 &&
         std::memcmp(a.ps_const, b.ps_const, sizeof(a.ps_const)) == 0 &&
         std::memcmp(a.bool_const, b.bool_const, sizeof(a.bool_const)) == 0 &&
         std::memcmp(a.loop_const, b.loop_const, sizeof(a.loop_const)) == 0 &&
         std::memcmp(a.rs2000, b.rs2000, sizeof(a.rs2000)) == 0 &&
         std::memcmp(a.rs2100, b.rs2100, sizeof(a.rs2100)) == 0 &&
         std::memcmp(a.rs2180, b.rs2180, sizeof(a.rs2180)) == 0 &&
         std::memcmp(a.rs2200, b.rs2200, sizeof(a.rs2200)) == 0 &&
         std::memcmp(a.rs2280, b.rs2280, sizeof(a.rs2280)) == 0 &&
         std::memcmp(a.rs2300, b.rs2300, sizeof(a.rs2300)) == 0 &&
         std::memcmp(a.rs2380, b.rs2380, sizeof(a.rs2380)) == 0 &&
         a.pixel_shader == b.pixel_shader && a.vertex_shader == b.vertex_shader && a.index_buffer == b.index_buffer &&
         a.decl_candidate == b.decl_candidate && std::memcmp(a.stream_vb, b.stream_vb, sizeof(a.stream_vb)) == 0 &&
         std::memcmp(a.stream_stride_dwords, b.stream_stride_dwords, sizeof(a.stream_stride_dwords)) == 0 &&
         std::memcmp(a.color_rt, b.color_rt, sizeof(a.color_rt)) == 0 && a.depth == b.depth &&
         std::memcmp(a.texture_ptr, b.texture_ptr, sizeof(a.texture_ptr)) == 0;
}
bool SameDrawInfo(const DrawInfo& a, const DrawInfo& b) {
  return a.valid == b.valid && a.kind == b.kind && a.prim == b.prim && a.indexed == b.indexed &&
         a.base_vertex == b.base_vertex && a.start_index == b.start_index && a.count == b.count &&
         a.up_stride_bytes == b.up_stride_bytes;
}
bool SameSurfaceDesc(const SurfaceDesc& a, const SurfaceDesc& b) {
  return a.object == b.object && a.surface_info == b.surface_info && a.color_or_depth_info == b.color_or_depth_info &&
         a.hiz == b.hiz && a.width == b.width && a.height == b.height && a.samples == b.samples && a.depth == b.depth &&
         a.format == b.format && a.guest_samples == b.guest_samples;
}
bool SameStencilFace(const nb::StencilFace& a, const nb::StencilFace& b) {
  return a.fail == b.fail && a.depth_fail == b.depth_fail && a.pass == b.pass && a.compare == b.compare;
}
bool SameBlend(const nb::BlendTarget& a, const nb::BlendTarget& b) {
  return a.enabled == b.enabled && a.src == b.src && a.dst == b.dst && a.op == b.op && a.src_a == b.src_a &&
         a.dst_a == b.dst_a && a.op_a == b.op_a && a.write_mask == b.write_mask;
}
bool SameSampler(const nb::SamplerDesc& a, const nb::SamplerDesc& b) {
  return a.mag_linear == b.mag_linear && a.min_linear == b.min_linear && a.mip_linear == b.mip_linear &&
         a.address_u == b.address_u && a.address_v == b.address_v && a.address_w == b.address_w &&
         a.border == b.border && a.max_anisotropy == b.max_anisotropy &&
         std::memcmp(&a.min_lod, &b.min_lod, sizeof(float)) == 0 &&
         std::memcmp(&a.max_lod, &b.max_lod, sizeof(float)) == 0 &&
         std::memcmp(&a.lod_bias, &b.lod_bias, sizeof(float)) == 0;
}
bool SameDrawSidePayload(const DrawSide& a, const DrawSide& b) {
  return a.vertex_bytes == b.vertex_bytes && a.index_count == b.index_count && a.vs_fnv == b.vs_fnv &&
         a.ps_fnv == b.ps_fnv && a.color_surface == b.color_surface && a.depth_surface == b.depth_surface &&
         SameSurfaceDesc(a.color_desc, b.color_desc) && SameSurfaceDesc(a.depth_desc, b.depth_desc) &&
         a.target_fail == b.target_fail && a.texture_fail == b.texture_fail && a.texture_slots == b.texture_slots &&
         std::memcmp(a.tex_content_key, b.tex_content_key, sizeof(a.tex_content_key)) == 0 && a.note == b.note &&
         a.n_vb_commit == b.n_vb_commit && a.cache_flush_sent == b.cache_flush_sent;
}
bool SameTranslatedDraw(const nb::Draw& a, const nb::Draw& b, const uint8_t* expected_vs, uint32_t expected_ordinal) {
  const auto& a_attributes = a.Attributes();
  const auto& b_attributes = b.Attributes();
  const auto& a_buffers = a.Buffers();
  const auto& b_buffers = b.Buffers();
  const auto& a_indices = a.Indices();
  const auto& b_indices = b.Indices();
  const auto& a_textures = a.Textures();
  const auto& b_textures = b.Textures();
  if (a.color != b.color || a.depth != b.depth || a.vs_key != b.vs_key || a.ps_key != b.ps_key ||
      a.vb_direct_mode != b.vb_direct_mode || a.vs_spirv != b.vs_spirv || a.ps_spirv != b.ps_spirv ||
      a.spec_constants != b.spec_constants || a.sample_shading != b.sample_shading || a.topology != b.topology ||
      a.cull != b.cull || a.front_ccw != b.front_ccw || a.depth_clamp != b.depth_clamp ||
      a_attributes.size() != b_attributes.size() || a_buffers.size() != b_buffers.size() ||
      a_indices.size() != b_indices.size() || a.index32 != b.index32 || a.count != b.count ||
      a_textures.size() != b_textures.size() || a.depth_test != b.depth_test || a.depth_write != b.depth_write ||
      a.depth_compare != b.depth_compare || a.stencil != b.stencil || a.stencil_ref != b.stencil_ref ||
      a.stencil_read != b.stencil_read || a.stencil_write != b.stencil_write || !SameStencilFace(a.stencil_front, b.stencil_front) ||
      !SameStencilFace(a.stencil_back, b.stencil_back) || !SameBlend(a.blend, b.blend) || a.ordinal != expected_ordinal ||
      a.jobs_only != b.jobs_only || a.index_cache_id != b.index_cache_id || a.index_cache_bytes != b.index_cache_bytes ||
      a.index_cache_upload != b.index_cache_upload || a.cache_flush != b.cache_flush ||
      a.pre_ops != b.pre_ops || a.pre_job_hashes != b.pre_job_hashes || a.pre_jobs.size() != b.pre_jobs.size() ||
      a.pre_job_names.size() != b.pre_job_names.size()) return false;
  if (!expected_vs || !const_used::UsedRowsEqual(a.vs_constants, expected_vs, a.vs_float_usage) ||
      std::memcmp(a.ps_constants, b.ps_constants, sizeof(a.ps_constants)) != 0 ||
      std::memcmp(a.shared, b.shared, sizeof(a.shared)) != 0 ||
      std::memcmp(a.viewport, b.viewport, sizeof(a.viewport)) != 0 ||
      std::memcmp(a.scissor, b.scissor, sizeof(a.scissor)) != 0 ||
      std::memcmp(a.blend_constants, b.blend_constants, sizeof(a.blend_constants)) != 0 ||
      (a_indices.size() && std::memcmp(a_indices.data(), b_indices.data(), a_indices.size()) != 0)) return false;
  for (size_t i = 0; i < a_attributes.size(); ++i) {
    const auto& x = a_attributes[i]; const auto& y = b_attributes[i];
    if (x.location != y.location || x.format != y.format || x.binding != y.binding || x.offset_bytes != y.offset_bytes) return false;
  }
  for (size_t i = 0; i < a_buffers.size(); ++i) {
    const auto& x = a_buffers[i]; const auto& y = b_buffers[i];
    if (x.stride_bytes != y.stride_bytes || x.prefix_bytes != y.prefix_bytes || x.cache_id != y.cache_id ||
        x.cache_size != y.cache_size || x.cache_upload != y.cache_upload || x.direct_src != y.direct_src ||
        x.direct_bytes != y.direct_bytes || x.direct_endian != y.direct_endian || x.data.size() != y.data.size() ||
        (x.data.size() && std::memcmp(x.data.data(), y.data.data(), x.data.size()) != 0)) return false;
  }
  for (size_t i = 0; i < a_textures.size(); ++i) {
    const auto& x = a_textures[i]; const auto& y = b_textures[i];
    if (x.slot != y.slot || x.dimension != y.dimension || x.texture != y.texture || !SameSampler(x.sampler, y.sampler)) return false;
  }
  for (size_t i = 0; i < a.pre_job_names.size(); ++i) {
    const char* x = a.pre_job_names[i]; const char* y = b.pre_job_names[i];
    if (x == y) continue;
    if (!x || !y || std::strcmp(x, y) != 0) return false;
  }
  return true;
}
void CopyCrowdFastDraw(nb::Draw& dst, const std::shared_ptr<const nb::Draw>& payload, const uint8_t* vs_constants,
                       const uint8_t* ps_constants, int const_used_mode, uint32_t ordinal) {
  const nb::Draw& src = *payload;
  dst.color = src.color;
  dst.depth = src.depth;
  dst.vs_key = src.vs_key;
  dst.ps_key = src.ps_key;
  dst.vb_direct_mode = src.vb_direct_mode;
  dst.vs_spirv = src.vs_spirv;
  dst.ps_spirv = src.ps_spirv;
  dst.spec_constants = src.spec_constants;
  dst.sample_shading = src.sample_shading;
  dst.topology = src.topology;
  dst.cull = src.cull;
  dst.front_ccw = src.front_ccw;
  dst.depth_clamp = src.depth_clamp;
  dst.shared_payload = payload;
  dst.attributes.clear();
  dst.buffers.clear();
  dst.indices.clear();
  dst.index32 = src.index32;
  dst.count = src.count;
  uint64_t guarded_vs_usage[const_used::kWords];
  uint64_t guarded_ps_usage[const_used::kWords];
  const uint64_t* vs_copy_usage = src.vs_float_usage;
  const uint64_t* ps_copy_usage = src.ps_float_usage;
  if (const_used_mode != 0) {
    (void)const_used::SelectCopyUsage(const_used::ShaderStage::kVertex, src.vs_key, src.vs_float_usage,
                                      false, false, ConstUsedOptions().guard, guarded_vs_usage);
    (void)const_used::SelectCopyUsage(const_used::ShaderStage::kPixel, src.ps_key, src.ps_float_usage,
                                      false, false, ConstUsedOptions().guard, guarded_ps_usage);
    vs_copy_usage = guarded_vs_usage;
    ps_copy_usage = guarded_ps_usage;
  }
  std::memcpy(dst.vs_float_usage, vs_copy_usage, sizeof(dst.vs_float_usage));
  std::memcpy(dst.ps_float_usage, ps_copy_usage, sizeof(dst.ps_float_usage));
  const const_used::CopyResult vs_copy = const_used::Copy(dst.vs_constants, dst.vs_float_retained, vs_constants,
                                                           vs_copy_usage, const_used_mode != 0,
                                                           ConstUsedOptions().pad_zero && const_used_mode != 0);
  const const_used::CopyResult ps_copy = const_used::Copy(dst.ps_constants, dst.ps_float_retained, ps_constants,
                                                           ps_copy_usage, const_used_mode != 0,
                                                           ConstUsedOptions().pad_zero && const_used_mode != 0);
  if (const_used_mode != 0 && ConstUsedOptions().bounds_diag) {
    if (!vs_copy.full_copy)
      CheckConstUsedReadCoverage("VS", src.vs_key, ordinal, src.vs_float_usage, false, vs_copy_usage);
    if (!ps_copy.full_copy)
      CheckConstUsedReadCoverage("PS", src.ps_key, ordinal, src.ps_float_usage, false, ps_copy_usage);
  }
  dst.const_used_mode = uint32_t(const_used_mode);
  uint32_t mismatches = 0;
  if (const_used_mode == 2) {
    auto full = std::make_shared<nb::FullFloatConstants>();
    std::memcpy(full->vs, vs_constants, sizeof(full->vs));
    std::memcpy(full->ps, ps_constants, sizeof(full->ps));
    dst.full_float_constants = std::move(full);
    mismatches = const_used::DifferenceCount(dst.vs_constants, dst.full_float_constants->vs, dst.vs_float_usage) +
                 const_used::DifferenceCount(dst.ps_constants, dst.full_float_constants->ps, dst.ps_float_usage);
  } else {
    dst.full_float_constants.reset();
  }
  if (const_used_mode || ConstUsedOptions().block_frames)
    nb::ConstUsedRecordCopy(vs_copy.copied_registers + ps_copy.copied_registers,
                            vs_copy.cleared_registers + ps_copy.cleared_registers, mismatches);
  std::memcpy(dst.shared, src.shared, sizeof(dst.shared));
  dst.textures.clear();
  std::memcpy(dst.viewport, src.viewport, sizeof(dst.viewport));
  std::memcpy(dst.scissor, src.scissor, sizeof(dst.scissor));
  dst.depth_test = src.depth_test;
  dst.depth_write = src.depth_write;
  dst.depth_compare = src.depth_compare;
  dst.stencil = src.stencil;
  dst.stencil_ref = src.stencil_ref;
  dst.stencil_read = src.stencil_read;
  dst.stencil_write = src.stencil_write;
  dst.stencil_front = src.stencil_front;
  dst.stencil_back = src.stencil_back;
  dst.blend = src.blend;
  std::memcpy(dst.blend_constants, src.blend_constants, sizeof(dst.blend_constants));
  dst.ordinal = ordinal;
  dst.pre_ops.clear();  // cached draws are eligible only when all pending-operation lists are empty
  dst.pre_jobs.clear();
  dst.pre_job_hashes.clear();
  dst.pre_job_names.clear();
  dst.jobs_only = src.jobs_only;
  dst.index_cache_id = src.index_cache_id;
  dst.index_cache_bytes = src.index_cache_bytes;
  dst.index_cache_upload = src.index_cache_upload;
  dst.cache_flush = src.cache_flush;
}
void AccountCrowdFastLookups(nr::shared::VbCache& vbc, const std::vector<CrowdFastLookup>& lookups) {
  for (const CrowdFastLookup& l : lookups) {
    if (l.result == nr::shared::VbCache::kHit) {
      if (l.index) {
        ++vbc.ibc_hit;
        vbc.ibc_hit_bytes += l.cache_bytes;
      } else {
        ++vbc.vbc_hit;
        vbc.vbc_hit_bytes += l.cache_bytes;
      }
    } else if (l.dynamic) {
      if (l.index) ++vbc.ibc_dyn;
      else ++vbc.vbc_dyn;
    }
  }
}
bool CrowdDrawCanBeCached(const nb::Draw& d, const DrawSide& side) {
  if (d.vb_direct_mode || d.pre_ops.size() || d.pre_jobs.size() || d.pre_job_hashes.size() || d.pre_job_names.size() ||
      d.jobs_only || d.cache_flush || d.index_cache_upload || side.texture_fail || side.target_fail ||
      side.n_vb_commit || side.cache_flush_sent) return false;
  for (const nb::VertexBuffer& vb : d.buffers)
    if (vb.direct_src || vb.cache_upload) return false;
  return true;
}
bool SameMemory(const uint8_t* guest_base, bool physical, uint32_t address, const std::vector<uint8_t>& raw) {
  if (!guest_base || raw.empty()) return false;
  const uint32_t guest_address = physical ? kPhysView + (address & kPhysMask) : address;
  if (!GuestRangeOk(guest_address, uint32_t(raw.size()))) return false;
  const uint8_t* current = physical ? guest_base + guest_address : guest_base + address;
  rr::NoteRead(guest_address, raw.size());
  return std::memcmp(current, raw.data(), raw.size()) == 0;
}

std::array<uint32_t, 4> CrowdFastWatchRange(uint32_t address, uint32_t bytes) {
  return {address, address + bytes, 0, 0};
}
bool CaptureCrowdFastGeneration(uint32_t address, uint32_t bytes, uint64_t* generation_sum) {
  if (!generation_sum || !bytes || uint64_t(address) + bytes > 0x20000000ull) return false;
  nr::shared::TextureWatch* const watch = textures::SharedWatch();
  rex::memory::Memory* const memory = textures::SharedMemory();
  if (!watch || !memory) return false;
  const uint32_t first = address >> 12;
  const uint32_t last = uint32_t((uint64_t(address) + bytes - 1) >> 12);
  bool needs_arm = false;
  for (uint32_t page = first; page <= last; ++page) needs_arm |= !watch->Armed(page);
  if (needs_arm) watch->Arm(memory, address, bytes);
  *generation_sum = watch->GenSum(CrowdFastWatchRange(address, bytes));
  return true;
}
bool CrowdFastSourceMatches(CrowdFastSource& source) {
  if (!source.bytes || source.raw.size() != source.bytes) return false;
  nr::shared::TextureWatch* const watch = textures::SharedWatch();
  if (watch && source.generation_valid &&
      watch->Clean(CrowdFastWatchRange(source.address, source.bytes), true, source.generation_sum)) return true;

  uint64_t generation_sum = 0;
  const bool watching = CaptureCrowdFastGeneration(source.address, source.bytes, &generation_sum);
  if (!SameMemory(g_guest_base, true, source.address, source.raw)) return false;
  if (watching) {
    if (!watch->Clean(CrowdFastWatchRange(source.address, source.bytes), true, generation_sum)) return false;
    source.generation_sum = generation_sum;
    source.generation_valid = true;
  }
  return true;
}
bool CrowdFastInputsMatch(CrowdFastCache& c, const EvHeader& e, const RegView& rv, const DeviceState& s,
                          const DrawInfo& di, uint64_t vs_fnv, uint64_t ps_fnv) {
  if (!c.valid || !c.draw || e.kind != EvKind::kDraw || !di.indexed || e.frame != c.frame || e.seq != c.seq + 1 ||
      e.emitter != c.emitter || vs_fnv != c.vs_fnv || ps_fnv != c.ps_fnv ||
      std::memcmp(e.args, c.args, sizeof(c.args)) != 0 || !SameDrawInfo(di, c.draw_info) ||
      !SameDeviceStateExceptVsConstants(s, c.state)) return false;
  (void)rv;  // DrawInfo and DeviceState cover every D3D-sourced input used by indexed TranslateDraw.
  if (!SameMemory(g_guest_base, false, c.index_object, c.index_object_raw) ||
      !CrowdFastSourceMatches(c.index_data)) return false;
  for (CrowdFastSource& source : c.vertex_sources)
    if (source.endian > 3 || !CrowdFastSourceMatches(source)) return false;
  return !c.vertex_sources.empty();
}
void UpdateCrowdFastSide(DrawSide& side, const EvHeader& e) {
  side.packet_addr = e.packet_addr;
  side.seq = e.seq;
}
void SaveCrowdFast(CrowdFastCache& c, const EvHeader& e, const DeviceState& s, const DrawInfo& di,
                   uint64_t vs_fnv, uint64_t ps_fnv, const nb::Draw& d, const DrawSide& side,
                   std::vector<uint8_t>& index_object, CrowdFastSource& index_data,
                   std::vector<CrowdFastSource>& vertex_sources, std::vector<CrowdFastLookup>& lookups) {
  bool stable_lookups = true;
  for (const CrowdFastLookup& l : lookups) {
    const bool stable_hit = l.result == nr::shared::VbCache::kHit && l.id && l.cache_bytes;
    const bool stable_dynamic = l.result == nr::shared::VbCache::kNo && l.dynamic;
    stable_lookups = stable_lookups && (stable_hit || stable_dynamic);
  }
  if (!stable_lookups || index_object.size() != 32 || !index_data.bytes || index_data.raw.empty() ||
      vertex_sources.empty() || !CrowdDrawCanBeCached(d, side)) {
    c.valid = false;
    return;
  }
  c.valid = true;
  c.frame = e.frame;
  c.seq = e.seq;
  c.emitter = e.emitter;
  std::memcpy(c.args, e.args, sizeof(c.args));
  c.vs_fnv = vs_fnv;
  c.ps_fnv = ps_fnv;
  c.state = s;
  std::memset(c.state.dirty, 0, sizeof(c.state.dirty));
  std::memset(c.state.vs_const, 0, sizeof(c.state.vs_const));
  c.draw_info = di;
  c.index_object = s.index_buffer;
  c.index_object_raw = std::move(index_object);
  c.index_data = std::move(index_data);
  c.lookups = std::move(lookups);
  c.vertex_sources = std::move(vertex_sources);
  c.draw = std::make_shared<const nb::Draw>(d);
  c.side = side;
  c.vertex_bytes = 0;
  for (const nb::VertexBuffer& vb : d.buffers) c.vertex_bytes += vb.direct_src ? vb.direct_bytes : vb.data.size();
  c.index_bytes = d.indices.size();
}
void LogCrowdFastStats(uint32_t frame, int mode) {
  static uint32_t last = UINT32_MAX;
  if (mode && frame && frame % 300 == 0 && frame != last) {
    REXLOG_INFO("[d3d-crowd-fast] frame={} mode={} target={} misses={} fast_hits={} checks={} mismatches={} input_misses={} geometry_misses={} cache_misses={}",
                frame, mode, g_crowd_fast_stats.target_draws, g_crowd_fast_stats.misses,
                g_crowd_fast_stats.fast_hits, g_crowd_fast_stats.checks, g_crowd_fast_stats.mismatches,
                g_crowd_fast_stats.input_misses, g_crowd_fast_stats.geometry_misses, g_crowd_fast_stats.cache_misses);
    g_crowd_fast_stats = CrowdFastStats{};
    last = frame;
  }
}

// True = leave this far crowd member out. Runs on the one translate thread. c56-c59 = view-projection, c76-c78 = first bone rows
// (the crowd VS reads them there); the constants are host-order floats in the register file.
bool CrowdThinStep(const EvHeader& e, const RegisterFile& regs, uint64_t vs_key, uint32_t index_count) {
  static const nr::backend::FixAltKnob knob = nr::backend::FixAltKnobFor("WWE2K14_D3D_CROWD_THIN");
  const bool on = nr::backend::FixAltInt(knob, 0) == 1;
  thread_local bool was_on = false;
  if (!on) {
    was_on = false;
    return false;
  }
  // Only the perspective crowd VS: see crowd_thin.h.
  if (vs_key != crowd_thin::kPerspectiveCrowdVs) return false;
  thread_local crowd_thin::Thinner thinner;
  thread_local uint32_t last_log = 0;
  const float* const c = reinterpret_cast<const float*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  if (!was_on) thinner.Reset();
  was_on = true;
  const float position[3] = {c[76 * 4 + 3], c[77 * 4 + 3], c[78 * 4 + 3]};
  const bool drop = thinner.Offer(e.frame, index_count, position, c + 56 * 4);
  if (e.frame - last_log >= 300) {
    const crowd_thin::Stats& st = thinner.stats();
    const double n = st.frames ? double(st.frames) : 1.0;
    REXLOG_INFO("[d3d-crowd-thin] frame={} per frame: near={:.1f} middle={:.1f} far={:.1f} left_out={:.1f} new_seats={:.2f} back={:.2f} redropped={:.2f} other={:.1f} cuts={:.3f} seats={} resets={}",
                e.frame, st.near_members / n, st.middle / n, st.far / n, st.dropped / n, st.new_seats / n, st.returned / n, st.redropped / n, st.other / n, st.cuts / n,
                thinner.seat_count(), st.resets);
    thinner.ClearStats();
    last_log = e.frame;
  }
  return drop;
}

// nr switches that change what nr puts in an nb::Draw; read once from the same environment variables so the shadow
// draw is built under the same configuration (A/B periods are assumed all-on).
struct Cfg {
  bool early_z = false;          // WWE2K14_NR_EARLY_Z=1 (needs NO_MSAA=1 as in nr)
  bool f24_conservative = false;  // WWE2K14_NR_F24_CONSERVATIVE=1
  bool shaders_b = false;         // WWE2K14_NR_SHADERS_B set
  bool regcheck = false;          // WWE2K14_D3D_REGCHECK=1: assert RegView == DeviceState at every draw (diagnostic)
  bool native_dof = false;        // WWE2K14_D3D_NATIVE_DOF=1: hand-written DOF composite PS (card D2, key 4E41544F444F0001)
  uint32_t native_efa4_mode = 0;  // =1 always on; =2 repeats 600 native / 600 translated frames
  Cfg() {
    auto on = [](const char* n) {
      const char* v = std::getenv(n);
      return v && std::strcmp(v, "1") == 0;
    };
    early_z = on("WWE2K14_NR_EARLY_Z") && on("WWE2K14_NR_NO_MSAA");
    // Card S3-22 (WWE2K14_D3D_NATIVE_DEPTH=1): host float32 depth, no float24 pixel-shader variants and no per-sample
    // shading for them, at every sample count (the D3D path has no EDRAM). The game's depth save/restore stays exact
    // because depth snapshots then carry the raw host depth (resolves.cpp, depth_to_r32f.hlsl flag 0x200).
    if (on("WWE2K14_D3D_NATIVE_DEPTH")) early_z = true;
    f24_conservative = on("WWE2K14_NR_F24_CONSERVATIVE");
    const char* b = std::getenv("WWE2K14_NR_SHADERS_B");
    shaders_b = b && *b;
    regcheck = on("WWE2K14_D3D_REGCHECK");
    native_dof = on("WWE2K14_D3D_NATIVE_DOF");
    const char* efa4 = std::getenv("WWE2K14_D3D_NATIVE_EFA4");
    if (efa4 && std::strcmp(efa4, "1") == 0) native_efa4_mode = 1;
    else if (efa4 && std::strcmp(efa4, "2") == 0) native_efa4_mode = 2;
  }
};
const Cfg& Config() {
  static const Cfg c;
  return c;
}

inline uint32_t Be32(uint32_t addr) {
  uint32_t v;
  rr::NoteRead(addr, 4);
  std::memcpy(&v, g_guest_base + addr, 4);
  return __builtin_bswap32(v);
}
inline uint32_t Be32Bytes(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline const uint8_t* Phys(uint32_t phys) { return g_guest_base + kPhysView + (phys & kPhysMask); }

struct Counters {
  uint64_t translated = 0, skipped = 0, rts_waits = 0, rts_wait_ms = 0;
  std::map<std::string, uint64_t> skip_why;
  uint64_t regcheck_draws = 0, regcheck_bad = 0;
};
Counters g_cnt;
uint64_t g_last_log = 0;

std::string g_frame_finals;  // card S3-44 diagnostic: final-pass constants per frame
std::string g_frame_skips;  // card S3-44 diagnostic (FLASH_TRACE): this frame's skipped draws (reason counts)
bool Skip(std::string* why, const std::string& w) {
  if (why) *why = w;
  ++g_cnt.skipped;
  static const bool ft = [] {
    const char* v = std::getenv("WWE2K14_NR_FLASH_TRACE");
    return v && *v == '1';
  }();
  if (ft && w != "primitive 1" && g_frame_skips.size() < 600) g_frame_skips += " [" + w + "]";
  auto it = g_cnt.skip_why.find(w);
  if (it != g_cnt.skip_why.end()) {
    ++it->second;
  } else if (g_cnt.skip_why.size() < 64) {
    g_cnt.skip_why[w] = 1;
  }
  return false;
}

// Card S3-42: a shader without contract is dumped once per key to $WWE2K14_NR_OUT/missing-shaders/<KEY>.<stage>.ucode
// (big-endian dwords, as the nr frontend dumps them) so it can be converted into the shader set.
void DumpMissingShader(const Shader* sh, bool vertex, uint64_t key) {
  if (!sh) return;
  static std::set<uint64_t> dumped;
  const uint64_t dump_key = key ^ (vertex ? 1ull : 0ull);
  const bool already_dumped = dumped.count(dump_key) != 0;
  const bool should_dump = !already_dumped && dumped.size() < 256;
  if (!should_dump && !glitch::CurrentDraw().sampled) return;
  const uint64_t ucode_key = sh::Fnv1a64BigEndian(sh->ucode_dwords(), sh->ucode_dword_count());
  const char* out = std::getenv("WWE2K14_NR_OUT");
  const std::string dir = std::string(out && *out ? out : ".") + "/missing-shaders";
  const std::string name = fmt::format("{:016X}.{}.ucode", ucode_key, vertex ? "vs" : "ps");
  const std::string path = dir + "/" + name;
  const char* dump = path.c_str();
  if (should_dump) {
    dumped.insert(dump_key);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    bool ok = false;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::vector<uint32_t> be(sh->ucode_dword_count());
      for (size_t i = 0; i < be.size(); ++i) be[i] = __builtin_bswap32(sh->ucode_dwords()[i]);
      ok = std::fwrite(be.data(), 4, be.size(), f) == be.size();
      std::fclose(f);
    }
    if (!ok) dump = "FAILED";
    REXLOG_INFO("[d3d-draw] missing shader {:016X}.{} (ucode key {:016X}, {} dwords) dump {}", key,
                vertex ? "vs" : "ps", ucode_key, sh->ucode_dword_count(),
                ok ? dir + "/" + name : std::string("FAILED"));
  } else if (!already_dumped) {
    dump = "dump_cap_reached";
  }
  glitch::MissingShader(glitch::CurrentDraw(), vertex ? 0 : 1, key, "missing_contract", ucode_key,
                        uint32_t(sh->ucode_dword_count()), dump);
}

void MaybeLog() {
  const uint64_t n = g_cnt.translated + g_cnt.skipped;
  if (n - g_last_log < 20000) return;
  g_last_log = n;
  std::string s;
  for (const auto& [k, v] : g_cnt.skip_why) s += " [" + k + "]=" + std::to_string(v);
  REXLOG_INFO("[d3d-draw] translated={} skipped={} rts_waits={} rts_wait_ms={} regcheck draws={} bad={}{}",
              g_cnt.translated, g_cnt.skipped, g_cnt.rts_waits, g_cnt.rts_wait_ms, g_cnt.regcheck_draws,
              g_cnt.regcheck_bad, s);
}

}  // namespace

// S3-3: static vertex buffers kept in the backend (draw-off mode only, so nr's own cache never shares the backend's
// id space; ids from 0x40000000). Entries use nr::shared::VbCache and are revalidated by the guest
// write watch of the texture layer, whole buffer uploaded once, buffers rewritten often fall back to the copy path.
nr::shared::VbCache* VbCacheD3d() {
  static nr::shared::VbCache* c = nullptr;  // worker thread only; created once the texture layer's watch exists
  if (!c) {
    if (nr::shared::TextureWatch* w = textures::SharedWatch()) {
      c = new nr::shared::VbCache(w);
      c->next_id = 0x40000000u;
    }
  }
  return c;
}
struct VbcStats { uint64_t hit = 0, build = 0, no = 0, hit_bytes = 0, build_bytes = 0, ib_hit = 0, ib_build = 0; } g_vbcs;
// S3-3 Q3: why a vertex buffer took the copy path while the cache is active (counts since the previous log line).
// `first`/`dyn` come from VbCache::Lookup's own kNo classification counters (this cache instance is used only here).
struct VbSkipStats { uint64_t inline_src = 0, small = 0, big = 0, full = 0, first = 0, dyn = 0; } g_vbskip;
// S3-3: vertex index range (lo, hi) of each built index-cache entry, by cache id (ids are never reused), so a hit still
// trims the vertex copies like a converted draw. Cleared with the cache (flush).
std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> g_ib_range;

// Perf probe (S3-3; always on, a few clock reads per draw): where translation time goes. Logged every 300 frames.
struct TransProf {
  uint64_t ns[8] = {}, vbytes = 0, ibytes = 0, draws = 0;
  uint32_t last_frame = 0, frames = 0;
} g_tp;
const char* const kTpName[8] = {"setup", "shaders", "targets", "prim", "vertex_fetch", "indices+vcopy", "consts+state", "textures"};
inline uint64_t TpNow() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct TpScope {
  uint64_t t = 0, sc = 1;  // C1-T3: sc = sampling scale N, so the per-frame log stays in real ms/f
  int cur = 0;
  bool tm = false;  // C1-T3: timing frame (stat::On); off = no clock reads at all
  explicit TpScope(uint32_t frame)
      : sc(wwe2k14::d3d::stat::Scale(frame)), tm(wwe2k14::d3d::stat::On(frame)) {
    if (tm) t = TpNow();
  }
  void To(int next) {
    if (tm) {
      const uint64_t n = TpNow();
      g_tp.ns[cur] += (n - t) * sc;
      t = n;
    }
    cur = next;
  }
  ~TpScope() { To(0); }
};

// Card S4-9: last-key shader lookup reuse. Shader-object and contract pointers are retained only for one worker thread
// and one exact (VS key, PS key, PS-presence) tuple. Mode 2 always takes the full path and checks repeated tuples.
int TStageFastMode() {
  static const int mode = [] {
    const char* v = std::getenv("WWE2K14_D3D_TSTAGE_FAST");
    if (!v) return 0;
    if (std::strcmp(v, "1") == 0) return 1;
    if (std::strcmp(v, "2") == 0) return 2;
    return 0;
  }();
  return mode;
}
struct TStageShaderCache {
  bool valid = false;
  uint64_t vkey = 0, pkey = 0;
  bool has_ps = false;
  const Shader* vsh = nullptr;
  const Shader* psh = nullptr;
  const ShaderEntry* vs = nullptr;
  const ShaderEntry* ps = nullptr;
  uint64_t fast_hits = 0, checks = 0, mismatches = 0;
};
TStageShaderCache& TStageCache() {
  thread_local TStageShaderCache cache;
  return cache;
}

// Card S3-44 diagnostic (WWE2K14_NR_FLASH_TRACE=1; remove when settled): for every k_8 texture of >= 320 texels width a
// draw samples (the movie planes), hash its base level when the worker translates the draw and again when the CP thread
// reaches the same draw (after the gate). A difference = the game changed the texture between its D3D call and the
// CP executing the draw - bytes the console GPU would read but the D3D path missed.
struct ProbeTex { uint32_t start, length; uint64_t hash; uint32_t frame; };
std::mutex g_probe_mu;
std::unordered_map<uint32_t, std::vector<ProbeTex>> g_probe;  // packet_addr -> textures
std::atomic<uint64_t> g_probe_same{0}, g_probe_changed{0}, g_probe_logged{0};
std::set<uint32_t> g_late_bases;  // guarded by g_probe_mu
namespace {
std::set<uint32_t> g_cp_shadow;  // card S3-67: CP-probe set when it does not feed g_late_bases (d3d mode); g_probe_mu
}  // namespace
std::atomic<uint64_t> g_late_waits{0}, g_late_timeouts{0}, g_late_skipped{0};
bool LateTexOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_D3D_LATE_TEX");
    if (v && (*v == '2' || *v == '3')) rex::graphics::SetNrWritePointerHook(&OnGuestKick);  // kick tracking from the first draw on
    return v && (*v == '1' || *v == '2' || *v == '3' ||
                *v == '4');  // 4: no wait here; late_upload.cpp reads at the release boundary (no kick hook needed)
  }();
  return on;
}
// Card S3-67 (WWE2K14_D3D_LATE_LEARN, read only with WWE2K14_D3D_LATE_TEX=4): who fills g_late_bases.
// 0 = cp (default, today: the CP-thread probe FlashProbeCp, keyed by the draw's packet address); 1 = d3d (the release
// learner below: hash at translate vs at late_upload's release boundary - the same point where the refill re-reads the
// bytes; the CP probe only runs with FLASH_TRACE=1 and then fills a shadow set); 2 = census (the CP probe feeds
// g_late_bases as today, the release and kick learners run as shadows). s67-census (kSwap-keyed predecessor, bin
// da31eae6): swap set == CP set (82/82; 64/64 logged first-learn frames equal); the kick learner missed 23 of 82 and was
// 16-96 frames late on 16 of 50, so it is a census-only diagnostic. Review late-review-0/1 F1: kSwap could precede the
// CP's present (1E7AF000 +8 frames in s67-d3d), hence the release keying.
namespace {
int LateLearnMode() {
  static const int m = [] {
    const char* v = std::getenv("WWE2K14_D3D_LATE_LEARN");
    const char* l = std::getenv("WWE2K14_D3D_LATE_TEX");
    const int want = !v ? 0 : std::strcmp(v, "d3d") == 0 ? 1 : std::strcmp(v, "census") == 0 ? 2 : 0;
    if (want && !(l && *l == '4')) {
      REXLOG_WARN("[d3d-learn] WWE2K14_D3D_LATE_LEARN={} ignored: needs WWE2K14_D3D_LATE_TEX=4 (never with 1-3)", v);
      return 0;
    }
    if (v && !want && std::strcmp(v, "cp") != 0) REXLOG_WARN("[d3d-learn] WWE2K14_D3D_LATE_LEARN={} unknown, using cp", v);
    if (want) REXLOG_INFO("[d3d-learn] late-base learner: {}", want == 1 ? "d3d (release)" : "census (cp feeds; release+kick shadows)");
    return want;
  }();
  return m;
}
}  // namespace
bool FlashProbeOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_NR_FLASH_TRACE");
    const char* l = std::getenv("WWE2K14_D3D_LATE_TEX");
    // Card S3-67: in LATE_LEARN=d3d mode the CP probe is a diagnostic only when FLASH_TRACE is enabled.
    if (LateLearnMode() == 1) return v && *v == '1';
    return (v && *v == '1') || (LateTexOn() && !(l && *l == '3'));  // mode 3 waits for every draw: no learning probe
  }();
  return on;
}
uint64_t ProbeHash(uint32_t start, uint32_t length) {
  rex::memory::Memory* m = textures::SharedMemory();
  if (!m) return 0;
  rr::NotePhys(start, length);
  return XXH3_64bits(m->TranslatePhysical<const uint8_t*>(start), length);
}
void FlashProbeCp(uint32_t packet_addr) {
  std::vector<ProbeTex> v;
  {
    std::lock_guard<std::mutex> lk(g_probe_mu);
    auto it = g_probe.find(packet_addr & 0x1FFFFFFFu);
    if (it == g_probe.end()) return;
    v.swap(it->second);
    g_probe.erase(it);
  }
  for (const ProbeTex& t : v) {
    const uint64_t h = ProbeHash(t.start, t.length);
    if (h == t.hash) {
      g_probe_same.fetch_add(1, std::memory_order_relaxed);
    } else {
      {
        std::lock_guard<std::mutex> lk(g_probe_mu);
        // card S3-44: read this texture at CP arrival from now on (LATE_TEX); card S3-67: shadow only in d3d mode
        const bool fresh = (LateLearnMode() == 1 ? g_cp_shadow : g_late_bases).insert(t.start).second;
        static uint32_t learn_logged = 0;
        if (fresh && LateLearnMode() && learn_logged++ < 64)
          REXLOG_INFO("[d3d-learn] cp learned {:08X}+{:X} frame={}", t.start, t.length, t.frame);
      }
      const uint64_t n = g_probe_changed.fetch_add(1, std::memory_order_relaxed) + 1;
      if (g_probe_logged.fetch_add(1, std::memory_order_relaxed) < 40) {
        std::fprintf(stderr, "[nr-flash] probe: texture %08X+%X changed between D3D call and CP (changed %llu, same %llu)\n",
                     t.start, t.length, (unsigned long long)n, (unsigned long long)g_probe_same.load());
      }
    }
  }
  const uint64_t tot = g_probe_same.load() + g_probe_changed.load();
  if (tot && tot % 2000 == 0) {
    std::fprintf(stderr, "[nr-flash] probe totals: same %llu changed %llu\n", (unsigned long long)g_probe_same.load(),
                 (unsigned long long)g_probe_changed.load());
  }
}
// frame: guest frame (S3-67 learn logs).
void FlashProbeRecord(uint32_t packet_addr, uint32_t frame, const DeviceState& s,
                      const std::set<uint32_t>& slots) {
  static bool hooked = [] {
    SetCpDrawProbe(&FlashProbeCp);
    return true;
  }();
  (void)hooked;
  if (packet_addr == 0xFFFFFFFFu) return;
  std::vector<ProbeTex> v;
  for (uint32_t slot : slots) {
    uint32_t start = 0, length = 0, format = 0, width = 0;
    if (!textures::FetchBaseRange(s, slot, &start, &length, &format, &width)) continue;
    if (!textures::IsDynamicBase(start)) continue;  // only textures whose bytes change (movie planes, dynamic UI)
    v.push_back({start, length, ProbeHash(start, length), frame});
  }
  if (v.empty()) return;
  std::lock_guard<std::mutex> lk(g_probe_mu);
  if (g_probe.size() > 4096) g_probe.clear();  // the CP never reached them (unpaired): bound the map
  g_probe[(packet_addr & 0x1FFFFFFFu)] = std::move(v);
}

// ---- Card S3-67 (SV-5): late-base learner keyed to D3D events (WWE2K14_D3D_LATE_LEARN=d3d|census) ----
// The guest kick (CP_RB_WPTR write, SDK hook, guest thread) is the earliest point the console GPU can read a draw's
// textures. Kick learner (census only): LateLearnNoteCall stores the kick count at each hooked D3D call's entry (game thread, by event seq). At
// translate the worker hashes every dynamic base the draw samples, but only while the kick covering the draw has not
// happened yet (otherwise the translate already read the bytes the console GPU reads at the earliest: counted
// post_kick). The kick hook (game thread) re-hashes the probes its kick covers: a different hash = the game wrote the
// texture between its D3D draw call and the kick. Release learner (LATE_LEARN=d3d, feeds g_late_bases): the same
// translate hash (taken before the draw's texture uploads read guest memory, review F4), re-hashed at late_upload's
// release boundary (present, or the IRQ_SYNC marker), right after the refill re-read: every probe pushed before a
// release is resolved by it, together with the uploads it releases. So a base is learned whenever its bytes changed
// inside the window a LATE_TEX=4 refill can correct (translate .. refill read), by construction; a write after the
// release is outside that window for both. (The kSwap-keyed predecessor resolved on the worker, which can run ahead of
// the CP's present: review F1.) Per address, never shrinks (same as the CP probe).
namespace {  // card S3-67 learner state (this file only)
struct LearnProbe { uint32_t start, length; uint64_t hash; uint32_t kick, frame; };
struct Learner {
  std::vector<LearnProbe> pending;  // g_probe_mu
  std::set<uint32_t> learned;       // g_probe_mu
  uint64_t probes = 0, changed = 0, same = 0, post_kick = 0, no_call = 0, dedup = 0, known = 0;
  uint64_t hook_ns = 0, hashed = 0, kicks = 0;  // resolving side: ns, bytes hashed, resolving calls
  uint32_t logged = 0;
};
Learner g_lk, g_lw;  // kick learner (census only), release learner
// Guest-kick count (advanced by LearnOnKick under g_probe_mu, so a probe pushed under the lock with an unchanged count
// is always resolved by the next kick) and the count at each hooked D3D call's entry, by event seq (seq << 32 | kicks).
std::atomic<uint32_t> g_learn_kicks{0};
std::atomic<uint64_t> g_call_kick[65536];
uint32_t LearnKicks() { return g_learn_kicks.load(std::memory_order_acquire); }
uint32_t LearnCallKick(uint32_t seq, bool* ok) {
  const uint64_t v = g_call_kick[seq & 0xFFFFu].load(std::memory_order_acquire);
  *ok = uint32_t(v >> 32) == seq && seq != 0;
  return uint32_t(v);
}

// Re-hash `v` (no lock held), then record the outcome under g_probe_mu. `feed`: insert learned bases into g_late_bases.
void LearnResolve(Learner& L, std::vector<LearnProbe>& v, bool feed, const char* name) {
  const uint64_t t0 = TpNow();
  uint64_t bytes = 0;
  std::vector<std::pair<uint32_t, uint32_t>> hit;  // changed: (start, index)
  for (uint32_t i = 0; i < v.size(); ++i) {
    const LearnProbe& p = v[i];
    bytes += p.length;
    if (ProbeHash(p.start, p.length) != p.hash) hit.push_back({p.start, i});
  }
  std::lock_guard<std::mutex> lk(g_probe_mu);
  L.changed += hit.size();
  L.same += v.size() - hit.size();
  for (const auto& h : hit) {
    if (feed) g_late_bases.insert(h.first);
    if (L.learned.insert(h.first).second && L.logged++ < 64) {
      const LearnProbe& p = v[h.second];
      REXLOG_INFO("[d3d-learn] {} learned {:08X}+{:X} frame={} kick={}", name, p.start, p.length, p.frame, p.kick);
    }
  }
  L.hashed += bytes;
  ++L.kicks;
  L.hook_ns += TpNow() - t0;
}

// Game thread: the SDK CP_RB_WPTR hook in d3d / census mode (installed by LateLearnInit).
void LearnOnKick(uint32_t value) {
  OnGuestKick(value);  // the SDK hook is one pointer: keep gate.cpp's kick count (LATE_TEX 2/3, SV-0d census) going
  std::vector<LearnProbe> v;
  {
    std::lock_guard<std::mutex> lk(g_probe_mu);
    g_learn_kicks.fetch_add(1, std::memory_order_acq_rel);
    v.swap(g_lk.pending);  // every pending probe was pushed while the kick count equalled its call's: this kick covers it
  }
  if (!v.empty()) LearnResolve(g_lk, v, LateLearnMode() == 1, "kick");
}
bool LateLearnInit() {
  if (LateLearnMode() != 2) return false;  // the kick learner (and its game-thread hook) is census-only
  rex::graphics::SetNrWritePointerHook(&LearnOnKick);
  return true;
}
}  // namespace

// Game thread, capture.cpp BeginPending (hooked D3D call entry): the kick count this call's draw starts from.
void LateLearnNoteCall(uint32_t seq) {
  static const bool on = LateLearnInit();
  if (!on) return;
  g_call_kick[seq & 0xFFFFu].store((uint64_t(seq) << 32) | LearnKicks(), std::memory_order_release);
}

// Worker, at translate (same candidates as FlashProbeRecord: dynamic bases the draw samples).
void LateLearnRecord(const EvHeader& e, const DeviceState& s, const std::set<uint32_t>& slots) {
  const int mode = LateLearnMode();
  if (!mode) return;
  bool call_ok = false;
  const uint32_t kd = LearnCallKick(e.seq, &call_ok);
  std::vector<std::pair<uint32_t, uint32_t>> cand;
  for (uint32_t slot : slots) {
    uint32_t start = 0, length = 0, format = 0, width = 0;
    if (!textures::FetchBaseRange(s, slot, &start, &length, &format, &width)) continue;
    if (!textures::IsDynamicBase(start)) continue;
    bool dup = false;
    for (const auto& c : cand) dup |= c.first == start;
    if (!dup) cand.push_back({start, length});
  }
  for (const auto& c : cand) {
    bool want_k = false, want_w = false;
    {
      std::lock_guard<std::mutex> lk(g_probe_mu);
      if (mode != 2) {
        // d3d mode: swap learner only (no kick hook installed)
      } else if (!call_ok) {
        ++g_lk.no_call;
      } else if (g_lk.learned.count(c.first)) {
        ++g_lk.known;
      } else if (LearnKicks() != kd) {
        ++g_lk.post_kick;
      } else {
        bool dup = false;
        for (const LearnProbe& p : g_lk.pending) dup |= p.start == c.first && p.kick == kd;
        if (dup) ++g_lk.dedup;
        else want_k = true;
      }
      {
        if (g_lw.learned.count(c.first)) {
          ++g_lw.known;
        } else {
          bool dup = false;
          for (const LearnProbe& p : g_lw.pending) dup |= p.start == c.first;
          if (dup) ++g_lw.dedup;
          else want_w = true;
        }
      }
    }
    if (!want_k && !want_w) continue;
    const uint64_t h = ProbeHash(c.first, c.second);
    std::lock_guard<std::mutex> lk(g_probe_mu);
    if (want_k) {
      if (LearnKicks() != kd) {
        ++g_lk.post_kick;  // the covering kick landed while hashing
      } else {
        g_lk.pending.push_back({c.first, c.second, h, kd, e.frame});
        ++g_lk.probes;
      }
    }
    if (want_w) {
      g_lw.pending.push_back({c.first, c.second, h, kd, e.frame});
      ++g_lw.probes;
    }
  }
}

// late_upload::Release (CP thread, backend lock held by its caller), after the refill loop: the release learner
// resolves every probe pushed since the previous release (lock order backend -> g_probe_mu, as textures.cpp's
// IsLateBase under the translate's backend lock).
void late_upload::LearnAtRelease() {
  const int mode = LateLearnMode();
  if (!mode) return;
  std::vector<LearnProbe> v;
  {
    std::lock_guard<std::mutex> lk(g_probe_mu);
    v.swap(g_lw.pending);
  }
  if (!v.empty()) LearnResolve(g_lw, v, mode == 1, "release");
}

// Worker, at every kSwap event: periodic [d3d-learn] line (no learning here since the review-F1 change).
void LateLearnFrameEnd() {
  const int mode = LateLearnMode();
  if (!mode) return;
  static uint32_t frames = 0;
  if (++frames % 600) return;
  std::lock_guard<std::mutex> lk(g_probe_mu);
  // Review F3: in d3d mode without FLASH_TRACE the CP probe is not installed - there is no CP set to compare with.
  const bool cp_on = mode == 2 || FlashProbeOn();
  const std::set<uint32_t> none;
  const std::set<uint32_t>& cp = !cp_on ? none : mode == 1 ? g_cp_shadow : g_late_bases;
  auto diff = [](const std::set<uint32_t>& a, const std::set<uint32_t>& b, std::string* list) {
    uint32_t n = 0;
    for (uint32_t x : a) {
      if (b.count(x)) continue;
      if (n++ < 12) *list += fmt::format(" {:08X}", x);
    }
    return n;
  };
  std::string cpk, kcp, cpw, wcp;
  const uint32_t n_cpk = diff(cp, g_lk.learned, &cpk), n_kcp = diff(g_lk.learned, cp, &kcp);
  const uint32_t n_cpw = diff(cp, g_lw.learned, &cpw), n_wcp = cp_on ? diff(g_lw.learned, cp, &wcp) : 0;
  const double f = double(frames);
  REXLOG_INFO("[d3d-learn] frames={} kick: probes={} changed={} same={} post_kick={} no_call={} dedup={} known={} "
              "learned={} kicks={} hook_ms/f={:.4f} hashed_MB/f={:.3f} | release: probes={} changed={} learned={} "
              "releases={} ms/f={:.4f} hashed_MB/f={:.3f} | cp({})={} late_bases={} | cp-kick={}{} | kick-cp={}{} | "
              "cp-rel={}{} | rel-cp={}{}",
              frames, g_lk.probes, g_lk.changed, g_lk.same, g_lk.post_kick, g_lk.no_call, g_lk.dedup, g_lk.known,
              g_lk.learned.size(), g_lk.kicks, double(g_lk.hook_ns) / 1e6 / f, double(g_lk.hashed) / 1048576.0 / f,
              g_lw.probes, g_lw.changed, g_lw.learned.size(), g_lw.kicks, double(g_lw.hook_ns) / 1e6 / f,
              double(g_lw.hashed) / 1048576.0 / f, !cp_on ? "off" : mode == 1 ? "shadow" : "feeds", cp.size(), g_late_bases.size(), n_cpk, cpk, n_kcp, kcp, n_cpw, cpw, n_wcp, wcp);
}

bool TranslateDraw(const EvHeader& e, const RegView& rv, const DeviceState& s, nb::Draw* dp, DrawSide* side,
                   std::string* why) {
  glitch::BeginDraw(e.frame, e.seq);
  TpScope tp(e.frame);
  ++g_tp.draws;
  if (e.frame != g_tp.last_frame) {
    g_tp.last_frame = e.frame;
    if (++g_tp.frames % 300 == 0) {
      std::string l;
      for (int k = 0; k < 8; ++k) l += " " + std::string(kTpName[k]) + "=" + std::to_string(double(g_tp.ns[k]) / 1e6 / g_tp.frames).substr(0, 6);
      REXLOG_INFO("[d3d-draw] translate split ms/f:{} | draws/f={:.0f} vertex_MB/f={:.2f} index_MB/f={:.2f} | vbcache "
                  "hit/f={:.0f} build/f={:.1f} copy/f={:.0f} hit_MB/f={:.2f} build_MB/f={:.2f} | ibcache hit/f={:.0f} "
                  "build/f={:.0f} | vbskip: inline={} small={} big={} full={} first={} dyn={}",
                  l, double(g_tp.draws) / g_tp.frames, double(g_tp.vbytes) / 1048576.0 / g_tp.frames,
                  double(g_tp.ibytes) / 1048576.0 / g_tp.frames, double(g_vbcs.hit) / g_tp.frames,
                  double(g_vbcs.build) / g_tp.frames, double(g_vbcs.no) / g_tp.frames,
                  double(g_vbcs.hit_bytes) / 1048576.0 / g_tp.frames, double(g_vbcs.build_bytes) / 1048576.0 / g_tp.frames,
                  double(g_vbcs.ib_hit) / g_tp.frames, double(g_vbcs.ib_build) / g_tp.frames,
                  g_vbskip.inline_src, g_vbskip.small, g_vbskip.big, g_vbskip.full, g_vbskip.first, g_vbskip.dyn);
      if (pd::On()) {
        std::string pl;
        for (int k = 0; k < pd::kN; ++k)
          pl += fmt::format(" {}={:.4f}ms/f({:.3f}us/call n/f={:.0f})", pd::kNames[k], double(pd::g_ns[k]) / 1e6 / g_tp.frames,
                            pd::g_cnt[k] ? double(pd::g_ns[k]) / 1e3 / pd::g_cnt[k] : 0.0, double(pd::g_cnt[k]) / g_tp.frames);
        REXLOG_INFO("[d3d-pd] fine split (worker, draws/f={:.0f}):{}", double(g_tp.draws) / g_tp.frames, pl);
        for (int k = 0; k < pd::kN; ++k) pd::g_ns[k] = pd::g_cnt[k] = 0;
      }
      if (nb::draw_profile::Active()) {
        const auto bp = nb::draw_profile::Take();
        if (pd::On()) {
          std::string bl, rl;
          for (int k = 0; k < nb::draw_profile::kWorkerN; ++k)
            bl += fmt::format(" {}={:.4f}ms/f({:.3f}us/call n/f={:.1f})", nb::draw_profile::kWorkerNames[k],
              double(bp.worker_ns[k]) / 1e6 / g_tp.frames,
              bp.worker_n[k] ? double(bp.worker_ns[k]) / 1e3 / bp.worker_n[k] : 0.0,
              double(bp.worker_n[k]) / g_tp.frames);
          for (int k = 0; k < nb::draw_profile::kRecordN; ++k)
            rl += fmt::format(" {}={:.3f}us/draw(n={})", nb::draw_profile::kRecordNames[k],
              bp.record_n[nb::draw_profile::kExecute] ? double(bp.record_ns[k]) / 1e3 / bp.record_n[nb::draw_profile::kExecute] : 0.0,
              bp.record_n[k]);
          REXLOG_INFO("[nr-pd-handoff]{}", bl);
          REXLOG_INFO("[nr-pd-exec]{}", rl);
        }
        if (bp.swaps || bp.checks || bp.mismatches || bp.fallbacks)
          REXLOG_INFO("[nr-draw-swap] window swaps={} checks={} mismatches={} fallbacks={}",
                      bp.swaps, bp.checks, bp.mismatches, bp.fallbacks);
      }
      const int tstage_mode = TStageFastMode();
      if (tstage_mode) {
        const TStageShaderCache& cache = TStageCache();
        REXLOG_INFO("[d3d-tstage] mode={} fast_hits={} checks={} mismatches={}", tstage_mode, cache.fast_hits,
                    cache.checks, cache.mismatches);
      }
      const nr::backend::DirectVertexUploadStats vb_direct = nr::backend::GetDirectVertexUploadStats();
      if (vb_direct.mode || vb_direct.direct_draws || vb_direct.check_draws || vb_direct.not_mode_off) {
        REXLOG_INFO("[nr-vb-direct] mode={} direct_draws={} direct_bytes={} check_draws={} check_bytes={} mismatch_draws={} "
                    "mismatch_bytes={} skipped{{record_queue={} batch={} crowd_candidate={} ring_room={} mode_off={}}}",
                    vb_direct.mode, vb_direct.direct_draws, vb_direct.direct_bytes, vb_direct.check_draws,
                    vb_direct.check_bytes, vb_direct.mismatch_draws, vb_direct.mismatch_bytes,
                    vb_direct.not_record_queue, vb_direct.not_batch, vb_direct.not_crowd_candidate,
                    vb_direct.not_ring_room, vb_direct.not_mode_off);
      }
      const nr::backend::RecReuseStats rec_reuse = nr::backend::GetRecReuseStats();
      if (rec_reuse.mode || rec_reuse.checks || rec_reuse.hits || rec_reuse.mismatches) {
        REXLOG_INFO("[nr-rec-reuse] mode={} hits={} checks={} mismatches={}", rec_reuse.mode,
                    rec_reuse.hits, rec_reuse.checks, rec_reuse.mismatches);
      }
      dtr::LogLine(g_tp.frames, e.frame);  // card S3-68 / V17b A/B schedule
      g_vbcs = VbcStats{};
      g_vbskip = VbSkipStats{};
      g_tp = TransProf{};
      g_tp.last_frame = e.frame;
    }
  }
  DrawSide local_side;
  if (!side) side = &local_side;
  *side = DrawSide();
  side->packet_addr = e.packet_addr;
  side->seq = e.seq;
  MaybeLog();
  if (e.kind != EvKind::kDraw && e.kind != EvKind::kDrawUP) return Skip(why, "not a draw event");
  const DrawInfo& di = rv.draw();
  if (!di.valid || di.seq != e.seq) return Skip(why, "draw args not applied");
  if (!g_guest_base) return Skip(why, "no guest base");
  if (!dp) return Skip(why, "no draw");
  const RegisterFile& regs = rv.regs();
  const Cfg& cfg = Config();
  nb::Draw& d = *dp;
  d.Reset();
  const int const_used_mode = ConstUsedModeForFrame(e.frame);
  d.const_used_mode = uint32_t(const_used_mode);
  nr::backend::SetV17cFrame(e.frame);
  d.vb_direct_mode = nr::backend::DirectVertexUploadMode();
  const bool native_efa4 = efa4::NativeActive(cfg.native_efa4_mode, e.frame);
  // G1: crowd and shadows can change in the in-game menu; latched once per guest frame (live_options.h).
  thread_local live::FrameLatch live_latch;
  {
    bool live_changed = false;
    if (live_latch.Update(e.frame, &live_changed) && live_changed) {
      REXLOG_INFO("[ovl-live] frame {} crowd={} shadows={} (menu change)", e.frame, live_latch.crowd,
                  live_latch.shadows ? 1 : 0);
    }
  }
  int crowd_mode = live_latch.crowd;
  bool shadows_on = live_latch.shadows;
  thread_local weakpc::CrowdDrawCadence crowd_cadence;
  uint32_t lit_shadow_texture = nb::kNone;
  bool remove_shadows = !shadows_on;
  if (remove_shadows) {
    std::string lit_error;
    lit_shadow_texture = textures::LitShadowTexture(&lit_error);
    if (!TranslateOnly() && lit_shadow_texture == nb::kNone) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true, std::memory_order_relaxed))
        REXLOG_WARN("[d3d-weakpc] shadows option not applied; lit texture creation failed: {}", lit_error);
      remove_shadows = false;
    }
  }
  // Card S3-68 (WWE2K14_D3D_DIRTY_TRANSLATE=1 reuse / =2 check): epochs of the register groups changed since the last draw.
  const int dtm = dtr::ModeForFrame(e.frame);
  if (dtm) dtr::BeginDraw(rv);
  int crowd_fast_mode = CrowdFastModeForFrame(e.frame, dtm);
  LogCrowdFastStats(e.frame, crowd_fast_mode);

  if (cfg.regcheck) {
    std::string first;
    ++g_cnt.regcheck_draws;
    const uint32_t bad = rv.CountMismatches(s, &first);
    if (bad) {
      if (++g_cnt.regcheck_bad <= 20)
        REXLOG_WARN("[d3d-draw] REGCHECK seq={} {} mismatching registers, first: {}", e.seq, bad, first);
    }
  }

  tp.To(1);
  // ---- shaders (WP5-impl identity -> SDK shader + nr contract) ----
  ShaderKey vkey, pkey;
  const bool have_shader_keys = rv.shader_keys(&vkey, &pkey);
  glitch::SetShaderKeys(vkey.fnv, pkey.fnv);
  if (!have_shader_keys || !vkey.fnv) {
    glitch::MissingShader(glitch::CurrentDraw(), 0, vkey.fnv, "shader_key_missing");
    return Skip(why, "no shader key");
  }
  const bool has_ps = s.pixel_shader != 0 && pkey.fnv != 0;
  if (crowd_mode != 1 && weakpc::DropCrowdDraw(crowd_mode, has_ps ? pkey.fnv : 0, e.frame, &crowd_cadence)) {
    live::Get().crowd_dropped.fetch_add(1, std::memory_order_relaxed);
    return Skip(why, crowd_mode == 0 ? "weak-PC crowd disabled" : "weak-PC crowd reduced (alternate draw)");
  }
  // WWE2K14_D3D_CROWD_THIN=1 (optional, default off): see crowd_thin.h. A left-out member counts as neither failed nor translated.
  if (has_ps && pkey.fnv == weakpc::kCrowdPixelShader && e.kind == EvKind::kDraw && di.indexed && CrowdThinStep(e, regs, vkey.fnv, di.count)) {
    if (side) side->thinned = true;
    return false;
  }
  if (remove_shadows && has_ps && weakpc::IsShadowPassPixelShader(pkey.fnv)) {
    live::Get().shadow_dropped.fetch_add(1, std::memory_order_relaxed);
    return Skip(why, fmt::format("weak-PC shadows disabled (shadow pass PS {:016X})", pkey.fnv));
  }
  sh::ShaderSet& set = sh::ShaderSet::Get();
  const uint64_t vs_key = vkey.fnv, ps_key = has_ps ? pkey.fnv : 0;
  const bool crowd_fast_target = ps_key == kCrowdFastPsFnv;
  bool crowd_fast_write_cache = crowd_fast_mode != 0 && crowd_fast_target && e.kind == EvKind::kDraw && di.indexed &&
                                !remove_shadows && !FlashProbeOn() && !LateTexOn() && LateLearnMode() == 0 &&
                                !glitch::CurrentDraw().sampled && !CensusEvery() && !MvCensusEvery();
  if (crowd_fast_mode && crowd_fast_target) ++g_crowd_fast_stats.target_draws;
  if (crowd_fast_mode && !crowd_fast_target) CrowdFast().valid = false;
  std::vector<CrowdFastLookup> crowd_lookups;
  bool crowd_check_candidate = false;
  if (crowd_fast_write_cache) {
    CrowdFastCache& cache = CrowdFast();
    const uint64_t input_match_start = CrowdFastProfileStart();
    bool inputs_match = CrowdFastInputsMatch(cache, e, rv, s, di, vs_key, ps_key);
    CrowdFastProfileAdd(0, input_match_start);
    // V17d: in fast mode, every 64th reusable member is translated in full and compared instead (in-run CHECK sample).
    static uint64_t crowd_sample_counter = 0;
    const bool crowd_sample_check = crowd_fast_mode == 1 && inputs_match && ((++crowd_sample_counter & 63) == 0);
    // V17d cache gate (Claude 11-03): the full path would run EvictIfFull and the vertex/index cache lookups. Reuse
    // only if the cache is not flushing and every recorded lookup still has the same kHit / dynamic kNo result; else
    // fall back (nothing mutated). CHECK 2 found builds/uploads at first crowd sightings (224/1.9M) that blind reuse skipped.
    if (inputs_match) {
      const uint64_t vbc_gate_start = CrowdFastProfileStart();
      static const bool vbc_on_fast = [] { const char* v = std::getenv("WWE2K14_D3D_VBC"); return !(v && *v == '0'); }();
      nr::shared::VbCache* const vbc_fast = NrDrawOff() && vbc_on_fast ? VbCacheD3d() : nullptr;
      if (!cache.lookups.empty() && !vbc_fast) inputs_match = false;
      if (vbc_fast) {
        vbc_fast->EvictIfFull(192ull << 20);  // same call the full path makes first; idempotent
        if (vbc_fast->flush) inputs_match = false;
        for (const CrowdFastLookup& l : cache.lookups) {
          uint32_t id = 0;
          if (!inputs_match) break;
          const int pr = vbc_fast->Probe(l.key, l.phys, l.bytes, e.frame, &id);
          if (pr != l.result || (pr == nr::shared::VbCache::kHit && id != l.id)) inputs_match = false;
        }
        if (!inputs_match) ++g_crowd_fast_stats.cache_misses;
        if (inputs_match && crowd_fast_mode == 1 && !crowd_sample_check) {
          // Probe proved the full path would only change counters; account for those without repeating map lookups.
          AccountCrowdFastLookups(*vbc_fast, cache.lookups);
        }
      }
      CrowdFastProfileAdd(1, vbc_gate_start);
    }
    if (inputs_match) {
      if (crowd_fast_mode == 1 && !crowd_sample_check) {
        const uint64_t draw_copy_start = CrowdFastProfileStart();
        CopyCrowdFastDraw(d, cache.draw,
                          reinterpret_cast<const uint8_t*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]),
                          reinterpret_cast<const uint8_t*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X]),
                          const_used_mode, e.seq);
        *side = cache.side;
        UpdateCrowdFastSide(*side, e);
        CrowdFastProfileAdd(2, draw_copy_start);
        ++g_crowd_fast_stats.fast_hits;
        ++g_cnt.translated;
        g_tp.vbytes += cache.vertex_bytes;
        g_tp.ibytes += cache.index_bytes;
        cache.seq = e.seq;
        UpdateCrowdFastSide(cache.side, e);
        return true;
      }
      crowd_check_candidate = true;
    } else {
      ++g_crowd_fast_stats.misses;
      if (cache.valid && cache.frame == e.frame && cache.seq + 1 == e.seq) {
        ++g_crowd_fast_stats.input_misses;
        // State/argument mismatch and changed guest geometry both fail closed. The full translator remains authoritative.
        if (SameDeviceStateExceptVsConstants(s, cache.state) && SameDrawInfo(di, cache.draw_info) &&
            vs_key == cache.vs_fnv && ps_key == cache.ps_fnv)
          ++g_crowd_fast_stats.geometry_misses;
      }
      cache.valid = false;
    }
  } else if (crowd_fast_mode && crowd_fast_target) {
    ++g_crowd_fast_stats.misses;
    CrowdFast().valid = false;
  }
  std::vector<uint8_t> crowd_index_object;
  CrowdFastSource crowd_index_data;
  std::vector<CrowdFastSource> crowd_vertex_sources;
  if (crowd_fast_write_cache) {
    // CHECK keeps the full path as the submitted result. A mismatch is counted without changing the translated draw.
  }
  // Card S3-68 (A): the identity / contract lookups are a pure function of the keys once they succeeded (entries are
  // never replaced, the contract set is loaded once); the noop check also of the render-state registers (R epoch).
  dtr::ShaderMemo* sm = dtm ? &dtr::ShaderSlot(vs_key, ps_key, has_ps) : nullptr;
  const bool sm_hit = sm && sm->valid && sm->vfnv == vs_key && sm->pfnv == ps_key && sm->has_ps == has_ps;
  const bool noop_hit = sm_hit && sm->noop_epoch == dtr::RsEpoch();
  bool sm_reset = !sm_hit;
  if (dtm) dtr::Count(dtr::kGShaders, sm_hit), dtr::Count(dtr::kGNoop, noop_hit);
  const int tstage_mode = TStageFastMode();
  TStageShaderCache* const tstage_cache = tstage_mode ? &TStageCache() : nullptr;
  const bool tstage_key_hit = tstage_cache && tstage_cache->valid && tstage_cache->vkey == vs_key &&
                              tstage_cache->pkey == ps_key && tstage_cache->has_ps == has_ps;
  auto tstage_check = [&](const Shader* actual_vsh, const Shader* actual_psh, const ShaderEntry* actual_vs,
                          const ShaderEntry* actual_ps) {
    if (tstage_mode != 2 || !tstage_key_hit) return;
    ++tstage_cache->checks;
    if (tstage_cache->vsh != actual_vsh || tstage_cache->psh != actual_psh || tstage_cache->vs != actual_vs ||
        tstage_cache->ps != actual_ps) {
      ++tstage_cache->mismatches;
      if (tstage_cache->mismatches <= 8)
        REXLOG_WARN("[d3d-tstage] check mismatch seq={} vs={:016X} ps={:016X}", e.seq, vs_key, ps_key);
    }
  };
  const Shader* vsh = nullptr;
  const Shader* psh = nullptr;
  const ShaderEntry* vs = nullptr;
  const ShaderEntry* ps = nullptr;
  if (dtm == 1 && sm_hit && tstage_mode != 2) {
    vsh = sm->vsh, psh = sm->psh, vs = sm->vs, ps = sm->ps;
  } else if (tstage_mode == 1 && dtm != 2 && tstage_key_hit) {
    if (set.size() == 0) {
      glitch::MissingShader(glitch::CurrentDraw(), 0, vs_key, "shader_set_not_loaded");
      if (has_ps) glitch::MissingShader(glitch::CurrentDraw(), 1, ps_key, "shader_set_not_loaded");
      return Skip(why, "shader set not loaded");
    }
    vsh = tstage_cache->vsh, psh = tstage_cache->psh, vs = tstage_cache->vs, ps = tstage_cache->ps;
    ++tstage_cache->fast_hits;
  } else {
    vsh = ShaderObject(vkey.fnv, 0);
    psh = has_ps ? ShaderObject(pkey.fnv, 1) : nullptr;
    if (!vsh || (has_ps && !psh)) {
      if (!vsh) glitch::MissingShader(glitch::CurrentDraw(), 0, vs_key, "microcode_missing");
      if (has_ps && !psh) glitch::MissingShader(glitch::CurrentDraw(), 1, ps_key, "microcode_missing");
      tstage_check(vsh, psh, vs, ps);
      return Skip(why, "shader microcode not captured");
    }
    if (set.size() == 0) {
      glitch::MissingShader(glitch::CurrentDraw(), 0, vs_key, "shader_set_not_loaded");
      if (has_ps) glitch::MissingShader(glitch::CurrentDraw(), 1, ps_key, "shader_set_not_loaded");
      tstage_check(vsh, psh, vs, ps);
      return Skip(why, "shader set not loaded");
    }
    vs = set.Find(vs_key, true);
    ps = has_ps ? set.Find(ps_key, false) : nullptr;
    // Card RT1: a VS without a shipped contract is translated at run time (the draw is skipped until it is ready);
    // CHECK mode also re-translates shipped VS contracts once and compares.
    sh::RuntimeShaderWaitStats rts_wait;
    if (!vs) vs = sh::RuntimeShaderResolve(set, vs_key, true, vsh->ucode_dwords(), vsh->ucode_dword_count(), &rts_wait);
    else if (sh::RuntimeShaderCheckEnabled())
      sh::RuntimeShaderCheck(vs_key, true, vsh->ucode_dwords(), vsh->ucode_dword_count(), vs);
    if (has_ps) {
      if (!ps) ps = sh::RuntimeShaderResolve(set, ps_key, false, psh->ucode_dwords(), psh->ucode_dword_count(), &rts_wait);
      else if (sh::RuntimeShaderCheckEnabled())
        sh::RuntimeShaderCheck(ps_key, false, psh->ucode_dwords(), psh->ucode_dword_count(), ps);
    }
    g_cnt.rts_waits += rts_wait.waits;
    g_cnt.rts_wait_ms += rts_wait.wait_ms;
    tstage_check(vsh, psh, vs, ps);
    if (!vs || (has_ps && !ps)) {
      DumpMissingShader(vs ? nullptr : vsh, true, vs_key);  // card S3-42
      DumpMissingShader(!has_ps || ps ? nullptr : psh, false, ps_key);
      return Skip(why, "shader without contract");
    }
    if (tstage_cache) {
      tstage_cache->valid = true;
      tstage_cache->vkey = vs_key, tstage_cache->pkey = ps_key, tstage_cache->has_ps = has_ps;
      tstage_cache->vsh = vsh, tstage_cache->psh = psh, tstage_cache->vs = vs, tstage_cache->ps = ps;
    }
    if (dtm == 2 && sm_hit && (sm->vsh != vsh || sm->psh != psh || sm->vs != vs || sm->ps != ps)) {
      dtr::Mismatch(dtr::kGShaders, e.seq, fmt::format("shader objects vs={:016X} ps={:016X}", vs_key, ps_key));
      sm_reset = true;
    }
  }
  side->vs_fnv = vs_key;
  side->ps_fnv = ps_key;
  side->prim = di.prim;
  side->indexed = di.indexed;
  if (!(dtm == 1 && noop_hit)) {
    const reg::RB_DEPTHCONTROL dc = draw_util::GetNormalizedDepthControl(regs);
    const uint32_t cm = draw_util::GetNormalizedColorMask(regs, psh ? psh->writes_color_targets() : 0);
    const bool writes_depth = dc.z_enable && dc.z_write_enable;
    const bool writes_stencil = dc.stencil_enable && regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask;
    if (!cm && !writes_depth && !writes_stencil) {
      if (dtm == 2 && noop_hit) dtr::Mismatch(dtr::kGNoop, e.seq, "noop check");
      return Skip(why, "noop: writes no colour, depth or stencil");
    }
  }
  if (!(dtm == 1 && sm_hit)) {
    for (const ShaderEntry* en : {vs, ps}) {
      if (en && !en->unsupported.empty()) return Skip(why, "shader unsupported: " + en->unsupported);
    }
  }
  if (dtm) {  // card S3-68 (A): fill / refresh
    if (sm_reset) dtr::ResetShaderMemo(*sm, vs_key, ps_key, has_ps);
    sm->valid = true;
    sm->vsh = vsh, sm->psh = psh, sm->vs = vs, sm->ps = ps;
    sm->noop_epoch = dtr::RsEpoch();
  }

  tp.To(2);
  // ---- render targets (WP6: D3D surface objects -> shadow host images; no EDRAM) ----
  uint32_t color_surface = nb::kNone, depth_surface = nb::kNone;
  {
    std::string tw;
    if (!targets::BindForDraw(s, &color_surface, &depth_surface, &side->color_desc, &side->depth_desc, &tw)) {
      side->target_fail = true;
      color_surface = depth_surface = nb::kNone;
    } else if (color_surface == nb::kNone && depth_surface == nb::kNone) {
      return Skip(why, "noop: no render target used");
    }
  }
  // Card G18b (lane g18c): a colour target taller/wider than the bound depth surface's shadow image. The depth slot
  // shares the colour's pitch (RB_SURFACE_INFO) but takes its rows from the depth object (+36 extent, or the 720-row
  // fallback), so the attire composite (2080x1024 colour) binds a 2080x720 depth image. A Vulkan framebuffer takes its
  // extent from colour 0 (plume_vulkan.cpp:2545) and the render area = framebuffer (:3477): an attachment smaller than
  // the framebuffer is invalid (VUID-VkFramebufferCreateInfo-flags-04533/04534); AMD clips colour writes to the depth
  // image (rows >= 720 never written = white band), NVIDIA/Intel do not. When the draw neither tests nor writes depth or
  // stencil, the depth surface has no effect on the draw: bind colour only. Default on; WWE2K14_D3D_FB_DEPTH_FIT=0 off.
  if (color_surface != nb::kNone && depth_surface != nb::kNone &&
      (side->depth_desc.height < side->color_desc.height || side->depth_desc.width < side->color_desc.width)) {
    static const bool fit = [] {
      const char* v = std::getenv("WWE2K14_D3D_FB_DEPTH_FIT");
      return !(v && *v == '0');
    }();
    static uint32_t fit_n = 0, used_n = 0;
    const reg::RB_DEPTHCONTROL fdc = draw_util::GetNormalizedDepthControl(regs);
    const bool uses_ds = fdc.z_enable || fdc.stencil_enable;
    uint32_t& n = uses_ds ? used_n : fit_n;
    if (++n <= 16 || (n & (n - 1)) == 0) {
      REXLOG_INFO("[g18c-fbfit] #{} {} colour obj={:08X} {}x{} depth obj={:08X} {}x{} ps={:016X} z={} stencil={}", n,
                  uses_ds ? "KEPT(depth/stencil used: framebuffer still mismatched)" : fit ? "dropped-depth" : "off",
                  side->color_desc.object, side->color_desc.width, side->color_desc.height, side->depth_desc.object,
                  side->depth_desc.width, side->depth_desc.height, side->ps_fnv, uint32_t(fdc.z_enable),
                  uint32_t(fdc.stencil_enable));
    }
    if (fit && !uses_ds) {
      depth_surface = nb::kNone;
      side->depth_desc = {};
    }
  }
  // Card G18b (G18d) CHECK: a framebuffer whose depth attachment is still smaller than colour 0 (expected 0: the depth
  // slot is sized to the bound colour in targets::Ensure; WWE2K14_D3D_DEPTH_COVER=0 brings the mismatch back).
  if (color_surface != nb::kNone && depth_surface != nb::kNone) {
    if (side->depth_desc.height < side->color_desc.height || side->depth_desc.width < side->color_desc.width) {
      targets::CountFbMismatch();
      const uint64_t n = targets::GetStats().fb_mismatch;
      if (n <= 16 || (n & (n - 1)) == 0) {
        REXLOG_WARN("[g18d-fbcheck] mismatched framebuffer #{} colour obj={:08X} {}x{} depth obj={:08X} {}x{} ps={:016X}", n,
                    side->color_desc.object, side->color_desc.width, side->color_desc.height, side->depth_desc.object,
                    side->depth_desc.width, side->depth_desc.height, side->ps_fnv);
      }
    } else {
      // Bounded diagnostic (remove when G18d closes): the first draws that use a depth image past its declared rows,
      // with their depth state (what PS F1CC033418E7B30F does on the composite).
      static uint32_t cov_logged = 0;
      if (cov_logged < 24) {
        const uint32_t dh = targets::DeclaredHeight(depth_surface);
        if (dh && dh < side->color_desc.height) {
          ++cov_logged;
          const reg::RB_DEPTHCONTROL cdc = draw_util::GetNormalizedDepthControl(regs);
          REXLOG_INFO("[g18d-covered] #{} colour obj={:08X} {}x{} depth obj={:08X} {}x{} (declared rows {}) ps={:016X} "
                      "z={} zwrite={} zfunc={} stencil={}",
                      cov_logged, side->color_desc.object, side->color_desc.width, side->color_desc.height,
                      side->depth_desc.object, side->depth_desc.width, side->depth_desc.height, dh, side->ps_fnv,
                      uint32_t(cdc.z_enable), uint32_t(cdc.z_write_enable), uint32_t(cdc.zfunc),
                      uint32_t(cdc.stencil_enable));
        }
      }
    }
  }
  const bool fixed16_shadow_format = side->color_desc.format == nb::TargetFormat::kRGBA16S ||
                                     side->color_desc.format == nb::TargetFormat::kRG16S;
  if (remove_shadows && weakpc::IsFixed16ShadowTarget(side->color_desc.width, side->color_desc.height,
                                                       side->color_desc.depth, fixed16_shadow_format))
    return Skip(why, "weak-PC shadows disabled (fixed16 shadow target 560x512)");
  d.ordinal = e.seq;
  d.color = color_surface;
  d.depth = depth_surface;
  side->color_surface = color_surface;
  side->depth_surface = depth_surface;

  const ShaderEntry* ps_use = ps;
  std::string ids;
  // Card S3-68 (B): the variant choice reads the shader entries / SDK objects (A key), RB_MODECONTROL / RB_DEPTH_INFO /
  // depth control (R epoch), whether a depth surface is bound and two cvars.
  const bool var_depth_bound = depth_surface != nb::kNone;
  const uint8_t var_cvars = uint8_t((REXCVAR_GET(depth_float24_convert_in_pixel_shader) ? 1 : 0) |
                                    (REXCVAR_GET(vulkan_float24_depth_pixel_rate) ? 2 : 0) |
                                    (native_efa4 ? 0x20u : 0u));
  const bool var_hit = sm && sm->var_epoch == dtr::RsEpoch() && sm->var_depth_bound == var_depth_bound &&
                       sm->var_cvars == var_cvars;
  if (dtm) dtr::Count(dtr::kGVariant, var_hit);
  if (dtm == 1 && var_hit) {
    ps_use = sm->ps_use;
    d.vs_key = sm->d_vs_key, d.ps_key = sm->d_ps_key, d.vs_spirv = sm->vs_spirv, d.ps_spirv = sm->ps_spirv;
    d.sample_shading = sm->sample_shading;
    ids = sm->ids;
  } else {
    const ShaderEntry* ps_src = nullptr;  // entry d.ps_spirv points into
    if (regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kDepthOnly) ps_use = nullptr;
    d.vs_key = vs_key;
    d.ps_key = ps_key;
    d.vs_spirv = &vs->spirv;
    d.ps_spirv = ps_use ? &ps_use->spirv : nullptr;
    ps_src = ps_use;
    // Float24 depth variants (as nr_frontend.cpp Draw; the SDK converts depth in the pixel shader). The decision uses
    // the surface binding of THIS draw; when targets failed (depth_surface == kNone) no variant is chosen, as in nr.
    if (depth_surface != nb::kNone && REXCVAR_GET(depth_float24_convert_in_pixel_shader) &&
        regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      const reg::RB_DEPTHCONTROL ndc = draw_util::GetNormalizedDepthControl(regs);
      if (ps_use && ndc.z_enable && cfg.early_z && !psh->writes_depth()) {
        ids = " earlyz";
      } else if (ps_use && ndc.z_enable) {
        if (ps_use->spirv_f24.empty()) return Skip(why, "no float24-depth variant of the pixel shader");
        const bool cons = cfg.f24_conservative && !ps_use->spirv_f24c.empty();
        d.ps_spirv = cons ? &ps_use->spirv_f24c : &ps_use->spirv_f24;
        d.ps_key = ps_key ^ (cons ? 0xF24C000000000000ull : 0xF240000000000000ull);
        d.sample_shading = !psh->writes_depth() && !REXCVAR_GET(vulkan_float24_depth_pixel_rate);
        ids = d.sample_shading ? " f24-sample-rate" : " f24";
      } else if (!ps_use && cfg.early_z &&
                 (ndc.zfunc != xenos::CompareFunction::kAlways || (ndc.z_enable && ndc.z_write_enable))) {
        ids = " earlyz-depth-only";
      } else if (!ps_use && (ndc.zfunc != xenos::CompareFunction::kAlways || (ndc.z_enable && ndc.z_write_enable))) {
        const ShaderEntry* f24do = set.FindRaw(0x00000000F24D0001ull);
        if (!f24do) return Skip(why, "no float24 depth-only pixel shader");
        const bool cons = cfg.f24_conservative && !f24do->spirv_f24c.empty();
        d.ps_spirv = cons ? &f24do->spirv_f24c : &f24do->spirv;
        d.ps_key = cons ? 0xF24C0000F24D0001ull : 0x00000000F24D0001ull;
        ps_src = f24do;
        d.sample_shading = !REXCVAR_GET(vulkan_float24_depth_pixel_rate);
        ids = " f24-depth-only";
      }
    }
    if (cfg.shaders_b) {  // card P27: second shader set, distinct keys
      constexpr uint64_t kSetBKeyXor = 0x5E7B000000000000ull;
      auto to_b = [](const ShaderEntry& en, const std::vector<uint8_t>* p) -> const std::vector<uint8_t>* {
        const std::vector<uint8_t>* b = p == &en.spirv ? &en.spirv_b : p == &en.spirv_f24 ? &en.spirv_f24_b
                                      : p == &en.spirv_f24c ? &en.spirv_f24c_b : nullptr;
        return b && !b->empty() ? b : nullptr;
      };
      if (const auto* b = to_b(*vs, d.vs_spirv)) d.vs_spirv = b, d.vs_key ^= kSetBKeyXor;
      if (ps_src && d.ps_spirv)
        if (const auto* b = to_b(*ps_src, d.ps_spirv)) d.ps_spirv = b, d.ps_key ^= kSetBKeyXor;
    }
    // Card D2 (WWE2K14_D3D_NATIVE_DOF=1, default off): replace the translated DOF composite PS
    // DC4817F76AA5B63E with the hand-written native shader 4E41544F444F0001 (same fetch slots,
    // same constant registers, plain mul/add instead of precise/nrMul). Only the plain translated
    // path swaps (not the float24-depth variants, not shader set B); ps_use stays the guest entry
    // so texture slots, memos and diagnostics are unchanged.
    if (cfg.native_dof && ps_use && d.ps_spirv == &ps_use->spirv && ps_key == 0xDC4817F76AA5B63Eull) {
      const ShaderEntry* nat = set.FindRaw(0x4E41544F444F0001ull);
      if (nat && !nat->spirv.empty()) {
        d.ps_spirv = &nat->spirv;
        d.ps_key = 0x4E41544F444F0001ull;
        static std::atomic<uint32_t> dof_swapped{0};
        if (dof_swapped.fetch_add(1) == 0)
          REXLOG_WARN("[d3d-dof] native DOF composite PS 4E41544F444F0001 swapped in for DC4817F76AA5B63E");
      } else {
        static std::atomic<uint32_t> dof_missing{0};
        if (dof_missing.fetch_add(1) == 0)
          REXLOG_WARN("[d3d-dof] WWE2K14_D3D_NATIVE_DOF=1 but 4E41544F444F0001 missing from the shader set; using translated");
      }
    }
    // V9: replace only the plain translated EFA4 PS. Keep ps_use as the guest entry for texture contracts and
    // bindings. Float24 and shader-set-B variants retain their translated forms.
    if (native_efa4 && ps_use && d.ps_spirv == &ps_use->spirv && ps_key == 0xEFA4A1431609E60Dull) {
      constexpr uint64_t kNativeEfa4Key = 0x4E41544F45464101ull;
      const ShaderEntry* nat = set.FindRaw(kNativeEfa4Key);
      if (nat && !nat->spirv.empty()) {
        d.ps_spirv = &nat->spirv;
        d.ps_key = kNativeEfa4Key;
        static std::atomic<uint32_t> efa4_swapped{0};
        if (efa4_swapped.fetch_add(1) == 0)
          REXLOG_INFO("[d3d-efa4] native material PS 4E41544F45464101 swapped in for EFA4A1431609E60D");
      } else {
        static std::atomic<uint32_t> efa4_missing{0};
        if (efa4_missing.fetch_add(1) == 0)
          REXLOG_WARN("[d3d-efa4] WWE2K14_D3D_NATIVE_EFA4 requested but 4E41544F45464101 missing; using translated");
      }
    }
    if (dtm == 2 && var_hit &&
        (sm->ps_use != ps_use || sm->d_vs_key != d.vs_key || sm->d_ps_key != d.ps_key || sm->vs_spirv != d.vs_spirv ||
         sm->ps_spirv != d.ps_spirv || sm->sample_shading != d.sample_shading || sm->ids != ids)) {
      dtr::Mismatch(dtr::kGVariant, e.seq, fmt::format("variant ps_key {:016X} vs memo {:016X}", d.ps_key, sm->d_ps_key));
    }
    if (dtm) {
      sm->var_epoch = dtr::RsEpoch(), sm->var_depth_bound = var_depth_bound, sm->var_cvars = var_cvars;
      sm->ps_use = ps_use;
      sm->d_vs_key = d.vs_key, sm->d_ps_key = d.ps_key, sm->vs_spirv = d.vs_spirv, sm->ps_spirv = d.ps_spirv;
      sm->sample_shading = d.sample_shading;
      sm->ids = ids;
    }
  }
  side->note = ids;

  tp.To(3);
  // ---- primitive ----
  const uint32_t prim = di.prim;
  const bool rect = prim == uint32_t(xenos::PrimitiveType::kRectangleList);
  switch (xenos::PrimitiveType(prim)) {
    case xenos::PrimitiveType::kTriangleList: d.topology = 4; break;
    case xenos::PrimitiveType::kTriangleStrip: d.topology = 5; break;
    case xenos::PrimitiveType::kQuadList: d.topology = 4; break;
    case xenos::PrimitiveType::kRectangleList:
      if (vs->source != "lib_clear_vs.hlsl") return Skip(why, "rectangle list with a non-library VS");
      d.topology = 4;
      break;
    default: return Skip(why, "primitive " + std::to_string(prim));
  }
  d.depth_clamp = regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  const auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (draw_util::IsPrimitivePolygonal(regs)) {
    d.cull = mode.cull_front && mode.cull_back ? 1 : mode.cull_front ? 2 : mode.cull_back ? 3 : 1;
    if (mode.cull_front && mode.cull_back) return Skip(why, "both faces culled - nothing drawn");
    d.front_ccw = mode.face == 0;
  }

  tp.To(4);
  // ---- vertex fetches -> buffers + attributes ----
  struct Pending {
    uint32_t binding, address, bytes, endian, stride_bytes, extent;
    const uint8_t* src;  // UP: captured bytes (raw guest order); else null = guest physical address
  };
  std::vector<Pending> pend;
  std::map<uint32_t, uint32_t> binding_of_fetch;
  size_t up_bytes = 0;
  const uint8_t* up_data = rv.up_vertices(&up_bytes);
  // Card S3-68 (D): the layout (unique fetches in first-use order = bindings, strides, extents, attributes) is a pure
  // function of the VS contract entry; addresses / sizes / endianness are read per draw.
  dtr::LayoutMemo* lm = dtm ? &dtr::LayoutSlot(vs) : nullptr;
  const bool lm_hit = lm && lm->valid && lm->vs == vs;
  if (dtm) dtr::Count(dtr::kGLayout, lm_hit);
  if (dtm == 1 && lm_hit) {
    for (const dtr::LayoutFetch& lf : lm->fetches) {
      const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(lf.fetch);
      nb::VertexBuffer vb;
      vb.stride_bytes = lf.stride_bytes;
      Pending p{uint32_t(d.buffers.size()), uint32_t(vf.address) << 2, uint32_t(vf.size) << 2, uint32_t(vf.endian), vb.stride_bytes, lf.extent, nullptr};
      if (!di.indexed && lf.fetch == 95) {  // BeginVertices inline stream (as below)
        if (!up_data) return Skip(why, "BeginVertices vertex bytes not captured");
        if (up_bytes < p.bytes) return Skip(why, "guest_range: BeginVertices bytes truncated");
        p.src = up_data;
      } else if (uint64_t(p.address) + p.bytes > kPhysMask + 1ull) {
        return Skip(why, "vertex buffer outside physical memory");
      }
      pend.push_back(p);
      d.buffers.push_back(std::move(vb));
    }
    d.attributes = lm->attrs;
  } else {
    for (const VFetch& f : vs->vfetch) {
      nb::VtxFormat fmt;
      bool half_words;
      if (!sh::VertexFormat(f, &fmt, &half_words)) return Skip(why, "vertex format " + std::to_string(f.format));
      auto bit = binding_of_fetch.find(f.fetch);
      if (bit == binding_of_fetch.end()) {
        const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(f.fetch);
        nb::VertexBuffer vb;
        vb.stride_bytes = f.stride * 4;
        Pending p{uint32_t(d.buffers.size()), uint32_t(vf.address) << 2, uint32_t(vf.size) << 2, uint32_t(vf.endian), vb.stride_bytes, 0, nullptr};
        if (!di.indexed && f.fetch == 95) {  // BeginVertices inline stream: the bytes the game wrote, captured at EndVertices
          if (!up_data) return Skip(why, "BeginVertices vertex bytes not captured");
          if (up_bytes < p.bytes) return Skip(why, "guest_range: BeginVertices bytes truncated");
          p.src = up_data;
        } else if (uint64_t(p.address) + p.bytes > kPhysMask + 1ull) {
          return Skip(why, "vertex buffer outside physical memory");
        }
        pend.push_back(p);
        bit = binding_of_fetch.emplace(f.fetch, uint32_t(d.buffers.size())).first;
        d.buffers.push_back(std::move(vb));
      }
      (void)half_words;
      d.attributes.push_back({f.location, fmt, bit->second, f.offset * 4});
      for (Pending& p : pend)
        if (p.binding == bit->second) p.extent = std::max(p.extent, f.offset * 4 + 16);
    }
    if (dtm) {
      std::vector<dtr::LayoutFetch> lfs(pend.size());
      for (const auto& [fetch, binding] : binding_of_fetch) lfs[binding].fetch = fetch;
      for (size_t i = 0; i < pend.size(); ++i) lfs[i].stride_bytes = pend[i].stride_bytes, lfs[i].extent = pend[i].extent;
      if (dtm == 2 && lm_hit) {
        bool same = lm->fetches.size() == lfs.size() && dtr::SameAttributes(lm->attrs, d.attributes);
        for (size_t i = 0; same && i < lfs.size(); ++i)
          same = lm->fetches[i].fetch == lfs[i].fetch && lm->fetches[i].stride_bytes == lfs[i].stride_bytes &&
                 lm->fetches[i].extent == lfs[i].extent;
        if (!same) dtr::Mismatch(dtr::kGLayout, e.seq, fmt::format("vertex layout vs={:016X}", vs_key));
      }
      lm->valid = true;
      lm->vs = vs;
      lm->fetches = std::move(lfs);
      lm->attrs = d.attributes;
    }
  }
  // S3-3: the vertex/index cache setup, before both lookups (nr does the equivalent at the top of its draw): a
  // same-draw EvictIfFull/flush must not clear an id this draw just handed to the backend.
  // Card S3-44 (b) A/B: WWE2K14_D3D_VBC=0 turns the D3D path's static vertex/index cache off (every draw copies).
  static const bool vbc_on = [] {
    const char* e = std::getenv("WWE2K14_D3D_VBC");
    return !(e && *e == '0');
  }();
  nr::shared::VbCache* const vbc = NrDrawOff() && vbc_on ? VbCacheD3d() : nullptr;
  if (vbc) {
    vbc->EvictIfFull(192ull << 20);
    if (vbc->flush) {
      d.cache_flush = true;
      side->cache_flush_sent = true;
      g_ib_range.clear();
    }
  }
  auto copy_vertices = [&](bool have_range, uint32_t lo, uint32_t hi, bool allow_direct = true) -> bool {
    for (const Pending& p : pend) {
      nb::VertexBuffer& vb = d.buffers[p.binding];
      const uint8_t* guest_source = p.src ? p.src : Phys(p.address);
      if (!p.src) rr::NotePhys(p.address & kPhysMask, p.bytes);  // RR1: cache lookup hash + copies read within this
      if (crowd_fast_write_cache && !p.src) {
        if (!GuestRangeOk(kPhysView + (p.address & kPhysMask), p.bytes)) return false;
        CrowdFastSource source;
        source.address = p.address & kPhysMask;
        source.bytes = p.bytes;
        source.endian = p.endian;
        source.generation_valid = CaptureCrowdFastGeneration(source.address, source.bytes, &source.generation_sum);
        source.raw.assign(guest_source, guest_source + p.bytes);
        crowd_vertex_sources.push_back(std::move(source));
        guest_source = crowd_vertex_sources.back().raw.data();
      }
      if (vbc) {
        // S3-3 Q3: the reason this candidate does not reach the cache lookup (no control-flow change).
        if (p.src) {
          ++g_vbskip.inline_src;
        } else if (p.bytes < 512) {
          ++g_vbskip.small;
        } else if (p.bytes > (32u << 20)) {
          ++g_vbskip.big;
        } else if (side->n_vb_commit >= 16) {
          ++g_vbskip.full;
        } else {
          nr::shared::VbCache::Key key;
          key.k = {0, p.address, p.bytes, p.endian};
          nr::shared::VbCache::Entry* ce = nullptr;
          const uint64_t first0 = vbc->vbc_first, dyn0 = vbc->vbc_dyn;
          const uint64_t pdl = pd::Now();
          const int r = vbc->Lookup(textures::SharedMemory(), key, p.address & kPhysMask, p.bytes, false, e.frame, false, &ce);
          pd::Add(pd::kVbLookup, pdl);
          if (crowd_fast_write_cache) {
            CrowdFastLookup lookup;
            lookup.key = key;
            lookup.phys = p.address & kPhysMask;
            lookup.bytes = p.bytes;
            lookup.id = ce ? ce->id : 0u;
            lookup.cache_bytes = ce ? ce->bytes : 0u;
            lookup.index = false;
            lookup.dynamic = r == nr::shared::VbCache::kNo && ce && ce->dyn_until > e.frame;
            lookup.result = r;
            crowd_lookups.push_back(std::move(lookup));
          }
          if (r != nr::shared::VbCache::kNo) {
            vb.cache_id = ce->id;
            vb.cache_size = p.bytes;
            vb.prefix_bytes = 0;
            if (r == nr::shared::VbCache::kBuild) {
              if (!GuestRangeOk(kPhysView + (p.address & kPhysMask), p.bytes)) return false;
              vb.cache_upload = true;
              vb.data.resize(p.bytes);
              sh::CopySwapDwords(vb.data.data(), guest_source, p.bytes, p.endian);
              side->vb_commit[side->n_vb_commit++] = ce;
              ++g_vbcs.build;
              g_vbcs.build_bytes += p.bytes;
            } else {
              vb.data.clear();
              ++g_vbcs.hit;
              g_vbcs.hit_bytes += p.bytes;
            }
            continue;
          }
          // Lookup classified its kNo itself (first sighting / dynamic); its counters are the one source for that.
          ++g_vbcs.no;
          g_vbskip.first += vbc->vbc_first - first0;
          g_vbskip.dyn += vbc->vbc_dyn - dyn0;
        }
      }
      const uint64_t pdv = pd::Now();
      const uint64_t stride = p.stride_bytes, bytes = p.bytes;
      uint64_t begin = 0, end = bytes;
      if (have_range && stride) {  // restrict the host copy to the vertex interval referenced by the draw
        begin = std::min<uint64_t>(bytes, uint64_t(lo) * stride);
        end = std::min<uint64_t>(bytes, uint64_t(hi) * stride + std::max<uint64_t>(stride, p.extent));
        end = std::max(end, begin);
      }
      vb.prefix_bytes = uint32_t(begin);
      if (!p.src && !GuestRangeOk(kPhysView + (p.address & kPhysMask) + uint32_t(begin), end - begin)) return false;
      const uint8_t* src = guest_source + begin;
      bool skip_legacy_copy = false;
      if (allow_direct && NrDrawOff() &&
          nr::backend::CanDirectVertexUpload(size_t(end), d.ps_key, d.vb_direct_mode, &skip_legacy_copy)) {
        vb.direct_src = src;
        vb.direct_bytes = uint32_t(end - begin);
        vb.direct_endian = p.endian;
        if (skip_legacy_copy) {
          vb.data.clear();
          continue;
        }
      }
      vb.data.resize(size_t(end - begin));
      sh::CopySwapDwords(vb.data.data(), src, size_t(end - begin), p.endian);
      pd::Add(pd::kVbCopy, pdv);
    }
    return true;
  };
  constexpr const char* kRangeFail = "guest_range: vertex buffer";

  tp.To(5);
  // ---- indices ----
  uint32_t count = di.count;
  if (di.indexed) {
    if (count > 65535) return Skip(why, "split draw (> 65535 indices) not translated");
    if (!s.index_buffer) return Skip(why, "no index buffer");
    // DrawIndexedVertices (85:15820-15912): IB object +0 bit31 = 32-bit indices, bits 30:29 = DMA swap mode, +24 = base.
    if (!GuestRangeOk(s.index_buffer, 32)) return Skip(why, "guest_range: index buffer object");
    rr::NoteRead(s.index_buffer, 32);
    if (crowd_fast_write_cache)
      crowd_index_object.assign(g_guest_base + s.index_buffer, g_guest_base + s.index_buffer + 32);
    const uint32_t ib_flags = crowd_fast_write_cache ? Be32Bytes(crowd_index_object.data()) : Be32(s.index_buffer);
    const uint32_t ib_base = crowd_fast_write_cache ? Be32Bytes(crowd_index_object.data() + 24) : Be32(s.index_buffer + 24);
    const bool i32 = (ib_flags & 0x80000000u) != 0;
    const uint32_t endian = (ib_flags >> 29) & 3;
    const uint32_t addr = ib_base + di.start_index * (i32 ? 4u : 2u);
    const uint32_t phys = (addr & kPhysMask) + ((((addr >> 20) & 0xFFFu) + 512u) & 0x1000u);
    if (uint64_t(phys & kPhysMask) + uint64_t(count) * (i32 ? 4 : 2) > kPhysMask + 1ull) return Skip(why, "index buffer outside physical memory");
    if (!GuestRangeOk(kPhysView + (phys & kPhysMask), uint64_t(count) * (i32 ? 4 : 2))) return Skip(why, "guest_range: indices");
    const uint8_t* src = Phys(phys);
    rr::NotePhys(phys & kPhysMask, uint64_t(count) * (i32 ? 4 : 2));
    if (crowd_fast_write_cache) {
      const size_t raw_index_bytes = size_t(count) * (i32 ? 4u : 2u);
      if (raw_index_bytes <= UINT32_MAX) {
        crowd_index_data.address = phys & kPhysMask;
        crowd_index_data.bytes = uint32_t(raw_index_bytes);
        crowd_index_data.endian = endian;
        crowd_index_data.generation_valid = CaptureCrowdFastGeneration(
            crowd_index_data.address, crowd_index_data.bytes, &crowd_index_data.generation_sum);
        crowd_index_data.raw.assign(src, src + raw_index_bytes);
        src = crowd_index_data.raw.data();
      }
    }
    const uint32_t reset = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
    const uint32_t offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
    const uint32_t vmin = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx, vmax = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
    const bool restart = mode.multi_prim_ib_ena;
    const size_t n = count;
    const uint32_t reset_cmp = i32 ? reset : (reset & 0xFFFF);
    // S3-3: index cache - the same VbCache as the vertex cache, key tag 1. Quad lists stay out (the 4->6 expansion
    // below rewrites d.indices); the cached bytes are the converted 32-bit host indices. On a hit the conversion is
    // skipped and the backend binds the cached buffer; on a build the converted bytes are uploaded and committed.
    int icache = nr::shared::VbCache::kNo;
    nr::shared::VbCache::Entry* ie = nullptr;
    if (vbc && prim != uint32_t(xenos::PrimitiveType::kQuadList) && n * 4 >= 256 && side->n_vb_commit < 16) {
      const uint32_t src_bytes = uint32_t(n) * (i32 ? 4u : 2u);
      nr::shared::VbCache::Key key;
      key.k = {1, phys & kPhysMask, src_bytes, endian, uint32_t(i32), offset, vmin, vmax, uint32_t(restart), reset_cmp,
               uint32_t(n)};
      const uint64_t pd0 = pd::Now();
      icache = vbc->Lookup(textures::SharedMemory(), key, phys & kPhysMask, src_bytes, true, e.frame, false, &ie);
      pd::Add(pd::kIxLookup, pd0);
      if (crowd_fast_write_cache) {
        CrowdFastLookup lookup;
        lookup.key = key;
        lookup.phys = phys & kPhysMask;
        lookup.bytes = src_bytes;
        lookup.id = ie ? ie->id : 0u;
        lookup.cache_bytes = ie ? ie->bytes : 0u;
        lookup.index = true;
        lookup.dynamic = icache == nr::shared::VbCache::kNo && ie && ie->dyn_until > e.frame;
        lookup.result = icache;
        crowd_lookups.push_back(std::move(lookup));
      }
      if (icache != nr::shared::VbCache::kNo) {
        d.index_cache_id = ie->id;
        d.index_cache_bytes = uint32_t(n * 4);
        if (icache == nr::shared::VbCache::kHit) ++g_vbcs.ib_hit;
      }
    }
    uint32_t lo = UINT32_MAX, hi = 0;
    if (icache == nr::shared::VbCache::kHit) {
      auto rit = g_ib_range.find(ie->id);
      if (rit != g_ib_range.end()) lo = rit->second.first, hi = rit->second.second;  // else: whole vertex buffers
    } else {
      const uint64_t pdc = pd::Now();
      d.indices.resize(n * 4);
      uint32_t* out = reinterpret_cast<uint32_t*>(d.indices.data());
      for (size_t i = 0; i < n; ++i) {
        uint32_t v;
        if (i32) {
          std::memcpy(&v, src + i * 4, 4);
          v = sh::SwapDword(v, endian);
        } else {
          uint16_t h;
          std::memcpy(&h, src + i * 2, 2);
          if (endian == 1 || endian == 2) h = sh::Swap16(h);
          v = h;
        }
        if (restart && v == reset_cmp) {
          out[i] = 0xFFFFFFFFu;
          continue;
        }
        const uint32_t r = (v + offset) & 0xFFFFFFu;
        const uint32_t c = std::min(std::max(r, vmin), vmax);
        lo = std::min(lo, c);
        hi = std::max(hi, c);
        out[i] = c;
      }
      if (icache == nr::shared::VbCache::kBuild) {
        g_ib_range[ie->id] = {lo, hi};
        ie->bytes = uint32_t(n * 4);
        d.index_cache_upload = true;
        side->vb_commit[side->n_vb_commit++] = ie;
        ++g_vbcs.ib_build;
      }
      pd::Add(pd::kIxConvert, pdc);
    }
    d.index32 = true;
    if (!copy_vertices(lo <= hi, lo, hi)) return Skip(why, kRangeFail);
  } else if (rect) {
    if (!copy_vertices(false, 0, 0, false)) return Skip(why, kRangeFail);
    // Rectangle list drawn with the library clear shader: 4th vertex mirrored across the longest edge, two triangles
    // (same construction as nr).
    if (d.buffers.size() != 1 || count % 3) return Skip(why, "rectangle list layout");
    nb::VertexBuffer& vb = d.buffers[0];
    const uint32_t stride_f = vb.stride_bytes / 4;
    if (!stride_f) return Skip(why, "rectangle list stride 0");
    std::vector<float> in(vb.data.size() / 4);
    std::memcpy(in.data(), vb.data.data(), in.size() * 4);
    if (in.size() < size_t(count) * stride_f) return Skip(why, "rectangle list vertex data short");
    std::vector<float> out;
    for (uint32_t r = 0; r * 3 < count; ++r) {
      const float* v[3] = {&in[(r * 3 + 0) * stride_f], &in[(r * 3 + 1) * stride_f], &in[(r * 3 + 2) * stride_f]};
      auto len2 = [&](int a, int b) {
        const float dx = v[a][0] - v[b][0], dy = v[a][1] - v[b][1];
        return dx * dx + dy * dy;
      };
      const float e12 = len2(1, 2), e20 = len2(2, 0), e01 = len2(0, 1);
      int first = 0;
      if (e20 > e12 && e20 >= e01) first = 1;
      else if (e01 > e12 && e01 > e20) first = 2;
      const int s0 = first, s1 = (first + 1) % 3, s2 = (first + 2) % 3;
      std::vector<float> v3(stride_f);
      for (uint32_t k = 0; k < stride_f; ++k) v3[k] = -v[s0][k] + v[s1][k] + v[s2][k];
      const float* strip[4] = {v[s0], v[s1], v[s2], v3.data()};
      for (int t : {0, 1, 2, 2, 1, 3}) out.insert(out.end(), strip[t], strip[t] + stride_f);
    }
    vb.data.resize(out.size() * 4);
    std::memcpy(vb.data.data(), out.data(), vb.data.size());
    count = uint32_t(out.size() / stride_f);
  } else {
    if (!copy_vertices(false, 0, 0)) return Skip(why, kRangeFail);
  }
  if (prim == uint32_t(xenos::PrimitiveType::kQuadList)) {
    // Each quad 0,1,2,3 -> triangles (0,1,3), (3,1,2): the SDK's strip 0,1,3,2 as a list.
    std::vector<uint32_t> src;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v = i;
      if (di.indexed) std::memcpy(&v, &d.indices[i * 4], 4);
      src.push_back(v);
    }
    std::vector<uint32_t> tri;
    for (uint32_t q = 0; q + 4 <= src.size(); q += 4)
      for (int k : {0, 1, 3, 3, 1, 2}) tri.push_back(src[q + k]);
    d.index32 = true;
    d.indices.resize(tri.size() * 4);
    std::memcpy(d.indices.data(), tri.data(), d.indices.size());
    count = uint32_t(tri.size());
  }
  d.count = count;
  side->index_count = di.indexed ? di.count : 0;
  for (const nb::VertexBuffer& vb : d.buffers)
    side->vertex_bytes += uint32_t(vb.direct_src ? vb.direct_bytes : vb.data.size());

  tp.To(6);
  for (const nb::VertexBuffer& vb : d.buffers) g_tp.vbytes += vb.direct_src ? vb.direct_bytes : vb.data.size();
  g_tp.ibytes += d.indices.size();
  // ---- constants (D3D captured full blocks) ----
  uint32_t spec = 0;
  const uint64_t pdk = pd::Now();
  const uint8_t* const raw_vs_constants = reinterpret_cast<const uint8_t*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  const uint8_t* const raw_ps_constants = reinterpret_cast<const uint8_t*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X]);
  const uint64_t* const vs_constant_usage = vsh ? vsh->constant_register_map().float_bitmap : nullptr;
  const uint64_t* const ps_constant_usage = psh ? psh->constant_register_map().float_bitmap : nullptr;
  uint64_t guarded_vs_usage[const_used::kWords];
  uint64_t guarded_ps_usage[const_used::kWords];
  const uint64_t* vs_copy_usage = vs_constant_usage;
  const uint64_t* ps_copy_usage = ps_constant_usage;
  if (const_used_mode != 0) {
    const bool vs_dynamic = vsh && vsh->constant_register_map().float_dynamic_addressing;
    const bool ps_dynamic = psh && psh->constant_register_map().float_dynamic_addressing;
    const bool ps_substituted = !ps_use || d.ps_key == 0x4E41544F444F0001ull ||
                                d.ps_key == 0x4E41544F45464101ull;
    // d.*_key identifies the shader actually bound after float24 and shader-set substitutions. A
    // base guest map must not authorize sparse copies for a variant with a different binary.
    (void)const_used::SelectCopyUsage(const_used::ShaderStage::kVertex, d.vs_key, vs_constant_usage,
                                      vs_dynamic, false, ConstUsedOptions().guard, guarded_vs_usage);
    (void)const_used::SelectCopyUsage(const_used::ShaderStage::kPixel, d.ps_key, ps_constant_usage,
                                      ps_dynamic, ps_substituted, ConstUsedOptions().guard, guarded_ps_usage);
    vs_copy_usage = guarded_vs_usage;
    ps_copy_usage = guarded_ps_usage;
  }
  if (vs_copy_usage) std::memcpy(d.vs_float_usage, vs_copy_usage, sizeof(d.vs_float_usage));
  else const_used::SetAll(d.vs_float_usage);
  if (ps_copy_usage) std::memcpy(d.ps_float_usage, ps_copy_usage, sizeof(d.ps_float_usage));
  else const_used::SetAll(d.ps_float_usage);
  const const_used::CopyResult vs_copy = const_used::Copy(d.vs_constants, d.vs_float_retained, raw_vs_constants,
                                                           vs_copy_usage, const_used_mode != 0,
                                                           ConstUsedOptions().pad_zero && const_used_mode != 0);
  const const_used::CopyResult ps_copy = const_used::Copy(d.ps_constants, d.ps_float_retained, raw_ps_constants,
                                                           ps_copy_usage, const_used_mode != 0,
                                                           ConstUsedOptions().pad_zero && const_used_mode != 0);
  if (const_used_mode != 0 && ConstUsedOptions().bounds_diag) {
    if (!vs_copy.full_copy)
      CheckConstUsedReadCoverage("VS", d.vs_key, d.ordinal, vs_constant_usage,
                                 vsh && vsh->constant_register_map().float_dynamic_addressing, vs_copy_usage);
    if (!ps_copy.full_copy)
      CheckConstUsedReadCoverage("PS", d.ps_key, d.ordinal, ps_constant_usage,
                                 psh && psh->constant_register_map().float_dynamic_addressing, ps_copy_usage);
  }
  uint32_t const_used_mismatches = 0;
  if (const_used_mode == 2) {
    auto full = std::make_shared<nb::FullFloatConstants>();
    std::memcpy(full->vs, raw_vs_constants, sizeof(full->vs));
    std::memcpy(full->ps, raw_ps_constants, sizeof(full->ps));
    d.full_float_constants = std::move(full);
    const_used_mismatches = const_used::DifferenceCount(d.vs_constants, d.full_float_constants->vs, d.vs_float_usage) +
                            const_used::DifferenceCount(d.ps_constants, d.full_float_constants->ps, d.ps_float_usage);
  } else {
    d.full_float_constants.reset();
  }
  if (const_used_mode || ConstUsedOptions().block_frames)
    nb::ConstUsedRecordCopy(vs_copy.copied_registers + ps_copy.copied_registers,
                            vs_copy.cleared_registers + ps_copy.cleared_registers, const_used_mismatches);
  if (glitch::CurrentDraw().sampled) {
    glitch::CheckConstants(glitch::CurrentDraw(), d.vs_constants, d.ps_constants, vs_constant_usage,
                           ps_constant_usage, sizeof(d.vs_constants));
  }
  std::memcpy(d.shared + 640, &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 32);
  std::memcpy(d.shared + 672, &regs.values[XE_GPU_REG_SHADER_CONSTANT_LOOP_00], 128);
  pd::Add(pd::kConstCopy, pdk);
  uint32_t param_gen = UINT32_MAX;
  // Card S3-68 (E): the render-state outputs below (interpolator mask, colour exponent scales, alpha test, viewport /
  // scissor / VTE, depth / stencil, blend) read the render-state registers (R epoch), the SDK pixel shader (A key),
  // whether a pixel shader is used, the primitive type (stencil back face), the scissor clamp width and one cvar.
  uint32_t st_clamp_w = side->color_desc.width ? side->color_desc.width : side->depth_desc.width;
  if (!st_clamp_w) st_clamp_w = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
  const bool st_hit = sm && sm->st_epoch == dtr::RsEpoch() && sm->st_clamp_w == st_clamp_w && sm->st_prim == prim &&
                      sm->st_ps_use == (ps_use != nullptr) && sm->st_cvars == var_cvars;
  const bool st_use = dtm == 1 && st_hit;
  if (dtm) dtr::Count(dtr::kGState, st_hit);
  if (st_use) {
    sm->st.ApplyTo(d);
    spec = sm->st.spec;
    param_gen = sm->st.param_gen;
  }
  if (!st_use && psh) psh->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(), regs.Get<reg::SQ_CONTEXT_MISC>(), param_gen);
  std::memcpy(d.shared + 800, &param_gen, 4);
  if (CensusEvery()) {
    const reg::RB_DEPTHCONTROL cdc = draw_util::GetNormalizedDepthControl(regs);
    CensusNote(*side, param_gen != UINT32_MAX, d.ps_key, cdc.z_enable && cdc.z_write_enable);
  }
  if (MvCensusEvery()) {
    const reg::RB_DEPTHCONTROL mdc = draw_util::GetNormalizedDepthControl(regs);
    MvCensusNote(*side, vs_key, raw_vs_constants, vs_constant_usage,
                 mdc.z_enable && mdc.z_write_enable);
  }
  {  // Card S3-39: depth-writing draws into a dual-resolution surface this frame (decides next frame's resolution)
    const reg::RB_DEPTHCONTROL sdc = draw_util::GetNormalizedDepthControl(regs);
    static const bool load_lock_count = [] {  // card N4e: counted only for LOCK_30=4 or the observe-only trace
      const char* m = std::getenv("WWE2K14_LOCK_30");
      const char* t = std::getenv("WWE2K14_LOAD_LOCK_TRACE");
      return (m && std::strcmp(m, "4") == 0) || (t && *t == '1');
    }();
    if (load_lock_count && sdc.z_enable && sdc.z_write_enable) ++g_load_zdraws;
    if (sdc.z_enable && sdc.z_write_enable && (nb::SurfaceHasAlt(side->color_surface) || nb::SurfaceHasAlt(side->depth_surface))) {
      ++g_scene_zdraws;
    }
  }
  const uint32_t index_min = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  const uint32_t index_max = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  std::memcpy(d.shared + 804, &index_min, 4);
  for (uint32_t rt = 0; rt < 4 && !st_use; ++rt) {
    const auto ci = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[rt]);
    int32_t bias = ci.color_exp_bias;
    if (ci.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        ci.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      bias -= 5;
    }
    const float scale = std::ldexp(1.0f, bias);
    std::memcpy(d.shared + 816 + rt * 4, &scale, 4);
  }
  std::memcpy(d.shared + 808, &index_max, 4);
  const uint32_t* bools = &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031];
  const uint32_t booleans = (bools[0] & 0xFFFF) | (bools[4] & 0xFFFF) << 16;
  std::memcpy(d.shared + 512, &booleans, 4);
  if (!st_use) {
    const auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
    const xenos::CompareFunction alpha_func =
        colorcontrol.alpha_test_enable ? colorcontrol.alpha_func : xenos::CompareFunction::kAlways;
    if (alpha_func != xenos::CompareFunction::kAlways && ps_use) {
      spec |= 2;  // SPEC_CONSTANT_ALPHA_TEST
      const float alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
      const uint32_t func = uint32_t(alpha_func);
      std::memcpy(d.shared + 528, &alpha_ref, 4);
      std::memcpy(d.shared + 532, &func, 4);
    }
  }
  d.spec_constants = spec;

  if (set.UsesDxil() && (spec & 2) && ps_use && d.ps_spirv) {
    // D3D12 ignores Vulkan specialization constants: select the matching DXIL alpha-test twin, as in
    // <wwe13-recomp-nr>/src/nr/nr_frontend.cpp:2650-2659.
    const std::vector<uint8_t>* s2 = nullptr;
    if (d.ps_spirv == &ps_use->spirv) s2 = &ps_use->spirv_s2;
    else if (d.ps_spirv == &ps_use->spirv_f24) s2 = &ps_use->spirv_f24_s2;
    else if (d.ps_spirv == &ps_use->spirv_b) s2 = &ps_use->spirv_s2_b;
    else if (d.ps_spirv == &ps_use->spirv_f24_b) s2 = &ps_use->spirv_f24_s2_b;
    if (!s2 || s2->empty()) return Skip(why, "no DXIL alpha-test shader variant");
    d.ps_spirv = s2;
    d.ps_key ^= 0x0A17000000000000ull;
  }

  if (!st_use) {
    // ---- viewport / scissor (same SDK helpers and arguments as nr; the D3D side has no tiling: window offset 0) ----
    const reg::RB_DEPTHCONTROL depth = draw_util::GetNormalizedDepthControl(regs);
    draw_util::ViewportInfo vi;
    draw_util::GetHostViewportInfo(regs, 1, 1, set.UsesDxil(), 16384, 16384, true, depth,
                                   REXCVAR_GET(depth_float24_convert_in_pixel_shader) && !cfg.early_z, true,
                                   psh && psh->writes_depth(), vi);
    draw_util::Scissor sc;
    // Card M2c7: the SDK clamps the scissor to RB_SURFACE_INFO.surface_pitch (a host safety clamp, not game state). In a
    // predicated-tiling scene the bound surface is one tile (pitch 320) while the shadow draws the scene once into an
    // image of the scene width (targets.cpp BeginTiling), so the clamp is the bound shadow image's width; that width IS
    // the pitch for every untiled surface, i.e. identical to the SDK clamp there.
    draw_util::GetScissor(regs, sc, false);
    {
      uint32_t clamp_w = side->color_desc.width ? side->color_desc.width : side->depth_desc.width;
      if (!clamp_w) clamp_w = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
      const uint32_t tl = std::min(sc.offset[0], clamp_w), br = std::min(sc.offset[0] + sc.extent[0], clamp_w);
      sc.offset[0] = tl;
      sc.extent[0] = br > tl ? br - tl : 0;
    }
    // UHD 630 see-through fix (2026-10-31, WWE2K14_D3D_VP_FLIP=0 turns it off): Intel Gen9 writes no depth for a D3D12
    // viewport with MinDepth > MaxDepth (the game's scene draws use min 0.5 / max 0, i.e. depth = 0.5 - 0.5 * z). Use the
    // equivalent non-reversed range (max, min) and flip z in the position epilogue instead: nrHostPosition computes
    // z_out = z * ndc_scale.z + ndc_offset.z * w, so z' = w - z_out = z * (-scale.z) + (1 - offset.z) * w gives the same
    // stored depth (0 + 0.5 * (1 - z_ndc) = 0.5 - 0.5 * z_ndc) with the viewport range ascending. Every reversed-range
    // draw gets both changes together (constants and viewport are set here, in one place).
    static const bool vp_flip = [] { const char* v = std::getenv("WWE2K14_D3D_VP_FLIP"); return !(v && *v == '0'); }();
    float ndc_scale_c[3], ndc_offset_c[3], z_min_c = vi.z_min, z_max_c = vi.z_max;
    std::memcpy(ndc_scale_c, vi.ndc_scale, 12);
    std::memcpy(ndc_offset_c, vi.ndc_offset, 12);
    if (vp_flip && set.UsesDxil() && z_min_c > z_max_c) {
      ndc_scale_c[2] = -ndc_scale_c[2];
      ndc_offset_c[2] = 1.0f - ndc_offset_c[2];
      std::swap(z_min_c, z_max_c);
    }
    std::memcpy(d.shared + 544, ndc_scale_c, 12);
    std::memcpy(d.shared + 560, ndc_offset_c, 12);
    const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
    static const float wide_k = wide_core::ParseK(std::getenv("WWE2K14_D3D_WIDE"));
    const bool screen_space = vte.vtx_xy_fmt != 0;
    if (wide_core::IsEnabled(wide_k)) {
      static const bool aspect_requested = wide_aspect::Enabled();
      const bool game_aspect_enabled = aspect_requested && wide_aspect::WideProjectionActive();
      if (g_wide_frame != e.frame) {
        g_wide_frame = e.frame;
        g_wide_frame_stats = {};
        g_wide_fullscreen_target = 0;
      }
      static const bool wide_hud = [] {
        const char* value = std::getenv("WWE2K14_D3D_WIDE_HUD");
        return !(value && std::strcmp(value, "0") == 0);
      }();
      const uint32_t surface_pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
      const int32_t target_width = wide_core::TargetImageDimension(
          int32_t(side->color_desc.width), int32_t(side->depth_desc.width), int32_t(surface_pitch));
      const int32_t target_height = wide_core::TargetImageDimension(
          int32_t(side->color_desc.height), int32_t(side->depth_desc.height), int32_t(vi.xy_extent[1]));
      const wide_core::PixelRect viewport{int32_t(vi.xy_offset[0]), int32_t(vi.xy_offset[1]), int32_t(vi.xy_extent[0]),
                                          int32_t(vi.xy_extent[1])};
      const wide_core::PixelRect scissor{int32_t(sc.offset[0]), int32_t(sc.offset[1]), int32_t(sc.extent[0]),
                                         int32_t(sc.extent[1])};
      const bool small_draw = (prim == uint32_t(xenos::PrimitiveType::kTriangleStrip) &&
                               (d.count == 3 || d.count == 4)) ||
                              (prim == uint32_t(xenos::PrimitiveType::kTriangleList) &&
                               (d.count == 3 || d.count == 6)) ||
                              (prim == uint32_t(xenos::PrimitiveType::kQuadList) && di.count == 4);
      const uint32_t target_object = side->color_desc.object;
      const bool is_hud = wide_hud && wide_core::IsHudDraw(
          depth.z_enable, small_draw, screen_space, color_surface != nb::kNone, target_object,
          g_wide_fullscreen_target != 0, g_wide_fullscreen_target);
      const bool covers_full_image = wide_core::IsFullscreenPass(depth.z_enable, small_draw, viewport, scissor,
                                                                  target_width, target_height);
      // Post passes are small draws with no depth test and a full-image viewport/scissor. A later small draw to the
      // same guest surface is the HUD candidate; scene draws need both colour and depth targets plus depth testing.
      const bool fullscreen_pass = rect || (covers_full_image && !is_hud);
      const bool has_scene_targets = color_surface != nb::kNone && depth_surface != nb::kNone;
      const wide_core::SkipReason skip_reason = wide_core::ClassifyDraw(
          screen_space, fullscreen_pass, has_scene_targets, depth.z_enable);
      const bool aspect_scene = game_aspect_enabled && skip_reason == wide_core::SkipReason::kNone;
      const bool squeeze = wide_core::ShouldApplyGpuSqueeze(skip_reason, is_hud, game_aspect_enabled);
      if ((squeeze || aspect_scene) && !is_hud && wide_probe::Enabled()) {
        wide_probe::ObserveSceneDraw(e.frame, vs_key, d.vs_constants,
                                     vsh ? vsh->constant_register_map().float_bitmap : nullptr);
      }
      if (squeeze) {
        const float image_center_ndc = wide_core::ImageCenterNdc(target_width, int32_t(vi.xy_offset[0]),
                                                                int32_t(vi.xy_extent[0]));
        const wide_core::NdcX squeezed = wide_core::SqueezeNdc(ndc_scale_c[0], ndc_offset_c[0], wide_k,
                                                               image_center_ndc);
        ndc_scale_c[0] = squeezed.scale;
        ndc_offset_c[0] = squeezed.offset;
        std::memcpy(d.shared + 544, ndc_scale_c, 12);
        std::memcpy(d.shared + 560, ndc_offset_c, 12);
        const wide_core::ScissorRect squeezed_scissor = wide_core::SqueezeScissor(
            int32_t(sc.offset[0]), int32_t(sc.extent[0]), int32_t(target_width), wide_k);
        sc.offset[0] = uint32_t(squeezed_scissor.x);
        sc.extent[0] = uint32_t(squeezed_scissor.width);
      }
      if (is_hud) ++g_wide_frame_stats.hud_squeezed;
      else if (squeeze) ++g_wide_frame_stats.squeezed;
      else if (aspect_scene) ++g_wide_frame_stats.aspect_scene_draws;
      else if (skip_reason == wide_core::SkipReason::kScreenSpace) ++g_wide_frame_stats.skipped_screen_space;
      else if (skip_reason == wide_core::SkipReason::kFullscreenPass) ++g_wide_frame_stats.skipped_fullscreen;
      else if (skip_reason == wide_core::SkipReason::kNoSceneTarget) ++g_wide_frame_stats.skipped_no_target;
      else if (skip_reason == wide_core::SkipReason::kNoDepthTest) ++g_wide_frame_stats.skipped_no_depth;
      if (covers_full_image && !rect && !is_hud && target_object) g_wide_fullscreen_target = target_object;
    }
    const uint32_t vte_flags = (vte.vtx_xy_fmt ? 1u : 0u) | (vte.vtx_z_fmt ? 2u : 0u) | (vte.vtx_w0_fmt ? 4u : 0u);
    std::memcpy(d.shared + 576, &vte_flags, 4);
    if (vi.xy_extent[0] && vi.xy_extent[1]) {
      d.viewport[0] = float(vi.xy_offset[0]);
      d.viewport[1] = float(vi.xy_offset[1]);
      d.viewport[2] = float(vi.xy_extent[0]);
      d.viewport[3] = float(vi.xy_extent[1]);
    } else {
      d.viewport[0] = d.viewport[1] = -1;
      d.viewport[2] = d.viewport[3] = 1;
    }
    d.viewport[4] = z_min_c;
    d.viewport[5] = z_max_c;
    d.scissor[0] = int32_t(sc.offset[0]);
    d.scissor[1] = int32_t(sc.offset[1]);
    d.scissor[2] = int32_t(sc.extent[0]);
    d.scissor[3] = int32_t(sc.extent[1]);

    // ---- depth / stencil ----
    d.depth_test = depth.z_enable;
    d.depth_write = depth.z_enable && depth.z_write_enable;
    d.depth_compare = depth.z_enable ? uint32_t(depth.zfunc) + 1 : 8;
    if (depth.stencil_enable) {
      d.stencil = true;
      const auto ref = regs.Get<reg::RB_STENCILREFMASK>();
      d.stencil_ref = ref.stencilref;
      d.stencil_read = ref.stencilmask;
      d.stencil_write = ref.stencilwritemask;
      d.stencil_front = {uint32_t(depth.stencilfail) + 1, uint32_t(depth.stencilzfail) + 1,
                         uint32_t(depth.stencilzpass) + 1, uint32_t(depth.stencilfunc) + 1};
      d.stencil_back = depth.backface_enable && draw_util::IsPrimitivePolygonal(regs)
                           ? nb::StencilFace{uint32_t(depth.stencilfail_bf) + 1, uint32_t(depth.stencilzfail_bf) + 1,
                                             uint32_t(depth.stencilzpass_bf) + 1, uint32_t(depth.stencilfunc_bf) + 1}
                           : d.stencil_front;
    }

    // ---- blend / colour mask, RT 0 ----
    const uint32_t color_mask = draw_util::GetNormalizedColorMask(regs, psh ? psh->writes_color_targets() : 0) & 0xF;
    const auto blend = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]);
    static const uint8_t kFactor[32] = {1, 2, 1, 1, 3, 4, 5, 6, 9, 10, 7, 8, 12, 13, 12, 13, 11};
    static const uint8_t kOp[8] = {1, 2, 4, 5, 3, 1, 1, 1};
    d.blend.write_mask = uint8_t(color_mask);
    if (color_mask) {
      d.blend.src = kFactor[uint32_t(blend.color_srcblend) & 31];
      d.blend.dst = kFactor[uint32_t(blend.color_destblend) & 31];
      d.blend.op = kOp[uint32_t(blend.color_comb_fcn) & 7];
      d.blend.src_a = kFactor[uint32_t(blend.alpha_srcblend) & 31];
      d.blend.dst_a = kFactor[uint32_t(blend.alpha_destblend) & 31];
      d.blend.op_a = kOp[uint32_t(blend.alpha_comb_fcn) & 7];
      d.blend.enabled = !(d.blend.src == 2 && d.blend.dst == 1 && d.blend.op == 1 && d.blend.src_a == 2 &&
                          d.blend.dst_a == 1 && d.blend.op_a == 1);
    }
  }
  if (dtm && !st_use) {  // card S3-68 (E): compare (check mode) and fill
    dtr::StateOut o;
    o.From(d, spec, param_gen);
    if (dtm == 2 && st_hit) {
      const std::string diff = o.Diff(sm->st);
      if (!diff.empty()) dtr::Mismatch(dtr::kGState, e.seq, "state " + diff);
    }
    sm->st = o;
    sm->st_epoch = dtr::RsEpoch(), sm->st_clamp_w = st_clamp_w, sm->st_prim = prim;
    sm->st_ps_use = ps_use != nullptr, sm->st_cvars = var_cvars;
  }

  tp.To(7);
  // ---- textures: every slot the converted shaders sample ----
  std::set<uint32_t> ours;
  // Card S3-68 (T): the sampled slots and their contract dimensions are a pure function of (VS, PS used). The set is
  // still built when a diagnostic / LATE_TEX path below reads it.
  dtr::SlotsMemo* tm = dtm ? &dtr::SlotsSlot(vs, ps_use) : nullptr;
  const bool tm_hit = tm && tm->valid && tm->vs == vs && tm->ps == ps_use;
  const bool tm_use = dtm == 1 && tm_hit && !LateTexOn() && !FlashProbeOn();
  if (dtm) dtr::Count(dtr::kGSlots, tm_hit);
  if (!tm_use) {
    for (const ShaderEntry* en : {vs, ps_use}) {
      if (en) ours.insert(en->texture_slots.begin(), en->texture_slots.end());
    }
  }
  if (dtm && !tm_use) {
    std::vector<std::pair<uint32_t, uint32_t>> sl;
    for (uint32_t slot : ours) {
      uint32_t dim = 1;
      for (const ShaderEntry* en : {vs, ps_use}) {
        if (en && en->texture_dims.count(slot)) dim = en->texture_dims.at(slot);
      }
      sl.emplace_back(slot, dim);
    }
    if (dtm == 2 && tm_hit && sl != tm->slots) dtr::Mismatch(dtr::kGSlots, e.seq, "texture slot list");
    tm->valid = true, tm->vs = vs, tm->ps = ps_use;
    tm->slots = std::move(sl);
  }
  // Card S3-44 (WWE2K14_D3D_LATE_TEX=1): a texture the game was seen to rewrite AFTER its D3D draw call but before the
  // CP reached the draw (movie planes, s370) is read only once the CP has reached this draw: the game kicked the ring
  // past it, so the bytes are what the console GPU would read. The backend lock is released while waiting (the CP
  // thread may need it to get here).
  if (LateTexOn() && e.packet_addr != 0xFFFFFFFFu) {
    bool late = false;
    {
      std::lock_guard<std::mutex> lk(g_probe_mu);
      for (uint32_t slot : ours) {
        uint32_t start = 0, length = 0, format = 0, width = 0;
        if (textures::FetchBaseRange(s, slot, &start, &length, &format, &width) && g_late_bases.count(start)) late = true;
      }
    }
    // Card S3-44 (WWE2K14_D3D_LATE_TEX=3): every draw waits for the guest kick (console GPUs read after the kick);
    // never while the game thread is blocked on a full capture queue (that wait would be circular).
    static const bool all_mode = [] {
      const char* v = std::getenv("WWE2K14_D3D_LATE_TEX");
      return v && *v == '3';
    }();
    if (all_mode) late = true;
    // FL-4 Design A (WWE2K14_D3D_LATE_TEX=4): translate never waits; the g_late_bases lookup above still marks the
    // draw (one source for "late"), but the bytes are re-read at the release boundary by late_upload.cpp.
    if (late_upload::On()) late = false;
    if (late && CaptureProducerBlocked()) {
      late = false;
      g_late_skipped.fetch_add(1, std::memory_order_relaxed);
    }
    if (late && ShadowMode() == 2) {  // mode 2: the worker holds BackendLock while translating
      g_late_waits.fetch_add(1, std::memory_order_relaxed);
      BackendLock().unlock();
      static const bool kick_mode = [] {
        const char* v = std::getenv("WWE2K14_D3D_LATE_TEX");
        return v && (*v == '2' || *v == '3');
      }();
      static const uint32_t wait_ms = [] {  // card S3-44: wait cap (S117: 30 ms caps cost UHD entrance stalls)
        const char* v = std::getenv("WWE2K14_D3D_LATE_TEX_MS");
        return v ? uint32_t(std::strtoul(v, nullptr, 10)) : 30u;
      }();
      const bool ok = kick_mode ? GateWaitKickAfter(e.seq, wait_ms) : GateWaitCpArrival(e.seq, wait_ms);
      if (!ok) g_late_timeouts.fetch_add(1, std::memory_order_relaxed);
      BackendLock().lock();
      const uint64_t n = g_late_waits.load();
      if (n == 1 || n % 2000 == 0) {
        REXLOG_INFO("[d3d-late] kick waits={} timeouts={} skipped(producer blocked)={} late_bases={}", n,
                    g_late_timeouts.load(), g_late_skipped.load(), g_late_bases.size());
      }
    }
  }
  // Card S3-55 diagnostic (WWE2K14_D3D_DOF_TRACE=N: every Nth draw of the DOF composite PS DC4817F76AA5B63E, at most
  // WWE2K14_D3D_DOF_MAX lines): its constants and, per bound slot, handle / content key / fetch dwords / ForSlot reason.
  static const uint32_t dof_every = [] { const char* v = std::getenv("WWE2K14_D3D_DOF_TRACE"); return v ? uint32_t(std::strtoul(v, nullptr, 10)) : 0u; }();
  static const uint32_t dof_max = [] { const char* v = std::getenv("WWE2K14_D3D_DOF_MAX"); return v ? uint32_t(std::strtoul(v, nullptr, 10)) : 600u; }();
  static uint32_t dof_seen = 0, dof_logged = 0;
  // Card S3-67 (off unless WWE2K14_D3D_LATE_LEARN=d3d|census with LATE_TEX=4): the translate-side hash is taken before
  // ForSlot reads the guest bytes for the upload (review F4): a write between the two is then seen as a change.
  LateLearnRecord(e, s, ours);
  const bool dof = dof_every && side->ps_fnv == 0xDC4817F76AA5B63Eull && dof_logged < dof_max && (dof_seen++ % dof_every) == 0;
  std::string dof_slots;
  const std::vector<std::pair<uint32_t, uint32_t>>* tm_slots = tm_use ? &tm->slots : nullptr;  // card S3-68 (T)
  size_t tm_i = 0;
  auto ours_it = ours.begin();
  nb::ResolveCensusDrawScope resolve_census_draw(e.frame);
  for (;;) {
    uint32_t slot, dim = 1;
    if (tm_slots) {
      if (tm_i == tm_slots->size()) break;
      slot = (*tm_slots)[tm_i].first, dim = (*tm_slots)[tm_i].second, ++tm_i;
    } else {
      if (ours_it == ours.end()) break;
      slot = *ours_it++;
      for (const ShaderEntry* en : {vs, ps_use}) {
        if (en && en->texture_dims.count(slot)) dim = en->texture_dims.at(slot);
      }
    }
    if (dim != 1 && dim != 3) return Skip(why, "texture dimension " + std::to_string(dim));
    nb::SamplerDesc wp8_sampler;
    uint64_t content_key = 0;
    std::string tw;
    // `dim` is the shader-contract dimension (1 = 2D, 3 = cube), as nr's TextureFor(want_cube = dim == 3).
    uint32_t handle;
    bool lit_shadow_input = false;
    if (remove_shadows) {
      uint32_t fetch_base = 0, fetch_length = 0, fetch_format = 0, fetch_width = 0;
      const bool have_fetch_base = textures::FetchBaseRange(s, slot, &fetch_base, &fetch_length, &fetch_format,
                                                             &fetch_width);
      lit_shadow_input = dim == 1 && have_fetch_base && weakpc::IsShadowInput(side->ps_fnv, slot, fetch_base);
    }
    if (lit_shadow_input) {
      handle = lit_shadow_texture;
      tw = "weak-PC shadows disabled: 1x1 lit texture";
      content_key = 0;  // synthetic option texture, not a game-derived texture-content key
    } else {
      const uint64_t pdt = pd::Now();
      handle = textures::ForSlot(s, slot, dim, &wp8_sampler, &content_key, &tw);
      pd::Add(pd::kTexForSlot, pdt);
    }
    nb::TextureBinding b;
    b.slot = slot;
    b.dimension = dim == 3 ? 2 : 0;
    b.texture = handle;
    // The sampler is decoded here (the fetch instruction's override lives in the shaders), the WP8 sampler is unused.
    const uint64_t pds = pd::Now();
    if (!dtm) {
      b.sampler = sh::SamplerFromFetch(regs.GetTextureFetch(slot), slot, vsh, psh);
    } else {  // card S3-68: valid while fetch slot `slot` is unchanged (slot epoch) and the SDK shaders are the same
      dtr::SamplerMemo& smp = dtr::SamplerSlot(slot);
      const bool smp_hit = smp.valid && smp.epoch == dtr::SlotEpoch(slot) && smp.vsh == vsh && smp.psh == psh;
      dtr::Count(dtr::kGSampler, smp_hit);
      if (dtm == 1 && smp_hit) {
        b.sampler = smp.sampler;
      } else {
        b.sampler = sh::SamplerFromFetch(regs.GetTextureFetch(slot), slot, vsh, psh);
        if (dtm == 2 && smp_hit && !dtr::SameSampler(smp.sampler, b.sampler))
          dtr::Mismatch(dtr::kGSampler, e.seq, "sampler slot " + std::to_string(slot));
        smp.valid = true, smp.epoch = dtr::SlotEpoch(slot), smp.vsh = vsh, smp.psh = psh, smp.sampler = b.sampler;
      }
    }
    pd::Add(pd::kTexSampler, pds);
    if (texq::Mode()) texq::Apply(s, handle, slot, vs, ps_use, d.vs_spirv, d.ps_spirv, b.sampler);
    if (handle == nb::kNone) side->texture_fail = true;
    if (d.textures.size() < 32) side->tex_content_key[d.textures.size()] = content_key;
    d.textures.push_back(b);
    if (dof) {
      const auto f = regs.GetTextureFetch(slot);
      uint32_t w[6];
      std::memcpy(w, &f, sizeof(w));
      char t[200];
      std::snprintf(t, sizeof(t), "\n[d3d-dof]   slot=%u h=%u ck=%016llX fetch=%08X %08X %08X %08X %08X %08X why=", slot, handle,
                    static_cast<unsigned long long>(content_key), w[0], w[1], w[2], w[3], w[4], w[5]);
      dof_slots += t;
      dof_slots += tw.substr(0, 160);
    }
  }
  if (dof) {
    ++dof_logged;
    const float* c = reinterpret_cast<const float*>(raw_ps_constants);
    auto C = [c](uint32_t r) { return fmt::format("c{}={:.4g},{:.4g},{:.4g},{:.4g}", r, c[r * 4], c[r * 4 + 1], c[r * 4 + 2], c[r * 4 + 3]); };
    REXLOG_INFO("[d3d-dof] frame={} draw#{} {} {} {} {} {} {} {} {} {} hash={:08X}{}", e.frame, dof_seen - 1, C(250), C(251), C(252),
                C(255), C(20), C(21), C(27), C(29), C(30), uint32_t(XXH3_64bits(raw_ps_constants, 4096)), dof_slots);
  }
  side->texture_slots = uint32_t(d.textures.size());
  if (FlashProbeOn()) FlashProbeRecord(e.packet_addr, e.frame, s, ours);  // card S3-44 diagnostic
  {  // Card S3-44 diagnostic (FLASH_TRACE): constants + texture content of the final full-screen passes (DeepSeek F-C2)
    static const bool ft = [] {
      const char* v = std::getenv("WWE2K14_NR_FLASH_TRACE");
      return v && *v == '1';
    }();
    static const uint64_t kFinal[] = {0xDC4817F76AA5B63Eull, 0x39DBF812206993A7ull, 0x438C2AF84C78A133ull,
                                      0xF1CC033418E7B30Full, 0x01E48C29D57B6A7Bull, 0xCE206EE96CDC3960ull};
    if (ft && g_frame_finals.size() < 1500 && std::find(std::begin(kFinal), std::end(kFinal), side->ps_fnv) != std::end(kFinal)) {
      const float* c = reinterpret_cast<const float*>(raw_ps_constants);
      char b[256];
      int n = std::snprintf(b, sizeof(b), " {%04X h%08X c0=%.3g,%.3g,%.3g,%.3g c1=%.3g,%.3g,%.3g,%.3g t:", uint32_t(side->ps_fnv >> 48),
                            uint32_t(XXH3_64bits(raw_ps_constants, 4096)), c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
      for (uint32_t i = 0; i < side->texture_slots && i < 4 && n < 230; ++i)
        n += std::snprintf(b + n, sizeof(b) - size_t(n), "%04X,", uint32_t(side->tex_content_key[i] & 0xFFFF));
      g_frame_finals += b;
      g_frame_finals += "}";
    }
  }
  ++g_cnt.translated;
  bool crowd_check_match = false;
  const uint64_t check_save_start = crowd_fast_write_cache ? CrowdFastProfileStart() : 0;
  if (crowd_check_candidate) {
    ++g_crowd_fast_stats.checks;
    CrowdFastCache& cache = CrowdFast();
    const uint8_t* const current_vs = reinterpret_cast<const uint8_t*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
    crowd_check_match = SameTranslatedDraw(d, *cache.draw, current_vs, e.seq) &&
                        SameDrawSidePayload(*side, cache.side);
    if (!crowd_check_match) {
      ++g_crowd_fast_stats.mismatches;
      if (g_crowd_fast_stats.mismatches <= 8) {
        // V17d diagnostic (Claude 11-03): which part differs
        auto diff = [&]() -> std::string {
          const nb::Draw& a = d; const nb::Draw& b = *cache.draw;
          const auto& a_indices = a.Indices(); const auto& b_indices = b.Indices();
          const auto& a_buffers = a.Buffers(); const auto& b_buffers = b.Buffers();
          const auto& a_textures = a.Textures(); const auto& b_textures = b.Textures();
          std::string r;
          if (!SameDrawSidePayload(*side, cache.side)) r += "side ";
          if (a.ordinal != e.seq) r += "ordinal ";
          if (!const_used::UsedRowsEqual(a.vs_constants, current_vs, a.vs_float_usage)) r += "vs_constants ";
          if (std::memcmp(a.ps_constants, b.ps_constants, sizeof(a.ps_constants))) r += "ps_constants ";
          if (std::memcmp(a.shared, b.shared, sizeof(a.shared))) r += "shared ";
          if (std::memcmp(a.viewport, b.viewport, sizeof(a.viewport)) || std::memcmp(a.scissor, b.scissor, sizeof(a.scissor))) r += "viewport/scissor ";
          if (a_indices.size() != b_indices.size() || (a_indices.size() && std::memcmp(a_indices.data(), b_indices.data(), a_indices.size()))) r += "indices ";
          if (a_buffers.size() != b_buffers.size()) r += "buffers_n ";
          else for (size_t i = 0; i < a_buffers.size(); ++i) {
            const auto& x = a_buffers[i]; const auto& y = b_buffers[i];
            if (x.data.size() != y.data.size() || (x.data.size() && std::memcmp(x.data.data(), y.data.data(), x.data.size()))) r += "vb_data ";
            if (x.cache_id != y.cache_id || x.cache_size != y.cache_size || x.cache_upload != y.cache_upload) r += "vb_cache ";
            if (x.direct_src != y.direct_src || x.direct_bytes != y.direct_bytes) r += "vb_direct ";
          }
          if (a_textures.size() != b_textures.size()) r += "textures_n ";
          else for (size_t i = 0; i < a_textures.size(); ++i) if (a_textures[i].texture != b_textures[i].texture) { r += "texture_handle "; break; }
          if (a.index_cache_id != b.index_cache_id || a.index_cache_bytes != b.index_cache_bytes || a.index_cache_upload != b.index_cache_upload) r += "index_cache ";
          if (a.count != b.count) r += "count ";
          if (r.empty()) r = "other-state";
          return r;
        };
        REXLOG_WARN("[d3d-crowd-fast] CHECK mismatch seq={} frame={} vs={:016X} ps={:016X} diff=[{}]; full TranslateDraw retained",
                    e.seq, e.frame, vs_key, ps_key, diff());
      }
    } else if (crowd_fast_write_cache) {
      // The full CHECK result matched every cached field, so retain its immutable payload and advance only record ids.
      cache.seq = e.seq;
      UpdateCrowdFastSide(cache.side, e);
    }
  }
  if (crowd_fast_write_cache && !crowd_check_match)
    SaveCrowdFast(CrowdFast(), e, s, di, vs_key, ps_key, d, *side, crowd_index_object, crowd_index_data,
                  crowd_vertex_sources, crowd_lookups);
  if (crowd_fast_write_cache) CrowdFastProfileAdd(3, check_save_start);
  return true;
}

std::string TakeFrameSkips() {
  std::string v;
  v.swap(g_frame_skips);
  v += " | finals:";
  v += g_frame_finals;
  g_frame_finals.clear();
  return v;
}

wide_core::FrameDrawStats TakeWideDrawStats(uint32_t frame) {
  return frame == g_wide_frame ? g_wide_frame_stats : wide_core::FrameDrawStats{};
}

void DrawSubmitted(const DrawSide& side) {
  for (uint32_t i = 0; i < side.n_vb_commit; ++i) static_cast<nr::shared::VbCache::Entry*>(side.vb_commit[i])->backend_has = true;
  if (side.cache_flush_sent && VbCacheD3d()) VbCacheD3d()->flush = false;
}

#else  // !WWE2K14_HAS_NR

std::string TakeFrameSkips() { return {}; }
wide_core::FrameDrawStats TakeWideDrawStats(uint32_t) { return {}; }
bool TranslateDraw(const EvHeader&, const RegView&, const DeviceState&, nr::backend::Draw*, DrawSide*,
                   std::string* why) {
  if (why) *why = "built without the nr backend";
  return false;
}

void DrawSubmitted(const DrawSide&) {}
void LateLearnNoteCall(uint32_t) {}
void LateLearnFrameEnd() {}
void late_upload::LearnAtRelease() {}

#endif


// ---- Card S3-31 census (diagnostic; WWE2K14_D3D_PAIR_CENSUS=N frames; remove when the scene-scale question is settled)
namespace {
struct CensusRow {
  uint32_t cw = 0, ch = 0, cs = 0, dw = 0, dh = 0;
  uint64_t draws = 0, pg = 0, zw = 0;
  std::map<uint64_t, uint64_t> pg_ps;  // pixel shader key -> draws using screen position (param_gen)
};
std::map<std::pair<uint32_t, uint32_t>, CensusRow> g_census;
uint32_t g_census_frames = 0;
uint32_t CensusEvery() {
  static const uint32_t n = [] {
    const char* e = std::getenv("WWE2K14_D3D_PAIR_CENSUS");
    return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 0u;
  }();
  return n;
}
void CensusNote(const DrawSide& side, bool pg, uint64_t ps_key, bool zwrite) {
  CensusRow& r = g_census[{side.color_surface, side.depth_surface}];
  r.cw = side.color_desc.width; r.ch = side.color_desc.height; r.cs = side.color_desc.samples;
  r.dw = side.depth_desc.width; r.dh = side.depth_desc.height;
  ++r.draws;
  if (zwrite) ++r.zw;
  if (pg) {
    ++r.pg;
    if (r.pg_ps.size() < 16 || r.pg_ps.count(ps_key)) ++r.pg_ps[ps_key];
  }
}
}  // namespace

// FL-4 Design A (WWE2K14_D3D_LATE_TEX=4): the learned late-base set lives here (g_late_bases, filled by the CP probe);
// late_upload.cpp reads it through this accessor, so "this base is late" has exactly one source.
bool late_upload::IsLateBase(uint32_t base_start) {
  std::lock_guard<std::mutex> lk(g_probe_mu);
  return g_late_bases.count(base_start) != 0;
}
// Card S3-39: a frame with >= 64 depth-writing draws into the screen pair is a 3D-scene frame: the next frame uses the
// scaled set; otherwise (menus, loading, results) the native set, so 2D screens stay sharp.
// Card N4e (WWE2K14_LOCK_30=4): per worker frame, feed the loading detector (worker thread) and publish its state.
void LoadLockFrameEnd() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_LOCK_30");
    return e && std::strcmp(e, "4") == 0;
  }();
  static const bool trace = [] {
    const char* e = std::getenv("WWE2K14_LOAD_LOCK_TRACE");
    return e && *e == '1';
  }();
  if (!on && !trace) { g_load_zdraws = 0; return; }  // trace alone = observe only (the guest hook acts on mode 4 only)
  static wwe2k14::loadlock::Detector det = [] {
    wwe2k14::loadlock::Detector d;
    if (const char* e = std::getenv("WWE2K14_LOAD_LOCK_ENTER")) d.enter_frames = uint32_t(std::strtoul(e, nullptr, 10));
    return d;
  }();
  static uint64_t frame = 0;
  const bool was = det.loading;
  const bool now = det.Step(g_load_zdraws);
  wwe2k14::loadlock::g_loading.store(now, std::memory_order_relaxed);
  if (trace) {
    static uint32_t last_bucket = ~0u;
    const uint32_t bucket = g_load_zdraws >= 64 ? 2 : (g_load_zdraws ? 1 : 0);
    if (bucket != last_bucket || was != now || (frame % 300) == 0)
      REXLOG_INFO("[load-lock] frame {} zdraws {} bucket {} loading {}", frame, g_load_zdraws, bucket, now);
    last_bucket = bucket;
  }
  if (was != now) REXLOG_INFO("[load-lock] frame {} loading -> {} (zdraws {})", frame, now, g_load_zdraws);
  ++frame;
  g_load_zdraws = 0;
}

void SceneScaleFrameEnd() {
  LateLearnFrameEnd();  // card S3-67 (worker, every kSwap: [d3d-learn] line; off unless WWE2K14_D3D_LATE_LEARN=d3d|census)
  static const bool dyn = [] {
    const char* e = std::getenv("WWE2K14_D3D_SCALE_DYNAMIC");
    return e && std::strcmp(e, "1") == 0;
  }();
  if (!dyn) return;
  // Card S3-43 (bounded diagnostic): how often the active set flips (each flip may show the other set's old content).
  static uint64_t frames = 0, flips = 0, flips_total = 0, scaled = 0;
  const bool want = g_scene_zdraws >= 64;
  if (nb::SetSceneScaleActive(want)) {
    ++flips, ++flips_total;
    // Card S3-44 (bounded diagnostic, FLASH_TRACE only; a handful per run): flip times, to line up with [nr-flash] spikes.
    static const bool ft = [] {
      const char* v = std::getenv("WWE2K14_NR_FLASH_TRACE");
      return v && *v == '1';
    }();
    if (ft) REXLOG_INFO("[d3d-scale] flip -> {} (zdraws {}, flips total {})", want ? "scaled" : "full", g_scene_zdraws, flips_total);
  }
  scaled += want;
  if (++frames == 600) {
    REXLOG_INFO("[d3d-scale] last 600 frames: set flips={} scaled_frames={} (flips total {})", flips, scaled,
                flips_total);
    frames = flips = scaled = 0;
  }
  g_scene_zdraws = 0;
}

// ---- U0b motion-vector census (diagnostic; WWE2K14_D3D_MV_CENSUS=N frames; docs/reports/20261101-upscaling-u0-
// feasibility.md; remove when U0 is settled). Per colour target: how many depth-writing draws share one view-projection
// (c56-c59) and how often it changes between frames; per draw that reads c76+ (world / bone rows): whether a draw with
// the same key existed last frame and whether its c76+ rows changed (= object motion a velocity pass must cover).
namespace {
struct MvSurf {
  uint32_t w = 0, h = 0, samples = 0;
  uint64_t zdraws = 0, vp_dominant = 0, vp_distinct = 0, frames = 0, vp_changed = 0;
  std::unordered_map<uint64_t, uint32_t> vp_frame;  // this frame: c56-c59 hash -> depth-writing draws
  uint64_t vp_last = 0;
};
struct MvTotals {
  uint64_t world_draws = 0, matched = 0, moved = 0, dup_keys = 0, rows = 0;
};
std::map<uint32_t, MvSurf> g_mv_surf;
std::unordered_map<uint64_t, uint64_t> g_mv_prev, g_mv_cur;  // draw key -> c76+ hash
std::unordered_map<uint64_t, uint32_t> g_mv_ordinal;          // base key -> draws so far this frame
MvTotals g_mv;
uint32_t g_mv_frames = 0;
uint32_t MvCensusEvery() {
  static const uint32_t n = [] {
    const char* e = std::getenv("WWE2K14_D3D_MV_CENSUS");
    return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 0u;
  }();
  return n;
}
void MvCensusNote(const DrawSide& side, uint64_t vs_key, const uint8_t* vs_constants, const uint64_t* vs_usage,
                  bool zwrite) {
  if (zwrite) {
    MvSurf& m = g_mv_surf[side.color_surface];
    m.w = side.color_desc.width, m.h = side.color_desc.height, m.samples = side.color_desc.samples;
    ++m.zdraws;
    ++m.vp_frame[XXH3_64bits(vs_constants + 56 * 16, 4 * 16)];
  }
  if (!vs_usage) return;
  uint32_t rows = 0;
  uint64_t h = 0;
  for (uint32_t r = 76; r < 256; ++r)
    if (vs_usage[r >> 6] & (uint64_t(1) << (r & 63))) ++rows, h = XXH3_64bits_withSeed(vs_constants + r * 16, 16, h);
  if (!rows) return;
  const uint64_t ids[5] = {vs_key, side.color_surface, side.index_count, side.vertex_bytes, rows};
  const uint64_t base = XXH3_64bits(ids, sizeof(ids));
  const uint32_t ord = g_mv_ordinal[base]++;
  if (ord) ++g_mv.dup_keys;
  const uint64_t key = XXH3_64bits_withSeed(&ord, 4, base);
  g_mv_cur[key] = h;
  ++g_mv.world_draws;
  g_mv.rows += rows;
  const auto p = g_mv_prev.find(key);
  if (p != g_mv_prev.end()) {
    ++g_mv.matched;
    if (p->second != h) ++g_mv.moved;
  }
}
}  // namespace

void MvCensusFrameEnd() {
  const uint32_t n = MvCensusEvery();
  if (!n) return;
  for (auto& kv : g_mv_surf) {
    MvSurf& m = kv.second;
    if (m.vp_frame.empty()) continue;
    uint64_t best = 0, best_n = 0;
    for (const auto& v : m.vp_frame)
      if (v.second > best_n) best = v.first, best_n = v.second;
    m.vp_dominant += best_n;
    m.vp_distinct += m.vp_frame.size();
    if (m.frames++ && best != m.vp_last) ++m.vp_changed;
    m.vp_last = best;
    m.vp_frame.clear();
  }
  g_mv_prev.swap(g_mv_cur);
  g_mv_cur.clear();
  g_mv_ordinal.clear();
  if (++g_mv_frames < n) return;
  for (const auto& kv : g_mv_surf) {
    const MvSurf& m = kv.second;
    if (m.zdraws < n) continue;  // < 1 depth-writing draw per frame
    REXLOG_INFO("[d3d-mv] surf c={} {}x{}x{} frames={} zdraws/f={:.1f} vp_dominant={:.1f}% vp_distinct/f={:.2f} "
                "vp_changed={:.1f}% of frames",
                kv.first, m.w, m.h, m.samples, m.frames, double(m.zdraws) / n,
                100.0 * double(m.vp_dominant) / double(m.zdraws), double(m.vp_distinct) / double(m.frames ? m.frames : 1),
                100.0 * double(m.vp_changed) / double(m.frames ? m.frames : 1));
  }
  const double wd = double(g_mv.world_draws ? g_mv.world_draws : 1);
  REXLOG_INFO("[d3d-mv] world draws/f={:.1f} rows/draw={:.1f} matched={:.1f}% moved={:.1f}% of matched dup_keys={:.1f}%",
              double(g_mv.world_draws) / n, double(g_mv.rows) / wd, 100.0 * double(g_mv.matched) / wd,
              100.0 * double(g_mv.moved) / double(g_mv.matched ? g_mv.matched : 1), 100.0 * double(g_mv.dup_keys) / wd);
  g_mv_surf.clear();
  g_mv = MvTotals{};
  g_mv_frames = 0;
}

void PairCensusFrameEnd() {
  const uint32_t n = CensusEvery();
  if (!n) return;
  if (++g_census_frames < n) return;
  for (const auto& kv : g_census) {
    const CensusRow& r = kv.second;
    if (r.draws < n) continue;  // < 1 draw per frame
    std::string pss;
    for (const auto& p : r.pg_ps) pss += fmt::format(" {:016x}:{:.1f}", p.first, double(p.second) / n);
    REXLOG_INFO("[d3d-census] pair c={} {}x{}x{} d={} {}x{} draws/f={:.1f} zwrite/f={:.1f} screenpos/f={:.1f}{}",
                kv.first.first, r.cw, r.ch, r.cs, kv.first.second, r.dw, r.dh, double(r.draws) / n, double(r.zw) / n,
                double(r.pg) / n, pss);
  }
  g_census.clear();
  g_census_frames = 0;
}

}  // namespace wwe2k14::d3d
