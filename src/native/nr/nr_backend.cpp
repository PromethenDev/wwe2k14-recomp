// Imported into WWE 2K14 native workspace (Track E, 2026-10-13) from
//   <wwe13-recomp-nr>/src/nr/nr_backend.cpp
//   repo HEAD 19c8bb361a9b7d6cba65afffc628bda023da2e0c (file last changed in 8d161fa),
//   original sha256 347d4f3d15ed33f1a34238e7db3db121ac4d626918cc8514615e8b717083b945.
// Local changes (docs/reports/20261013-nr-import.md): WWE13_NR* renamed to WWE2K14_NR*; Vulkan only - the D3D12
// interface creation, WWE13_NR_D3D12 and the D3D12 depth probe removed; IsD3D12() is constant false, so the
// remaining "[wwe13 nr] D3D12 only" branches are dead code kept for a later mechanical prune.
// WWE '13 native renderer - GPU backend (see nr_backend.h). plume (MIT) Vulkan.

#include "nr_backend.h"
#include "rt_view_formats.h"
#include "../v8_diag_format.h"
#include "nr_constants32.h"
#include "nr_record_queue.h"
#include "nr_draw_swap.h"
#include "nr_draw_profile.h"
#include "crowd_group.h"
#include "fix_alt.h"
#include "fix_alt_knobs.h"
#include "rec_reuse.h"
#include "nr_vb_swap.h"
#include "../d3d/v17c_ab.h"

#include <algorithm>
#include <deque>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include "vma_kinds.h"
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include <array>
#include <atomic>
#include <functional>
#include <filesystem>
#include <thread>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <time.h>
#endif
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#include <fstream>
#include <iterator>
#ifndef WWE2K14_NO_TEST_TOOLS
#include <string_view>
#endif

#include <plume_render_interface.h>
#include <plume_vulkan.h>
#ifdef _WIN32
#include <plume_d3d12.h>  // card C3: D3D12ExistingDevice, D3D12Texture / D3D12TextureView / D3D12CommandFence handles
#endif

// XXH3_64bits (WWE2K14_NR_DRAW_FAST pipeline-key hash). The backend is a separate compilation unit
// (tools/nr/build-backend.sh) without the SDK's rex include path, so it uses the vendored single
// header directly (thirdparty/xxHash), inlined exactly as rex/hash.h does for the frontend.
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
#ifdef _WIN32
// Ported D3D12 factory declaration: <wwe13-recomp-nr>/src/nr/nr_backend.cpp:37-41.
// (The card C3 overload CreateD3D12Interface(const D3D12ExistingDevice&) is declared in plume_d3d12.h.)
std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
}

namespace nr::backend {
namespace draw_profile {
namespace {
Snapshot producer;
std::atomic<uint64_t> record_ns[kRecordN]{}, record_n[kRecordN]{};
}
void Add(Worker k, uint64_t t0) {
  if (!On()) return;
  producer.worker_ns[k] += Now() - t0;
  ++producer.worker_n[k];
}
void RecordAdd(Record k, uint64_t ns) {
  record_ns[k].fetch_add(ns, std::memory_order_relaxed);
  record_n[k].fetch_add(1, std::memory_order_relaxed);
}
bool Active() { return On() || draw_swap::Mode() != 0; }
Snapshot Take() {
  Snapshot result = producer;
  producer = {};
  for (int i = 0; i < kRecordN; ++i) {
    result.record_ns[i] = record_ns[i].exchange(0, std::memory_order_relaxed);
    result.record_n[i] = record_n[i].exchange(0, std::memory_order_relaxed);
  }
  return result;
}
}  // namespace draw_profile
void CrowdFlushBoundary();  // card U2c: execute a pending consecutive crowd run before other backend work
namespace {

using namespace plume;

// Bindless tables (shader_common_nr.h): set 0 = 2D, 1 = 3D, 2 = cube textures, 3 = samplers.
// Index 0 of every table is a 1x1 magenta "not bound" texture / default sampler, so a slot the
// frontend failed to bind is visible (and the frontend's binding audit counts it).
constexpr uint32_t kTableSize = 4096;
// D3D12 only: Plume's shader-visible sampler heap is capped at 2048 descriptors; retain the 2000-table limit from
// <wwe13-recomp-nr>/src/nr/nr_backend.cpp:53-55, with overflow handling at :898-903.
constexpr uint32_t kSamplerTableSizeD3D12 = 2000;
// Constant upload layout: guard | VS 4096 | PS 4096 | shared 1024 | guard. The guards absorb
// relative-addressed constant reads with a negative a0 (first attempt, XW4: device loss on Intel).
constexpr uint32_t kGuard = 4096;
constexpr uint32_t kConstBytes = kGuard + 4096 + 4096 + 1024 + kGuard;
// M4 batching: all commands go into one command list; the CPU waits for the GPU only before it reads
// results or when a per-batch ring is full. So every per-draw resource must stay valid until that wait:
// constants live in a ring of kConstSlots slots (each with its own zeroed guards, never written), and buffers,
// released textures and their table indices are retired until the wait (Flush).
// 12288 slots (~210 MB of upload memory) instead of 4096: the 4096-slot ring filled every ~3 frames on UHD 630 and
// drained the GPU each time (docs/cards/P2-uhd-frame-cost.md #3).
constexpr uint32_t kConstSlots = 12288;
static_assert(uint64_t(kConstBytes) * kConstSlots == nr_constants32::kSlotBytes * nr_constants32::kSlots,
              "nr_constants32 ring size must match the backend constants ring");

struct Texture {
  std::unique_ptr<RenderTexture> texture;
  std::shared_ptr<RenderTexture> shared;  // resolve reuse: an image shared with its registry snapshot (texture null)
  std::unique_ptr<RenderTextureView> view;
  uint32_t table_index = 0;
  bool phys_snap_image = false;  // U7: `Image()` is physical-size; non-draw reads may materialize logical extent
  uint32_t phys_snap_logical_width = 0, phys_snap_logical_height = 0;
  uint64_t mem_last_use_present = 0;
  uint8_t mem_snapshot_count = 0;
  uint32_t mem_snapshot_ids[16] = {};
  RenderTexture* Image() const { return texture ? texture.get() : shared.get(); }
  // FL-4 Design A (WWE2K14_D3D_LATE_TEX=4): CreateTexture's staging buffer and the per-level layout it wrote, kept so
  // RefillTextureUpload can rewrite the same bytes before the list that recorded the copies is submitted. `staging`
  // stays valid while create_serial == State::submit_serial (any submission makes Retire free nothing until a wait,
  // and every wait is preceded by a submission); create_cmd pins the command list it was recorded into.
  struct StagingLevel {
    uint64_t offset = 0, bytes = 0;  // bytes: the exact size the tight copy wrote (pitch == 0 only)
    uint32_t pitch = 0, tight = 0, rows = 0;
  };
  RenderBuffer* staging = nullptr;
  std::vector<StagingLevel> staging_levels;
  uint64_t create_serial = 0;
  uint32_t create_cmd = 0;
};
// One host render target (an SDK RenderTargetKey).
struct SurfaceObj {
  std::unique_ptr<RenderTexture> image;     // colour, or D32F S8
  std::unique_ptr<RenderTexture> resolved;  // colour 4x: 1x copy for reading averaged colour
  std::unique_ptr<RenderTextureView> sample_view;   // [wwe13 nr] D3D12 only: typed default colour SRV (Vulkan binds the image's own view)
  std::unique_ptr<RenderTextureView> int_view;      // colour with an integer transfer format
  std::unique_ptr<RenderTextureView> depth_view;    // depth aspect (sampled by D3D depth snapshots)
  std::unique_ptr<RenderTextureView> stencil_view;  // stencil aspect
  std::unique_ptr<RenderFramebuffer> transfer_fb;   // integer clears (int_view or image alone)
  RenderFormat format = RenderFormat::UNKNOWN;      // host format
  RenderFormat int_format = RenderFormat::UNKNOWN;  // integer view format, UNKNOWN = none
  bool int_view_disabled = false;                   // SNORM16 -> FLOAT16 fallback
  bool depth = false;
  uint32_t samples = 1, width = 0, height = 0;
  // Card S3-46b (WWE2K14_D3D_FSR1=1): physical-size copy slots of this scaled 720 pair's full snapshots, used in turn
  // (read only by the FSR 1 present; State::fsr1_slot_gen holds the generation of each slot's latest copy). One slot
  // is not enough: the game presents a snapshot older than the pair's latest one (lane A s46b-fsr: 1 slot -> stale).
  static constexpr size_t kFsr1PhysSlots = 4;
  std::shared_ptr<RenderTexture> fsr1_phys[kFsr1PhysSlots];
  uint8_t fsr1_phys_next = 0;
  // Card S3-31 (WWE2K14_D3D_SCENE_SCALE): callers use logical coordinates (lw x lh); width/height above are the host
  // image's. scale == 1: lw/lh == width/height.
  float scale = 1.0f;
  uint32_t lw = 0, lh = 0;
  uint64_t mem_last_use_present = 0;
  // Card S3-39 (WWE2K14_D3D_SCALE_DYNAMIC=1): the other resolution's resources of a scaled surface (native size) and
  // which set is active (0 = the scaled primary, 1 = native); SetSceneScaleActive swaps them between frames.
  std::unique_ptr<SurfaceObj> alt;
  uint8_t mode = 0;
};
// Card S3-31: one logical -> physical edge rule for every rect of a scaled surface (the full logical extent maps to the
// full image, so whole-image predicates hold).
int32_t PhysEdge(const SurfaceObj& t, int32_t v, bool y) {
  if (t.scale == 1.0f) return v;
  const int32_t L = int32_t(y ? t.lh : t.lw), P = int32_t(y ? t.height : t.width);
  if (v >= L) return P;
  if (v <= 0) return 0;
  return std::min<int32_t>(int32_t(std::lround(double(v) * t.scale)), P);
}
// One pooled command list + its fence. A list may only be re-recorded (begin()) after its fence
// proves the GPU finished it: Kick() waits when it wraps around, Submit() waits for everything.
// State elision (WWE2K14_NR_STATE_ELIDE=1): per-list cache of the graphics state this list last bound,
// so ExecuteDraw can skip redundant binds and share one render pass across consecutive draws to the
// same framebuffer. `fb != nullptr` means "we set this framebuffer and left its render pass open";
// any non-draw recording that ends the pass must call EndDrawPass() so this stays accurate.
struct ElideState {
  RenderFramebuffer* fb = nullptr;  // framebuffer last set on this list (null = none / pass closed)
  RenderPipeline* pipeline = nullptr;
  RenderPipelineLayout* graphics_layout = nullptr;
  RenderDescriptorSet* sets[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  RenderDescriptorSet* check_set4 = nullptr;  // REC_REUSE=2: old always-bind reference, invalidated with this list
  bool has_viewport = false;
  float viewport[6] = {};
  bool has_scissor = false;
  int32_t scissor[4] = {};
};
struct CmdEntry {
  std::unique_ptr<RenderCommandList> list;
  std::unique_ptr<RenderCommandFence> fence;
  bool in_flight = false;
  uint64_t serial = 0;  // submission number of the last executeCommandLists of this list (frame slots wait by it)
  ElideState elide;
  // WWE2K14_NR_GPU_TIMERS=1: timestamp i starts a segment of public-call kind segs[i] (UINT32_MAX = list end).
  std::unique_ptr<RenderQueryPool> qpool;
  std::vector<uint32_t> segs;
  std::vector<std::string> items;  // WWE2K14_NR_GPU_ITEMS=1: labels of the per-item segments (seg id kProfFnCount + index)
  // Card D1 (WWE2K14_NR_GPU_DRAWS=1): own pool; dq = next free query; drecs = intervals / frame boundaries recorded in it;
  // dframes = the frames (ascending) those records belong to (each holds one pending count of its frame until harvest).
  std::unique_ptr<RenderQueryPool> dpool;
  uint32_t dq = 0;
  struct DpRec {
    uint32_t label;  // UINT32_MAX = frame boundary (one query), else an interval (queries q, q+1)
    uint32_t q;
    uint64_t frame;
  };
  std::vector<DpRec> drecs;
  std::vector<uint64_t> dframes;
  // Card P24 (WWE2K14_NR_CONST_FENCE=1): the constants-ring generation this list was recorded in and the lowest ring slot
  // any of its draws reads (written by it, or an older slot re-pushed by the dedupe cache). A list never
  // spans a ring restart (the fenced wrap Kick()s, Submit() re-begins), so one generation per recording suffices.
  uint64_t const_gen = 0;
  uint32_t const_lo = UINT32_MAX;  // UINT32_MAX = reads no slot
};
constexpr uint32_t kGpuTimerCap = 2048;  // timestamps per command list
constexpr uint32_t kCmdListPoolCap = 64;  // bounded ring; grows to this, then wraps with a fence wait

// WWE2K14_NR_DRAW_FAST=1: XXH3-64 of the pipeline key -> {full key bytes, pipeline}. The full key bytes
// confirm the hash on a hit, so a collision can never return the wrong pipeline; a miss or collision
// falls back to the std::map create path below (unchanged).
struct PipelineEntry {
  std::string key;
  RenderPipeline* pipeline = nullptr;
};

// Constants dedupe (WWE2K14_NR_CONST_DEDUPE=1): in ordinary cached CPU memory, the bytes of the last
// vs/ps/shared block actually written to the constants ring, plus the GPU address each was written to.
// A block whose bytes equal this cache AND whose address is still valid (the ring has not been reused)
// can be re-pushed without a new write. `valid` is cleared whenever the ring wraps (Submit() resets
// const_next); Kick() deliberately does not clear it (see Kick()).
struct ConstDedupeBlock {
  bool valid = false;
  uint64_t address = 0;
  uint8_t bytes[4096] = {};  // vs/ps: 4096 used; shared: only the first 1024 used
};

// Frame slots (WWE2K14_NR_FRAME_SLOTS=1, Vulkan + direct present; SVR method: three frames in flight, one fence
// each). Everything the CPU writes for a frame, or must keep alive until the GPU has read it, lives in one of
// kFrameSlots sets; at each directly presented frame the active set is parked with the serial of the frame's last
// submission and the set of the frame kFrameSlots-1 back becomes active after waiting for THAT frame only.
// Submit() (ring full / readback) still waits for everything and resets the active set, as before.
constexpr uint32_t kFrameSlots = 3;
struct Texture;
struct FrameRes {
  std::unique_ptr<RenderBuffer> constants;
  uint8_t* constants_mapped = nullptr;
  uint64_t constants_address = 0;
  uint32_t const_next = 0;
  std::unique_ptr<RenderBuffer> upload_ring;
  uint8_t* upload_ring_mapped = nullptr;
  uint64_t upload_ring_size = 0, upload_ring_offset = 0;
  std::vector<std::unique_ptr<RenderDescriptorSet>> resolve_sets, resolve_depth_sets, detile_sets, resolve_tex_sets,
      f16_lean_sets;
  uint32_t resolve_next = 0, resolve_depth_next = 0, detile_next = 0, resolve_tex_next = 0, f16_lean_next = 0;
  std::vector<std::unique_ptr<RenderDescriptorSet>> depth_img_sets;  // card S3-38
  uint32_t depth_img_next = 0;
  std::vector<std::unique_ptr<RenderDescriptorSet>> fsr1_sets;
  uint32_t fsr1_next = 0;
  std::vector<std::unique_ptr<RenderBuffer>> retired_buffers;
  std::vector<std::unique_ptr<Texture>> retired_textures;
  std::vector<std::shared_ptr<RenderTexture>> retired_snapshots;
  std::vector<uint32_t> pending_free_indices;
  uint64_t serial = 0;  // last submission of the frame that used this set (0 = nothing pending)
};

struct CachedBuf {
  std::unique_ptr<RenderBuffer> buf;
  uint64_t size = 0;
};
struct State {
  std::unique_ptr<RenderInterface> api;
  RenderShaderFormat shader_format = RenderShaderFormat::SPIRV;  // from the chosen interface's capabilities
  // [wwe13 nr] D3D12 only: last layout nr put each texture in (the Vulkan path reads plume's VulkanTexture::textureLayout).
  std::unordered_map<const RenderTexture*, RenderTextureLayout> d3d_layouts;
  uint32_t sampler_table_size = kTableSize;
  bool sampler_overflow_logged = false;
  std::unique_ptr<RenderDevice> device;
  std::unique_ptr<RenderCommandQueue> queue;
  std::vector<CmdEntry> cmd_pool;   // command list ring (see CmdEntry); list[cmd_active] is being recorded
  RenderCommandList* list = nullptr;  // alias into cmd_pool[cmd_active]
  uint32_t cmd_active = 0;
  fix_alt::Scheduler fix_alt_scheduler;
  bool fix_alt_on = false;  // cached WWE2K14_NR_FIX_ALT state for draw hot paths
  std::atomic<bool> kick_hold{false};  // FL-4: while set, early submits are deferred (SetKickHold; the record thread reads it)
  uint32_t draws_since_kick = 0;    // draws recorded into the active list since the last Kick/Submit
  std::unique_ptr<RenderPipelineLayout> layout;
  std::unique_ptr<RenderDescriptorSet> tables[4];
  nr_constants32::AbiPlan abi;  // constants ABI chosen before device creation (default: legacy)
  std::unique_ptr<RenderDescriptorSet> const32_set;  // constants32: set 4 = whole constants ring
  std::unique_ptr<RenderBuffer> constants;
  std::vector<std::unique_ptr<RenderBuffer>> vram_ballast;  // card V1 diagnostic (WWE2K14_NR_VRAM_BALLAST_MB)
  uint8_t* constants_mapped = nullptr;
  uint64_t constants_address = 0;
  std::vector<std::unique_ptr<Texture>> textures;  // handle = index
  uint32_t next_table_index = 1;
  std::vector<uint32_t> free_table_indices;
  std::vector<std::unique_ptr<RenderSampler>> samplers;
  std::map<std::string, uint32_t> sampler_index;  // desc bytes -> table index
  std::vector<SurfaceObj> surfaces;  // handle = index
  std::unordered_map<uint64_t, uint32_t> surface_by_key;
  std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<RenderFramebuffer>> framebuffers;  // (colour, depth)
  // Card S3-52b: images / views / framebuffers of released surfaces, freed once the GPU passed `serial` (ReapSurfaces).
  struct RetiredSurface {
    uint64_t serial = 0;
    uint32_t handle = kNone;
    SurfaceObj obj;
    std::vector<std::unique_ptr<RenderFramebuffer>> fbs;
  };
  std::vector<RetiredSurface> retired_surfaces;
  std::unordered_map<uint64_t, std::unique_ptr<RenderShader>> shaders;
  std::unordered_map<uint64_t, std::unique_ptr<RenderShader>> crowd_vs;  // card U2c: derived SV_InstanceID variants
  std::map<std::string, std::unique_ptr<RenderPipeline>> pipelines;
  std::unordered_map<uint64_t, PipelineEntry> pipeline_hash;  // WWE2K14_NR_DRAW_FAST=1 fast lookup (hash -> key+pipeline)
  std::string pipeline_key_buf;  // reusable key scratch: the hit path never allocates
  bool draw_fast = false;        // WWE2K14_NR_DRAW_FAST=1
  bool state_elide = false;      // WWE2K14_NR_STATE_ELIDE=1: share render passes + skip redundant binds
  bool const_dedupe = false;     // WWE2K14_NR_CONST_DEDUPE=1: skip rewriting an unchanged vs/ps/shared block
  std::atomic<int> rec_reuse{0};          // WWE2K14_NR_REC_REUSE=1: skip repeated set-4 binds; =2: CHECK
  std::atomic<uint32_t> vb_direct_mode{0};  // WWE2K14_NR_VB_DIRECT=1 fast / =2 CHECK, sampled per draw
  bool v17c_ab_on = false;
  int v17c_rec_reuse_mode = 0, v17c_ab_mode = 0;
  uint32_t v17c_vb_direct_mode = 0;
  ConstDedupeBlock dedupe[3];    // 0 = vs (4096), 1 = ps (4096), 2 = shared (1024) -- last written copy + address
  // Resolve-LUT conversion shared by the D3D fixed16 snapshot path.
  std::unique_ptr<RenderPipelineLayout> resolve_layout;
  // Descriptor sets for the fixed16 buffer/image conversions, reused after a wait.
  std::vector<std::unique_ptr<RenderDescriptorSet>> resolve_sets;
  uint32_t resolve_next = 0;
  std::unique_ptr<RenderBuffer> resolve_lut, resolve_rb;
  // D3D depth snapshots use this depth-texture + staging-buffer descriptor layout.
  std::unique_ptr<RenderPipelineLayout> resolve_depth_layout;
  std::vector<std::unique_ptr<RenderDescriptorSet>> resolve_depth_sets;
  uint32_t resolve_depth_next = 0;
  // Card S3-38 (WWE2K14_D3D_DEPTH_IMG=1): depth snapshot written straight into its R32F storage image
  // (depth_to_r32f_img variants), no staging buffer + buffer->image copy.
  std::unique_ptr<RenderPipelineLayout> depth_img_layout;
  std::unique_ptr<RenderShader> depth_img_cs, depth_img_cs_1x;
  std::unique_ptr<RenderPipeline> depth_img_pipeline, depth_img_pipeline_1x;
  std::vector<std::unique_ptr<RenderDescriptorSet>> depth_img_sets;
  uint32_t depth_img_next = 0;
  bool depth_lean_on = false;     // WWE2K14_D3D_DEPTH_LEAN=1: pooled storage-image output
  bool depth_lean_check = false;  // WWE2K14_D3D_DEPTH_LEAN=check: compare 1/16 conversions by readback
  uint64_t depth_lean_check_seq = 0;
  uint64_t depth_lean_check_compared = 0, depth_lean_check_equal = 0, depth_lean_check_different = 0,
           depth_lean_check_failed = 0;
  // Card S3-46 (WWE2K14_D3D_FSR1=1): scaled RGBA8 snapshot EASU, optionally followed by RCAS.
  std::unique_ptr<RenderPipelineLayout> fsr1_layout;
  std::unique_ptr<RenderShader> fsr1_easu_cs, fsr1_rcas_cs;
  std::unique_ptr<RenderPipeline> fsr1_easu_pipeline, fsr1_rcas_pipeline;
  std::unique_ptr<RenderSampler> fsr1_sampler;
  std::vector<std::unique_ptr<RenderDescriptorSet>> fsr1_sets;
  uint32_t fsr1_next = 0;
  bool fsr1_on = false;
  float fsr1_sharpness = 0.25f;
  // Card S3-46b: pooled FSR 1 images and their counters ([d3d-fsr1] line every kFsr1LogEvery presents).
  std::unordered_map<const RenderTexture*, uint64_t> fsr1_slot_gen;  // phys slot -> generation of its latest copy
  uint64_t fsr1_gen_next = 0;
  std::vector<std::shared_ptr<RenderTexture>> fsr1_ring;  // output images (one size at a time)
  uint32_t fsr1_ring_w = 0, fsr1_ring_h = 0;
  size_t fsr1_ring_next = 0;
  struct Fsr1Stats {
    uint64_t presents = 0, upscaled = 0, stale = 0, failed = 0, keep_copies = 0, phys_allocs = 0, out_allocs = 0,
             out_reuse = 0;
  } fsr1_stats;
  // D3D depth surface -> R32F values for a k_24_8_FLOAT fetch (depth_to_r32f variants); uses the resolve_depth layout
  // and descriptor sets.
  std::unique_ptr<RenderShader> depth_r32f_cs, depth_r32f_cs_1x;
  std::unique_ptr<RenderPipeline> depth_r32f_pipeline, depth_r32f_pipeline_1x;
  // D3D fixed16 resolve -> linear UNORM texels (resolve_color_linear variant); uses
  // the resolve layout and descriptor sets, its own source buffer.
  std::unique_ptr<RenderShader> f16_linear_cs;
  std::unique_ptr<RenderPipeline> f16_linear_pipeline;
  std::unique_ptr<RenderBuffer> f16_src;
  uint64_t f16_src_size = 0;
  // Fixed16 fused conversion reads the source image directly (resolve_color_tex_linear variant).
  std::vector<std::unique_ptr<RenderDescriptorSet>> resolve_tex_sets;
  uint32_t resolve_tex_next = 0;
  // Card S3-23 (WWE2K14_D3D_F16_FUSED=1): fixed16 snapshot conversion reading the source image directly
  // (resolve_color_tex_linear: SRC_TEXTURE + LINEAR_OUT), no image -> buffer copy first.
  std::unique_ptr<RenderPipelineLayout> f16_tex_layout;
  std::unique_ptr<RenderShader> f16_tex_cs;
  std::unique_ptr<RenderPipeline> f16_tex_pipeline;
  // G4 (WWE2K14_D3D_F16_LEAN): single graphics pass into a pooled UNORM snapshot image.
  int f16_lean_mode = 0;  // 0 off, 1 fast, 2 sampled legacy-vs-fast CHECK
  std::unique_ptr<RenderPipelineLayout> f16_lean_layout;
  std::unique_ptr<RenderShader> f16_lean_vs, f16_lean_ps;
  std::unique_ptr<RenderPipeline> f16_lean_rg_pipeline, f16_lean_rgba_pipeline;
  std::vector<std::unique_ptr<RenderDescriptorSet>> f16_lean_sets;
  uint32_t f16_lean_next = 0;
  uint64_t f16_lean_check_ordinal = 0;
  // Fixed16/depth resolve shader bindings retain a no-slot table; D3D destination detile owns its own input page table.
  std::unique_ptr<RenderBuffer> mirror_pt;
  std::vector<uint32_t> pt_cpu;
  bool pt_dirty = false;
  // Card S3-69 B2-perf2: isolated scratch input/page-table/output for destination-canvas GPU detiles. Do not alias the
  // frontend mirror: it can contain GPU-resident copy-outs that are not present in guest physical memory.
  std::unique_ptr<RenderBuffer> dest_seed_pool, dest_seed_pt, dest_seed_out;
  uint64_t dest_seed_pool_size = 0, dest_seed_out_size = 0;
  std::unique_ptr<RenderShader> detile_cs;
  std::unique_ptr<RenderPipelineLayout> detile_layout;
  std::unique_ptr<RenderPipeline> detile_pipeline;
  std::vector<std::unique_ptr<RenderDescriptorSet>> detile_sets;  // one per dispatch, reused after a wait
  uint32_t detile_next = 0;
  std::string shader_dir;  // set by shared texture-detile initialization; used by DetileDepth24Available()
  // Batch state, reset by Flush() after the GPU wait.
  uint32_t const_next = 0;
  std::vector<std::unique_ptr<RenderBuffer>> retired_buffers;
  // Upload ring (WWE2K14_NR_UPLOAD_RING=1): one persistent, persistently-mapped buffer used as both
  // vertex and index buffer; per-draw uploads sub-allocate from it. The offset resets in Submit()
  // after WaitAll(); Kick() must not touch it (kicked/pending lists may still read earlier bytes).
  std::unordered_map<uint32_t, CachedBuf> vb_cache;  // D3D VBC ids -> immutable UPLOAD buffer (exec thread only)
  std::unique_ptr<RenderBuffer> upload_ring;
  uint8_t* upload_ring_mapped = nullptr;
  uint64_t upload_ring_size = 0;
  uint64_t upload_ring_offset = 0;
  std::vector<std::unique_ptr<Texture>> retired_textures;
  // Resolve reuse (WWE2K14_NR_RESOLVE_REUSE=1): GPU copies of resolved render-target regions, by snapshot id.
  std::vector<std::shared_ptr<RenderTexture>> snapshots;
  std::vector<uint32_t> free_snapshots;
  // Card S3-50: snapshot ids / texture handles may be reserved by the caller thread while the record thread runs queued
  // jobs (WWE2K14_NR_RECORD_THREAD=3). snap_next = next never-used id (== snapshots.size() when nothing is reserved);
  // free_snapshots and snap_next are guarded by snap_alloc_mu. Texture handles are never reused (tex_next only grows).
  uint32_t snap_next = 0;
  std::mutex snap_alloc_mu;
  std::atomic<uint32_t> tex_next{0};
  std::vector<RenderFormat> snapshot_formats;  // host format of each snapshot id (same indexing as snapshots)
  std::vector<uint64_t> snapshot_last_use_present;  // opt-in memory-owner idle age; indexed with snapshots
  // Physical-size image for a scaled snapshot (same indexing as snapshots; null image = none). Whole-image readers
  // (direct present, TextureFromSnapshot) use it. U7 may make it the primary image and defer the logical-size copy
  // until a crop/readback/mip/cube/game-memory consumer asks for it.
  struct SnapHi {
    std::shared_ptr<RenderTexture> image;
    uint32_t width = 0, height = 0;
    bool upscale = false;  // card S3-46: physical copy of a BELOW-native surface, read only by the FSR 1 present
    uint64_t fsr1_gen = 0;  // card S3-46b: generation of that copy in its surface's slot (State::fsr1_slot_gen)
    uint32_t logical_width = 0, logical_height = 0;
    bool logical_pending = false;  // U7: snapshots[id] aliases image until a logical-size consumer asks for it
  };
  std::vector<SnapHi> snapshot_hi;
  std::vector<std::shared_ptr<RenderTexture>> retired_snapshots;  // released ids; pending commands may read them
  // Card L2-B (WWE2K14_NR_SNAP_POOL=1): images of retired snapshots, recycled by (w,h,format) after the wait-gated clear.
  struct SnapKey {
    uint32_t w = 0, h = 0;
    RenderFormat f = RenderFormat::UNKNOWN;
    bool storage = false;
    bool operator==(const SnapKey& o) const { return w == o.w && h == o.h && f == o.f && storage == o.storage; }
  };
  struct SnapPoolEntry {
    SnapKey k;
    std::shared_ptr<RenderTexture> img;
    uint64_t bytes = 0;
    uint64_t mem_last_use_present = 0;
  };
  std::vector<SnapPoolEntry> snap_pool;  // oldest first
  uint64_t snap_pool_bytes = 0;
  std::unordered_map<const RenderTexture*, SnapKey> snap_keys;  // images SnapshotColor created with the pool on
  struct F16PoolEntry {
    SnapKey k;
    std::shared_ptr<RenderTexture> img;
    uint64_t bytes = 0;
    uint64_t mem_last_use_present = 0;
  };
  std::vector<F16PoolEntry> f16_lean_pool;  // retired fixed16 images, reusable by (w,h,format)
  uint64_t f16_lean_pool_bytes = 0;
  std::unordered_map<const RenderTexture*, SnapKey> f16_lean_keys;
  std::unordered_map<const RenderTexture*, std::unique_ptr<RenderFramebuffer>> f16_lean_fbs;
  std::mutex snap_mu;
  // Direct GPU presentation: the emulator's device is adopted (SetSharedVulkanDevice); views handed to the presenter
  // with their images, oldest first, kept for kPresentHold presents.
  bool shared_device = false;
  struct PresentView {
    std::shared_ptr<RenderTexture> image;
    std::unique_ptr<RenderTextureView> view;
  };
  std::vector<PresentView> present_views;
  std::vector<uint32_t> pending_free_indices;
  std::string device_name;
  bool gpu_items = false;            // WWE2K14_NR_GPU_ITEMS=1 (with GPU_TIMERS): GPU time per resolve / texture build / seed-detile item
  bool gpu_timers = false;           // WWE2K14_NR_GPU_TIMERS=1 (and profiling on)
  bool gpu_draws = false;            // card D1, WWE2K14_NR_GPU_DRAWS=1 (with GPU_TIMERS): timestamp pair per draw / op, per-frame summary
  bool gpu_draws_keys = false;       // card D1, WWE2K14_NR_GPU_DRAWS=2: also one [nr-gpu-key] line per key per frame (replays)
  bool gpu_shader_top = false;        // bounded effective PS+VS top-30 summary every 120 frames
  bool gpu_passes = false;           // WWE2K14_NR_GPU_PASSES=1 (with GPU_TIMERS): GPU ms per render pass of the draws (label: size/format/draw count)
  int pass_idx = -1;                 // index into CmdEntry::items of the open per-pass segment, -1 = none
  uint32_t pass_draws = 0;           // draws seen in the open per-pass segment
  uint32_t gpu_cur = UINT32_MAX;     // kind of the open timer segment in the active list
  bool frame_slots = false;          // WWE2K14_NR_FRAME_SLOTS=1 (see FrameRes)
  // Card S3-6 (draw-off only, SetRetireByFrame): without frame slots, retired buffers/textures/snapshots were freed only in
  // Submit() (WaitAll), which the D3D-fed renderer reaches rarely - GBs of retired resolve snapshots piled up (UHD 630 out
  // of device memory). FrameEnd moves them here with the frame's submit serial and frees them after that serial's wait,
  // kRetireFrames frames later.
  struct RetireBatch {
    uint64_t serial = 0;
    std::vector<std::unique_ptr<RenderBuffer>> buffers;
    std::vector<std::unique_ptr<Texture>> textures;
    std::vector<std::shared_ptr<RenderTexture>> snapshots;
    std::vector<uint32_t> table_indices;  // card S3-34: bindless slots of the textures above, reusable with them
  };
  bool retire_by_frame = false;
  std::vector<RetireBatch> retire_q;  // oldest first
  FrameRes parked[kFrameSlots];      // sets of the non-active slots (parked[frame_slot] is an empty placeholder)
  uint32_t frame_slot = 0;
  uint64_t submit_serial = 0;
  bool mem_owners_on = false;
  uint64_t mem_present = 0, mem_ring_last_use_present = 0;
  // GAP-mem1 bounded diagnostic (WWE2K14_NR_MEM_OWNERS=1): upload ring high-water since the last line, ring slots taken,
  // new images, constants-ring fence waits.
  uint64_t mem_upload_hw = 0, mem_const_taken = 0, mem_new_tex = 0, mem_const_waits = 0;
  // Card P24: constants ring wrap. const_slots = effective ring size (kConstSlots; WWE2K14_NR_CONST_SLOTS=N, test only,
  // makes it smaller to force wraps). With WWE2K14_NR_CONST_FENCE=1 the wrap does not Submit(): the active list is
  // kicked, the generation advances, and slot s of the new generation is written only after every older-generation list
  // whose const_lo <= s has been waited for (const_safe = first slot not yet proven free). The other per-batch resources
  // that Submit() would reset (retired buffers/textures/snapshots, pending table indices, upload ring offset, resolve /
  // detile descriptor rings) are swapped with fence_spare, the set parked at the previous wrap, which is
  // reused only after every list up to its serial finished (WaitSerial; normally long done).
  uint32_t const_slots = kConstSlots;
  bool const_fence = false;
  uint64_t const_gen = 0;
  uint32_t const_safe = kConstSlots;
  FrameRes fence_spare;
};
State* g = nullptr;
struct F16LeanCheckStats {
  uint64_t sampled = 0, compared = 0, equal = 0, different = 0, readback_failed = 0;
  bool has_first_difference = false;
  uint64_t first_sample = 0;
  uint32_t first_x = 0, first_y = 0, first_channel = 0;
  uint16_t first_legacy = 0, first_lean = 0;
} g_f16_lean_check;
// Card S3-52: a surface handle whose images still exist (ReleaseSurface leaves an empty entry; handles are not reused).
inline bool LiveSurface(uint32_t s) { return s < g->surfaces.size() && g->surfaces[s].image != nullptr; }
inline void TouchSnapshot(uint32_t id) {
  if (!g || !g->mem_owners_on || id >= g->snapshot_last_use_present.size()) return;
  g->snapshot_last_use_present[id] = g->mem_present;
}
inline void TouchSurface(uint32_t surface) {
  if (!g || !g->mem_owners_on || surface >= g->surfaces.size()) return;
  SurfaceObj& s = g->surfaces[surface];
  s.mem_last_use_present = g->mem_present;
  if (s.alt) s.alt->mem_last_use_present = g->mem_present;
}
inline void TrackTextureSnapshot(Texture* texture, uint32_t id) {
  if (!g || !g->mem_owners_on || !texture || texture->mem_snapshot_count >= 16) return;
  texture->mem_snapshot_ids[texture->mem_snapshot_count++] = id;
}
inline void TouchTexture(Texture* texture) {
  if (!g || !g->mem_owners_on || !texture) return;
  texture->mem_last_use_present = g->mem_present;
  for (uint32_t i = 0; i < texture->mem_snapshot_count; ++i) TouchSnapshot(texture->mem_snapshot_ids[i]);
}

// M4 instrument I1 (nr_backend.h Stats): off unless SetProfiling(true).
bool g_prof = false;
bool g_phys_snap_on = false;  // U7: WWE2K14_NR_PHYS_SNAP=1, default ON (env "0" turns off)
bool g_phys_snap_check = false;  // U7: WWE2K14_NR_PHYS_SNAP_CHECK=1, default off
// Diagnostic (WWE2K14_NR_DEPTH_DIAG=1, default off): per-depth-resolve evidence table, see DdLog.
const bool g_ddiag = [] { const char* v = std::getenv("WWE2K14_NR_DEPTH_DIAG"); return v && *v == '1'; }();
struct DdSurf { uint64_t clear = 0, draw = 0, draws_since_clear = 0, clears = 0, draws = 0; };
std::unordered_map<uint32_t, DdSurf> g_dd_s;
uint64_t g_dd_seq = 0;
uint32_t g_dd_cur_surface = 0xFFFFFFFFu;
struct DdRes { uint64_t seq; uint32_t surface; float mn, mx; uint32_t nz, n; };
std::unordered_map<const void*, DdRes> g_dd_img;
Stats g_stats;
bool g_batch_stats = false;
bool g_batch_check = false;
uint64_t NowNs();
// Per thread: the record thread (WWE2K14_NR_RECORD_THREAD=1) runs its own outermost ExecuteDraw scope.
thread_local int g_prof_depth = 0;
thread_local bool t_on_record_thread = false;
// Card S3-50: id / handle reserved by the caller for the queued job running on this thread (kNone = none).
thread_local uint32_t t_reserved_snap = kNone;
thread_local uint32_t t_reserved_tex = kNone;
thread_local uint32_t t_reserved_surface = kNone;
// Waits until the record thread has recorded every submitted draw (no-op on the record thread itself and when the
// switch is off). Called first by every public entry point except the draw submission. `fn` = the call, for the stats.
void SyncRecordThread(uint32_t fn, bool tail = false, BatchDrainKind kind = kBatchDrainValueCall);
void StartRecordThread();
constexpr uint32_t kRecordSlotsMax = 512;  // storage; slots in flight = RecordSlotsFor(frame), default 128 (today)
RecordQueue<Draw, kRecordSlotsMax>* g_rq = nullptr;  // null = draws are recorded inline by the renderer thread (default)
std::atomic<RecordQueue<Draw, kRecordSlotsMax>*> g_rq_watchdog{nullptr};  // published once for the external capture watchdog
std::atomic<uint64_t> g_record_cpu_previous_ns{0};
uint32_t g_batch_n = 0;  // WWE2K14_NR_RECORD_BATCH; 0 keeps the existing queue protocol
bool RecordCpuDiagnosticOn() {
  static const bool on = [] {
    const char* value = std::getenv("WWE2K14_NR_RECORD_CPU");
    return value && std::strcmp(value, "1") == 0;
  }();
  return on;
}
uint64_t ReadRecordThreadCpuNs() {
  if (!g_rq) return 0;
#ifdef _WIN32
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!GetThreadTimes(g_rq->worker_native_handle(), &creation, &exit, &kernel, &user)) return 0;
  auto to100ns = [](FILETIME value) {
    return (uint64_t(value.dwHighDateTime) << 32) | uint64_t(value.dwLowDateTime);
  };
  return (to100ns(kernel) + to100ns(user)) * 100;
#elif defined(__linux__)
  clockid_t clock_id{};
  if (pthread_getcpuclockid(g_rq->worker_native_handle(), &clock_id) != 0) return 0;
  timespec ts{};
  if (clock_gettime(clock_id, &ts) != 0) return 0;
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#else
  return 0;
#endif
}
constexpr const char* kBatchDrainNames[kBatchDrainKindCount] = {
    "public_surface", "public_clear", "public_create_texture", "public_release_texture", "public_draw",
    "public_read_color", "public_resolve", "public_detile", "value_call", "surface_release", "surface_alt",
    "texture_order", "read_snapshot", "read_texture", "snapshot_table", "present", "resolve_lut", "kick",
    "finish", "detile", "frame_tail", "ring_full"};
void CountBatchDrain(BatchDrainKind kind, bool waited, uint64_t ns) {
  if (!g_batch_stats || uint32_t(kind) >= kBatchDrainKindCount) return;
  ++g_stats.batch_drain_calls[kind];
  if (waited) {
    ++g_stats.batch_drain_waits[kind];
    g_stats.batch_drain_ns[kind] += ns;
  }
}
constexpr uint64_t kBatchStreamSeed = 0x9E3779B185EBCA87ull;
uint64_t g_batch_producer_stream = kBatchStreamSeed, g_batch_consumer_stream = kBatchStreamSeed;
uint64_t g_batch_producer_ops = 0, g_batch_consumer_ops = 0;
uint64_t g_batch_checked_frames = 0, g_batch_check_mismatches = 0;
std::atomic<uint64_t> g_const_used_copied{0}, g_const_used_cleared{0}, g_const_used_mismatches{0};
std::atomic<uint64_t> g_const_used_batch_differences{0}, g_const_used_crowd_differences{0};
std::atomic<uint32_t> g_record_consumer_phase{0}, g_record_consumer_ordinal{0}, g_record_consumer_item{0};
std::atomic<uint64_t> g_record_consumer_job_hash{0}, g_record_consumer_wait_serial{0};
std::atomic<uint32_t> g_record_consumer_wait{0};  // 0 none, 1 Plume queue submit, 2 command-fence wait
std::atomic<const char*> g_record_consumer_operation{nullptr};
std::atomic<const char*> g_record_consumer_wait_name{nullptr};
uint64_t BatchMixWords(uint64_t h, std::initializer_list<uint64_t> values) {
  for (uint64_t v : values) h = XXH3_64bits_withSeed(&v, sizeof(v), h);
  return h;
}
uint64_t BatchHashWords(const char* tag, std::initializer_list<uint64_t> values, bool force = false) {
  if (!g_batch_check && !force) return 0;
  return BatchMixWords(XXH3_64bits(tag, std::strlen(tag)), values);
}
uint64_t BatchHashBytes(uint64_t h, const void* data, size_t size, bool force = false) {
  if (!g_batch_check && !force) return 0;
  h = BatchMixWords(h, {uint64_t(size)});
  if (size && data) h = XXH3_64bits_withSeed(data, size, h);
  return h;
}
uint32_t BatchFloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
uint64_t BatchHashTextureDesc(uint64_t h, const TextureDesc& d) {
  if (!g_batch_check) return 0;
  h = BatchMixWords(h, {d.width, d.height, d.levels, uint32_t(d.format), uint32_t(d.cube)});
  return BatchHashBytes(h, d.swizzle, sizeof(d.swizzle));
}
uint64_t BatchHashLevels(uint64_t h, const std::vector<LevelData>& levels) {
  if (!g_batch_check) return 0;
  h = BatchMixWords(h, {uint64_t(levels.size())});
  for (const LevelData& l : levels) {
    h = BatchMixWords(h, {l.width, l.height, l.mip, l.face});
    h = BatchHashBytes(h, l.bytes.data(), l.bytes.size());
  }
  return h;
}
uint64_t BatchHashSampler(uint64_t h, const SamplerDesc& s) {
  h = BatchMixWords(h, {uint64_t(s.mag_linear), uint64_t(s.min_linear), uint64_t(s.mip_linear), s.address_u,
                        s.address_v, s.address_w, s.border, s.max_anisotropy, BatchFloatBits(s.min_lod),
                        BatchFloatBits(s.max_lod), BatchFloatBits(s.lod_bias)});
  return h;
}
uint64_t BatchHashResolveParams(const ResolveParams& p) {
  return BatchHashWords("resolve_params", {p.w, p.h, p.src_bpp, p.bpp_log2, p.flags, p.endian, p.dx, p.dy,
                                           p.pitch_aligned, uint32_t(p.base_offset), p.dst_size, p.x0, p.y0,
                                           p.kbase, p.kpitch32, p.select, p.ms, p.dst_start});
}
uint64_t BatchHashDetileParams(const DetileParams& p) {
  return BatchHashWords("detile_params", {p.src_base, p.row_pitch, p.pitch_blocks, p.off_x, p.off_y, p.bw, p.bh,
                                           p.bpb_log2, p.tiled, p.endian, p.mode, p.out_offset, p.out_size});
}
uint64_t BatchHashDraw(const Draw& d, bool full_constants = false, bool force = false) {
  if (!g_batch_check && !force) return 0;
  uint64_t h = BatchHashWords("draw", {d.color, d.depth, d.vs_key, d.ps_key, d.spec_constants,
                                       uint64_t(d.sample_shading), d.topology, d.cull, uint64_t(d.front_ccw),
                                       uint64_t(d.depth_clamp), uint64_t(d.Attributes().size()),
                                       uint64_t(d.Buffers().size()), uint64_t(d.index32), d.count,
                                       uint64_t(d.Textures().size()), uint64_t(d.depth_test), uint64_t(d.depth_write),
                                       d.depth_compare, uint64_t(d.stencil), d.stencil_ref, d.stencil_read,
                                       d.stencil_write, d.ordinal, d.index_cache_id, d.index_cache_bytes,
                                       uint64_t(d.index_cache_upload), uint64_t(d.cache_flush)}, force);
  for (const VertexAttribute& a : d.Attributes())
    h = BatchMixWords(h, {a.location, uint32_t(a.format), a.binding, a.offset_bytes});
  for (const VertexBuffer& b : d.Buffers()) {
    h = BatchMixWords(h, {b.stride_bytes, b.prefix_bytes, b.cache_id, b.cache_size, uint64_t(b.cache_upload)});
    h = BatchHashBytes(h, b.data.data(), b.data.size(), force);
  }
  h = BatchHashBytes(h, d.Indices().data(), d.Indices().size(), force);
  const uint8_t* vs = full_constants && d.full_float_constants ? d.full_float_constants->vs : d.vs_constants;
  const uint8_t* ps = full_constants && d.full_float_constants ? d.full_float_constants->ps : d.ps_constants;
  h = BatchHashBytes(h, vs, sizeof(d.vs_constants), force);
  h = BatchHashBytes(h, ps, sizeof(d.ps_constants), force);
  h = BatchHashBytes(h, d.shared, sizeof(d.shared), force);
  for (const TextureBinding& t : d.Textures()) {
    h = BatchMixWords(h, {t.slot, t.dimension, t.texture});
    h = BatchHashSampler(h, t.sampler);
  }
  h = BatchHashBytes(h, d.viewport, sizeof(d.viewport), force);
  h = BatchHashBytes(h, d.scissor, sizeof(d.scissor), force);
  h = BatchMixWords(h, {uint64_t(d.stencil_front.fail), d.stencil_front.depth_fail, d.stencil_front.pass,
                        d.stencil_front.compare, uint64_t(d.stencil_back.fail), d.stencil_back.depth_fail,
                        d.stencil_back.pass, d.stencil_back.compare, uint64_t(d.blend.enabled), d.blend.src,
                        d.blend.dst, d.blend.op, d.blend.src_a, d.blend.dst_a, d.blend.op_a, d.blend.write_mask});
  h = BatchHashBytes(h, d.blend_constants, sizeof(d.blend_constants), force);
  return h;
}
void BatchStreamEvent(bool producer, uint64_t op_hash) {
  if (!g_batch_check) return;
  if (producer) {
    g_batch_producer_stream = XXH3_64bits_withSeed(&op_hash, sizeof(op_hash), g_batch_producer_stream);
    ++g_batch_producer_ops;
  } else {
    g_batch_consumer_stream = XXH3_64bits_withSeed(&op_hash, sizeof(op_hash), g_batch_consumer_stream);
    ++g_batch_consumer_ops;
  }
}
uint64_t BatchReleaseHash(uint32_t op) {
  return BatchHashWords((op >> 30) == 1 ? "release_texture" : "release_snapshot", {op & 0x3FFFFFFFu});
}
void BatchStreamProducerSlot(const Draw& d) {
  if (d.const_used_mode == 2 && d.full_float_constants) {
    const uint64_t used_hash = BatchHashDraw(d, false, true);
    const uint64_t full_hash = BatchHashDraw(d, true, true);
    if (used_hash != full_hash) g_const_used_batch_differences.fetch_add(1, std::memory_order_relaxed);
  }
  if (!g_batch_check) return;
  for (uint32_t op : d.pre_ops) BatchStreamEvent(true, BatchReleaseHash(op));
  for (uint64_t hash : d.pre_job_hashes) BatchStreamEvent(true, hash);
  if (!d.jobs_only) BatchStreamEvent(true, BatchHashDraw(d));
}
Draw* AcquireRecordSlot(bool draw_handoff = false) {
  if (!g_rq) return nullptr;
  const bool pd = draw_profile::On();
  if (!pd && (!g_batch_stats || !g_batch_n)) return g_rq->Acquire();
  const uint64_t before = g_rq->stats().full_waits;
  const uint64_t wait_before = g_rq->stats().full_wait_ns;
  const uint64_t t0 = NowNs();
  Draw* slot = g_rq->Acquire();
  const uint64_t dt = NowNs() - t0;
  if (g_rq->stats().full_waits != before) {
    if (g_batch_stats && g_batch_n) CountBatchDrain(kBatchDrainRingFull, true, dt);
    if (pd) {
      draw_profile::producer.worker_ns[draw_profile::kHandoffWait] += g_rq->stats().full_wait_ns - wait_before;
      draw_profile::producer.worker_n[draw_profile::kHandoffWait] += g_rq->stats().full_waits - before;
      if (draw_handoff) {
        draw_profile::producer.worker_ns[draw_profile::kSubmitFullWait] += g_rq->stats().full_wait_ns - wait_before;
        draw_profile::producer.worker_n[draw_profile::kSubmitFullWait] += g_rq->stats().full_waits - before;
      }
    }
  }
  if (pd) {
    draw_profile::producer.worker_ns[draw_profile::kAcquireSlot] += dt;
    ++draw_profile::producer.worker_n[draw_profile::kAcquireSlot];
    if (draw_handoff) {
      draw_profile::producer.worker_ns[draw_profile::kSubmitAcquire] += dt;
      ++draw_profile::producer.worker_n[draw_profile::kSubmitAcquire];
    }
  }
  return slot;
}

struct SurfaceReservation {
  uint32_t handle = kNone, width = 0, height = 0;
  std::atomic<bool> done{false};
  std::atomic<bool> has_alt{false};
};
std::unordered_map<uint64_t, std::shared_ptr<SurfaceReservation>> g_surface_reservations;  // producer-side only
std::unordered_map<uint32_t, std::shared_ptr<SurfaceReservation>> g_surface_by_handle;  // producer-side only
std::unordered_set<uint64_t> g_surface_failure_logged;  // record-thread side: one diagnostic per failed target key
std::mutex g_surface_warning_mutex;
std::vector<std::string> g_surface_warnings;
void QueueSurfaceWarning(const std::string& line) {
  std::lock_guard<std::mutex> lock(g_surface_warning_mutex);
  if (g_surface_warnings.size() < 64) g_surface_warnings.push_back(line);
}
std::atomic<uint32_t> g_surface_next{0};
// Card S3-50 (WWE2K14_NR_RECORD_THREAD=3): snapshot / texture / clear / release calls do not wait for the record
// thread; they reserve their id on the caller and queue the call itself (g_pending_jobs, handed over with the next draw
// or run by the next waiting call). Only calls that return data the caller reads now still wait (SyncRecordThread).
std::vector<uint32_t> g_pending_ops;  // renderer thread only: releases not yet handed to the record thread
bool g_async = false;
std::vector<std::function<void()>> g_pending_jobs;  // caller side (serialized by the frontend's backend lock)
std::vector<uint64_t> g_pending_job_hashes;  // batch-check sidecar, kept in the same order as g_pending_jobs
std::vector<const char*> g_pending_job_names;  // stable diagnostic labels, kept in the same order as g_pending_jobs
bool g_slot_held = false;                           // caller holds an acquired, unsubmitted draw slot
thread_local int t_backend_depth = 0;               // public backend calls open on this thread (ProfScope)
bool ResolveCensusOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_RESOLVE_CENSUS");
    return e && std::strcmp(e, "1") == 0;
  }();
  return on;
}
struct ResolveCensusKeyHash {
  size_t operator()(const ResolveCensusKey& k) const {
    uint64_t h = (uint64_t(k.dest_base) << 32) | k.dest_format;
    h ^= (uint64_t(k.width) << 32) | k.height;
    h = (h ^ (h >> 29)) * 0x9E3779B185EBCA87ull;
    h ^= (uint64_t(k.resolve_class) << 32) | k.source_handle;
    h = (h ^ (h >> 31)) * 0xC2B2AE3D27D4EB4Full;
    h ^= k.source_target + (uint64_t(k.msaa) << 56);
    return size_t(h ^ (h >> 32));
  }
};
struct ResolveCensusKeyEqual {
  bool operator()(const ResolveCensusKey& a, const ResolveCensusKey& b) const {
    return a.dest_base == b.dest_base && a.dest_format == b.dest_format && a.width == b.width && a.height == b.height &&
           a.resolve_class == b.resolve_class && a.source_handle == b.source_handle && a.source_target == b.source_target &&
           a.msaa == b.msaa;
  }
};
std::mutex g_resolve_census_mutex;
std::unordered_map<ResolveCensusKey, ResolveCensusRow, ResolveCensusKeyHash, ResolveCensusKeyEqual> g_resolve_census_pending;
ResolveCensusCounts g_resolve_census_pending_total{};
struct ActiveResolveCensus { ResolveCensusKey key; uint64_t consumers = 0; };
std::unordered_map<uint32_t, ActiveResolveCensus> g_resolve_census_active;
std::map<uint64_t, ResolveCensusFrame> g_resolve_census_frames;
uint64_t g_resolve_census_current_frame = 0;
thread_local ResolveCensusKey t_resolve_census_key{};
thread_local bool t_resolve_census_active = false, t_resolve_census_draw = false;
thread_local uint64_t t_resolve_census_draw_frame = 0;
thread_local std::vector<uint32_t> t_resolve_census_draw_destinations;
ResolveCensusRow& CensusPendingRow(const ResolveCensusKey& key) {
  ResolveCensusRow& row = g_resolve_census_pending[key];
  row.key = key;
  return row;
}
void CensusFrameEvent(uint64_t frame, uint64_t ResolveCensusFrame::*field, uint64_t count = 1) {
  if (!frame) return;
  ResolveCensusFrame& f = g_resolve_census_frames[frame];
  f.*field += count;
  while (g_resolve_census_frames.size() > 8) g_resolve_census_frames.erase(g_resolve_census_frames.begin());
}
uint64_t CensusEventFrame() {
  return t_resolve_census_active && t_resolve_census_key.valid ? t_resolve_census_key.frame : g_resolve_census_current_frame;
}
void CensusGlobal(uint64_t ResolveCensusCounts::*field, uint64_t count = 1) {
  g_resolve_census_pending_total.*field += count;
}
template <uint64_t ResolveCensusCounts::*Field>
void CensusCount(uint64_t count = 1) {
  if (!ResolveCensusOn() || !t_resolve_census_active || !t_resolve_census_key.valid || !count) return;
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  CensusPendingRow(t_resolve_census_key).counts.*Field += count;
}
void CensusRecordCopy(uint64_t bytes, bool blit, uint32_t source_line = 0) {
  if (!ResolveCensusOn()) return;
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  ResolveCensusCounts& all = g_resolve_census_pending_total;
  if (blit) all.blits++;
  else all.copies++;
  all.bytes_copied += bytes;
  if (t_resolve_census_active && t_resolve_census_key.valid) {
    ResolveCensusCounts& c = CensusPendingRow(t_resolve_census_key).counts;
    if (blit) ++c.blits;
    else {
      ++c.copies;
      ResolveCensusSiteCounts& site = c.copy_sites[source_line];
      ++site.calls;
      site.bytes += bytes;
    }
    c.bytes_copied += bytes;
  }
  CensusFrameEvent(CensusEventFrame(), blit ? &ResolveCensusFrame::blits : &ResolveCensusFrame::copies);
}
void AsyncResolveCensusJob(std::function<void()> job, const ResolveCensusKey& key, bool active) {
  if (!active) {
    job();
    return;
  }
  ResolveCensusScope scope(key);
  job();
}
void AsyncFail(const char* what, const std::string& error);  // card S3-50 (defined with the record thread)
// True when this public call may be queued instead of waiting: async mode, on the caller thread, outermost call.
inline bool AsyncCall() { return (g_async || g_batch_n) && g_rq && !t_on_record_thread && t_backend_depth == 0; }
inline bool BatchCall() { return g_batch_n && g_rq && !t_on_record_thread && t_backend_depth == 0; }
// Queues a call behind everything queued before it. A run of calls with no draw in between is handed over in a
// slot of its own (when the caller holds no unsubmitted draw slot) so the record thread starts on it early.
void EnqueueJob(std::function<void()> job, uint64_t op_hash, const char* name) {
  const bool census_active = ResolveCensusOn() && t_resolve_census_active;
  const ResolveCensusKey census_key = t_resolve_census_key;
  if (census_active) {
    g_pending_jobs.push_back([job = std::move(job), census_key]() mutable {
      AsyncResolveCensusJob(std::move(job), census_key, true);
    });
  } else {
    g_pending_jobs.push_back(std::move(job));
  }
  g_pending_job_hashes.push_back(op_hash);
  g_pending_job_names.push_back(name);
  if ((g_slot_held && !g_batch_n) || g_pending_jobs.size() < 8) return;
  Draw* d = AcquireRecordSlot();
  d->Reset();
  d->jobs_only = true;
  d->pre_ops.swap(g_pending_ops);
  g_pending_ops.clear();
  d->pre_jobs.swap(g_pending_jobs);
  d->pre_job_hashes.swap(g_pending_job_hashes);
  d->pre_job_names.swap(g_pending_job_names);
  g_pending_jobs.clear();
  g_pending_job_hashes.clear();
  g_pending_job_names.clear();
  BatchStreamProducerSlot(*d);
  if (g_batch_n) g_rq->SubmitBatch(false, g_batch_n);
  else g_rq->Submit();
}
void FlushPendingRecordOps() {
  if (!g_rq || (g_pending_ops.empty() && g_pending_jobs.empty())) return;
  Draw* d = AcquireRecordSlot();
  d->Reset();
  d->jobs_only = true;
  d->pre_ops.swap(g_pending_ops);
  g_pending_ops.clear();
  d->pre_jobs.swap(g_pending_jobs);
  d->pre_job_hashes.swap(g_pending_job_hashes);
  d->pre_job_names.swap(g_pending_job_names);
  g_pending_jobs.clear();
  g_pending_job_hashes.clear();
  g_pending_job_names.clear();
  BatchStreamProducerSlot(*d);
  if (g_batch_n) g_rq->SubmitBatch(false, g_batch_n);
  else g_rq->Submit();
}
template <class F>
auto QueueSyncCall(BatchDrainKind kind, uint64_t op_hash, F&& body) -> std::invoke_result_t<std::decay_t<F>> {
  using R = std::invoke_result_t<std::decay_t<F>>;
  if constexpr (std::is_void_v<R>) {
    EnqueueJob([fn = std::forward<F>(body)]() mutable { fn(); }, op_hash, kBatchDrainNames[uint32_t(kind)]);
    SyncRecordThread(kProfFnCount, false, kind);
  } else {
    auto result = std::make_shared<std::optional<R>>();
    EnqueueJob([result, fn = std::forward<F>(body)]() mutable { result->emplace(fn()); }, op_hash,
               kBatchDrainNames[uint32_t(kind)]);
    SyncRecordThread(kProfFnCount, false, kind);
    return result->has_value() ? std::move(**result) : R{};
  }
}
// Card S3-50: the caller reserves the id/handle its queued call will create (the call consumes it via t_reserved_*).
// A call that fails leaves its reservation unused: the entry stays empty (readers see a missing snapshot/texture, as
// for a released one) and the id is never reused (no other holder can be confused by it). Failures are reported
// through TakeDrawFailures as "async <call>: <error>".
uint32_t ReserveSnapshotId() {
  std::lock_guard<std::mutex> lk(g->snap_alloc_mu);
  if (!g->free_snapshots.empty()) {
    const uint32_t id = g->free_snapshots.back();
    g->free_snapshots.pop_back();
    return id;
  }
  return g->snap_next++;
}
template <class F>
uint32_t QueueSnapshotCall(const char* what, uint64_t op_hash, F body) {
  const uint32_t id = ReserveSnapshotId();
  EnqueueJob([id, what, body]() mutable {
    t_reserved_snap = id;
    std::string err;
    const uint32_t r = body(&err);
    if (t_reserved_snap == id) {
      t_reserved_snap = kNone;
      AsyncFail(what, err.empty() ? std::string("failed") : err);
    } else if (r != id) {
      AsyncFail(what, "created another snapshot id");
    }
  }, g_batch_check ? BatchMixWords(op_hash, {id}) : 0, what);
  return id;
}
template <class F>
uint32_t QueueTextureCall(const char* what, uint64_t op_hash, F body) {
  const uint32_t handle = g->tex_next.fetch_add(1, std::memory_order_relaxed);
  EnqueueJob([handle, what, body]() mutable {
    t_reserved_tex = handle;
    std::string err;
    const uint32_t r = body(&err);
    if (t_reserved_tex == handle) {
      t_reserved_tex = kNone;
      AsyncFail(what, err.empty() ? std::string("failed") : err);
    } else if (r != handle) {
      AsyncFail(what, "created another texture handle");
    }
  }, g_batch_check ? BatchMixWords(op_hash, {handle}) : 0, what);
  return handle;
}
template <class F>
bool QueueBoolCall(const char* what, uint64_t op_hash, F body) {
  EnqueueJob([what, body]() mutable {
    std::string err;
    if (!body(&err)) AsyncFail(what, err);
  }, op_hash, what);
  return true;
}
uint64_t g_rq_full_seen = 0, g_rq_sleeps_seen = 0, g_rq_full_ns_seen = 0;
// Card S3-29b diagnostic: what a backend caller is blocked on (0 none, 1 Kick ring-wrap fence, 2 WaitSerial fence,
// 3 WaitAll fence, 4 record-thread Sync). Read by the D3D watchdog.
std::atomic<uint32_t> g_backend_wait{0};
uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct RecordConsumerWaitScope {
  RecordConsumerWaitScope(uint32_t kind, uint64_t serial, const char* name) : active(t_on_record_thread) {
    if (!active) return;
    g_record_consumer_wait_serial.store(serial, std::memory_order_relaxed);
    g_record_consumer_wait_name.store(name, std::memory_order_release);
    g_record_consumer_wait.store(kind, std::memory_order_release);
  }
  ~RecordConsumerWaitScope() {
    if (!active) return;
    g_record_consumer_wait.store(0, std::memory_order_release);
    g_record_consumer_wait_serial.store(0, std::memory_order_relaxed);
    g_record_consumer_wait_name.store(nullptr, std::memory_order_release);
  }
  bool active;
};
struct RecordMutexLock {
  RecordMutexLock(std::mutex& mutex, const char* name) : mutex_(mutex), record_thread_(t_on_record_thread) {
    if (record_thread_) {
      g_record_consumer_wait_name.store(name, std::memory_order_release);
      g_record_consumer_wait.store(3, std::memory_order_release);
    }
    mutex_.lock();
    if (record_thread_) {
      g_record_consumer_wait.store(0, std::memory_order_release);
      g_record_consumer_wait_name.store(nullptr, std::memory_order_release);
    }
  }
  ~RecordMutexLock() { mutex_.unlock(); }
  std::mutex& mutex_;
  bool record_thread_;
};
std::mutex g_pipeline_create_mu;  // serializes live/prewarm pipeline creation, shader-cache access, and cache reads
// ---- GPU timers (WWE2K14_NR_GPU_TIMERS=1, needs profiling) ----
// A timestamp is written only when the kind of public call changes (draws -> resolve -> draws ...), so
// consecutive draws share one segment; segment i lasts from timestamp i to i+1 on the GPU timeline.
thread_local uint32_t g_open_fn = UINT32_MAX;  // outermost public call in progress (re-marked after a mid-call Submit/Kick)
static void GpuPassClose(CmdEntry& e) {  // WWE2K14_NR_GPU_PASSES: append the draw count to the open pass label
  if (g->pass_idx >= 0 && size_t(g->pass_idx) < e.items.size()) e.items[g->pass_idx] += " draws=" + std::to_string(g->pass_draws);
  g->pass_idx = -1;
  g->pass_draws = 0;
}
// ---- GPU draw profiler (card D1, WWE2K14_NR_GPU_DRAWS=1 or 2 (+ per-key lines); needs GPU_TIMERS=1 and profiling; default off = nothing) ----
// A timestamp pair around every executed draw and every non-draw backend operation (public call kind, ItemScope kind,
// pass switch / pass end), in a per-list pool of its own, plus one boundary timestamp per frame (GpuDrawsFrame, at the
// frontend's swap). Timestamps are BOTTOM_OF_PIPE (plume): an interval = the GPU time from the completion of everything
// recorded before it to the completion of its own commands, so in-list intervals tile the timeline and the per-frame gap
// (span - covered) is GPU time spent between lists (idle / submission / other queue users) or on commands recorded
// outside any interval. Nested operations switch: the outer interval is closed, the inner one opened, the outer one
// reopened after it (so nothing is counted twice). Only one thread records at a time (non-draw calls sync the record
// thread), so this state is plain. Harvested per list after its fence wait; a frame is summarised once its boundary and
// every list holding its records have been harvested: one [nr-gpu-frame] line, and every 60 frames a [nr-gpu-top] table.
constexpr uint32_t kDpCap = 4096;  // queries per command list (EARLY_SUBMIT lists hold <= 256 draws); overflow is counted
constexpr uint32_t kDpNone = UINT32_MAX;
const char* const kDpFnNames[kProfFnCount] = {"surface", "clear", "tex_create", "tex_release", "draw_exec",
                                              "read_color", "resolve", "seed_detile", "exec_consts", "exec_pipeline", "exec_upload",
                                              "exec_record"};
struct DpLabel {
  std::string name;
  bool draw;
  uint64_t ps_key = 0, vs_key = 0;
};
struct DpIv {
  uint64_t b, e;
  uint32_t label;
};
struct DpFrame {
  uint64_t begin = 0, end = 0;
  bool has_begin = false, has_end = false;
  bool closed = false;       // GpuDrawsFrame ran for it (no more records will be added)
  bool boundary_lost = false;  // its boundary query did not fit: span unknown
  bool scene_once = false;
  uint64_t fe_frame = 0;     // the frontend's frame number (= the [nr-profile] frame=)
  int pending = 0;           // lists holding records of this frame not yet harvested
  uint64_t overflow = 0;     // intervals dropped (pool full)
  std::vector<DpIv> iv;
};
std::vector<DpLabel> g_dp_labels;
std::unordered_map<uint64_t, uint32_t> g_dp_ids;  // XXH3 of the label's identity -> index into g_dp_labels
std::map<uint64_t, DpFrame> g_dp_frames;
uint64_t g_dp_next_emit = 1;     // watermark: frames below it were emitted and erased; late records for them are dropped
uint64_t g_dp_late = 0;          // records dropped below the watermark (cumulative)
size_t g_dp_stuck = 120;         // frames waiting before the head is emitted incomplete (WWE2K14_NR_GPU_DRAWS_STUCK, test knob)
uint64_t g_dp_frame = 1, g_dp_first = 1;  // frame being recorded (frontend numbering: first swap is frame 1)
uint32_t g_dp_open = kDpNone;    // label of the interval open in the active list
uint32_t g_dp_open_q = 0;        // its begin query
uint32_t g_dp_reopen = kDpNone;  // label closed by a list end, reopened by the next list begin
struct DpWin {  // 60-frame window for the top table
  uint32_t frames = 0, scene_frames = 0;
  uint64_t first = 0, last = 0, span = 0, gap = 0;
  std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> by;  // label -> {ns, count}
} g_dp_win;
struct DpShaderWin {
  uint32_t frames = 0;
  uint64_t first = 0, last = 0;
  std::map<wwe2k14::v8diag::ShaderKey, std::pair<uint64_t, uint64_t>> by;  // effective PS+VS -> {GPU ns, draws}
} g_dp_shader_win;
bool DpOn() { return g && g->gpu_draws && g_prof && g->list; }
uint32_t DpIntern(uint64_t h, const char* name, bool draw, uint64_t ps_key = 0, uint64_t vs_key = 0) {
  auto it = g_dp_ids.find(h);
  if (it != g_dp_ids.end()) return it->second;
  g_dp_labels.push_back({name, draw, ps_key, vs_key});
  g_dp_ids.emplace(h, uint32_t(g_dp_labels.size() - 1));
  return uint32_t(g_dp_labels.size() - 1);
}
uint32_t DpOpLabel(const char* name) {  // op labels hash their name with a 1 prefix byte (draw keys: 0)
  char buf[64];
  const int n = std::snprintf(buf, sizeof(buf), "\x01%s", name);
  return DpIntern(XXH3_64bits(buf, size_t(std::min(n, int(sizeof(buf) - 1)))), name, false);
}
// Draw key: effective PS+VS + render target + vertex count bucket (4^k).
uint32_t DpDrawLabel(uint64_t ps_key, uint64_t vs_key, const SurfaceObj* c, const SurfaceObj* d, uint32_t count) {
  uint32_t bucket = 4;
  while (bucket < count && bucket < (1u << 30)) bucket <<= 2;
  struct {
    uint8_t tag;
    uint8_t pad[7];
    uint64_t ps, vs;
    uint32_t cw, ch, cs, cf, dw, dh, ds, df, n;
  } k = {0, {}, ps_key, vs_key, c ? c->width : 0, c ? c->height : 0, c ? c->samples : 0, c ? uint32_t(c->format) : 0xFFFFFFFFu,
         d ? d->width : 0, d ? d->height : 0, d ? d->samples : 0, d ? uint32_t(d->format) : 0xFFFFFFFFu, bucket};
  const uint64_t h = XXH3_64bits(&k, sizeof(k));
  auto it = g_dp_ids.find(h);
  if (it != g_dp_ids.end()) return it->second;
  char buf[208];
  std::snprintf(buf, sizeof(buf), "draw ps=%016llx vs=%016llx c=%ux%ux%u/f%d d=%ux%ux%u/f%d n<=%u",
                (unsigned long long)ps_key, (unsigned long long)vs_key, k.cw, k.ch, k.cs, c ? int(k.cf) : -1, k.dw,
                k.dh, k.ds, d ? int(k.df) : -1, bucket);
  return DpIntern(h, buf, true, ps_key, vs_key);
}
void DpNoteFrame(CmdEntry& e) {
  if (e.dframes.empty() || e.dframes.back() != g_dp_frame) {
    e.dframes.push_back(g_dp_frame);
    ++g_dp_frames[g_dp_frame].pending;
  }
}
void DpClose() {
  if (g_dp_open == kDpNone) return;
  g_dp_open = kDpNone;
  if (!g || !g->list) return;
  CmdEntry& e = g->cmd_pool[g->cmd_active];
  if (e.dpool) g->list->writeTimestamp(e.dpool.get(), g_dp_open_q + 1);
}
// Closes the open interval and opens `label` (both queries are reserved here, so the close always fits).
void DpOpen(uint32_t label) {
  if (!DpOn()) return;
  DpClose();
  CmdEntry& e = g->cmd_pool[g->cmd_active];
  if (!e.dpool) return;
  if (e.dq + 2 > kDpCap) {
    ++g_dp_frames[g_dp_frame].overflow;
    return;
  }
  DpNoteFrame(e);
  g->list->writeTimestamp(e.dpool.get(), e.dq);
  e.drecs.push_back({label, e.dq, g_dp_frame});
  g_dp_open = label;
  g_dp_open_q = e.dq;
  e.dq += 2;
}
// RAII: an operation nested in whatever is open; the outer label is reopened afterwards.
struct DpScope {
  DpScope() = default;
  explicit DpScope(uint32_t label) { Begin(label); }
  void Begin(uint32_t label) {
    if (!DpOn()) return;
    prev_ = g_dp_open;
    DpOpen(label);
    on_ = true;
  }
  ~DpScope() {
    if (!on_) return;
    DpClose();
    if (prev_ != kDpNone) DpOpen(prev_);
  }
  uint32_t prev_ = kDpNone;
  bool on_ = false;
};
// Before list->end() of the active list: a render pass still open (elided draws) is ended here, inside a "pass-end"
// interval, instead of implicitly by end() after the last timestamp (same commands, same order).
void DpListEnd(CmdEntry& e) {
  if (!g || !g->gpu_draws || !e.dpool) return;
  g_dp_reopen = g_dp_open;
  if (e.elide.fb && DpOn()) {
    DpOpen(DpOpLabel("pass-end"));
    e.list->setFramebuffer(nullptr);
    e.elide.fb = nullptr;
  }
  DpClose();
}
void DpEmitFrame(uint64_t id, DpFrame& f, bool incomplete);
void DpFinalize(bool force) {
  while (!g_dp_frames.empty()) {
    auto it = g_dp_frames.begin();
    DpFrame& f = it->second;
    const bool ready = f.closed && f.pending == 0 && (f.has_end || f.boundary_lost) && (f.has_begin || it->first == g_dp_first);
    // Safety: a frame that cannot complete (a list never harvested) is emitted incomplete once 16 newer frames wait.
    const bool stuck = g_dp_frames.size() > g_dp_stuck;
    if (!ready && !stuck && !force) break;
    if (!f.closed && !force) break;  // the frame still being recorded
    DpEmitFrame(it->first, f, !ready);
    g_dp_next_emit = it->first + 1;
    g_dp_frames.erase(it);
  }
}
void DpListBegin(CmdEntry& e) {
  if (!g || !g->gpu_draws) return;
  if (!e.dpool) e.dpool = g->device->createQueryPool(kDpCap);
  if (!e.drecs.empty() || !e.dframes.empty()) {  // never harvested (not expected): release its frames' pending counts
    for (uint64_t fr : e.dframes) {
      auto it = g_dp_frames.find(fr);
      if (it != g_dp_frames.end()) --it->second.pending;
    }
    e.drecs.clear();
    e.dframes.clear();
  }
  e.dq = 0;
  if (!e.dpool) return;
  e.list->resetQueryPool(e.dpool.get(), 0, kDpCap);
  g_dp_open = kDpNone;
  if (g_dp_reopen != kDpNone) {
    const uint32_t l = g_dp_reopen;
    g_dp_reopen = kDpNone;
    DpOpen(l);
  }
}
void DpHarvest(CmdEntry& e) {
  if (!g || !g->gpu_draws || !e.dpool || (e.drecs.empty() && e.dframes.empty())) return;
  e.dpool->queryResults();
  const uint64_t* r = e.dpool->getResults();
  for (const CmdEntry::DpRec& rec : e.drecs) {
    if (rec.label == kDpNone) {
      if (rec.frame >= g_dp_next_emit) {
        DpFrame& f = g_dp_frames[rec.frame];
        f.end = r[rec.q];
        f.has_end = true;
      } else {
        ++g_dp_late;
      }
      if (rec.frame + 1 >= g_dp_next_emit) {
        DpFrame& n = g_dp_frames[rec.frame + 1];
        n.begin = r[rec.q];
        n.has_begin = true;
      } else {
        ++g_dp_late;
      }
    } else if (rec.frame >= g_dp_next_emit) {
      g_dp_frames[rec.frame].iv.push_back({r[rec.q], r[rec.q + 1], rec.label});
    } else {
      ++g_dp_late;
    }
  }
  for (uint64_t fr : e.dframes) {
    auto it = g_dp_frames.find(fr);  // already emitted (stuck rule) frames are gone: nothing to release
    if (it != g_dp_frames.end()) --it->second.pending;
  }
  e.drecs.clear();
  e.dframes.clear();
  e.dq = 0;
  DpFinalize(false);
}
void DpEmitTop() {
  DpWin& w = g_dp_win;
  std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> v(w.by.begin(), w.by.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
  const double fr = w.frames ? double(w.frames) : 1.0;
  std::fprintf(stderr, "[nr-gpu-top] frames=%llu-%llu n=%u scene_once_frames=%u span_ms=%.3f gap_ms=%.3f (per frame)\n",
               (unsigned long long)w.first, (unsigned long long)w.last, w.frames, w.scene_frames, w.span * 1e-6 / fr,
               w.gap * 1e-6 / fr);
  for (size_t i = 0; i < v.size() && i < 25; ++i) {
    const DpLabel& l = g_dp_labels[v[i].first];
    std::fprintf(stderr, "[nr-gpu-top] #%zu ms=%.3f share=%.1f%% count=%.1f key=%s%s\n", i + 1,
                 v[i].second.first * 1e-6 / fr, w.span ? 100.0 * double(v[i].second.first) / double(w.span) : 0.0,
                 double(v[i].second.second) / fr, l.draw ? "" : "op:", l.name.c_str());
  }
  w = DpWin{};
}
void DpEmitShaderTop() {
  std::vector<wwe2k14::v8diag::ShaderRow> rows;
  rows.reserve(g_dp_shader_win.by.size());
  for (const auto& [key, totals] : g_dp_shader_win.by) rows.push_back({key, totals.first, totals.second});
  const std::string line = wwe2k14::v8diag::FormatShaderTop(g_dp_shader_win.first, g_dp_shader_win.last,
                                                            g_dp_shader_win.frames, rows.size(), std::move(rows));
  std::fprintf(stderr, "%s", line.c_str());
  g_dp_shader_win = DpShaderWin{};
}
void DpEmitFrame(uint64_t id, DpFrame& f, bool incomplete) {
  std::sort(f.iv.begin(), f.iv.end(), [](const DpIv& a, const DpIv& b) { return a.b < b.b; });
  uint64_t lo = f.has_begin ? f.begin : (f.iv.empty() ? 0 : f.iv.front().b);
  const uint64_t hi = f.has_end ? f.end : 0;
  const uint64_t span = hi > lo ? hi - lo : 0;
  uint64_t labelled = 0, covered = 0, outside = 0, cur_b = 0, cur_e = 0, draw_ns = 0;
  uint32_t draws = 0;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> ops;  // op label -> {ns, count}
  std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> keys;  // WWE2K14_NR_GPU_DRAWS=2: every label -> {ns, count}
  for (const DpIv& v : f.iv) {
    const uint64_t d = v.e > v.b ? v.e - v.b : 0;
    labelled += d;
    // Union inside [lo, hi]: covered; any part outside the frame span is reported (expected 0).
    const uint64_t b = std::max(v.b, lo), e = hi ? std::min(v.e, hi) : v.e;
    outside += d - (e > b ? e - b : 0);
    if (e > b) {
      if (b > cur_e) {
        covered += cur_e - cur_b;
        cur_b = b;
        cur_e = e;
      } else if (e > cur_e) {
        cur_e = e;
      }
    }
    if (g_dp_labels[v.label].draw) {
      ++draws;
      draw_ns += d;
    } else {
      auto& o = ops[v.label];
      o.first += d;
      ++o.second;
    }
    const DpLabel& label = g_dp_labels[v.label];
    if (g->gpu_shader_top && label.draw && label.ps_key && label.vs_key) {
      auto& shader = g_dp_shader_win.by[{label.ps_key, label.vs_key}];
      shader.first += d;
      ++shader.second;
    } else if (!g->gpu_shader_top) {
      auto& wb = g_dp_win.by[v.label];
      wb.first += d;
      ++wb.second;
    }
    if (g->gpu_draws_keys) {
      auto& k = keys[v.label];
      k.first += d;
      ++k.second;
    }
  }
  covered += cur_e - cur_b;
  const uint64_t gap = span > covered ? span - covered : 0;
  char buf[2048];
  int n = std::snprintf(buf, sizeof(buf),
                        "[nr-gpu-frame] frame=%llu scene_once=%d span_ms=%.3f labelled_ms=%.3f covered_ms=%.3f gap_ms=%.3f "
                        "overlap_ms=%.3f outside_ms=%.3f draws=%u draw_ms=%.3f ivs=%zu overflow=%llu%s%s late_dropped=%llu ops:",
                        (unsigned long long)(f.fe_frame ? f.fe_frame : id), f.scene_once ? 1 : 0, span * 1e-6,
                        labelled * 1e-6, covered * 1e-6, gap * 1e-6,
                        (labelled > covered + outside ? labelled - covered - outside : 0) * 1e-6, outside * 1e-6, draws,
                        draw_ns * 1e-6, f.iv.size(), (unsigned long long)f.overflow, incomplete ? " incomplete=1" : "",
                        f.boundary_lost ? " boundary_lost=1" : "", (unsigned long long)g_dp_late);
  for (const auto& [l, o] : ops) {
    if (n >= int(sizeof(buf))) break;
    n += std::snprintf(buf + n, sizeof(buf) - n, " %s=%llu/%.3f", g_dp_labels[l].name.c_str(), (unsigned long long)o.second,
                       o.first * 1e-6);
  }
  std::fprintf(stderr, "%s\n", buf);
  for (const auto& [l, k] : keys) {
    std::fprintf(stderr, "[nr-gpu-key] frame=%llu scene_once=%d ms=%.4f count=%llu key=%s%s\n",
                 (unsigned long long)(f.fe_frame ? f.fe_frame : id), f.scene_once ? 1 : 0, k.first * 1e-6,
                 (unsigned long long)k.second, g_dp_labels[l].draw ? "" : "op:", g_dp_labels[l].name.c_str());
  }
  const uint64_t frame_no = f.fe_frame ? f.fe_frame : id;
  if (g->gpu_shader_top) {
    if (!g_dp_shader_win.frames) g_dp_shader_win.first = frame_no;
    g_dp_shader_win.last = frame_no;
    if (++g_dp_shader_win.frames >= 120) DpEmitShaderTop();
  } else {
    DpWin& w = g_dp_win;
    if (!w.frames) w.first = frame_no;
    w.last = frame_no;
    ++w.frames;
    if (f.scene_once) ++w.scene_frames;
    w.span += span;
    w.gap += gap;
    if (w.frames >= 60) DpEmitTop();
  }
}
void GpuMark(uint32_t fn) {
  if (!g || !g->gpu_timers || !g_prof || !g->list) return;
  if (g->gpu_passes && g->pass_idx >= 0) {
    if (fn == kProfExecuteDraw) { ++g->pass_draws; return; }  // stay inside the per-pass segment
    GpuPassClose(g->cmd_pool[g->cmd_active]);
  }
  if (fn == g->gpu_cur) return;
  CmdEntry& e = g->cmd_pool[g->cmd_active];
  if (!e.qpool) return;
  if (e.segs.size() + 1 >= kGpuTimerCap) {
    ++g_stats.gpu_overflow;
    return;
  }
  g->list->writeTimestamp(e.qpool.get(), uint32_t(e.segs.size()));
  e.segs.push_back(fn);
  g->gpu_cur = fn;
  ++g_stats.gpu_marks;
}
// After list->begin(): reset the list's pool (outside any render pass) and re-open the current kind.
void GpuListBegin(CmdEntry& e) {
  DpListBegin(e);  // card D1
  if (!g || !g->gpu_timers) return;
  if (!e.qpool) e.qpool = g->device->createQueryPool(kGpuTimerCap);
  e.segs.clear();
  e.items.clear();
  g->pass_idx = -1;
  g->pass_draws = 0;
  g->gpu_cur = UINT32_MAX;
  if (!e.qpool) return;
  e.list->resetQueryPool(e.qpool.get(), 0, kGpuTimerCap);
  if (g_open_fn != UINT32_MAX) GpuMark(g_open_fn);
}
// Before list->end(): close the last segment.
void GpuListEnd(CmdEntry& e) {
  DpListEnd(e);  // card D1
  if (g && g->gpu_passes) GpuPassClose(e);
  if (!g || !g->gpu_timers || !e.qpool || e.segs.empty() || e.segs.back() == UINT32_MAX) return;
  e.list->writeTimestamp(e.qpool.get(), uint32_t(e.segs.size()));
  e.segs.push_back(UINT32_MAX);
}
// After the list's fence wait: add every segment's GPU time to its kind.
void GpuHarvest(CmdEntry& e) {
  DpHarvest(e);  // card D1
  if (!g || !g->gpu_timers || !e.qpool || e.segs.size() < 2) return;
  e.qpool->queryResults();
  const uint64_t* r = e.qpool->getResults();
  for (size_t i = 0; i + 1 < e.segs.size(); ++i) {
    if (e.segs[i] < kProfFnCount && r[i + 1] >= r[i]) g_stats.gpu_ns[e.segs[i]] += r[i + 1] - r[i];
    else if (e.segs[i] >= kProfFnCount && e.segs[i] != UINT32_MAX && r[i + 1] >= r[i] &&
             e.segs[i] - kProfFnCount < e.items.size()) {
      std::fprintf(stderr, "[nr-gpu-item] %s ms=%.3f\n", e.items[e.segs[i] - kProfFnCount].c_str(),
                   double(r[i + 1] - r[i]) * 1e-6);
    }
  }
  e.items.clear();
  ++g_stats.gpu_lists;
  e.segs.clear();
}

// WWE2K14_NR_GPU_PASSES=1: a new render pass of draws starts here; the segment lasts until the next non-draw call or pass.
static void GpuPassBegin(const SurfaceObj* color, const SurfaceObj* depth) {
  if (!g || !g->gpu_passes || !g->gpu_timers || !g_prof || !g->list) return;
  CmdEntry& e = g->cmd_pool[g->cmd_active];
  if (!e.qpool) return;
  if (g->pass_idx >= 0) {
    if (g->pass_draws > 0) --g->pass_draws;  // the current draw belongs to the new pass
    GpuPassClose(e);
  }
  char buf[128];
  std::snprintf(buf, sizeof(buf), "pass c=%ux%ux%u/f%d d=%ux%u/f%d", color ? color->width : 0, color ? color->height : 0,
                color ? color->samples : 0, color ? int(color->format) : -1, depth ? depth->width : 0,
                depth ? depth->height : 0, depth ? int(depth->format) : -1);
  e.items.push_back(buf);
  const int idx = int(e.items.size() - 1);
  GpuMark(kProfFnCount + uint32_t(idx));
  g->pass_idx = idx;
  g->pass_draws = 1;
}

// WWE2K14_NR_GPU_ITEMS=1: brackets one item of GPU work (a resolve, a texture build...) with its own timer segment.
struct ItemScope {
  ItemScope(const char* kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (DpOn()) dp_.Begin(DpOpLabel(kind));  // card D1: own interval, independent of GPU_ITEMS
    if (!g || !g->gpu_items || !g->gpu_timers || !g_prof || !g->list) return;
    CmdEntry& e = g->cmd_pool[g->cmd_active];
    if (!e.qpool) return;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s %08X %u %u %u", kind, a, b, c, d);
    e.items.push_back(buf);
    GpuMark(kProfFnCount + uint32_t(e.items.size() - 1));
    on_ = true;
  }
  ~ItemScope() {
    if (on_ && g_open_fn != UINT32_MAX) GpuMark(g_open_fn);
  }
  bool on_ = false;
  DpScope dp_;  // destroyed after the body above: closes the item interval, reopens the outer one
};

// Inclusive time of a public call; nested public calls are counted in the outermost one only.
BatchDrainKind BatchDrainForProf(uint32_t f) {
  switch (f) {
    case kProfSurface: return kBatchDrainPublicSurface;
    case kProfClear: return kBatchDrainPublicClear;
    case kProfCreateTexture: return kBatchDrainPublicCreateTexture;
    case kProfReleaseTexture: return kBatchDrainPublicReleaseTexture;
    case kProfExecuteDraw: return kBatchDrainPublicDraw;
    case kProfReadColor: return kBatchDrainPublicReadColor;
    case kProfResolve: return kBatchDrainPublicResolve;
    case kProfSeedDetile: return kBatchDrainPublicDetile;
    default: return kBatchDrainValueCall;
  }
}
struct ProfScope {
  explicit ProfScope(ProfFn f) : f_(f) {
    ++t_backend_depth;
    if (!t_on_record_thread) SyncRecordThread(f, false, BatchDrainForProf(f));
    if (f != kProfExecuteDraw && t_backend_depth == 1) CrowdFlushBoundary();
    if (!g_prof) return;
    counted_ = true;
    outer_ = g_prof_depth++ == 0;
    if (outer_) {
      t0_ = NowNs();
      g_open_fn = f;
      GpuMark(f);
      if (f != kProfExecuteDraw && DpOn()) dp_.Begin(DpOpLabel(kDpFnNames[f]));  // card D1 (draws label themselves)
    }
  }
  ~ProfScope() {
    --t_backend_depth;
    if (!counted_) return;
    --g_prof_depth;
    if (!outer_) return;
    g_open_fn = UINT32_MAX;
    const uint64_t ns = NowNs() - t0_;
    g_stats.fn_ns[f_] += ns;
    g_stats.fn_calls[f_] += 1;
    if (!t_on_record_thread) g_stats.total_ns += ns;  // the record thread's time is overlapped, not renderer-thread time
  }
  ProfFn f_;
  bool counted_ = false, outer_ = false;
  uint64_t t0_ = 0;
  DpScope dp_;  // card D1: the public call's interval (outermost non-draw calls)
};
// Sub-timers inside ExecuteDraw (always on when profiling, independent of the opt-in render flags).
// Unlike ProfScope this does NOT touch g_prof_depth or total_ns (those belong to the outer public-call
// ProfScope), so it decomposes draw_exec into exec_consts / exec_pipeline / exec_upload / exec_record
// without double counting. start()/stop() delimit each region so the variables that survive across
// regions need not be re-scoped.
thread_local bool t_pd_record_execute = false;
struct SubTimer {
  explicit SubTimer(ProfFn f) : f_(f) { start(); }
  void start() {
    if (!g_prof && !(draw_profile::On() && t_pd_record_execute)) return;
    t0_ = NowNs();
    started_ = true;
  }
  void stop() {
    if (!started_) return;
    started_ = false;
    const uint64_t elapsed = NowNs() - t0_;
    if (draw_profile::On() && t_pd_record_execute) {
      switch (f_) {
        case kProfExecConsts: draw_profile::RecordAdd(draw_profile::kConsts, elapsed); break;
        case kProfExecPipeline: draw_profile::RecordAdd(draw_profile::kPipeline, elapsed); break;
        case kProfExecUpload: draw_profile::RecordAdd(draw_profile::kUpload, elapsed); break;
        case kProfExecRecord: draw_profile::RecordAdd(draw_profile::kRecord, elapsed); break;
        default: break;
      }
    }
    if (!g_prof) return;
    g_stats.fn_ns[f_] += elapsed;
    g_stats.fn_calls[f_] += 1;
  }
  ~SubTimer() { stop(); }
  ProfFn f_;
  uint64_t t0_ = 0;
  bool started_ = false;
};
void CountBuffer() {
  if (g_prof) ++g_stats.buffers;
}

// Card D9 (WWE2K14_NR_FENCE_WATCH=1, default off): bounded diagnostic for the "CP stuck in WaitSerial" freeze
// (S3-68 L3). Off: the same waitForCommandFence call as before. On: wait with a 2 s timeout first; on timeout log
// the waited serial, every in-flight list's fence status and whether another thread holds the shared queue mutex,
// then (after 60 s) fall back to the original infinite wait.
bool FenceWatchOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_NR_FENCE_WATCH");
    return v && *v && *v != '0';
  }();
  return on;
}
void FenceWatchReport(const CmdEntry& w, uint32_t code, uint32_t waited_s) {
  if (IsD3D12()) return;
  auto* vkdev = static_cast<plume::VulkanDevice*>(g->device.get());
  const VkFence wf = static_cast<plume::VulkanCommandFence*>(w.fence.get())->vk;
  std::string lists;
  uint64_t max_done = 0;
  for (const CmdEntry& e : g->cmd_pool) {
    if (!e.in_flight) continue;
    const VkResult r = vkGetFenceStatus(vkdev->vk, static_cast<plume::VulkanCommandFence*>(e.fence.get())->vk);
    if (r == VK_SUCCESS && e.serial > max_done) max_done = e.serial;
    char b[48];
    std::snprintf(b, sizeof(b), "%s%llu:%d", lists.empty() ? "" : ",", static_cast<unsigned long long>(e.serial), int(r));
    lists += b;
  }
  const char* qm = "n/a";
  auto* cq = static_cast<plume::VulkanCommandQueue*>(g->queue.get());
  if (cq && cq->queue && cq->queue->mutex) {
    if (cq->queue->mutex->try_lock()) {
      cq->queue->mutex->unlock();
      qm = "free";
    } else {
      qm = "held";
    }
  }
  std::fprintf(stderr,
               "[nr-fence-watch] waited %u s code=%u serial=%llu submit_serial=%llu active=%zu pool=%zu status=%d "
               "in_flight=[%s] max_signalled_in_flight=%llu queue_mutex=%s\n",
               waited_s, code, static_cast<unsigned long long>(w.serial),
               static_cast<unsigned long long>(g->submit_serial), size_t(g->cmd_active), g->cmd_pool.size(),
               int(vkGetFenceStatus(vkdev->vk, wf)), lists.c_str(), static_cast<unsigned long long>(max_done), qm);
  std::fflush(stderr);
}
void WaitFenceWatched(CmdEntry& e, uint32_t code) {
  RecordConsumerWaitScope record_wait(2, e.serial, "command_fence");
  if (FenceWatchOn() && !IsD3D12()) {
    auto* vkdev = static_cast<plume::VulkanDevice*>(g->device.get());
    const VkFence f = static_cast<plume::VulkanCommandFence*>(e.fence.get())->vk;
    uint32_t waited = 0;
    for (;;) {
      const VkResult r = vkWaitForFences(vkdev->vk, 1, &f, VK_TRUE, 2000000000ull);
      if (r != VK_TIMEOUT) {
        if (waited) {
          std::fprintf(stderr, "[nr-fence-watch] completed after %u s code=%u serial=%llu result=%d\n", waited, code,
                       static_cast<unsigned long long>(e.serial), int(r));
          std::fflush(stderr);
        }
        break;
      }
      waited += 2;
      if (waited == 2 || waited % 10 == 0) FenceWatchReport(e, code, waited);
      if (waited >= 60) break;  // fall back to the original infinite wait
    }
  }
  g->queue->waitForCommandFence(e.fence.get());  // signalled: returns at once and resets the fence
}
// Ends the active command list and submits it (with its fence) WITHOUT waiting. Submission order on
// the one queue keeps every list's GPU work ordered, so no cross-list barriers are needed.
void SubmitActive() {
  CrowdFlushBoundary();
  CmdEntry& cur = g->cmd_pool[g->cmd_active];
  GpuListEnd(cur);
  cur.list->end();
  RecordConsumerWaitScope record_wait(1, g->submit_serial + 1, "plume_queue_submit");
  g->queue->executeCommandLists(cur.list.get(), cur.fence.get());
  cur.in_flight = true;
  cur.serial = ++g->submit_serial;
}
// Waits for every submitted list; then all fences are reset and every list is reusable.
void WaitAll() {
  for (CmdEntry& e : g->cmd_pool) {
    if (e.in_flight) {
      g_backend_wait.store(3, std::memory_order_relaxed);
      WaitFenceWatched(e, 3);
      g_backend_wait.store(0, std::memory_order_relaxed);
      e.in_flight = false;
      GpuHarvest(e);
    }
  }
}
// The constants ring is about to be reused from slot 0: every cached dedupe address is stale. Called
// only from Submit() (the sole site that resets const_next after WaitAll()); Kick() must NOT call this
// because a kicked list may still be reading the slots this batch has already written.
void InvalidateConstDedupe() {
  for (int i = 0; i < 3; ++i) g->dedupe[i].valid = false;
}
bool g_snap_pool_env = true;  // card L2-B: default on; WWE2K14_NR_SNAP_POOL=0 disables (read at Init)
constexpr size_t kSnapPoolMax = 256;
// GAP-mem1: the pool's own estimate counts 8 bytes per pixel for every format but R32F.
inline uint64_t SnapPoolMaxBytes() {
  static const uint64_t n = (MemLeanMode() & kMemLeanPool) ? (128ull << 20) : (512ull << 20);
  return n;
}
// Card L2-B: called where a retired-snapshot list is cleared after the wait that proves the GPU is done with it.
// Images nothing else references (use_count == their occurrences in the list: RetainSnapshot can queue one twice) go to
// the pool; the rest are simply dropped as before.
void ClearRetiredSnapshots(std::vector<std::shared_ptr<RenderTexture>>& v) {
  if (v.empty()) return;
  {
    RecordMutexLock lock(g->snap_mu, "snapshot_table_mutex");
    if (!g->snap_keys.empty() || !g->f16_lean_keys.empty()) {
      std::unordered_map<const RenderTexture*, long> occ;
      for (const auto& p : v) ++occ[p.get()];
      for (auto& p : v) {
        if (!p) continue;
        auto kit = g->snap_keys.find(p.get());
        auto fit = g->f16_lean_keys.find(p.get());
        if (kit == g->snap_keys.end() && fit == g->f16_lean_keys.end()) continue;
        long& n = occ[p.get()];
        if (n == 0) continue;  // already handled via an earlier entry of the same image
        if (kit != g->snap_keys.end()) {
          const State::SnapKey key = kit->second;
          if (p.use_count() == n &&
              (g_snap_pool_env || (g->depth_lean_on && key.storage && key.f == RenderFormat::R32_FLOAT))) {
            State::SnapPoolEntry e;
            e.k = key;
            e.bytes = uint64_t(key.w) * key.h * (key.f == RenderFormat::R32_FLOAT ? 4 : 8);
            e.img = p;
            e.mem_last_use_present = g->mem_present;
            g->snap_pool_bytes += e.bytes;
            g->snap_pool.push_back(std::move(e));
            while (g->snap_pool.size() > kSnapPoolMax || g->snap_pool_bytes > SnapPoolMaxBytes()) {
              g->snap_pool_bytes -= g->snap_pool.front().bytes;
              g->snap_keys.erase(g->snap_pool.front().img.get());
              g->snap_pool.erase(g->snap_pool.begin());
            }
          } else {
            g->snap_keys.erase(kit);  // still referenced elsewhere (shared texture, present view): not recyclable
          }
        } else {
          const State::SnapKey key = fit->second;
          if (p.use_count() == n && g->f16_lean_mode) {
            State::F16PoolEntry e;
            e.k = key;
            e.bytes = uint64_t(key.w) * key.h * RenderFormatSize(key.f);
            e.img = p;
            e.mem_last_use_present = g->mem_present;
            g->f16_lean_pool_bytes += e.bytes;
            g->f16_lean_pool.push_back(std::move(e));
            while (g->f16_lean_pool.size() > kSnapPoolMax || g->f16_lean_pool_bytes > SnapPoolMaxBytes()) {
              const RenderTexture* old = g->f16_lean_pool.front().img.get();
              g->f16_lean_pool_bytes -= g->f16_lean_pool.front().bytes;
              g->f16_lean_keys.erase(old);
              g->f16_lean_fbs.erase(old);
              g->f16_lean_pool.erase(g->f16_lean_pool.begin());
            }
          } else {
            g->f16_lean_keys.erase(fit);  // a shared texture still owns this image; do not recycle its framebuffer
            g->f16_lean_fbs.erase(p.get());
          }
        }
        n = 0;
      }
    }
  }
  v.clear();
}
// Execute the open command list (with wait), free everything the batch retired, and restart the
// per-batch rings. Called only where the CPU needs GPU results or a ring is full.
void ReapSurfaces(bool all);  // card S3-52b
void Submit() {
  SubmitActive();  // end + execute the active list (its fence becomes in flight)
  const uint64_t t0 = g_prof ? NowNs() : 0;
  WaitAll();  // the only place the CPU blocks on the GPU (submit_ms = this wait)
  if (g_prof) {
    ++g_stats.submits;
    g_stats.submit_ns += NowNs() - t0;
  }
  g->cmd_pool[g->cmd_active].list->begin();
  g->list = g->cmd_pool[g->cmd_active].list.get();
  GpuListBegin(g->cmd_pool[g->cmd_active]);
  if (g->state_elide || g->rec_reuse.load(std::memory_order_relaxed))
    g->cmd_pool[g->cmd_active].elide = ElideState{};
  g->retired_buffers.clear();
  g->upload_ring_offset = 0;  // safe: WaitAll() above proved every pending/kicked list is done with the ring
  g->retired_textures.clear();
  ClearRetiredSnapshots(g->retired_snapshots);
  for (State::RetireBatch& b : g->retire_q) {  // card S3-6: WaitAll covered them too
    ClearRetiredSnapshots(b.snapshots);
    g->free_table_indices.insert(g->free_table_indices.end(), b.table_indices.begin(), b.table_indices.end());  // S3-34
  }
  g->retire_q.clear();
  ReapSurfaces(true);  // card S3-52b: WaitAll covered every list that could reference them
  g->free_table_indices.insert(g->free_table_indices.end(), g->pending_free_indices.begin(),
                               g->pending_free_indices.end());
  g->pending_free_indices.clear();
  g->const_next = 0;
  InvalidateConstDedupe();  // the ring wraps to slot 0: prior vs/ps/shared addresses are stale
  ++g->const_gen;           // card P24: every list is idle (WaitAll), so the whole ring is free
  g->const_safe = g->const_slots;
  g->cmd_pool[g->cmd_active].const_gen = g->const_gen;
  g->cmd_pool[g->cmd_active].const_lo = UINT32_MAX;
  g->detile_next = 0;
  g->resolve_next = 0;
  g->resolve_depth_next = 0;
  g->depth_img_next = 0;
  g->fsr1_next = 0;
  g->resolve_tex_next = 0;
  g->f16_lean_next = 0;
  g->draws_since_kick = 0;
}
// Early submit (WWE2K14_NR_EARLY_SUBMIT=N>0): hand the recorded work to the GPU WITHOUT waiting and
// continue recording into the next pooled list, so the GPU can overlap with the rest of the CPU's
// recording. Only called between draws; with state elision a render pass may still be open, but
// SubmitActive() -> end() ends it before submit. Nothing per-batch is retired or
// recycled here: const_next / detile_next / resolve_next, retired buffers/textures,
// pending_free_indices, staging buffers and descriptor sets all stay owned until the next Submit().
// Invariant: submission order on the one queue orders EXECUTION only. Every op that reads memory written by
// an earlier (kicked) list must record its own barrier in its list; never rely on submission order alone.
void Kick() {
  SubmitActive();
  if (++g->cmd_active == g->cmd_pool.size()) {
    if (g->cmd_pool.size() < kCmdListPoolCap) {
      CmdEntry e;
      e.list = g->queue->createCommandList();
      e.fence = g->device->createCommandFence();
      if (e.list && e.fence) g->cmd_pool.push_back(std::move(e));
      else g->cmd_active = 0;  // creation failed: keep the pool size and wrap (with a fence wait)
    } else {
      g->cmd_active = 0;  // ring is full: wrap and reuse the oldest list (with a fence wait)
    }
  }
  CmdEntry& next = g->cmd_pool[g->cmd_active];
  if (next.in_flight) {  // this list is still on the GPU: wait for it before re-recording
    g_backend_wait.store(1, std::memory_order_relaxed);
    WaitFenceWatched(next, 1);
    g_backend_wait.store(0, std::memory_order_relaxed);
    next.in_flight = false;
    GpuHarvest(next);
  }
  next.list->begin();
  g->list = next.list.get();
  GpuListBegin(next);
  if (g->state_elide || g->rec_reuse.load(std::memory_order_relaxed)) next.elide = ElideState{};
  next.const_gen = g->const_gen;
  next.const_lo = UINT32_MAX;
  g->draws_since_kick = 0;
  if (g_prof) ++g_stats.kicks;
}
void Retire(std::unique_ptr<RenderBuffer> b) {
  if (b) g->retired_buffers.push_back(std::move(b));
}

// Exchange the active per-batch resources other than the constants ring (fields of State) with a parked set.
void SwapBatchRes(FrameRes& f) {
  std::swap(g->upload_ring, f.upload_ring);
  std::swap(g->upload_ring_mapped, f.upload_ring_mapped);
  std::swap(g->upload_ring_size, f.upload_ring_size);
  std::swap(g->upload_ring_offset, f.upload_ring_offset);
  std::swap(g->resolve_sets, f.resolve_sets);
  std::swap(g->resolve_next, f.resolve_next);
  std::swap(g->resolve_depth_sets, f.resolve_depth_sets);
  std::swap(g->resolve_depth_next, f.resolve_depth_next);
  std::swap(g->depth_img_sets, f.depth_img_sets);
  std::swap(g->depth_img_next, f.depth_img_next);
  std::swap(g->fsr1_sets, f.fsr1_sets);
  std::swap(g->fsr1_next, f.fsr1_next);
  std::swap(g->detile_sets, f.detile_sets);
  std::swap(g->detile_next, f.detile_next);
  std::swap(g->resolve_tex_sets, f.resolve_tex_sets);
  std::swap(g->resolve_tex_next, f.resolve_tex_next);
  std::swap(g->f16_lean_sets, f.f16_lean_sets);
  std::swap(g->f16_lean_next, f.f16_lean_next);
  std::swap(g->retired_buffers, f.retired_buffers);
  std::swap(g->retired_textures, f.retired_textures);
  std::swap(g->retired_snapshots, f.retired_snapshots);
  std::swap(g->pending_free_indices, f.pending_free_indices);
}
// Frame slots: exchange the active per-frame set (fields of State) with a parked one.
void SwapFrameRes(FrameRes& f) {
  std::swap(g->constants, f.constants);
  std::swap(g->constants_mapped, f.constants_mapped);
  std::swap(g->constants_address, f.constants_address);
  std::swap(g->const_next, f.const_next);
  SwapBatchRes(f);
}
// Waits for every submitted list up to submission `serial` (the queue executes in order).
void WaitSerial(uint64_t serial) {
  for (CmdEntry& e : g->cmd_pool) {
    if (e.in_flight && e.serial <= serial) {
      g_backend_wait.store(2, std::memory_order_relaxed);
      WaitFenceWatched(e, 2);
      g_backend_wait.store(0, std::memory_order_relaxed);
      e.in_flight = false;
      GpuHarvest(e);
    }
  }
}
// Called once per directly presented frame, after its work was submitted (Kick). Parks the active set with that
// submission's serial, then activates the oldest set once the frame that used it has finished on the GPU.
// Card S3-8 test knob (draw-off only): WWE2K14_NR_RETIRE_FRAMES=k frees / waits for the batch presented k frames ago
// (default 2; 0 = the CP waits for this frame's GPU work at its present).
size_t RetireFrames() {
  static const FixAltKnob knob = FixAltKnobFor("WWE2K14_NR_RETIRE_FRAMES");
  const int64_t frames = FixAltInt(knob, 2);
  return frames > 0 ? size_t(frames) : size_t(0);
}
void RetireByFrame(size_t retire_frames) {
  if (g->retired_buffers.empty() && g->retired_textures.empty() && g->retired_snapshots.empty() &&
      g->pending_free_indices.empty()) {
    return;
  }
  State::RetireBatch b;
  b.serial = g->submit_serial;  // every list that can reference these was submitted at or before this serial
  std::swap(b.buffers, g->retired_buffers);
  std::swap(b.textures, g->retired_textures);
  std::swap(b.snapshots, g->retired_snapshots);
  // Card S3-34: the released textures' table slots retire with them. They used to stay in pending_free_indices until a
  // full Submit (rare in draw-off), so the 4096-slot table filled during long sessions ("texture table full" refusals,
  // wrestlers missing from the second match on).
  std::swap(b.table_indices, g->pending_free_indices);
  g->retire_q.push_back(std::move(b));
  while (g->retire_q.size() > retire_frames) {
    State::RetireBatch& old = g->retire_q.front();
    WaitSerial(old.serial);  // normally already done (that frame was presented kRetireFrames frames ago)
    old.buffers.clear();
    old.textures.clear();
    ClearRetiredSnapshots(old.snapshots);
    g->free_table_indices.insert(g->free_table_indices.end(), old.table_indices.begin(), old.table_indices.end());
    g->retire_q.erase(g->retire_q.begin());
  }
}
// Card S3-52b: frees released surfaces whose last possible user (the list that was active at the release) has finished.
// Non-blocking unless `all` (the caller proved every submitted list finished, e.g. after WaitAll).
void ReapSurfaces(bool all) {
  auto& v = g->retired_surfaces;
  v.erase(std::remove_if(v.begin(), v.end(),
                         [all](const State::RetiredSurface& r) {
                           return all || (r.serial <= g->submit_serial && GpuReachedSerial(r.serial));
                         }),
          v.end());
}
void FrameEnd() {
  if (!g->retired_surfaces.empty()) ReapSurfaces(false);
  if (!g->frame_slots) {
    if (g->retire_by_frame) {
      const size_t retire_frames = RetireFrames();  // sampled once at this frame-end boundary
      RetireByFrame(retire_frames);
      if (retire_frames == 0) WaitSerial(g->submit_serial);  // S3-8 knob: nothing retired this frame still waits
    }
    return;
  }
  FrameRes& cur = g->parked[g->frame_slot];
  SwapFrameRes(cur);  // cur now holds this frame's set; State holds the empty placeholder
  cur.serial = g->submit_serial;
  g->frame_slot = (g->frame_slot + 1) % kFrameSlots;
  FrameRes& next = g->parked[g->frame_slot];
  const uint64_t t0 = g_prof ? NowNs() : 0;
  if (next.serial) WaitSerial(next.serial);
  if (g_prof) g_stats.submit_ns += NowNs() - t0;
  next.serial = 0;
  next.retired_buffers.clear();
  next.retired_textures.clear();
  ClearRetiredSnapshots(next.retired_snapshots);
  g->free_table_indices.insert(g->free_table_indices.end(), next.pending_free_indices.begin(),
                               next.pending_free_indices.end());
  next.pending_free_indices.clear();
  next.const_next = 0;
  next.upload_ring_offset = 0;
  next.resolve_next = next.resolve_depth_next = next.detile_next = next.resolve_tex_next = next.f16_lean_next = 0;
  next.depth_img_next = 0;
  next.fsr1_next = 0;
  SwapFrameRes(next);  // State holds the next frame's set; next is the placeholder
  InvalidateConstDedupe();  // cached constant addresses point into another set
}

// Card P24 (WWE2K14_NR_CONST_FENCE=1): the constants ring is full. Instead of Submit() (WaitAll + reset), hand the active
// list to the GPU (Kick: same list boundary as Submit), restart the ring in a new generation whose slots are proven free
// lazily (ConstFenceAdvance), and swap the other per-batch resources with the set parked at the previous wrap.
void ConstFenceWrap() {
  Kick();  // every list holding this generation's draws is now submitted; the new active list holds none
  FrameRes& sp = g->fence_spare;
  if (sp.serial) WaitSerial(sp.serial);  // lists that used the spare set (previous wrap and older): normally done
  sp.serial = 0;
  sp.retired_buffers.clear();
  sp.retired_textures.clear();
  ClearRetiredSnapshots(sp.retired_snapshots);
  g->free_table_indices.insert(g->free_table_indices.end(), sp.pending_free_indices.begin(),
                               sp.pending_free_indices.end());
  sp.pending_free_indices.clear();
  sp.upload_ring_offset = 0;
  sp.resolve_next = sp.resolve_depth_next = sp.detile_next = sp.resolve_tex_next = sp.f16_lean_next = 0;
  sp.depth_img_next = 0;
  sp.fsr1_next = 0;
  SwapBatchRes(sp);  // State: the free set; sp: this generation's, owned until every list up to now has finished
  sp.serial = g->submit_serial;
  g->const_next = 0;
  ++g->const_gen;
  g->const_safe = 0;  // nothing proven free yet: ConstFenceAdvance before the first write
  g->cmd_pool[g->cmd_active].const_gen = g->const_gen;
  InvalidateConstDedupe();  // cached vs/ps/shared addresses point at slots that will be rewritten
}
// Card P24: proves slots [const_next, const_next + const_slots/8) free by waiting only for the older-generation lists
// that read a slot below that bound (oldest first in practice), then moves const_safe to the lowest slot still read by
// an older list that is in flight.
void ConstFenceAdvance() {
  const uint32_t chunk = std::max<uint32_t>(1, g->const_slots / 8);
  const uint32_t target = std::min<uint32_t>(g->const_slots, g->const_next + chunk);
  for (CmdEntry& e : g->cmd_pool) {
    if (e.in_flight && e.const_gen < g->const_gen && e.const_lo < target) {
      WaitFenceWatched(e, 5);
      e.in_flight = false;
      GpuHarvest(e);
      if (g_prof) ++g_stats.const_fence_waits;
    }
  }
  uint32_t safe = g->const_slots;
  for (const CmdEntry& e : g->cmd_pool)
    if (e.in_flight && e.const_gen < g->const_gen) safe = std::min(safe, e.const_lo);
  g->const_safe = safe;  // >= target
}
}  // namespace
// Card P22: WWE2K14_NR_PRECISE_BARRIERS=1 (read at Init); plume reads the device flag per barrier call.
bool g_precise_env = false;
void ApplyPreciseBarriers() {
  if (!g || !g->device || IsD3D12()) return;
  static_cast<plume::VulkanDevice*>(g->device.get())
      ->k14PreciseBarriers.store(g_precise_env, std::memory_order_relaxed);
}
namespace {
// Allocates the next constants-ring slot. *wrapped = the ring restarted (every cached address is stale).
uint32_t ConstSlot(bool* wrapped) {
  *wrapped = false;
  if (g->const_next >= g->const_slots) {
    const uint64_t t0 = g_prof ? NowNs() : 0;
    if (g->const_fence) ConstFenceWrap();
    else Submit();  // ring full with fenced reuse disabled: wait, then safely reuse from slot 0
    *wrapped = true;
    if (g_prof) {
      ++g_stats.const_wraps;
      g_stats.const_wrap_wait_ns += NowNs() - t0;
    }
  }
  if (g->const_fence && g->const_next >= g->const_safe) {
    const uint64_t t0 = g_prof ? NowNs() : 0;
    ++g->mem_const_waits;
    ConstFenceAdvance();
    if (g_prof) g_stats.const_wrap_wait_ns += NowNs() - t0;
  }
  ++g->mem_const_taken;
  return g->const_next++;
}
// Card P24: records that the active list reads the ring slot holding `address` (any block of it).
inline void ConstRef(uint64_t address) {
  CmdEntry& e = g->cmd_pool[g->cmd_active];
  const uint32_t slot = uint32_t((address - g->constants_address) / kConstBytes);
  if (slot < e.const_lo) e.const_lo = slot;
}

// Upload ring (WWE2K14_NR_UPLOAD_RING=1): alignment for sub-allocations. Vertex buffer offsets must be a
// multiple of the device's minVertexInputBindingStrideAlignment (16 B is safe on the dev GPU); index
// offsets a multiple of the index type size (Vulkan: 4 for uint32, 2 for uint16).
constexpr uint64_t kRingVertexAlign = 16;
constexpr uint64_t kRingIndexAlign32 = 4;
constexpr uint64_t kRingIndexAlign16 = 2;

uint32_t VbDirectMode() { return g ? g->vb_direct_mode.load(std::memory_order_relaxed) : 0u; }
uint32_t ConfiguredVbDirectMode() {
  const char* e = std::getenv("WWE2K14_NR_VB_DIRECT");
  if (e && std::strcmp(e, "1") == 0) return 1u;
  if (e && std::strcmp(e, "2") == 0) return 2u;
  return 0u;
}

struct VbDirectCounts {
  uint64_t direct_draws = 0, direct_bytes = 0;
  uint64_t check_draws = 0, check_bytes = 0, mismatch_draws = 0, mismatch_bytes = 0;
  uint64_t not_record_queue = 0, not_batch = 0, not_crowd_candidate = 0, not_ring_room = 0, not_mode_off = 0;
} g_vb_direct_stats;
struct RecReuseCounts {
  uint64_t hits = 0, checks = 0, mismatches = 0, logged = 0;
} g_rec_reuse_stats;

bool RingHasRoom(uint64_t bytes, uint64_t align) {
  if (!g->upload_ring || !g->upload_ring_mapped || !align || (align & (align - 1))) return false;
  if (g->upload_ring_offset > UINT64_MAX - (align - 1)) return false;
  const uint64_t off = (g->upload_ring_offset + align - 1) & ~(align - 1);
  return off <= g->upload_ring_size && bytes <= g->upload_ring_size - off;
}

// Sub-allocate `bytes` at `align`-byte alignment from the persistent upload ring. When the allocation
// does not fit in the remaining space, the ring grows (double, at least enough for `bytes`): the old
// ring is Retire()-ed because already-recorded commands may still reference it until the next wait.
// Returns the byte offset of the allocation, or UINT64_MAX when the ring is unavailable (the caller
// falls back to the per-draw createBuffer path). Never rewrites bytes a kicked/pending list may read:
// the offset only advances here and resets in Submit() after WaitAll().
uint64_t RingAlloc(uint64_t bytes, uint64_t align) {
  if (!g->upload_ring || !g->upload_ring_mapped) return UINT64_MAX;
  const uint64_t off = (g->upload_ring_offset + align - 1) & ~(align - 1);
  if (off + bytes > g->upload_ring_size) {
    uint64_t n = g->upload_ring_size ? g->upload_ring_size * 2 : (1ull << 26);
    while (n < bytes) n <<= 1;
    CountBuffer();
    auto ring = g->device->createBuffer(
        RenderBufferDesc::UploadBuffer(n, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
    uint8_t* mapped = ring ? static_cast<uint8_t*>(ring->map()) : nullptr;
    if (!ring || !mapped) return UINT64_MAX;
    // F1: the old ring was mapped once (Init or a previous growth) and never unmapped; the per-draw
    // fallback buffers unmap before Retire, but the ring path did not. Unmap it before Retire so its
    // VMA allocation is not destroyed while still mapped (a debug-build VMA assert; the shipped
    // RelWithDebInfo build compiles that assert out, which is why the tested runs passed).
    g->upload_ring->unmap();
    Retire(std::move(g->upload_ring));
    g->upload_ring = std::move(ring);
    g->upload_ring_mapped = mapped;
    g->upload_ring_size = n;
    g->upload_ring_offset = bytes;  // this allocation occupies [0, bytes) of the fresh ring
    return 0;
  }
  g->upload_ring_offset = off + bytes;
  if (g->upload_ring_offset > g->mem_upload_hw) g->mem_upload_hw = g->upload_ring_offset;
  return off;
}

// Layout transition; plume tracks every texture's current layout itself.
}  // namespace
// One request source for both shader selection (before Init) and API creation. D3D12 is deliberately Windows-only;
// unset and all other values preserve Vulkan selection.
bool BackendWantsDxil() {
#ifdef _WIN32
  const char* backend = std::getenv("WWE2K14_NR_BACKEND");
  return backend && std::strcmp(backend, "d3d12") == 0;
#else
  return false;
#endif
}
bool IsD3D12() { return g && g->shader_format == RenderShaderFormat::DXIL; }
// Card C3: plume texture description on either backend (the plume texture types are unrelated structs).
const RenderTextureDesc& D3D12TextureDescOf(RenderTexture* t) {
#ifdef _WIN32
  if (IsD3D12()) return static_cast<plume::D3D12Texture*>(t)->desc;
#endif
  return static_cast<plume::VulkanTexture*>(t)->desc;
}
namespace {
// [wwe13 nr] Current layout of a texture: Vulkan reads plume's own tracking (unchanged); D3D12 uses nr's map
// (UNKNOWN until nr issues the first barrier, which is the conservative answer for every caller).
RenderTextureLayout CurrentLayout(RenderTexture* t) {
  if (!IsD3D12()) return static_cast<plume::VulkanTexture*>(t)->textureLayout;
  auto it = g->d3d_layouts.find(t);
  return it == g->d3d_layouts.end() ? RenderTextureLayout::UNKNOWN : it->second;
}
bool VulkanPreciseBarriersOn() {
  if (!g || !g->device || IsD3D12()) return false;
  return static_cast<plume::VulkanDevice*>(g->device.get())->k14PreciseBarriers.load(std::memory_order_relaxed);
}
// createTexture + reset of the D3D12 layout record (a recycled pointer must not inherit a stale layout).
std::unique_ptr<RenderTexture> NewTexture(const RenderTextureDesc& d) {
  auto t = g->device->createTexture(d);
  // Card M2b2-A: plume's VulkanTexture ctor returns with vk == null when vmaCreateImage fails (plume_vulkan.cpp:1137-1141)
  // and createTexture still hands the object back. Report that as failure so callers' 'createTexture failed' paths run.
  // The Vulkan allocation-failure probe is not valid for a D3D12 RenderTexture.
  if (t && !IsD3D12() && static_cast<plume::VulkanTexture*>(t.get())->vk == VK_NULL_HANDLE) {
    ++g_stats.tex_alloc_fail;
    return nullptr;
  }
  if (t) ResolveCensusImage();
  if (t) ++g->mem_new_tex;
  if (t && IsD3D12()) g->d3d_layouts.erase(t.get());
  return t;
}
// ---- Card S3-15 lazy colour snapshots (see SnapshotColor) ----
struct LazySnap {
  uint32_t surface = 0;
  RenderTexture* src = nullptr;
  uint32_t x = 0, y = 0, w = 0, h = 0;
  std::vector<uint32_t> ids;  // snapshot ids that share this pending copy (RetainSnapshot)
};
bool g_lazy_on = false;
std::vector<LazySnap> g_lazy;  // pending copies (small); touched only with the record thread synced / on it
struct LazyStats { uint64_t created = 0, materialized = 0, by_reader = 0, by_write = 0, skipped = 0; } g_lazy_stats;
bool IsWriteLayout(RenderTextureLayout l) {
  return l == RenderTextureLayout::COLOR_WRITE || l == RenderTextureLayout::DEPTH_WRITE ||
         l == RenderTextureLayout::COPY_DEST || l == RenderTextureLayout::RESOLVE_DEST ||
         l == RenderTextureLayout::GENERAL;
}
std::shared_ptr<RenderTexture> CopySurfaceRegion(SurfaceObj& t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                                 State::SnapHi* hi, std::string* error);
void MaterializeLazy(size_t i) {
  LazySnap e = std::move(g_lazy[i]);
  g_lazy.erase(g_lazy.begin() + i);  // before the copy: its barriers may look the list up again
  std::string err;
  State::SnapHi hi;
  std::shared_ptr<RenderTexture> image = CopySurfaceRegion(g->surfaces[e.surface], e.x, e.y, e.w, e.h, &hi, &err);
  for (uint32_t id : e.ids) {
    g->snapshots[id] = image;  // null on failure: readers report a bad snapshot
    g->snapshot_hi[id] = hi;
  }
  ++g_lazy_stats.materialized;
}
void MaterializeSource(RenderTexture* t) {
  for (size_t i = 0; i < g_lazy.size();) {
    if (g_lazy[i].src == t) {
      MaterializeLazy(i);
      ++g_lazy_stats.by_write;
    } else {
      ++i;
    }
  }
}
bool HasLazySource(const RenderTexture* t) {
  for (const LazySnap& e : g_lazy)
    if (e.src == t) return true;
  return false;
}
void EnsureSnap(uint32_t id) {
  TouchSnapshot(id);
  if (g_lazy.empty()) return;
  for (size_t i = 0; i < g_lazy.size(); ++i) {
    for (uint32_t x : g_lazy[i].ids) {
      if (x == id) {
        MaterializeLazy(i);
        ++g_lazy_stats.by_reader;
        return;
      }
    }
  }
}

void Barrier(RenderTexture* t, RenderTextureLayout wanted) {
  if (!g_lazy.empty() && IsWriteLayout(wanted) && HasLazySource(t)) MaterializeSource(t);
  if (CurrentLayout(t) == wanted) return;
  if (IsD3D12()) g->d3d_layouts[t] = wanted;
  RenderTextureBarrier b(t, wanted);
  g->list->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY | RenderBarrierStage::COMPUTE, nullptr, 0,
                    &b, 1);
  ResolveCensusBarrier();
}
// Same-layout barrier before writing an attachment again: with one command list, consecutive render
// passes on the same target need an explicit dependency (the per-call GPU wait used to provide it).
void WriteBarrier(RenderTexture* t, RenderTextureLayout wanted) {
  if (!g_lazy.empty() && HasLazySource(t)) MaterializeSource(t);
  if (IsD3D12()) g->d3d_layouts[t] = wanted;
  RenderTextureBarrier b(t, wanted);
  g->list->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY | RenderBarrierStage::COMPUTE, nullptr, 0,
                    &b, 1);
  ResolveCensusBarrier();
}
void BufferBarrier(RenderBuffer* buffer) {
  RenderBufferBarrier b(buffer, RenderBufferAccess::READ | RenderBufferAccess::WRITE);
  g->list->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY | RenderBarrierStage::COMPUTE, &b, 1,
                    nullptr, 0);
  ResolveCensusBarrier();
}
// Card S3-51 (AMD blotches): plume's legacy buffer barrier takes its SOURCE stages from the buffer's previous barrier
// (VulkanBuffer::barrierStages, NONE for a fresh buffer) plus TOP_OF_PIPE, and dispatch() does not record stages. A
// fresh staging buffer written by a compute dispatch and then copied to an image therefore got a barrier with
// srcStage = TOP_OF_PIPE only: the compute writes were not made available to the copy (NVIDIA tolerates it, AMD can
// copy stale/partial data). Priming the buffer with a barrier before the dispatch makes the next barrier include
// COMPUTE. WWE2K14_NR_COMPUTE_SRC_FIX=0 turns it off for an A/B.
void PrimeComputeOutput(RenderBuffer* buffer) {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_NR_COMPUTE_SRC_FIX");
    return !(v && *v == '0');
  }();
  if (on) BufferBarrier(buffer);
}
// Card P22 step 3: adjacent Barrier/WriteBarrier/BufferBarrier calls with nothing recorded between them, merged into one
// barriers() call (= one vkCmdPipelineBarrier) while precise barriers are on; otherwise each call is issued at once,
// exactly as before. Same resources, layouts and stages as the separate calls; flushed by Flush() or the destructor
// (scope end). A resource already in the batch flushes it first (one item per resource per call).
struct BarrierBatch {
  RenderTextureBarrier tex[4];
  RenderBufferBarrier buf[4];
  uint32_t ntex = 0, nbuf = 0;
  const bool on = VulkanPreciseBarriersOn();
  BarrierBatch() = default;
  BarrierBatch(const BarrierBatch&) = delete;
  BarrierBatch& operator=(const BarrierBatch&) = delete;
  ~BarrierBatch() { Flush(); }
  bool Has(const RenderTexture* t) const {
    for (uint32_t i = 0; i < ntex; ++i)
      if (tex[i].texture == t) return true;
    return false;
  }
  void Add(RenderTexture* t, RenderTextureLayout wanted) {
    if (Has(t) || ntex == 4) Flush();
    if (IsD3D12()) g->d3d_layouts[t] = wanted;
    tex[ntex++] = RenderTextureBarrier(t, wanted);
  }
  void Layout(RenderTexture* t, RenderTextureLayout wanted) {  // = Barrier()
    if (!on) return Barrier(t, wanted);
    if (!g_lazy.empty() && IsWriteLayout(wanted) && HasLazySource(t)) Flush(), MaterializeSource(t);
    if (Has(t)) Flush();
    if (CurrentLayout(t) == wanted) return;
    Add(t, wanted);
  }
  void Write(RenderTexture* t, RenderTextureLayout wanted) {  // = WriteBarrier()
    if (!on) return WriteBarrier(t, wanted);
    if (!g_lazy.empty() && HasLazySource(t)) Flush(), MaterializeSource(t);
    Add(t, wanted);
  }
  void Buffer(RenderBuffer* b) {  // = BufferBarrier()
    if (!on) return BufferBarrier(b);
    for (uint32_t i = 0; i < nbuf; ++i)
      if (buf[i].buffer == b) Flush();
    if (nbuf == 4) Flush();
    buf[nbuf++] = RenderBufferBarrier(b, RenderBufferAccess::READ | RenderBufferAccess::WRITE);
  }
  void Flush() {
    if (ntex || nbuf) {
      g->list->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY | RenderBarrierStage::COMPUTE, buf, nbuf,
                        tex, ntex);
      ResolveCensusBarrier();
    }
    ntex = nbuf = 0;
  }
};

// ---- [wwe13 nr] D3D12 copy rules (all no-ops / unchanged paths on Vulkan) ----
// D3D12 copies need COPY_DEST / COPY_SOURCE, but BufferBarrier leaves UAV buffers in UNORDERED_ACCESS. CopyBarrier
// moves a DEFAULT-heap buffer into the copy state before a copy; CopyDone returns a UAV buffer to BufferBarrier's
// state after it. Never call either on UPLOAD/READBACK buffers (their state is fixed).
void CopyBarrier(RenderBuffer* buffer, bool write) {
  if (!IsD3D12()) return;
  RenderBufferBarrier b(buffer, write ? RenderBufferAccess::WRITE : RenderBufferAccess::READ);
  g->list->barriers(RenderBarrierStage::COPY, &b, 1, nullptr, 0);
}
void CopyDone(RenderBuffer* buffer) {
  if (IsD3D12()) BufferBarrier(buffer);
}
// D3D12 placed footprints need a 256-byte row pitch (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT); nr's buffers are packed
// tightly. Tight pitch in bytes of a row of `row_texels` texels, and whether D3D12 needs it re-pitched.
uint32_t TightPitch(RenderFormat f, uint32_t row_texels) {
  const uint32_t bw = RenderFormatBlockWidth(f);
  return (row_texels + bw - 1) / bw * RenderFormatSize(f);
}
bool NeedsRepitch(RenderFormat f, uint32_t row_texels) { return IsD3D12() && TightPitch(f, row_texels) % 256 != 0; }
uint32_t AlignedPitch(RenderFormat f, uint32_t row_texels) { return (TightPitch(f, row_texels) + 255) & ~255u; }
// Row width in texels that makes plume's placed footprint use `pitch` bytes.
uint32_t PitchTexels(RenderFormat f, uint32_t pitch) { return pitch / RenderFormatSize(f) * RenderFormatBlockWidth(f); }
uint32_t BlockRows(RenderFormat f, uint32_t h) {
  const uint32_t bw = RenderFormatBlockWidth(f);
  return (h + bw - 1) / bw;
}
std::unique_ptr<RenderBuffer> RepitchTemp(uint64_t bytes) {
  auto temp = g->device->createBuffer(RenderBufferDesc::DefaultBuffer(bytes));
  if (temp) CopyBarrier(temp.get(), true);  // COMMON -> COPY_DEST, tracked explicitly
  return temp;
}
// Copies `box` of `src` (w x h texels) into `dst` at `dst_offset`, tightly packed. The caller has put `dst` in a
// copy-destination state (CopyBarrier, or a READBACK heap). D3D12 with an unaligned pitch: via an aligned temp.
bool CopyTextureToTight(RenderBuffer* dst, uint64_t dst_offset, RenderFormat f, uint32_t w, uint32_t h,
                        RenderTexture* src, const RenderBox& box, uint32_t mip = 0) {
  if (!NeedsRepitch(f, w)) {
    g->list->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(dst, f, w, h, 1, w, dst_offset),
                               RenderTextureCopyLocation::Subresource(src, mip), 0, 0, 0, &box);
    ResolveCensusCopy(uint64_t(TightPitch(f, w)) * BlockRows(f, h));
    return true;
  }
  const uint32_t tight = TightPitch(f, w), aligned = AlignedPitch(f, w), rows = BlockRows(f, h);
  auto temp = RepitchTemp(uint64_t(aligned) * rows);
  if (!temp) return false;
  g->list->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(temp.get(), f, w, h, 1, PitchTexels(f, aligned)),
                             RenderTextureCopyLocation::Subresource(src, mip), 0, 0, 0, &box);
  ResolveCensusCopy(uint64_t(tight) * rows);
  CopyBarrier(temp.get(), false);
  for (uint32_t r = 0; r < rows; ++r) {
    g->list->copyBufferRegion(RenderBufferReference(dst, dst_offset + uint64_t(r) * tight),
                              RenderBufferReference(temp.get(), uint64_t(r) * aligned), tight);
    ResolveCensusCopy(tight);
  }
  Retire(std::move(temp));
  return true;
}
// Copies a tightly packed w x h image (rows of `row` texels) at `src_offset` of `src` into a texture subresource.
// The caller has put `src` in a copy-source state (CopyBarrier, or an UPLOAD heap). D3D12 unaligned: via a temp.
bool CopyTightToTexture(RenderTexture* dst, uint32_t mip, uint32_t face, RenderBuffer* src, uint64_t src_offset,
                        RenderFormat f, uint32_t w, uint32_t h, uint32_t row, uint32_t source_line = 0) {
  if (!NeedsRepitch(f, row)) {
    g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(dst, mip, face),
                               RenderTextureCopyLocation::PlacedFootprint(src, f, w, h, 1, row, src_offset), 0, 0);
    ResolveCensusCopyAt(uint64_t(TightPitch(f, row)) * BlockRows(f, h), source_line ? source_line : __LINE__);
    return true;
  }
  const uint32_t tight = TightPitch(f, row), aligned = AlignedPitch(f, row), rows = BlockRows(f, h);
  auto temp = RepitchTemp(uint64_t(aligned) * rows);
  if (!temp) return false;
  for (uint32_t r = 0; r < rows; ++r) {
    g->list->copyBufferRegion(RenderBufferReference(temp.get(), uint64_t(r) * aligned),
                              RenderBufferReference(src, src_offset + uint64_t(r) * tight), tight);
    ResolveCensusCopyAt(tight, source_line ? source_line : __LINE__);
  }
  CopyBarrier(temp.get(), false);
  g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(dst, mip, face),
                             RenderTextureCopyLocation::PlacedFootprint(temp.get(), f, w, h, 1, PitchTexels(f, aligned)),
                             0, 0);
  ResolveCensusCopyAt(uint64_t(tight) * rows, source_line ? source_line : __LINE__);
  Retire(std::move(temp));
  return true;
}

void EndDrawPass();

// U7 non-draw consumers retain the prior logical-size contract. The physical image remains the whole-image draw source;
// materialize the old linearly-upscaled (colour) / nearest-upscaled (depth) form only when a reader needs it.
bool EnsureLogicalSnap(uint32_t id, std::string* error) {
  EnsureSnap(id);
  if (!g || id >= g->snapshots.size() || !g->snapshots[id]) {
    if (error) *error = "logical snapshot: bad snapshot";
    return false;
  }
  if (id >= g->snapshot_hi.size() || !g->snapshot_hi[id].logical_pending) return true;
  State::SnapHi& hi = g->snapshot_hi[id];
  if (!hi.image || !hi.logical_width || !hi.logical_height) {
    if (error) *error = "logical snapshot: missing physical extent";
    return false;
  }
  const RenderFormat format = g->snapshot_formats[id];
  std::shared_ptr<RenderTexture> logical(
      NewTexture(RenderTextureDesc::Texture2D(hi.logical_width, hi.logical_height, 1, format)));
  if (!logical) {
    if (error) *error = "logical snapshot: createTexture failed";
    return false;
  }
  if (g->state_elide) EndDrawPass();
  {
    BarrierBatch bb;
    bb.Layout(hi.image.get(), RenderTextureLayout::COPY_SOURCE);
    bb.Layout(logical.get(), RenderTextureLayout::COPY_DEST);
  }
  const bool linear = format != RenderFormat::R32_FLOAT;
  const bool blit = g->list->blitTexture(logical.get(), RenderBox(0, 0, int32_t(hi.logical_width), int32_t(hi.logical_height)),
                                         hi.image.get(), RenderBox(0, 0, int32_t(hi.width), int32_t(hi.height)), linear);
  Barrier(hi.image.get(), RenderTextureLayout::SHADER_READ);
  if (!blit) {
    if (error) *error = "logical snapshot: scaled blit unsupported";
    return false;
  }
  ResolveCensusBlit(uint64_t(TightPitch(format, hi.logical_width)) * BlockRows(format, hi.logical_height));
  Barrier(logical.get(), RenderTextureLayout::SHADER_READ);
  g->snapshots[id] = std::move(logical);
  hi.logical_pending = false;
  ++g_stats.phys_snap_logical_materialized;
  return true;
}

// Whole-snapshot texture handles share SnapHi::image for draws. Refresh that same physical image after a logical
// snapshot write so handles created before materialization observe the updated destination.
bool RefreshPhysicalSnapshotDrawImage(uint32_t id, std::string* error) {
  if (!g || id >= g->snapshot_hi.size() || id >= g->snapshots.size() || id >= g->snapshot_formats.size()) return true;
  const State::SnapHi& hi = g->snapshot_hi[id];
  if (!hi.image || hi.upscale || !hi.width || !hi.height || !hi.logical_width || !hi.logical_height ||
      !g->snapshots[id] || hi.image.get() == g->snapshots[id].get())
    return true;

  RenderTexture* logical = g->snapshots[id].get();
  {
    BarrierBatch bb;
    bb.Layout(logical, RenderTextureLayout::COPY_SOURCE);
    bb.Layout(hi.image.get(), RenderTextureLayout::COPY_DEST);
  }
  const RenderFormat format = g->snapshot_formats[id];
  const bool linear = format != RenderFormat::R32_FLOAT;
  const bool blit = g->list->blitTexture(hi.image.get(), RenderBox(0, 0, int32_t(hi.width), int32_t(hi.height)), logical,
                                         RenderBox(0, 0, int32_t(hi.logical_width), int32_t(hi.logical_height)), linear);
  Barrier(logical, RenderTextureLayout::SHADER_READ);
  Barrier(hi.image.get(), RenderTextureLayout::SHADER_READ);
  if (!blit) {
    if (error) *error = "physical snapshot: refresh draw image unsupported";
    return false;
  }
  ResolveCensusBlit(uint64_t(TightPitch(format, hi.width)) * BlockRows(format, hi.height));
  return true;
}

// U7 CHECK keeps the legacy logical output, downsamples it to the physical extent, and compares raw texels with
// tolerance 0. This is intentionally a readback diagnostic and is not a performance path.
void ComparePhysicalSnapshot(const std::shared_ptr<RenderTexture>& logical, const std::shared_ptr<RenderTexture>& physical,
                             uint32_t logical_w, uint32_t logical_h, uint32_t physical_w, uint32_t physical_h,
                             RenderFormat format, bool linear) {
  ++g_stats.phys_snap_checks;
  if (!logical || !physical || !logical_w || !logical_h || !physical_w || !physical_h) {
    ++g_stats.phys_snap_check_failures;
    return;
  }
  const uint64_t bytes = uint64_t(TightPitch(format, physical_w)) * BlockRows(format, physical_h);
  std::shared_ptr<RenderTexture> downsampled(
      NewTexture(RenderTextureDesc::Texture2D(physical_w, physical_h, 1, format)));
  CountBuffer();
  auto logical_readback = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
  CountBuffer();
  auto physical_readback = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
  if (!downsampled || !logical_readback || !physical_readback) {
    ++g_stats.phys_snap_check_failures;
    return;
  }
  if (g->state_elide) EndDrawPass();
  {
    BarrierBatch bb;
    bb.Layout(logical.get(), RenderTextureLayout::COPY_SOURCE);
    bb.Layout(downsampled.get(), RenderTextureLayout::COPY_DEST);
  }
  if (!g->list->blitTexture(downsampled.get(), RenderBox(0, 0, int32_t(physical_w), int32_t(physical_h)), logical.get(),
                            RenderBox(0, 0, int32_t(logical_w), int32_t(logical_h)), linear)) {
    ++g_stats.phys_snap_check_failures;
    return;
  }
  {
    BarrierBatch bb;
    bb.Layout(downsampled.get(), RenderTextureLayout::COPY_SOURCE);
    bb.Layout(physical.get(), RenderTextureLayout::COPY_SOURCE);
  }
  const RenderBox box(0, 0, int32_t(physical_w), int32_t(physical_h));
  const bool downsample_copy = CopyTextureToTight(logical_readback.get(), 0, format, physical_w, physical_h,
                                                  downsampled.get(), box);
  const bool physical_copy = CopyTextureToTight(physical_readback.get(), 0, format, physical_w, physical_h,
                                                physical.get(), box);
  Barrier(logical.get(), RenderTextureLayout::SHADER_READ);
  Barrier(physical.get(), RenderTextureLayout::SHADER_READ);
  Submit();
  if (!downsample_copy || !physical_copy) {
    ++g_stats.phys_snap_check_failures;
    return;
  }
  const uint8_t* a = static_cast<const uint8_t*>(logical_readback->map());
  const uint8_t* b = static_cast<const uint8_t*>(physical_readback->map());
  if (!a || !b) {
    if (a) logical_readback->unmap();
    if (b) physical_readback->unmap();
    ++g_stats.phys_snap_check_failures;
    return;
  }
  const uint32_t texel_bytes = RenderFormatSize(format);
  uint64_t mismatches = 0, over_tol = 0, max_color = 0, max_depth_e6 = 0;
  const bool is_rgba8 = format == RenderFormat::R8G8B8A8_UNORM, is_r32f = format == RenderFormat::R32_FLOAT;
  for (uint64_t i = 0, n = uint64_t(physical_w) * physical_h; i < n; ++i) {
    const uint8_t* ta = a + i * texel_bytes;
    const uint8_t* tb = b + i * texel_bytes;
    if (std::memcmp(ta, tb, texel_bytes) == 0) continue;
    ++mismatches;
    if (is_rgba8) {
      uint64_t worst = 0;
      for (int c = 0; c < 4; ++c) worst = std::max<uint64_t>(worst, uint64_t(std::abs(int(ta[c]) - int(tb[c]))));
      max_color = std::max(max_color, worst);
      if (worst > 4) ++over_tol;
    } else if (is_r32f) {
      float fa, fb;
      std::memcpy(&fa, ta, 4);
      std::memcpy(&fb, tb, 4);
      const double d = std::fabs(double(fa) - double(fb));
      max_depth_e6 = std::max(max_depth_e6, uint64_t(std::min(d, 1e12) * 1e6));
      if (!(d <= 1e-3)) ++over_tol;
    } else {
      ++over_tol;
    }
  }
  g_stats.phys_snap_over_tol += over_tol;
  g_stats.phys_snap_max_color = std::max(g_stats.phys_snap_max_color, max_color);
  g_stats.phys_snap_max_depth_e6 = std::max(g_stats.phys_snap_max_depth_e6, max_depth_e6);
  logical_readback->unmap();
  physical_readback->unmap();
  g_stats.phys_snap_mismatch_texels += mismatches;
}

// ---- State elision (WWE2K14_NR_STATE_ELIDE=1) bookkeeping ----
// The elide cache of the command list currently being recorded (g->list == cmd_pool[cmd_active].list).
ElideState& Elide() { return g->cmd_pool[g->cmd_active].elide; }
// Ends any open render pass and forgets the tracked framebuffer. Cheap no-op when no pass is open
// (setFramebuffer(nullptr) just clears plume's target; endActiveRenderPass() does nothing). Call it at
// the start of every non-draw entry point that records commands, so the pass left open by an elided draw
// is always closed before an op that is illegal inside a render pass (resolve, dispatch) or that changes
// the target, and so a later draw re-sets the framebuffer and re-issues its barriers.
void EndDrawPass() {
  CrowdFlushBoundary();
  DpScope dp;  // card D1: the end of a render pass left open by elided draws gets its own interval
  if (Elide().fb && DpOn()) dp.Begin(DpOpLabel("pass-end"));
  g->list->setFramebuffer(nullptr);
  Elide().fb = nullptr;
}
// Drop the cached graphics binds (pipeline / layout / sets / viewport / scissor). Needed whenever any
// non-draw graphics operation binds state on this list and when a list begins.
void InvalidateElideBinds() {
  ElideState& e = Elide();
  e.pipeline = nullptr;
  e.graphics_layout = nullptr;
  for (int i = 0; i < 5; ++i) e.sets[i] = nullptr;
  e.check_set4 = nullptr;
  e.has_viewport = false;
  e.has_scissor = false;
}

RenderFormat ToFormat(TexFormat f) {
  switch (f) {
    case TexFormat::kRGBA8: return RenderFormat::R8G8B8A8_UNORM;
    case TexFormat::kBC1: return RenderFormat::BC1_UNORM;
    case TexFormat::kBC2: return RenderFormat::BC2_UNORM;
    case TexFormat::kBC3: return RenderFormat::BC3_UNORM;
    case TexFormat::kR32F: return RenderFormat::R32_FLOAT;
    case TexFormat::kRGBA16: return RenderFormat::R16G16B16A16_UNORM;
    case TexFormat::kRGBA16S: return RenderFormat::R16G16B16A16_SNORM;
    case TexFormat::kR8: return RenderFormat::R8_UNORM;
    case TexFormat::kRGBA8S: return RenderFormat::R8G8B8A8_SNORM;
    case TexFormat::kRG16: return RenderFormat::R16G16_UNORM;
    case TexFormat::kRG16S: return RenderFormat::R16G16_SNORM;
    default: return RenderFormat::UNKNOWN;
  }
}
bool IsBlock(TexFormat f) { return f == TexFormat::kBC1 || f == TexFormat::kBC2 || f == TexFormat::kBC3; }

RenderFormat ToFormat(VtxFormat f) {
  switch (f) {
    case VtxFormat::kR32_UINT: return RenderFormat::R32_UINT;
    case VtxFormat::kR32G32_UINT: return RenderFormat::R32G32_UINT;
    case VtxFormat::kR32G32B32_UINT: return RenderFormat::R32G32B32_UINT;
    case VtxFormat::kR32G32B32A32_UINT: return RenderFormat::R32G32B32A32_UINT;
    case VtxFormat::kR32_SFLOAT: return RenderFormat::R32_FLOAT;
    case VtxFormat::kR32G32_SFLOAT: return RenderFormat::R32G32_FLOAT;
    case VtxFormat::kR32G32B32_SFLOAT: return RenderFormat::R32G32B32_FLOAT;
    case VtxFormat::kR32G32B32A32_SFLOAT: return RenderFormat::R32G32B32A32_FLOAT;
    case VtxFormat::kR8G8B8A8_UNORM: return RenderFormat::R8G8B8A8_UNORM;
    case VtxFormat::kR8G8B8A8_SNORM: return RenderFormat::R8G8B8A8_SNORM;
    case VtxFormat::kR8G8B8A8_UINT: return RenderFormat::R8G8B8A8_UINT;
    case VtxFormat::kR8G8B8A8_SINT: return RenderFormat::R8G8B8A8_SINT;
    case VtxFormat::kR16G16_SFLOAT: return RenderFormat::R16G16_FLOAT;
    case VtxFormat::kR16G16B16A16_SFLOAT: return RenderFormat::R16G16B16A16_FLOAT;
    case VtxFormat::kR16G16_UNORM: return RenderFormat::R16G16_UNORM;
    case VtxFormat::kR16G16_SNORM: return RenderFormat::R16G16_SNORM;
    case VtxFormat::kR16G16_UINT: return RenderFormat::R16G16_UINT;
    case VtxFormat::kR16G16_SINT: return RenderFormat::R16G16_SINT;
    case VtxFormat::kR16G16B16A16_UNORM: return RenderFormat::R16G16B16A16_UNORM;
    case VtxFormat::kR16G16B16A16_SNORM: return RenderFormat::R16G16B16A16_SNORM;
    case VtxFormat::kR16G16B16A16_UINT: return RenderFormat::R16G16B16A16_UINT;
    case VtxFormat::kR16G16B16A16_SINT: return RenderFormat::R16G16B16A16_SINT;
    default: return RenderFormat::UNKNOWN;
  }
}

RenderSwizzle ToSwizzle(uint8_t s) {
  switch (s) {
    case 0: return RenderSwizzle::R;
    case 1: return RenderSwizzle::G;
    case 2: return RenderSwizzle::B;
    case 3: return RenderSwizzle::A;
    case 4: return RenderSwizzle::ZERO;
    default: return RenderSwizzle::ONE;
  }
}

uint32_t SamplerIndex(const SamplerDesc& s) {
  std::string key(reinterpret_cast<const char*>(&s), sizeof(s));
  auto it = g->sampler_index.find(key);
  if (it != g->sampler_index.end()) return it->second;
  RenderSamplerDesc d;
  d.magFilter = s.mag_linear ? RenderFilter::LINEAR : RenderFilter::NEAREST;
  d.minFilter = s.min_linear ? RenderFilter::LINEAR : RenderFilter::NEAREST;
  d.mipmapMode = s.mip_linear ? RenderMipmapMode::LINEAR : RenderMipmapMode::NEAREST;
  auto addr = [](uint32_t a) {
    switch (a) {
      case 0: return RenderTextureAddressMode::WRAP;
      case 1: return RenderTextureAddressMode::MIRROR;
      case 2: return RenderTextureAddressMode::CLAMP;
      case 3: return RenderTextureAddressMode::MIRROR_ONCE;
      default: return RenderTextureAddressMode::BORDER;
    }
  };
  d.addressU = addr(s.address_u);
  d.addressV = addr(s.address_v);
  d.addressW = addr(s.address_w);
  d.borderColor = s.border == 2   ? RenderBorderColor::OPAQUE_WHITE
                  : s.border == 1 ? RenderBorderColor::OPAQUE_BLACK
                                  : RenderBorderColor::TRANSPARENT_BLACK;
  d.anisotropyEnabled = s.max_anisotropy > 0;
  d.maxAnisotropy = s.max_anisotropy ? s.max_anisotropy : 1;
  d.mipLODBias = s.lod_bias;
  d.minLOD = s.min_lod;
  d.maxLOD = s.max_lod;
  if (g->samplers.size() >= g->sampler_table_size) {
    // [wwe13 nr] Only reachable on D3D12 (table 2000); Vulkan's 4096 is unchanged. Fall back to the default sampler (0), loudly.
    if (!g->sampler_overflow_logged) {
      g->sampler_overflow_logged = true;
      std::fprintf(stderr, "nr: SAMPLER TABLE FULL (%u entries): further distinct samplers fall back to sampler 0\n",
                   g->sampler_table_size);
    }
    return 0;
  }
  auto sampler = g->device->createSampler(d);
  const uint32_t index = uint32_t(g->samplers.size());
  g->tables[3]->setSampler(index, sampler.get());
  g->samplers.push_back(std::move(sampler));
  g->sampler_index[key] = index;
  return index;
}

RenderShader* Shader(uint64_t key, const std::vector<uint8_t>* spirv) {
  if (!spirv || spirv->empty()) return nullptr;
  RecordMutexLock lock(g_pipeline_create_mu, "shader_cache_mutex");
  auto it = g->shaders.find(key);
  if (it != g->shaders.end()) return it->second.get();
  auto shader = g->device->createShader(spirv->data(), spirv->size(), "main", g->shader_format);
  RenderShader* raw = shader.get();
  g->shaders[key] = std::move(shader);
  return raw;
}


// Library shader file name: SPIR-V <stem>.spv (unchanged); D3D12 <stem>.s0.dxil (tools/nr/build-shaders-dxil.sh).
std::string LibShaderPath(const std::string& dir, const std::string& stem) {
  return dir + "/" + stem + (g->shader_format == RenderShaderFormat::SPIRV ? ".spv" : ".s0.dxil");
}

}  // namespace

namespace {
RenderFormat ToFormat(TargetFormat f) {
  switch (f) {
    case TargetFormat::kRGBA8: return RenderFormat::R8G8B8A8_UNORM;
    case TargetFormat::kRGBA16F: return RenderFormat::R16G16B16A16_FLOAT;
    case TargetFormat::kR32F: return RenderFormat::R32_FLOAT;
    case TargetFormat::kRG16F: return RenderFormat::R16G16_FLOAT;
    case TargetFormat::kRG32F: return RenderFormat::R32G32_FLOAT;
    case TargetFormat::kRG16S: return RenderFormat::R16G16_SNORM;
    case TargetFormat::kRGBA16S: return RenderFormat::R16G16B16A16_SNORM;
    case TargetFormat::kD24S8: return RenderFormat::D24_UNORM_S8_UINT;
    case TargetFormat::kD24FS8: return RenderFormat::D32_FLOAT_S8_UINT;
  }
  return RenderFormat::UNKNOWN;
}
}  // namespace

bool ResolveCensusEnabled() { return ResolveCensusOn(); }
ResolveCensusScope::ResolveCensusScope(const ResolveCensusKey& key) {
  if (!ResolveCensusOn() || !key.valid) return;
  previous_ = t_resolve_census_key;
  previous_valid_ = t_resolve_census_active;
  t_resolve_census_key = key;
  t_resolve_census_active = true;
  active_ = true;
}
ResolveCensusScope::~ResolveCensusScope() {
  if (!active_) return;
  t_resolve_census_key = previous_;
  t_resolve_census_active = previous_valid_;
}
ResolveCensusDrawScope::ResolveCensusDrawScope(uint64_t frame) {
  if (!ResolveCensusOn()) return;
  t_resolve_census_draw = true;
  t_resolve_census_draw_frame = frame;
  t_resolve_census_draw_destinations.clear();
  active_ = true;
}
ResolveCensusDrawScope::~ResolveCensusDrawScope() {
  if (!active_) return;
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  for (uint32_t base : t_resolve_census_draw_destinations) {
    const auto active = g_resolve_census_active.find(base);
    if (active == g_resolve_census_active.end()) continue;
    ++active->second.consumers;
    CensusGlobal(&ResolveCensusCounts::consumers);
    CensusFrameEvent(t_resolve_census_draw_frame, &ResolveCensusFrame::consumers);
  }
  t_resolve_census_draw_destinations.clear();
  t_resolve_census_draw = false;
  t_resolve_census_draw_frame = 0;
}
void ResolveCensusResolve(const ResolveCensusKey& key) {
  if (!ResolveCensusOn() || !key.valid) return;
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  CensusGlobal(&ResolveCensusCounts::resolves);
  ++CensusPendingRow(key).counts.resolves;
  auto old = g_resolve_census_active.find(key.dest_base);
  if (old != g_resolve_census_active.end()) {
    CensusPendingRow(old->second.key).counts.consumers += old->second.consumers;
    g_resolve_census_active.erase(old);
  }
  if (key.resolve_class == 1 || key.resolve_class == 2 || key.resolve_class == 3 || key.resolve_class == 4 ||
      key.resolve_class == 7) {
    g_resolve_census_active.emplace(key.dest_base, ActiveResolveCensus{key, 0});
  }
}
void ResolveCensusConsumer(uint32_t dest_base) {
  if (!ResolveCensusOn() || !t_resolve_census_draw) return;
  for (uint32_t base : t_resolve_census_draw_destinations) if (base == dest_base) return;
  t_resolve_census_draw_destinations.push_back(dest_base);
}
void ResolveCensusImage() {
  if (!ResolveCensusOn()) return;
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  CensusGlobal(&ResolveCensusCounts::images);
  if (t_resolve_census_active && t_resolve_census_key.valid)
    ++CensusPendingRow(t_resolve_census_key).counts.images;
  CensusFrameEvent(CensusEventFrame(), &ResolveCensusFrame::images);
}
void ResolveCensusCopyAt(uint64_t bytes, uint32_t source_line) { CensusRecordCopy(bytes, false, source_line); }
void ResolveCensusBlit(uint64_t bytes) { CensusRecordCopy(bytes, true); }
void ResolveCensusCompute() {
  if (!ResolveCensusOn()) return;
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  CensusGlobal(&ResolveCensusCounts::compute);
  if (t_resolve_census_active && t_resolve_census_key.valid)
    ++CensusPendingRow(t_resolve_census_key).counts.compute;
  CensusFrameEvent(CensusEventFrame(), &ResolveCensusFrame::compute);
}
void ResolveCensusMsaaResolve(uint64_t bytes) {
  if (!ResolveCensusOn()) return;
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  CensusGlobal(&ResolveCensusCounts::msaa_resolves);
  g_resolve_census_pending_total.bytes_copied += bytes;
  if (t_resolve_census_active && t_resolve_census_key.valid) {
    ResolveCensusCounts& c = CensusPendingRow(t_resolve_census_key).counts;
    ++c.msaa_resolves;
    c.bytes_copied += bytes;
  }
  CensusFrameEvent(CensusEventFrame(), &ResolveCensusFrame::msaa_resolves);
}
void ResolveCensusBarrier() {
  if (!ResolveCensusOn()) return;
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  CensusGlobal(&ResolveCensusCounts::barriers);
  if (t_resolve_census_active && t_resolve_census_key.valid)
    ++CensusPendingRow(t_resolve_census_key).counts.barriers;
  CensusFrameEvent(CensusEventFrame(), &ResolveCensusFrame::barriers);
}
void ResolveCensusReadback(uint64_t bytes) {
  if (!ResolveCensusOn()) return;
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  CensusGlobal(&ResolveCensusCounts::cpu_readbacks);
  g_resolve_census_pending_total.cpu_readback_bytes += bytes;
  if (t_resolve_census_active && t_resolve_census_key.valid) {
    ResolveCensusCounts& c = CensusPendingRow(t_resolve_census_key).counts;
    ++c.cpu_readbacks;
    c.cpu_readback_bytes += bytes;
  }
}
void ResolveCensusGuestWriteback(uint64_t bytes) {
  if (!ResolveCensusOn()) return;
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  CensusGlobal(&ResolveCensusCounts::guest_writebacks);
  g_resolve_census_pending_total.guest_writeback_bytes += bytes;
  if (t_resolve_census_active && t_resolve_census_key.valid) {
    ResolveCensusCounts& c = CensusPendingRow(t_resolve_census_key).counts;
    ++c.guest_writebacks;
    c.guest_writeback_bytes += bytes;
  }
}
void ResolveCensusFetchBind(const ResolveCensusKey& resolve_key, const ResolveCensusFetchKey& key, uint64_t frame, bool rebuilt,
                            uint64_t guest_detile_bytes, uint64_t guest_upload_bytes, uint64_t resolve_paste_bytes) {
  if (!ResolveCensusOn() || !resolve_key.valid) return;
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  ResolveCensusFetchCounts& c = CensusPendingRow(resolve_key).counts.fetches[key];
  ++c.binds;
  c.rebuilds += rebuilt;
  c.guest_detile_bytes += guest_detile_bytes;
  c.guest_upload_bytes += guest_upload_bytes;
  c.resolve_paste_bytes += resolve_paste_bytes;
  (void)frame;  // caller supplies the frame for explicit attribution; per-frame frequency uses the census window cadence.
}
std::vector<ResolveCensusRow> TakeResolveCensusWindow() {
  std::vector<ResolveCensusRow> rows;
  if (!ResolveCensusOn()) return rows;
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  rows.reserve(g_resolve_census_pending.size());
  for (auto& entry : g_resolve_census_pending) rows.push_back(std::move(entry.second));
  g_resolve_census_pending.clear();
  ResolveCensusRow total{};
  total.key.valid = false;
  total.counts = g_resolve_census_pending_total;
  rows.push_back(std::move(total));
  g_resolve_census_pending_total = ResolveCensusCounts{};
  return rows;
}
ResolveCensusFrame TakeResolveCensusFrame(uint64_t frame) {
  if (!ResolveCensusOn()) return {};
  std::lock_guard<std::mutex> lock(g_resolve_census_mutex);
  g_resolve_census_current_frame = frame + 1;
  auto it = g_resolve_census_frames.find(frame);
  if (it == g_resolve_census_frames.end()) return {};
  const ResolveCensusFrame result = it->second;
  g_resolve_census_frames.erase(it);
  return result;
}

uint32_t TargetBytesPerPixel(TargetFormat f) {
  return f == TargetFormat::kRGBA16F || f == TargetFormat::kRG32F || f == TargetFormat::kRGBA16S ? 8 : 4;
}

bool Ready() { return g != nullptr; }
bool FixAltEnabled() { return g && g->fix_alt_scheduler.enabled(); }
int FixAltPeriodSeconds() { return g ? g->fix_alt_scheduler.period_seconds() : 0; }
bool FixAltPhaseB() { return g && g->fix_alt_scheduler.phase_b(); }
int FixAltFrame(uint64_t now_ns) {
  if (!FixAltEnabled()) return -1;
  const int phase = g->fix_alt_scheduler.Tick(now_ns);
  if (phase >= 0) SetFixAltPhaseB(phase == 1);
  return phase;
}
uint32_t BackendWaitCode() { return g_backend_wait.load(std::memory_order_relaxed); }  // card S3-29b
void SetRetireByFrame(bool on) {
  if (g) g->retire_by_frame = on;
}
void SetLazySnapshots(bool on) { g_lazy_on = on; }
void LazySnapshotStats(uint64_t out[5]) {
  out[0] = g_lazy_stats.created, out[1] = g_lazy_stats.materialized, out[2] = g_lazy_stats.by_reader;
  out[3] = g_lazy_stats.by_write, out[4] = g_lazy_stats.skipped;
}
bool ShaderFormatIsDxil() { return g && g->shader_format == RenderShaderFormat::DXIL; }
void SetProfiling(bool on) { g_prof = on; }
// Night 11-18 (docs/tasks/NIGHT-1118-PLAN.md): WWE2K14_NR_RECORD_SLOTS = record-queue slots in flight (default 128 =
// before; 2..512), _AB=N: the new value only in odd blocks of N frames. sync-why2 measured 60 ring_full waits/f.
void RecordQueueFrame(uint64_t frame) {
  static const uint32_t slots = [] {
    const char* e = std::getenv("WWE2K14_NR_RECORD_SLOTS");
    const unsigned long v = e && *e ? std::strtoul(e, nullptr, 10) : 0;
    return v >= 2 && v <= kRecordSlotsMax ? uint32_t(v) : 128u;
  }();
  static const uint32_t ab = [] {
    const char* e = std::getenv("WWE2K14_NR_RECORD_SLOTS_AB");
    const unsigned long v = e && *e ? std::strtoul(e, nullptr, 10) : 0;
    return v <= 1000000 ? uint32_t(v) : 0u;
  }();
  static bool logged = false;
  if (!logged && (slots != 128 || ab)) {
    logged = true;
    std::fprintf(stderr, "[nr] record queue slots=%u (default 128) ab=%u\n", slots, ab);
  }
  if (!g_rq) return;
  g_rq->SetCapacity((!ab || ((frame / ab) & 1u)) ? slots : 128u);
}
std::string BatchDrainStatsLine() {
  if (!g_batch_stats) return {};
  std::ostringstream out;
  bool first = true;
  out.setf(std::ios::fixed);
  out.precision(3);
  for (uint32_t i = 0; i < kBatchDrainKindCount; ++i) {
    const uint64_t calls = g_stats.batch_drain_calls[i];
    const uint64_t waits = g_stats.batch_drain_waits[i];
    const uint64_t ns = g_stats.batch_drain_ns[i];
    if (!calls && !waits) continue;
    out << (first ? "" : " ") << kBatchDrainNames[i] << '=' << calls << '/' << waits << '/' << (ns * 1e-6);
    first = false;
  }
  return out.str();
}
void RecordBatchFrameEnd(uint64_t frame) {
  if (!g_batch_check || !g_rq) return;
  SyncRecordThread(kProfFnCount, true, kBatchDrainFrameTail);
  const bool match = g_batch_producer_stream == g_batch_consumer_stream &&
                     g_batch_producer_ops == g_batch_consumer_ops;
  ++g_batch_checked_frames;
  if (!match) ++g_batch_check_mismatches;
  ++g_stats.batch_check_frames;
  if (!match) ++g_stats.batch_check_mismatches;
  if (!match || frame == 1 || frame % 600 == 0) {
    std::fprintf(stderr,
                 "[nr-batch-check] frame=%llu checked=%llu mismatches=%llu equal=%u ops=%llu/%llu hash=%016llX/%016llX\n",
                 (unsigned long long)frame, (unsigned long long)g_batch_checked_frames,
                 (unsigned long long)g_batch_check_mismatches, match ? 1u : 0u,
                 (unsigned long long)g_batch_producer_ops, (unsigned long long)g_batch_consumer_ops,
                 (unsigned long long)g_batch_producer_stream, (unsigned long long)g_batch_consumer_stream);
  }
  g_batch_producer_stream = g_batch_consumer_stream = kBatchStreamSeed;
  g_batch_producer_ops = g_batch_consumer_ops = 0;
}
// Card V1 round 3 (review F1): the ONE heap the rt-evict trigger and its per-image placement checks use: the first
// DEVICE_LOCAL heap (-1 = none). [nr-mem-owners] attributes heap 0; a device whose first device-local heap is not 0 is
// logged once (no such device in any log so far: every heap line starts with h0L).
int TriggerHeapIndex(const VkPhysicalDeviceMemoryProperties* mp) {
  int idx = -1;
  for (uint32_t i = 0; mp && i < mp->memoryHeapCount; ++i) {
    if (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
      idx = int(i);
      break;
    }
  }
  static bool logged = false;
  if (!logged && mp) {
    logged = true;
    if (idx != 0)
      std::fprintf(stderr, "[nr] rt-evict trigger heap = %d (not heap 0): [nr-mem-owners] rt= is not the same partition\n",
                   idx);
  }
  return idx;
}
bool LocalHeapBudget(uint64_t* usage, uint64_t* budget) {
  if (!g || !g->device || IsD3D12()) return false;
  auto* vk = static_cast<plume::VulkanDevice*>(g->device.get());
  if (!vk->allocator) return false;
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
  vmaGetHeapBudgets(vk->allocator, budgets);
  const VkPhysicalDeviceMemoryProperties* mp = nullptr;
  vmaGetMemoryProperties(vk->allocator, &mp);
  const int i = TriggerHeapIndex(mp);
  if (i < 0) return false;
  *usage = budgets[i].usage;
  *budget = budgets[i].budget;
  return true;
}
uint64_t SurfaceDeviceLocalBytes() {
  if (!g || !g->device || IsD3D12()) return 0;
  SyncRecordThread(kProfFnCount, false, kBatchDrainValueCall);  // the record thread may be creating surfaces
  auto* vk = static_cast<plume::VulkanDevice*>(g->device.get());
  if (!vk->allocator) return 0;
  const VkPhysicalDeviceMemoryProperties* props = nullptr;
  vmaGetMemoryProperties(vk->allocator, &props);
  const int heap = TriggerHeapIndex(props);
  if (!props || heap < 0) return 0;
  uint64_t bytes = 0;
  auto add = [&](RenderTexture* image) {
    if (!image) return;
    auto* r = static_cast<plume::VulkanTexture*>(image);
    if (!r->allocation) return;
    const uint32_t mt = r->allocationInfo.memoryType;
    if (mt < props->memoryTypeCount && props->memoryTypes[mt].heapIndex == uint32_t(heap)) bytes += r->allocationInfo.size;
  };
  auto add_surface = [&](const SurfaceObj& s) {
    add(s.image.get());
    add(s.resolved.get());
    if (s.alt) {
      add(s.alt->image.get());
      add(s.alt->resolved.get());
    }
  };
  for (const SurfaceObj& s : g->surfaces) add_surface(s);
  for (const auto& r : g->retired_surfaces) add_surface(r.obj);
  return bytes;
}
// Card V1 round 3: trigger-heap bytes of one live surface (colour/depth image, MSAA resolved copy, scaled alt set).
uint64_t SurfaceLocalHeapBytes(uint32_t handle) {
  if (!g || !g->device || IsD3D12()) return 0;
  SyncRecordThread(kProfFnCount, false, kBatchDrainValueCall);  // the record thread may be creating surfaces
  if (!LiveSurface(handle)) return 0;
  auto* vk = static_cast<plume::VulkanDevice*>(g->device.get());
  if (!vk->allocator) return 0;
  const VkPhysicalDeviceMemoryProperties* props = nullptr;
  vmaGetMemoryProperties(vk->allocator, &props);
  const int heap = TriggerHeapIndex(props);
  if (!props || heap < 0) return 0;
  uint64_t bytes = 0;
  auto add = [&](RenderTexture* image) {
    if (!image) return;
    auto* r = static_cast<plume::VulkanTexture*>(image);
    if (!r->allocation) return;
    const uint32_t mt = r->allocationInfo.memoryType;
    if (mt < props->memoryTypeCount && props->memoryTypes[mt].heapIndex == uint32_t(heap)) bytes += r->allocationInfo.size;
  };
  const SurfaceObj& s = g->surfaces[handle];
  add(s.image.get());
  add(s.resolved.get());
  if (s.alt) {
    add(s.alt->image.get());
    add(s.alt->resolved.get());
  }
  return bytes;
}
bool GetSurfaceMemoryInfo(const std::vector<uint32_t>& handles, std::vector<SurfaceMemoryInfo>* out) {
  if (!out) return false;
  if (BatchCall()) {
    uint64_t hash = BatchHashWords("surface_memory_info", {handles.size()});
    for (size_t i = 0; i < handles.size(); ++i)
      hash = BatchHashWords("surface_memory_info_item", {hash, i, handles[i]});
    return QueueSyncCall(kBatchDrainValueCall, hash, [handles, out] { return GetSurfaceMemoryInfo(handles, out); });
  }
  if (!g || !g->device || IsD3D12()) return false;
  SyncRecordThread(kProfFnCount, false, kBatchDrainValueCall);
  if (!g->retired_surfaces.empty()) ReapSurfaces(false);
  auto* vk = static_cast<plume::VulkanDevice*>(g->device.get());
  if (!vk->allocator) return false;
  const VkPhysicalDeviceMemoryProperties* props = nullptr;
  vmaGetMemoryProperties(vk->allocator, &props);
  const int heap = TriggerHeapIndex(props);
  if (!props || heap < 0) return false;

  out->assign(handles.size(), SurfaceMemoryInfo{});
  auto add = [&](RenderTexture* image, uint64_t* bytes) {
    if (!image) return;
    auto* r = static_cast<plume::VulkanTexture*>(image);
    if (!r->allocation) return;
    const uint32_t mt = r->allocationInfo.memoryType;
    if (mt < props->memoryTypeCount && props->memoryTypes[mt].heapIndex == uint32_t(heap))
      *bytes += r->allocationInfo.size;
  };
  auto add_surface = [&](const SurfaceObj& s, uint64_t* bytes) {
    add(s.image.get(), bytes);
    add(s.resolved.get(), bytes);
    for (const auto& image : s.fsr1_phys) add(image.get(), bytes);
    if (s.alt) {
      add(s.alt->image.get(), bytes);
      add(s.alt->resolved.get(), bytes);
      for (const auto& image : s.alt->fsr1_phys) add(image.get(), bytes);
    }
  };
  std::unordered_map<uint32_t, const State::RetiredSurface*> retired;
  for (const State::RetiredSurface& surface : g->retired_surfaces) retired.emplace(surface.handle, &surface);
  for (size_t i = 0; i < handles.size(); ++i) {
    const uint32_t handle = handles[i];
    if (LiveSurface(handle)) {
      (*out)[i].live = true;
      for (const LazySnap& pending : g_lazy)
        if (pending.surface == handle) {
          (*out)[i].pending_read = true;
          break;
        }
      add_surface(g->surfaces[handle], &(*out)[i].local_heap_bytes);
    } else {
      auto it = retired.find(handle);
      if (it != retired.end()) {
        (*out)[i].retired = true;
        add_surface(it->second->obj, &(*out)[i].local_heap_bytes);
      }
    }
  }
  return true;
}
std::string HeapBudgets() {
  if (!g || !g->device || IsD3D12()) return {};
  auto* vk = static_cast<plume::VulkanDevice*>(g->device.get());
  if (!vk->allocator) return {};
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
  vmaGetHeapBudgets(vk->allocator, budgets);
  const VkPhysicalDeviceMemoryProperties* mp = nullptr;
  vmaGetMemoryProperties(vk->allocator, &mp);
  std::string out;
  for (uint32_t i = 0; mp && i < mp->memoryHeapCount; ++i) {
    char b[96];
    std::snprintf(b, sizeof(b), "%sh%u%s=%.0f/%.0fMB(alloc %.0f)", out.empty() ? "" : " ", i,
                  (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "L" : "", budgets[i].usage / 1048576.0,
                  budgets[i].budget / 1048576.0, budgets[i].statistics.blockBytes / 1048576.0);
    out += b;
  }
  // Card U2: WWE2K14_NR_MEM_BUDGET mode (budgets above are the driver's when > 0) and over-budget fallback count.
  if (vk->k14MemBudgetMode > 0) {
    out += " budget_mode=" + std::to_string(vk->k14MemBudgetMode) +
           " over_budget_fallbacks=" + std::to_string(vk->k14OverBudgetFallbacks.load(std::memory_order_relaxed)) +
           " steered=" + std::to_string(vk->k14OverBudgetSteered.load(std::memory_order_relaxed)) +
           " budget_pct=" + std::to_string(vk->k14MemBudgetPct) +  // card AMD780M (plume k14 M1)
           " reuse_admitted=" + std::to_string(vk->k14BudgetReuseAdmitted.load(std::memory_order_relaxed));
  }
  return out;
}

std::string MemKindsOnGrowth() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_NR_MEM_KINDS");
    return !(e && e[0] == '0');
  }();
  static uint64_t next_step = 256ull << 20;  // logging thread only (targets.cpp periodic)
  static int lines = 0;
  if (!on || lines >= 16 || !g || !g->device || IsD3D12()) return {};
  auto* vk = static_cast<plume::VulkanDevice*>(g->device.get());
  if (!vk->allocator) return {};
  VmaTotalStatistics total = {};
  vmaCalculateStatistics(vk->allocator, &total);
  const uint64_t blocks = total.total.statistics.blockBytes;
  if (blocks < next_step) return {};
  while (next_step <= blocks) next_step += 256ull << 20;
  char* json = nullptr;
  vmaBuildStatsString(vk->allocator, &json, VK_TRUE);
  if (!json) return {};
  const nb_vma::Kinds k = nb_vma::Parse(json);
  const uint64_t rt = k.rt, tex = k.tex, buf = k.buf, other = k.other, n_rt = k.n_rt, n_tex = k.n_tex, n_buf = k.n_buf,
                 big_tex = k.big_tex;
  vmaFreeStatsString(vk->allocator, json);
  ++lines;
  char b[320];
  std::snprintf(b, sizeof(b), "vma_blocks=%lluMB used=%lluMB | targets=%lluMB (%llu) textures=%lluMB (%llu, largest %lluKB) "
                "buffers=%lluMB (%llu) other=%lluMB | ", (unsigned long long)(blocks >> 20),
                (unsigned long long)(total.total.statistics.allocationBytes >> 20), (unsigned long long)(rt >> 20),
                (unsigned long long)n_rt, (unsigned long long)(tex >> 20), (unsigned long long)n_tex,
                (unsigned long long)(big_tex >> 10), (unsigned long long)(buf >> 20), (unsigned long long)n_buf,
                (unsigned long long)(other >> 20));
  return b + HeapBudgets();
}

uint32_t MemLeanMode() {
  static const uint32_t bits = [] {
    const char* e = std::getenv("WWE2K14_NR_MEM_LEAN");
    return e && *e ? uint32_t(std::strtoul(e, nullptr, 0)) : 0u;
  }();
  return bits;
}

bool MemoryOwnersDue() {
  if (!g || IsD3D12() || !g->mem_owners_on) return false;
  ++g->mem_present;
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last{};
  const Clock::time_point now = Clock::now();
  if (last.time_since_epoch().count() == 0) {
    last = now;
    return false;
  }
  if (now - last < std::chrono::seconds(10)) return false;
  last = now;
  return true;
}

std::string MemoryOwnersLine(const MemoryOwnerFrontendAges& frontend) {
  if (!g || IsD3D12() || !g->mem_owners_on || !g->device) return {};
  auto* vk = static_cast<plume::VulkanDevice*>(g->device.get());
  if (!vk->allocator) return {};
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
  vmaGetHeapBudgets(vk->allocator, budgets);
  const VkPhysicalDeviceMemoryProperties* props = nullptr;
  vmaGetMemoryProperties(vk->allocator, &props);
  if (!props || !props->memoryHeapCount) return {};

  struct Gauge { uint64_t bytes = 0, allocations = 0; };
  Gauge tex, rt, dest_images, snapshots, rings, vb_cache;
  uint64_t texture_old = 0, rt_slots = 0, rt_old = 0, snapshot_entries = 0, snapshot_old = 0, ring_old = 0;
  const bool h0_local = (props->memoryHeaps[0].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
  // GAP-mem1: a discrete card keeps upload memory outside heap 0; this env counts every heap (the UHD has only one).
  static const bool all_heaps = [] { const char* e = std::getenv("WWE2K14_NR_MEM_OWNERS_ALL"); return e && *e == '1'; }();
  uint64_t snap_pool_real = 0, snap_big_bytes = 0, snap_big_n = 0;
  auto old600 = [&](uint64_t used) {
    return g->mem_present > used && g->mem_present - used > 600;
  };
  auto add_image = [&](RenderTexture* image, Gauge& owner) {
    if (!h0_local || !image) return;
    auto* resource = static_cast<plume::VulkanTexture*>(image);
    if (!resource->allocation) return;
    const uint32_t mt = resource->allocationInfo.memoryType;
    if (mt >= props->memoryTypeCount || (!all_heaps && props->memoryTypes[mt].heapIndex != 0)) return;
    owner.bytes += resource->allocationInfo.size;
    ++owner.allocations;
  };
  auto add_buffer = [&](RenderBuffer* buffer, Gauge& owner) {
    if (!h0_local || !buffer) return false;
    auto* resource = static_cast<plume::VulkanBuffer*>(buffer);
    if (!resource->allocation) return false;
    const uint32_t mt = resource->allocationInfo.memoryType;
    if (mt >= props->memoryTypeCount || (!all_heaps && props->memoryTypes[mt].heapIndex != 0)) return false;
    owner.bytes += resource->allocationInfo.size;
    ++owner.allocations;
    return true;
  };

  // Snapshot IDs and shared holders can refer to one image. Gather pointer values and sort/unique once per 10 s sample;
  // this does no allocation-identity hashing and does not run on draw/texture hot paths.
  std::vector<uintptr_t> snap_ptrs;
  std::vector<uintptr_t> dest_ptrs;
  std::vector<uint32_t> resolve_ids = frontend.resolve_snapshot_ids;
  std::sort(resolve_ids.begin(), resolve_ids.end());
  resolve_ids.erase(std::unique(resolve_ids.begin(), resolve_ids.end()), resolve_ids.end());
  auto add_snap = [&](const std::shared_ptr<RenderTexture>& image) {
    if (image) snap_ptrs.push_back(reinterpret_cast<uintptr_t>(image.get()));
  };
  auto add_snap_raw = [&](RenderTexture* image) {
    if (image) snap_ptrs.push_back(reinterpret_cast<uintptr_t>(image));
  };
  auto add_surface = [&](SurfaceObj& surface, bool count_slot) {
    if (count_slot && (surface.image || surface.resolved)) {
      ++rt_slots;
      if (old600(surface.mem_last_use_present)) ++rt_old;
    }
    add_image(surface.image.get(), rt);
    add_image(surface.resolved.get(), rt);
    for (const auto& image : surface.fsr1_phys) add_snap(image);
    if (surface.alt) {
      add_image(surface.alt->image.get(), rt);
      add_image(surface.alt->resolved.get(), rt);
      for (const auto& image : surface.alt->fsr1_phys) add_snap(image);
    }
  };
  auto add_texture = [&](Texture* texture) {
    if (!texture) return;
    if (texture->texture) {
      add_image(texture->texture.get(), tex);
      if (static_cast<plume::VulkanTexture*>(texture->texture.get())->allocation) {
        // Only count old image entries that actually have a heap-0 device-local allocation.
        auto* resource = static_cast<plume::VulkanTexture*>(texture->texture.get());
        const uint32_t mt = resource->allocationInfo.memoryType;
        if (h0_local && resource->allocation && mt < props->memoryTypeCount &&
            props->memoryTypes[mt].heapIndex == 0 && old600(texture->mem_last_use_present)) ++texture_old;
      }
    }
    if (texture->shared) add_snap(texture->shared);
  };
  auto add_frame_snapshots = [&](const FrameRes& frame) {
    for (const auto& image : frame.retired_snapshots) add_snap(image);
    for (const auto& texture : frame.retired_textures) add_texture(texture.get());
  };
  auto add_frame_rings = [&](const FrameRes& frame) {
    if (add_buffer(frame.constants.get(), rings) && old600(g->mem_ring_last_use_present)) ++ring_old;
    if (add_buffer(frame.upload_ring.get(), rings) && old600(g->mem_ring_last_use_present)) ++ring_old;
    for (const auto& buffer : frame.retired_buffers) add_buffer(buffer.get(), rings);
  };

  for (auto& texture : g->textures) add_texture(texture.get());
  for (auto& texture : g->retired_textures) add_texture(texture.get());
  for (auto& surface : g->surfaces) add_surface(surface, true);
  for (auto& surface : g->retired_surfaces) add_surface(surface.obj, true);
  for (const auto& image : g->snapshots) add_snap(image);
  for (const auto& hi : g->snapshot_hi) add_snap(hi.image);
  for (const auto& image : g->retired_snapshots) add_snap(image);
  for (const auto& entry : g->snap_pool) {
    add_snap(entry.img);
    if (old600(entry.mem_last_use_present)) ++snapshot_old;
  }
  for (const auto& entry : g->f16_lean_pool) {
    add_snap(entry.img);
    if (old600(entry.mem_last_use_present)) ++snapshot_old;
  }
  for (const auto& view : g->present_views) add_snap(view.image);
  for (const auto& image : g->fsr1_ring) add_snap(image);
  for (const auto& surface : g->surfaces)
    for (const auto& image : surface.fsr1_phys) add_snap(image);
  for (const auto& surface : g->retired_surfaces)
    for (const auto& image : surface.obj.fsr1_phys) add_snap(image);
  for (const auto& frame : g->parked) {
    for (const auto& image : frame.retired_snapshots) add_snap(image);
    for (const auto& texture : frame.retired_textures) add_snap(texture ? texture->shared : std::shared_ptr<RenderTexture>{});
  }
  for (const auto& frame : g->retire_q) {
    for (const auto& image : frame.snapshots) add_snap(image);
    for (const auto& texture : frame.textures) add_snap(texture ? texture->shared : std::shared_ptr<RenderTexture>{});
  }
  for (uint32_t id : resolve_ids) {
    if (id < g->snapshots.size()) add_snap_raw(g->snapshots[id].get());
    if (id < g->snapshot_hi.size()) add_snap(g->snapshot_hi[id].image);
    if (id < g->snapshots.size() && g->snapshots[id])
      dest_ptrs.push_back(reinterpret_cast<uintptr_t>(g->snapshots[id].get()));
    if (id < g->snapshot_hi.size() && g->snapshot_hi[id].image)
      dest_ptrs.push_back(reinterpret_cast<uintptr_t>(g->snapshot_hi[id].image.get()));
  }
  add_frame_snapshots(g->fence_spare);
  std::sort(snap_ptrs.begin(), snap_ptrs.end());
  snap_ptrs.erase(std::unique(snap_ptrs.begin(), snap_ptrs.end()), snap_ptrs.end());
  std::sort(dest_ptrs.begin(), dest_ptrs.end());
  dest_ptrs.erase(std::unique(dest_ptrs.begin(), dest_ptrs.end()), dest_ptrs.end());
  for (uintptr_t ptr : snap_ptrs) {
    Gauge& owner = std::binary_search(dest_ptrs.begin(), dest_ptrs.end(), ptr) ? dest_images : snapshots;
    add_image(reinterpret_cast<RenderTexture*>(ptr), owner);
    auto* snap_res = static_cast<plume::VulkanTexture*>(reinterpret_cast<RenderTexture*>(ptr));
    if (snap_res->allocation && snap_res->allocationInfo.size >= (2ull << 20)) {
      snap_big_bytes += snap_res->allocationInfo.size;
      ++snap_big_n;
    }
  }
  for (const auto& entry : g->snap_pool)
    if (entry.img) snap_pool_real += static_cast<plume::VulkanTexture*>(entry.img.get())->allocationInfo.size;
  for (size_t i = 0; i < g->snapshots.size() && i < g->snapshot_last_use_present.size(); ++i) {
    if (!g->snapshots[i]) continue;
    if (std::binary_search(resolve_ids.begin(), resolve_ids.end(), uint32_t(i))) continue;
    ++snapshot_entries;
    if (old600(g->snapshot_last_use_present[i])) ++snapshot_old;
  }
  for (const auto& frame : g->parked) {
    for (const auto& texture : frame.retired_textures) add_texture(texture.get());
    add_frame_rings(frame);
  }
  for (const auto& frame : g->retire_q)
  {
    for (const auto& texture : frame.textures) add_texture(texture.get());
    for (const auto& buffer : frame.buffers) add_buffer(buffer.get(), rings);
  }
  for (const auto& buffer : g->retired_buffers) add_buffer(buffer.get(), rings);
  add_frame_rings(g->fence_spare);
  if (add_buffer(g->constants.get(), rings) && old600(g->mem_ring_last_use_present)) ++ring_old;
  if (add_buffer(g->upload_ring.get(), rings) && old600(g->mem_ring_last_use_present)) ++ring_old;
  for (const auto& entry : g->vb_cache) add_buffer(entry.second.buf.get(), vb_cache);

  // The active/retired texture owner counts and their use ages are also reported from the D3D cache.
  // Its byte count is the backend's actual heap-0 allocation size above, not the cache's logical byte estimate.
  const uint64_t attributed = tex.bytes + rt.bytes + dest_images.bytes + snapshots.bytes + rings.bytes + vb_cache.bytes;
  uint64_t block_bytes = budgets[0].statistics.blockBytes;
  uint64_t allocation_bytes = budgets[0].statistics.allocationBytes;
  uint64_t total_allocations = budgets[0].statistics.allocationCount;
  if (all_heaps) {
    for (uint32_t i = 1; i < props->memoryHeapCount; ++i) {
      block_bytes += budgets[i].statistics.blockBytes;
      allocation_bytes += budgets[i].statistics.allocationBytes;
      total_allocations += budgets[i].statistics.allocationCount;
    }
  }
  const uint64_t other_free_bytes = block_bytes > allocation_bytes ? block_bytes - allocation_bytes : 0;
  const uint64_t other_unattributed_bytes = allocation_bytes > attributed ? allocation_bytes - attributed : 0;
  const uint64_t classified_allocations = tex.allocations + rt.allocations + dest_images.allocations +
                                          snapshots.allocations + rings.allocations + vb_cache.allocations;
  const uint64_t other_unattributed_allocations =
      total_allocations > classified_allocations ? total_allocations - classified_allocations : 0;
  const uint64_t other_bytes = other_free_bytes + vb_cache.bytes + other_unattributed_bytes;
  const uint64_t other_allocations = vb_cache.allocations + other_unattributed_allocations;

  auto vector_sets = [](const auto& sets) -> uint64_t { return uint64_t(sets.size()); };
  auto frame_sets = [&](const FrameRes& frame) {
    return vector_sets(frame.resolve_sets) + vector_sets(frame.resolve_depth_sets) + vector_sets(frame.detile_sets) +
           vector_sets(frame.resolve_tex_sets) + vector_sets(frame.f16_lean_sets) + vector_sets(frame.depth_img_sets) +
           vector_sets(frame.fsr1_sets);
  };
  uint64_t descriptor_sets = 0;
  for (const auto& table : g->tables) descriptor_sets += table != nullptr;
  descriptor_sets += vector_sets(g->resolve_sets) + vector_sets(g->resolve_depth_sets) + vector_sets(g->detile_sets) +
                     vector_sets(g->resolve_tex_sets) + vector_sets(g->f16_lean_sets) + vector_sets(g->depth_img_sets) +
                     vector_sets(g->fsr1_sets);
  for (const auto& frame : g->parked) descriptor_sets += frame_sets(frame);
  descriptor_sets += frame_sets(g->fence_spare);
  const uint64_t pipelines = g->pipelines.size() + bool(g->depth_img_pipeline) + bool(g->depth_img_pipeline_1x) +
      bool(g->fsr1_easu_pipeline) + bool(g->fsr1_rcas_pipeline) + bool(g->depth_r32f_pipeline) +
      bool(g->depth_r32f_pipeline_1x) + bool(g->f16_linear_pipeline) + bool(g->f16_tex_pipeline) +
      bool(g->f16_lean_rg_pipeline) + bool(g->f16_lean_rgba_pipeline) + bool(g->detile_pipeline);

  char line[1536];
  std::snprintf(line, sizeof(line),
      "[nr-mem-owners] present=%llu h0_local=%u usage=%llu budget=%llu vma_block=%llu vma_allocs=%llu "
      "tex=%llu/%llu cache=%llu old600=%llu img_old600=%llu rt=%llu/%llu slots=%llu old600=%llu "
      "dest_registry=%llu/%llu entries=%llu old600=%llu snapshots=%llu/%llu entries=%llu old600=%llu "
      "check_refs=0/0 refs=0 old600=0 "
      "rings=%llu/%llu old600=%llu descriptors=0/%llu pipelines=0/%llu "
      "other=%llu/%llu other_free=%llu vb_cache=%llu/%llu other_unattr=%llu/%llu sum=%llu",
      static_cast<unsigned long long>(g->mem_present), h0_local ? 1u : 0u,
      static_cast<unsigned long long>(budgets[0].usage), static_cast<unsigned long long>(budgets[0].budget),
      static_cast<unsigned long long>(block_bytes), static_cast<unsigned long long>(total_allocations),
      static_cast<unsigned long long>(tex.bytes), static_cast<unsigned long long>(tex.allocations),
      static_cast<unsigned long long>(frontend.texture_entries), static_cast<unsigned long long>(frontend.texture_old600),
      static_cast<unsigned long long>(texture_old),
      static_cast<unsigned long long>(rt.bytes), static_cast<unsigned long long>(rt.allocations),
      static_cast<unsigned long long>(rt_slots), static_cast<unsigned long long>(rt_old),
      static_cast<unsigned long long>(dest_images.bytes), static_cast<unsigned long long>(dest_images.allocations),
      static_cast<unsigned long long>(frontend.resolve_entries),
      static_cast<unsigned long long>(frontend.resolve_old600),
      static_cast<unsigned long long>(snapshots.bytes), static_cast<unsigned long long>(snapshots.allocations),
      static_cast<unsigned long long>(snapshot_entries), static_cast<unsigned long long>(snapshot_old),
      static_cast<unsigned long long>(rings.bytes), static_cast<unsigned long long>(rings.allocations),
      static_cast<unsigned long long>(ring_old), static_cast<unsigned long long>(descriptor_sets),
      static_cast<unsigned long long>(pipelines), static_cast<unsigned long long>(other_bytes),
      static_cast<unsigned long long>(other_allocations), static_cast<unsigned long long>(other_free_bytes),
      static_cast<unsigned long long>(vb_cache.bytes), static_cast<unsigned long long>(vb_cache.allocations),
      static_cast<unsigned long long>(other_unattributed_bytes),
      static_cast<unsigned long long>(other_unattributed_allocations),
      static_cast<unsigned long long>(tex.bytes + rt.bytes + dest_images.bytes + snapshots.bytes + rings.bytes + vb_cache.bytes +
                                     other_free_bytes + other_unattributed_bytes));
  std::string out = line;
  VmaTotalStatistics stats{};
  vmaCalculateStatistics(vk->allocator, &stats);
  char part[256];
  std::snprintf(part, sizeof(part),
                " | lean=%u pool=%llu/%zu snap_big=%llu/%llu upload_hw=%llu upload_ring=%llu slots_taken=%llu const_waits=%llu new_tex=%llu types:",
                MemLeanMode(), static_cast<unsigned long long>(snap_pool_real), g->snap_pool.size(),
                static_cast<unsigned long long>(snap_big_bytes), static_cast<unsigned long long>(snap_big_n),
                static_cast<unsigned long long>(g->mem_upload_hw), static_cast<unsigned long long>(g->upload_ring_size),
                static_cast<unsigned long long>(g->mem_const_taken), static_cast<unsigned long long>(g->mem_const_waits),
                static_cast<unsigned long long>(g->mem_new_tex));
  out += part;
  g->mem_upload_hw = 0;
  for (uint32_t i = 0; i < props->memoryTypeCount; ++i) {
    const VmaStatistics& st = stats.memoryType[i].statistics;
    if (!st.blockCount) continue;
    std::snprintf(part, sizeof(part), " t%u(h%u,f%x)=blocks %u/%lluMB allocs %u/%lluMB", i, props->memoryTypes[i].heapIndex,
                  unsigned(props->memoryTypes[i].propertyFlags), st.blockCount,
                  static_cast<unsigned long long>(st.blockBytes >> 20), st.allocationCount,
                  static_cast<unsigned long long>(st.allocationBytes >> 20));
    out += part;
  }
  return out;
}
// Card D1: frame boundary (see the GPU draw profiler above). The frontend calls it once per swap after the frame's work.
void GpuDrawsFrame(uint64_t frame, bool scene_once) {
  if (!g || !g->gpu_draws) return;
  SyncRecordThread(kProfFnCount, true, kBatchDrainFrameTail);  // every draw of the frame is recorded first
  if (!DpOn()) return;
  DpClose();
  CmdEntry& e = g->cmd_pool[g->cmd_active];
  DpFrame& f = g_dp_frames[g_dp_frame];
  f.closed = true;
  f.scene_once = scene_once;
  f.fe_frame = frame;
  if (e.dpool && e.dq + 1 <= kDpCap) {
    DpNoteFrame(e);
    g->list->writeTimestamp(e.dpool.get(), e.dq);
    e.drecs.push_back({kDpNone, e.dq, g_dp_frame});
    e.dq += 1;
  } else {
    f.boundary_lost = true;
  }
  ++g_dp_frame;
  // Live: lists are Kick()ed and only waited for on reuse / WaitAll, possibly many frames later. Read the results of every
  // in-flight list whose fence is already signalled now, without blocking and without touching in_flight / the fence
  // (the later wait returns at once and its harvest finds nothing left). Bookkeeping only: the list is not reset.
  for (CmdEntry& c : g->cmd_pool)
    if (c.in_flight && c.fence && g->queue->k14IsCommandFenceSignalled(c.fence.get())) DpHarvest(c);
  DpFinalize(false);
}
bool GpuDrawsActive() { return g && g->gpu_draws && g_prof; }

Stats& GetStats() {
  SyncRecordThread(kProfFnCount, true, kBatchDrainFrameTail);
  if (g && g->device && !IsD3D12()) {  // card P22: plume's vkCmdPipelineBarrier counters
    auto* vk = static_cast<plume::VulkanDevice*>(g->device.get());
    g_stats.vk_barrier_calls += vk->k14BarrierCalls.exchange(0, std::memory_order_relaxed);
    g_stats.vk_barrier_items += vk->k14BarrierItems.exchange(0, std::memory_order_relaxed);
    g_stats.vk_barrier_legacy += vk->k14BarrierLegacy.exchange(0, std::memory_order_relaxed);
  }
  if (g_rq) {
    g_stats.rec_full_waits += g_rq->stats().full_waits - g_rq_full_seen;
    g_rq_full_seen = g_rq->stats().full_waits;
    g_stats.rec_full_wait_ns += g_rq->stats().full_wait_ns - g_rq_full_ns_seen;
    g_rq_full_ns_seen = g_rq->stats().full_wait_ns;
    g_stats.rec_sleeps += g_rq->worker_sleeps() - g_rq_sleeps_seen;
    g_rq_sleeps_seen = g_rq->worker_sleeps();
  }
  return g_stats;
}
uint64_t TotalNs() { return g_stats.total_ns; }
std::string DeviceName() { return g ? g->device_name : std::string(); }

namespace {
void InitResolve(const std::string& shader_dir);
void InitSharedTextureDetile(const std::string& shader_dir);

bool g_shared_set = false;
SharedVulkanDevice g_shared;
bool g_shared_d3d12_set = false;  // card C3
SharedD3D12Device g_shared_d3d12;
bool g_d3d12_present_set = false;
D3D12PresentInfo g_d3d12_present;

// Persistent Vulkan pipeline cache (SVR method: backend_vulkan.cpp Load/SavePipelineCache). The GPU drivers' own
// disk caches are keyed per executable (UHD 630, 2026-10-09: the first replay of every new exe build spent 11-16 s
// building 104-120 pipelines, later runs of the same exe <0.1 s), so every update would bring the stalls back.
// File: magic, version, payload size, XXH3-64 of the payload, then vkGetPipelineCacheData's bytes (the driver header
// is checked again by plume). WWE2K14_NR_PIPELINE_CACHE=0 turns it off; WWE2K14_NR_PIPELINE_CACHE_FILE overrides the file.
struct PipelineCacheFileHeader {
  char magic[4];  // "NRPC"
  uint32_t version;
  uint64_t size;
  uint64_t hash;
};
std::string g_pipeline_cache_file;
// Read at first use, not at static initialisation: on the Xbox (UWP) the settings file is applied to the environment after
// static initialisers ran (XB3 2026-11-13), so a static read never saw WWE2K14_NR_D3D12_PSO_CACHE=1 there.
bool D3d12PsoCacheOn() {
  static const bool on = [] {
    const char* enabled = std::getenv("WWE2K14_NR_D3D12_PSO_CACHE");
    return enabled && std::strcmp(enabled, "1") == 0;
  }();
  return on;
}
// Xbox XB4 (2026-11-14): WWE2K14_NR_D3D12_PREWARM=1 records and prewarms the portable pipeline key list on D3D12 WITHOUT
// the PSO library (cache ON = device lost on the console, XB3). The key list (<cache stem>.keys) holds DXIL content hashes,
// so records from a Vulkan list are skipped by hash; the first session records, later sessions build before play.
// Read lazily for the same UWP reason as D3d12PsoCacheOn. Off by default (PC D3D12 unchanged).
bool D3d12PrewarmOn() {
  static const bool on = [] {
    const char* enabled = std::getenv("WWE2K14_NR_D3D12_PREWARM");
    return enabled && std::strcmp(enabled, "1") == 0;
  }();
  return on;
}
uint64_t g_pipeline_cache_saved_count = 0;  // pipelinesCreated at the last load / save
uint64_t g_pipeline_cache_seen_count = 0;
uint32_t g_pipeline_cache_quiet_swaps = 0;  // swaps since the last new pipeline
std::atomic<bool> g_pipeline_cache_saving{false};
size_t g_pipeline_cache_saved_size = 0;  // payload bytes loaded / last written (same size: only cache hits, no write)

// Main-draw pipeline key (ExecuteDrawNow; M4: built from the desc fields only, zeroed first). Card S3-57 also writes it
// to the portable key list with vs/ps replaced by the XXH3-64 of the SPIR-V bytes: a field change needs a new
// kPipelineKeyVersion (the size check below catches most).
struct PipelineKey {
  uint64_t vs, ps, vs_key, ps_key;
  uint32_t spec, topology, cull, front_ccw, depth_clip, depth_enabled, depth_write, depth_func;
  uint32_t stencil_enabled, stencil_read, stencil_write, stencil_ref;
  uint32_t stencil_front[4], stencil_back[4];  // pass, fail, depth fail, compare
  uint32_t rt_count, rt_format, depth_format, samples, sample_shading;
  uint32_t blend_enabled, blend[6], write_mask;
};
static_assert(sizeof(PipelineKey) == 168, "PipelineKey changed: bump kPipelineKeyVersion (card S3-57)");

// Keep this encoding byte-for-byte identical to the live draw cache key: PipelineKey bytes, raw slot structs,
// then the four consecutive u32 fields beginning at each input element's location.
void BuildPipelineKey(std::string& key, const PipelineKey& pk, const std::vector<RenderInputSlot>& slots,
                      const std::vector<RenderInputElement>& elements) {
  key.clear();
  key.append(reinterpret_cast<const char*>(&pk), sizeof(pk));
  key.append(reinterpret_cast<const char*>(slots.data()), slots.size() * sizeof(RenderInputSlot));
  for (const RenderInputElement& e : elements) key.append(reinterpret_cast<const char*>(&e.location), sizeof(uint32_t) * 4);
}

// The detached prewarm thread may still be finishing during process teardown. Leak these containers like the legacy
// keep vector; all accesses to both are serialized by g_pipeline_create_mu.
std::unordered_map<std::string, std::unique_ptr<RenderPipeline>>& g_warm_pipelines =
    *new std::unordered_map<std::string, std::unique_ptr<RenderPipeline>>();
std::unordered_set<std::string>& g_live_pipeline_keys = *new std::unordered_set<std::string>();

// Card S3-57 (WWE2K14_NR_PREWARM=1): every draw pipeline the renderer creates on a genuine miss is recorded as a portable
// record (no live pointer or guest address; shaders by SPIR-V content hash) in <cache stem>.keys next to the
// pipeline cache (GPU-neutral). At the first swap a background thread creates the
// pipelines of the list read at startup into the same VkPipelineCache. With WWE2K14_NR_PREWARM_ADOPT enabled they
// are held under the byte-identical live key and transferred on a live miss; =0 preserves the old leaked keep vector.
// The portable record substitutes SPIR-V content hashes for the live shader pointers, which are restored from the
// shared shader cache before building the live key. File: "NRPK", version, count, payload size, XXH3-64 of the payload; payload = per record
// u32 length + bytes. Each record: u32 scope 0 + PipelineKey + u32 n + n x {index, stride, class} + u32 m +
// m x {location, format, slot, offset}. Older scope-1 records are skipped.
constexpr uint32_t kPipelineKeyVersion = 1;
struct PipelineKeyFileHeader {
  char magic[4];  // "NRPK"
  uint32_t version;
  uint32_t count;
  uint32_t reserved;
  uint64_t size;
  uint64_t hash;
};
bool g_prewarm = false;
bool g_prewarm_adopt = true;  // WWE2K14_NR_PREWARM_ADOPT=0 restores legacy keep-vector/no-adoption behavior
std::string g_prewarm_file;
std::mutex g_prewarm_mu;  // g_prewarm_records / g_prewarm_seen (record thread appends, save worker copies)
std::vector<std::string> g_prewarm_records;  // file records first (in file order), then new ones in first-seen order
std::unordered_set<std::string> g_prewarm_seen;
size_t g_prewarm_loaded = 0;  // records read from the file: the prewarm list
size_t g_prewarm_saved = 0;   // records in the file (save worker only)
bool g_prewarm_started = false;  // swap thread only
std::atomic<bool> g_prewarm_running{false};
std::atomic<bool> g_prewarm_stop{false};
// Card R14: WWE2K14_NR_PREWARM_BLOCK (unset/1 = on, 0 = background as before): the first frame waits until the known
// pipelines are built, with a progress box (wwe2k14_app.h PrewarmOverlay), so the driver never compiles them mid-match.
bool g_prewarm_block = [] {
  const char* e = std::getenv("WWE2K14_NR_PREWARM_BLOCK");
  return !(e && std::strcmp(e, "0") == 0);
}();
std::atomic<size_t> g_prewarm_total{0}, g_prewarm_done{0};
std::atomic<uint64_t> g_prewarm_t0_ns{0}, g_prewarm_end_ns{0};

void CreateOrAdoptPipeline(const std::string& key, const RenderGraphicsPipelineDesc& desc,
                           std::unique_ptr<RenderPipeline>* pipeline, bool* adopted) {
  *adopted = false;
  RecordMutexLock lock(g_pipeline_create_mu, "pipeline_create_mutex");
  g_live_pipeline_keys.insert(key);
  if (g_prewarm_adopt) {
    auto warm = g_warm_pipelines.find(key);
    if (warm != g_warm_pipelines.end()) {
      *pipeline = std::move(warm->second);
      g_warm_pipelines.erase(warm);
      *adopted = true;
      return;
    }
  }
  *pipeline = g->device->createGraphicsPipeline(desc);
}

void PutU32(std::string* s, uint32_t v) { s->append(reinterpret_cast<const char*>(&v), sizeof(v)); }

void RecordPipelineKey(std::string record) {
  RecordMutexLock lk(g_prewarm_mu, "pipeline_key_table_mutex");
  if (g_prewarm_seen.insert(record).second) g_prewarm_records.push_back(std::move(record));
}

// Card S3-57 scope 0 record: the key with the live shader pointers replaced by the SPIR-V content hashes.
void RecordMainPipeline(PipelineKey pk, const Draw& d, const std::vector<RenderInputSlot>& slots,
                        const std::vector<RenderInputElement>& elements) {
  pk.vs = XXH3_64bits(d.vs_spirv->data(), d.vs_spirv->size());
  pk.ps = d.ps_spirv && !d.ps_spirv->empty() ? XXH3_64bits(d.ps_spirv->data(), d.ps_spirv->size()) : 0;
  std::string r;
  PutU32(&r, 0);
  r.append(reinterpret_cast<const char*>(&pk), sizeof(pk));
  PutU32(&r, uint32_t(slots.size()));
  for (const RenderInputSlot& sl : slots) {
    for (uint32_t v : {sl.index, sl.stride, uint32_t(sl.classification)}) PutU32(&r, v);
  }
  PutU32(&r, uint32_t(elements.size()));
  for (const RenderInputElement& e : elements) {
    for (uint32_t v : {e.location, uint32_t(e.format), e.slotIndex, e.alignedByteOffset}) PutU32(&r, v);
  }
  RecordPipelineKey(std::move(r));
}

// One NRPK list; nothing on a missing or invalid file.
std::vector<std::string> ReadPipelineKeyFile(const std::filesystem::path& path) {
  std::vector<uint8_t> file;
  if (std::ifstream in{path, std::ios::binary}) {
    file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  PipelineKeyFileHeader h = {};
  bool ok = false;
  if (file.size() >= sizeof(h)) {
    std::memcpy(&h, file.data(), sizeof(h));
    ok = std::memcmp(h.magic, "NRPK", 4) == 0 && h.version == kPipelineKeyVersion &&
         h.size == file.size() - sizeof(h) && XXH3_64bits(file.data() + sizeof(h), size_t(h.size)) == h.hash;
  }
  std::vector<std::string> records;
  for (size_t at = sizeof(h); ok && records.size() < h.count;) {
    uint32_t n = 0;
    if (file.size() - at < sizeof(n)) break;
    std::memcpy(&n, file.data() + at, sizeof(n));
    at += sizeof(n);
    if (file.size() - at < n) break;
    records.emplace_back(reinterpret_cast<const char*>(file.data() + at), n);
    at += n;
  }
  if (!file.empty() && (!ok || records.size() != h.count)) {
    const auto utf8 = path.u8string();  // std::string or std::u8string by language standard
    const std::string text(utf8.begin(), utf8.end());
    std::fprintf(stderr, "[nr-prewarm] key list invalid or another version, ignored: %s\n", text.c_str());
    records.clear();
  }
  return records;
}

void LoadPipelineKeys() {
  for (std::string& r : ReadPipelineKeyFile(std::filesystem::u8path(g_prewarm_file))) RecordPipelineKey(std::move(r));
  g_prewarm_saved = g_prewarm_records.size();
  // Card SEED-updaters: updating never touches userdata, so a player's list can predate the shipped shaders. The list that
  // ships beside the game is added behind theirs. Records for a shader set in use are built; the rest are skipped by hash.
  // Wide on Windows: getenv gives code page bytes, and an accented install folder must not stop the game.
#ifdef _WIN32
  const wchar_t* seed = _wgetenv(L"WWE2K14_NR_PREWARM_SEED");
#else
  const char* seed = std::getenv("WWE2K14_NR_PREWARM_SEED");
#endif
  if (seed && *seed) {
    for (std::string& r : ReadPipelineKeyFile(std::filesystem::path(seed))) RecordPipelineKey(std::move(r));
  }
  g_prewarm_loaded = g_prewarm_records.size();
}

// Save worker only (after SavePipelineCache): writes the list when records were added; replaced by rename.
void SavePipelineKeys() {
  std::string payload;
  size_t count = 0;
  {
    std::lock_guard<std::mutex> lk(g_prewarm_mu);
    count = g_prewarm_records.size();
    if (count == g_prewarm_saved) return;
    for (const std::string& r : g_prewarm_records) {
      PutU32(&payload, uint32_t(r.size()));
      payload += r;
    }
  }
  PipelineKeyFileHeader h = {{'N', 'R', 'P', 'K'}, kPipelineKeyVersion, uint32_t(count), 0, payload.size(),
                             XXH3_64bits(payload.data(), payload.size())};
  std::error_code ec;
  const std::filesystem::path file = std::filesystem::u8path(g_prewarm_file);
  std::filesystem::create_directories(file.parent_path(), ec);
  const std::filesystem::path tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    out.write(payload.data(), std::streamsize(payload.size()));
    if (!out) return;
  }
  std::filesystem::rename(tmp, file, ec);
  if (!ec) {
    g_prewarm_saved = count;
    std::fprintf(stderr, "[nr-prewarm] recorded %zu keys -> %s\n", count, g_prewarm_file.c_str());
  }
}

void LoadPipelineCache(plume::VulkanDevice* vk) {
  // One file per GPU (vendor-device before the extension): a PC with two GPUs keeps both caches.
  char id[32];
  std::snprintf(id, sizeof(id), "-%04x-%04x", vk->physicalDeviceProperties.vendorID, vk->physicalDeviceProperties.deviceID);
  std::filesystem::path name = std::filesystem::u8path(g_pipeline_cache_file);
  name.replace_filename(name.stem().string() + id + name.extension().string());
  g_pipeline_cache_file = name.u8string();
  std::vector<uint8_t> file;
  if (std::ifstream in{g_pipeline_cache_file, std::ios::binary}) {
    file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  PipelineCacheFileHeader h = {};
  const uint8_t* payload = nullptr;
  size_t size = 0;
  const char* state = file.empty() ? "new" : "invalid file, starting empty";
  if (file.size() >= sizeof(h)) {
    std::memcpy(&h, file.data(), sizeof(h));
    if (std::memcmp(h.magic, "NRPC", 4) == 0 && h.version == 1 && h.size == file.size() - sizeof(h) &&
        XXH3_64bits(file.data() + sizeof(h), size_t(h.size)) == h.hash) {
      payload = file.data() + sizeof(h);
      size = size_t(h.size);
    }
  }
  const bool loaded = vk->setPipelineCacheData(payload, size);
  g_pipeline_cache_saved_size = loaded ? size : 0;
  if (payload && loaded) state = "loaded";
  else if (payload) state = "from another GPU or driver, starting empty";
  std::fprintf(stderr, "[nr] pipeline cache %s (%zu KB): %s\n", state, size / 1024, g_pipeline_cache_file.c_str());
  g_pipeline_cache_saved_count = g_pipeline_cache_seen_count = vk->pipelinesCreated.load();
}

void SavePipelineCache(plume::VulkanDevice* vk) {
  std::vector<uint8_t> data;
  {
    std::lock_guard<std::mutex> lk(g_pipeline_create_mu);  // card S3-57: not during a (prewarm) create
    data = vk->getPipelineCacheData();
  }
  if (data.empty() || data.size() == g_pipeline_cache_saved_size) return;
  PipelineCacheFileHeader h = {{'N', 'R', 'P', 'C'}, 1, data.size(), XXH3_64bits(data.data(), data.size())};
  std::error_code ec;
  const std::filesystem::path file = std::filesystem::u8path(g_pipeline_cache_file);
  std::filesystem::create_directories(file.parent_path(), ec);
  const std::filesystem::path tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out) return;
  }
  std::filesystem::rename(tmp, file, ec);
  if (!ec) {
    g_pipeline_cache_saved_size = data.size();
    std::fprintf(stderr, "[nr] pipeline cache saved (%zu KB)\n", data.size() / 1024);
  }
}

#ifdef _WIN32
#pragma pack(push, 1)
struct D3D12PipelineCacheFileHeader {
  char magic[4];
  uint32_t version;
  uint32_t luidLow;
  int32_t luidHigh;
  uint64_t driverVersion;
  uint32_t vendor;
  uint64_t payloadSize;
  uint64_t hash;
};
#pragma pack(pop)

void LoadPipelineCacheD3D12(plume::D3D12Device* dev) {
  const std::filesystem::path file = std::filesystem::u8path(g_pipeline_cache_file + ".d3d12pso");
  std::vector<uint8_t> bytes;
  std::error_code ec;
  const bool exists = std::filesystem::exists(file, ec);
  const char* rejectReason = ec ? "file status error" : nullptr;
  bool missing = !ec && !exists;
  if (exists && !ec) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      rejectReason = "file read error";
    } else {
      bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      if (in.bad()) rejectReason = "file read error";
    }
  }

  D3D12PipelineCacheFileHeader header = {};
  const uint8_t* payload = nullptr;
  size_t payloadSize = 0;
  if (!rejectReason && !missing) {
    if (bytes.size() < sizeof(header)) {
      rejectReason = "short header";
    } else {
      std::memcpy(&header, bytes.data(), sizeof(header));
      if (std::memcmp(header.magic, "NRPD", 4) != 0 || header.version != 1) {
        rejectReason = "magic/version mismatch";
      } else if (header.payloadSize != bytes.size() - sizeof(header)) {
        rejectReason = "payload size mismatch";
      } else if (XXH3_64bits(bytes.data() + sizeof(header), size_t(header.payloadSize)) != header.hash) {
        rejectReason = "payload hash mismatch";
      } else {
        DXGI_ADAPTER_DESC1 adapterDesc = {};
        const HRESULT adapterRes = dev->adapter ? dev->adapter->GetDesc1(&adapterDesc) : E_POINTER;
        if (FAILED(adapterRes)) {
          rejectReason = "adapter LUID unavailable";
        } else if (header.luidLow != adapterDesc.AdapterLuid.LowPart ||
                   header.luidHigh != adapterDesc.AdapterLuid.HighPart) {
          rejectReason = "adapter LUID mismatch";
        } else if (header.driverVersion != dev->getDescription().driverVersion) {
          rejectReason = "driver version mismatch";
        } else if (header.vendor != uint32_t(dev->getDescription().vendor)) {
          rejectReason = "vendor mismatch";
        } else {
          payload = bytes.data() + sizeof(header);
          payloadSize = size_t(header.payloadSize);
        }
      }
    }
  }

  bool loaded = false;
  if (!rejectReason && !missing) {
    loaded = dev->setPipelineLibraryData(payload, payloadSize);
    if (!loaded) rejectReason = "pipeline library rejected payload";
  }
  if (rejectReason) {
    std::fprintf(stderr, "[nr-pso] cache rejected (%s); starting empty\n", rejectReason);
    payload = nullptr;
    payloadSize = 0;
    loaded = false;
    dev->setPipelineLibraryData(nullptr, 0);
  } else if (missing) {
    std::fprintf(stderr, "[nr-pso] cache new; starting empty: %s\n", file.string().c_str());
    dev->setPipelineLibraryData(nullptr, 0);
  } else {
    std::fprintf(stderr, "[nr-pso] cache loaded (%zuB): %s\n", payloadSize, file.string().c_str());
  }
  g_pipeline_cache_saved_size = loaded ? payloadSize : 0;
  g_pipeline_cache_saved_count = g_pipeline_cache_seen_count = dev->pipelinesCreated.load();
}

void SavePipelineCacheD3D12(plume::D3D12Device* dev) {
  const std::filesystem::path file = std::filesystem::u8path(g_pipeline_cache_file + ".d3d12pso");
  std::vector<uint8_t> data;
  {
    std::lock_guard<std::mutex> lk(g_pipeline_create_mu);
    data = dev->getPipelineLibraryData();
  }
  if (data.empty() || data.size() == g_pipeline_cache_saved_size) return;

  DXGI_ADAPTER_DESC1 adapterDesc = {};
  const HRESULT adapterRes = dev->adapter ? dev->adapter->GetDesc1(&adapterDesc) : E_POINTER;
  if (FAILED(adapterRes)) {
    std::fprintf(stderr, "[nr-pso] save failed (adapter LUID unavailable)\n");
    return;
  }
  D3D12PipelineCacheFileHeader header = {{'N', 'R', 'P', 'D'}, 1, adapterDesc.AdapterLuid.LowPart,
                                          adapterDesc.AdapterLuid.HighPart, dev->getDescription().driverVersion,
                                          uint32_t(dev->getDescription().vendor), uint64_t(data.size()),
                                          XXH3_64bits(data.data(), data.size())};
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  const std::filesystem::path tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out) return;
  }
  std::filesystem::rename(tmp, file, ec);
  if (!ec) {
    g_pipeline_cache_saved_size = data.size();
    std::fprintf(stderr, "[nr-pso] saved %zuB hits=%llu misses=%llu created=%llu\n", data.size(),
                 static_cast<unsigned long long>(dev->psoCacheHits.load()),
                 static_cast<unsigned long long>(dev->psoCacheMisses.load()),
                 static_cast<unsigned long long>(dev->pipelinesCreated.load()));
  }
}

// cache=false (XB4, prewarm without the PSO library): same quiet-swap timing, writes only the key list.
void MaybeSavePipelineCacheD3D12(bool cache) {
  if (!g || g_pipeline_cache_file.empty()) return;
  auto* dev = static_cast<plume::D3D12Device*>(g->device.get());
  const uint64_t n = dev->pipelinesCreated.load();
  if (n != g_pipeline_cache_seen_count) {
    g_pipeline_cache_seen_count = n;
    g_pipeline_cache_quiet_swaps = 0;
    return;
  }
  if (n == g_pipeline_cache_saved_count || ++g_pipeline_cache_quiet_swaps < 2) return;
  static std::chrono::steady_clock::time_point last_save;
  const auto now = std::chrono::steady_clock::now();
  if (g_prewarm_running && now - last_save < std::chrono::seconds(10)) return;
  if (g_pipeline_cache_saving.exchange(true)) return;
  last_save = now;
  g_pipeline_cache_saved_count = n;
  std::thread([dev, cache] {
    if (cache) SavePipelineCacheD3D12(dev);
    if (g_prewarm) SavePipelineKeys();
    g_pipeline_cache_saving = false;
  }).detach();
}
#endif
}  // namespace
void SetSharedVulkanDevice(const SharedVulkanDevice& device) {
  g_shared = device;
  g_shared_set = true;
}

void SetSharedD3D12Device(const SharedD3D12Device& device) {
  g_shared_d3d12 = device;
  g_shared_d3d12_set = device.device != nullptr;
}

bool TakeD3D12PresentInfo(D3D12PresentInfo* out) {
  if (!g_d3d12_present_set || !out) return false;
  *out = g_d3d12_present;
  g_d3d12_present_set = false;
  return true;
}

void SetPipelineCacheFile(const std::string& path) {
  const char* off = std::getenv("WWE2K14_NR_PIPELINE_CACHE");
  const char* file = std::getenv("WWE2K14_NR_PIPELINE_CACHE_FILE");
  g_pipeline_cache_file = off && std::strcmp(off, "0") == 0 ? std::string() : file && *file ? std::string(file) : path;
  const char* adopt = std::getenv("WWE2K14_NR_PREWARM_ADOPT");
  g_prewarm_adopt = !(adopt && std::strcmp(adopt, "0") == 0);
  // Card S3-57: the key list is GPU-neutral (no vendor-device suffix): <cache stem>.keys in the cache directory.
  const char* prewarm = std::getenv("WWE2K14_NR_PREWARM");
  g_prewarm = prewarm && std::strcmp(prewarm, "1") == 0 && !g_pipeline_cache_file.empty() &&
              (!IsD3D12() || D3d12PsoCacheOn() || D3d12PrewarmOn());
  if (g_prewarm) {
    g_prewarm_file = std::filesystem::u8path(g_pipeline_cache_file).replace_extension(".keys").u8string();
    LoadPipelineKeys();
  }
}

void MaybeSavePipelineCache() {
#ifdef _WIN32
  if (IsD3D12()) {
    if (D3d12PsoCacheOn()) MaybeSavePipelineCacheD3D12(true);
    else if (g_prewarm) MaybeSavePipelineCacheD3D12(false);
    return;
  }
#endif
  if (!g || g_pipeline_cache_file.empty() || IsD3D12()) return;
  auto* vk = static_cast<plume::VulkanDevice*>(g->device.get());
  const uint64_t n = vk->pipelinesCreated.load();
  if (n != g_pipeline_cache_seen_count) {
    g_pipeline_cache_seen_count = n;
    g_pipeline_cache_quiet_swaps = 0;
    return;
  }
  // New pipelines, then two frames without any: keep them. Written on a worker thread (no frame hitch); the
  // process may exit mid-write, so the file is replaced by rename only once complete.
  if (n == g_pipeline_cache_saved_count || ++g_pipeline_cache_quiet_swaps < 2) return;
  // Card S3-57: the prewarm thread adds a pipeline every few frames; save at most every 10 s while it runs.
  static std::chrono::steady_clock::time_point last_save;
  const auto now = std::chrono::steady_clock::now();
  if (g_prewarm_running && now - last_save < std::chrono::seconds(10)) return;
  if (g_pipeline_cache_saving.exchange(true)) return;
  last_save = now;
  g_pipeline_cache_saved_count = n;
  std::thread([vk] {
    SavePipelineCache(vk);
    if (g_prewarm) SavePipelineKeys();
    g_pipeline_cache_saving = false;
  }).detach();
}

bool Init(const std::string& shader_dir, std::string* error) {
  if (g) return true;
  auto state = std::make_unique<State>();
  state->fix_alt_scheduler = fix_alt::Scheduler(fix_alt::ParseSeconds(std::getenv("WWE2K14_NR_FIX_ALT")));
  state->fix_alt_on = state->fix_alt_scheduler.enabled();
  SetFixAltPhaseB(false);
  if (state->fix_alt_scheduler.enabled()) {
    std::fprintf(stderr, "[nr-fix-alt] enabled period_s=%d (phase A starts at first swap)\n",
                 state->fix_alt_scheduler.period_seconds());
  }
  if (const char* mem_owners = std::getenv("WWE2K14_NR_MEM_OWNERS"))
    state->mem_owners_on = std::strcmp(mem_owners, "1") == 0;
#ifdef _WIN32
  const bool use_d3d12 = BackendWantsDxil();
  if (use_d3d12) {
    if (g_shared_d3d12_set) {
      plume::D3D12ExistingDevice e;
      e.device = static_cast<ID3D12Device*>(g_shared_d3d12.device);
      e.directQueue = static_cast<ID3D12CommandQueue*>(g_shared_d3d12.direct_queue);
      state->api = plume::CreateD3D12Interface(e);
      state->shared_device = state->api != nullptr;
      std::fprintf(stderr, "[nr] direct present: adopting the emulator's D3D12 device\n");
    } else {
      state->api = CreateD3D12Interface();
    }
  } else
#else
  constexpr bool use_d3d12 = false;
#endif
  if (!use_d3d12 && g_shared_set) {
    // Direct GPU presentation: adopt the emulator's instance, device and queue (one queue, one lock).
    plume::VulkanExistingDevice e;
    e.instance = static_cast<VkInstance>(g_shared.instance);
    e.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(g_shared.get_instance_proc_addr);
    e.apiVersion = g_shared.api_version;
    e.physicalDevice = static_cast<VkPhysicalDevice>(g_shared.physical_device);
    e.device = static_cast<VkDevice>(g_shared.device);
    e.queueFamilyIndex = g_shared.queue_family;
    e.queue = static_cast<VkQueue>(g_shared.queue);
    e.queueMutex = static_cast<std::recursive_mutex*>(g_shared.queue_mutex);
    e.descriptorIndexing = g_shared.descriptor_indexing;
    e.scalarBlockLayout = g_shared.scalar_block_layout;
    e.bufferDeviceAddress = g_shared.buffer_device_address;
    e.nullDescriptor = g_shared.null_descriptor;
    e.samplerMirrorClampToEdge = g_shared.sampler_mirror_clamp_to_edge;
    e.geometryShader = g_shared.geometry_shader;
    state->api = CreateVulkanInterface(e);
    state->shared_device = true;
    std::fprintf(stderr, "[nr] direct present: adopting the emulator's Vulkan device\n");
  } else if (!use_d3d12) {
    state->api = CreateVulkanInterface();
  }
  if (!state->api) {
    *error = use_d3d12 ? "plume: no D3D12 interface" : "plume: no Vulkan interface";
    return false;
  }
  state->shader_format = state->api->getCapabilities().shaderFormat;
  if (use_d3d12 && state->shader_format != RenderShaderFormat::DXIL) {
    *error = "plume: D3D12 interface does not expose DXIL";
    return false;
  }
  if (state->shader_format != RenderShaderFormat::SPIRV) state->sampler_table_size = kSamplerTableSizeD3D12;
  // Constants ABI (G56 B1): chosen before device creation from the switch and the actual primary shader
  // folder that d3d/runtime.cpp loads. Switch and marker must agree; a configured secondary folder must
  // use the same ABI. Default off = legacy 64-bit buffer-device-address constants path.
  {
    const std::string secondary_folder =
        nr_constants32::SecondaryFolder(std::getenv(nr_constants32::kSecondaryFolderSwitch));
    const nr_constants32::Selection abi_sel = nr_constants32::ReadSelection(
        shader_dir, secondary_folder, std::getenv(nr_constants32::kSwitch));
    if (!abi_sel.ok) {
      *error = "constants ABI: " + abi_sel.error + " (primary shader folder: " + shader_dir + ")";
      return false;
    }
    if (abi_sel.enabled && state->shader_format != RenderShaderFormat::SPIRV) {
      *error = "constants ABI: constants32 requires SPIR-V shaders";
      return false;
    }
    state->abi = nr_constants32::Plan(abi_sel.enabled);
    if (state->abi.constants32) {
      std::fprintf(stderr,
                   "[nr] constants ABI: constants32 (set 4 ByteAddressBuffer over the whole %llu-byte ring, "
                   "32-bit byte offsets, base 0). Constants loads do not use Int64; this backend still "
                   "requires bufferDeviceAddress and descriptor indexing.\n",
                   (unsigned long long)(nr_constants32::kSlotBytes * nr_constants32::kSlots));
    } else {
      std::fprintf(stderr, "[nr] constants ABI: legacy (64-bit buffer-device-address constants)\n");
    }
  }
  // NR_DEVICE: substring of the GPU name (default: the first device whose name contains
  // "5060", the development GPU, else plume's choice).
  std::string wanted = std::getenv("NR_DEVICE") ? std::getenv("NR_DEVICE") : "5060";
  std::string chosen;
  const std::vector<std::string> device_names = state->api->getDeviceNames();
  for (const std::string& name : device_names) {
    std::fprintf(stderr, "[nr] %s adapter: %s\n", use_d3d12 ? "D3D12" : "Vulkan", name.c_str());
    if (chosen.empty() && name.find(wanted) != std::string::npos) chosen = name;
  }
  std::fprintf(stderr, "[nr] adapter chosen: %s (NR_DEVICE=%s)\n",
               chosen.empty() ? "(plume default)" : chosen.c_str(), wanted.c_str());
  state->device = state->api->createDevice(chosen);
  if (!state->device) {
    *error = "plume: createDevice failed";
    return false;
  }
  state->device_name = state->device->getDescription().name;
  std::fprintf(stderr, "[nr] device created: %s\n", state->device_name.c_str());
#ifdef _WIN32
  if (use_d3d12) {
    // Xbox XB1 (2026-11-09): every graphics PSO failed with E_INVALIDARG on the console. Log what this D3D12 device
    // reports (feature level, highest shader model, 64-bit shader ops, binding tier, root signature) once at init.
    ID3D12Device* dev = static_cast<plume::D3D12Device*>(state->device.get())->d3d;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                  D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {};
    fl.NumFeatureLevels = UINT(std::size(levels));
    fl.pFeatureLevelsRequested = levels;
    const HRESULT fl_hr = dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl));
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_7};
    HRESULT sm_hr = E_INVALIDARG;
    while ((sm_hr = dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) == E_INVALIDARG &&
           sm.HighestShaderModel > D3D_SHADER_MODEL_5_1) {
      sm.HighestShaderModel = D3D_SHADER_MODEL(int(sm.HighestShaderModel) - 1);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    const HRESULT o1_hr = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1));
    D3D12_FEATURE_DATA_ROOT_SIGNATURE rs = {D3D_ROOT_SIGNATURE_VERSION_1_1};
    const HRESULT rs_hr = dev->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs, sizeof(rs));
    std::fprintf(stderr,
                 "[nr] D3D12 caps: feature level 0x%X (hr 0x%lX) | highest shader model 0x%X (hr 0x%lX) | "
                 "Int64ShaderOps=%d (hr 0x%lX) WaveOps=%d | DoublePrecision=%d binding tier=%d typed UAV loads=%d "
                 "ROV=%d | root signature 0x%X (hr 0x%lX)\n",
                 unsigned(fl.MaxSupportedFeatureLevel), fl_hr, unsigned(sm.HighestShaderModel), sm_hr,
                 int(o1.Int64ShaderOps), o1_hr, int(o1.WaveOps), int(o.DoublePrecisionFloatShaderOps),
                 int(o.ResourceBindingTier), int(o.TypedUAVLoadAdditionalFormats), int(o.ROVsSupported),
                 unsigned(rs.HighestVersion), rs_hr);
  }
#endif
  if (!g_pipeline_cache_file.empty() && state->shader_format == RenderShaderFormat::SPIRV) {
    LoadPipelineCache(static_cast<plume::VulkanDevice*>(state->device.get()));  // before any pipeline is created
  }
#ifdef _WIN32
  else if (!g_pipeline_cache_file.empty() && state->shader_format == RenderShaderFormat::DXIL && D3d12PsoCacheOn()) {
    LoadPipelineCacheD3D12(static_cast<plume::D3D12Device*>(state->device.get()));  // before any pipeline is created
  }
#endif
  const RenderDeviceCapabilities& caps = state->device->getCapabilities();
  if (!caps.descriptorIndexing || !caps.bufferDeviceAddress) {
    *error = "device lacks descriptor indexing or buffer device address";
    return false;
  }
  state->queue = state->device->createCommandQueue(RenderCommandListType::DIRECT);
  {
    CmdEntry e;
    e.list = state->queue->createCommandList();
    e.fence = state->device->createCommandFence();
    if (!e.list || !e.fence) {
      *error = "plume: command list / fence creation failed";
      return false;
    }
    state->cmd_pool.push_back(std::move(e));
    state->list = state->cmd_pool[0].list.get();
  }
  // Upload ring (WWE2K14_NR_UPLOAD_RING=1): a single persistent, persistently-mapped upload buffer used
  // as both vertex and index buffer (VkBufferUsageFlagBits VERTEX|INDEX, see plume_vulkan.cpp:831-832).
  // Default off: the per-draw createBuffer path in ExecuteDraw stays byte-identical and remains the
  // fallback. A failed ring allocation falls back to it (ExecuteDraw handles a null/empty ring).
  if (const char* ur = std::getenv("WWE2K14_NR_UPLOAD_RING")) {
    const long n = std::strtol(ur, nullptr, 10);
    if (n > 0) {
      // GAP-mem1: start small (RingAlloc grows on demand, retiring the old ring).
      state->upload_ring_size = (MemLeanMode() & kMemLeanUpload) ? (1ull << 24) : (1ull << 26);  // 16 / 64 MB
      state->upload_ring = state->device->createBuffer(RenderBufferDesc::UploadBuffer(
          state->upload_ring_size, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
      if (state->upload_ring) state->upload_ring_mapped = static_cast<uint8_t*>(state->upload_ring->map());
      if (!state->upload_ring || !state->upload_ring_mapped) {
        state->upload_ring.reset();
        state->upload_ring_mapped = nullptr;
        state->upload_ring_size = 0;
      }
    }
  }
  // Draw fast (WWE2K14_NR_DRAW_FAST=1): XXH3-64 pipeline-key hash with full-key confirmation on hit.
  // Default off: the std::string key + std::map lookup in ExecuteDraw stays byte-identical.
  if (const char* df = std::getenv("WWE2K14_NR_DRAW_FAST")) {
    state->draw_fast = std::strcmp(df, "1") == 0;
  }
  // State elision (WWE2K14_NR_STATE_ELIDE=1): share one render pass across consecutive draws to the same
  // framebuffer and skip redundant per-draw binds. Default off: ExecuteDraw stays byte-identical.
  if (const char* se = std::getenv("WWE2K14_NR_STATE_ELIDE")) {
    state->state_elide = std::strcmp(se, "1") == 0;
  }
  // Constants dedupe (WWE2K14_NR_CONST_DEDUPE=1): skip rewriting a vs/ps/shared block whose bytes and
  // address are unchanged (see ExecuteDraw). Default off: ExecuteDraw's unconditional writes stay
  // byte-identical to today.
  if (const char* cd = std::getenv("WWE2K14_NR_CONST_DEDUPE")) {
    state->const_dedupe = std::strcmp(cd, "1") == 0;
  }
  if (const char* rr = std::getenv("WWE2K14_NR_REC_REUSE")) {
    state->rec_reuse.store(std::strcmp(rr, "1") == 0 ? 1 : (std::strcmp(rr, "2") == 0 ? 2 : 0),
                           std::memory_order_relaxed);
  }
  state->vb_direct_mode.store(ConfiguredVbDirectMode(), std::memory_order_relaxed);
  state->v17c_ab_on = wwe2k14::d3d::v17c_ab::Enabled();
  if (state->v17c_ab_on) {
    state->v17c_rec_reuse_mode = state->rec_reuse.load(std::memory_order_relaxed);
    state->v17c_vb_direct_mode = state->vb_direct_mode.load(std::memory_order_relaxed);
    state->v17c_ab_mode = 0;  // phase A always starts with the legacy worker path
    state->rec_reuse.store(0, std::memory_order_relaxed);
    state->vb_direct_mode.store(0, std::memory_order_relaxed);
  }

  RenderDescriptorRange ranges[4] = {
      RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, kTableSize),
      RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, kTableSize),
      RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, kTableSize),
      RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 0, state->sampler_table_size)};
  RenderDescriptorSetDesc sets[4];
  for (int i = 0; i < 4; ++i) {
    sets[i] = RenderDescriptorSetDesc(&ranges[i], 1, true, i == 3 ? state->sampler_table_size : kTableSize);
    state->tables[i] = state->device->createDescriptorSet(sets[i]);
    if (!state->tables[i]) {
      *error = "createDescriptorSet failed";
      return false;
    }
  }
  RenderPushConstantRange push(0, 0, 0, 3 * sizeof(uint64_t),
                               RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
  // constants32: set 4 binding 0 is a ByteAddressBuffer over the whole constants ring. Legacy keeps four
  // sets and the buffer-device-address constants ring.
  RenderDescriptorRange const_range(RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER, 0, 1);
  RenderDescriptorSetDesc layout_sets[5] = {sets[0], sets[1], sets[2], sets[3],
                                            RenderDescriptorSetDesc(&const_range, 1)};
  RenderPipelineLayoutDesc layout_desc(&push, 1, layout_sets, state->abi.descriptor_set_count);
  // D3D12 (step C): the DXIL shaders read the per-draw constant blocks from
  // `ByteAddressBuffer g_NrConstants : register(t0, space4)` at 32-bit byte offsets (the 3 pushed
  // values), so the constants ring is bound as a root SRV. Vulkan layout is unchanged.
  const RenderRootDescriptorDesc nr_const_root(0, 4, RenderRootDescriptorType::SHADER_RESOURCE);
  if (state->shader_format != RenderShaderFormat::SPIRV) {
    layout_desc.rootDescriptorDescs = &nr_const_root;
    layout_desc.rootDescriptorDescsCount = 1;
    // D3D12 rejects a PSO with an input layout unless the root signature allows the input assembler
    // (d3d-w03: every game draw PSO failed).
    layout_desc.allowInputLayout = true;
  }
  state->layout = state->device->createPipelineLayout(layout_desc);
  // GAP-mem1: a smaller allocated ring. Not with constants32 (its shaders declare the full ring) or frame slots.
  if ((MemLeanMode() & kMemLeanConst) && !state->abi.constants32) state->const_slots = kConstSlots / 2;
  state->constants = state->device->createBuffer(RenderBufferDesc::UploadBuffer(
      uint64_t(kConstBytes) * state->const_slots,
      state->abi.constants_ring_storage ? RenderBufferFlag::STORAGE
                                        : (RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE)));
  if (state->abi.constants32 && state->constants) {
    state->const32_set = state->device->createDescriptorSet(layout_sets[4]);
    if (!state->const32_set) {
      *error = "constants32 descriptor set creation failed";
      return false;
    }
    state->const32_set->setBuffer(0, state->constants.get(), uint64_t(kConstBytes) * kConstSlots);
  }
  if (!state->layout || !state->constants) {
    *error = "pipeline layout / constant buffer creation failed";
    return false;
  }
  state->constants_mapped = static_cast<uint8_t*>(state->constants->map());
  if (!state->constants_mapped) {  // k14 G37: allocation failed inside createBuffer
    *error = "constant buffer map failed (out of memory?)";
    return false;
  }
  // constants_address is the base added to every pushed constant offset. Legacy Vulkan: the buffer
  // device address (push values are 64-bit addresses). D3D12 and constants32: 0, so the push values are
  // plain byte offsets into the ring (D3D12 binds it as a root SRV, constants32 as set 4 binding 0).
  static_assert(uint64_t(kConstBytes) * kConstSlots <= 0xFFFFFFFFull, "constant byte offsets must fit in 32 bits");
  state->constants_address = nr_constants32::ConstantAddressBase(
      state->abi, state->shader_format == RenderShaderFormat::SPIRV,
      (!state->abi.constants32 && state->shader_format == RenderShaderFormat::SPIRV)
          ? state->constants->getDeviceAddress()
          : 0);
  std::memset(state->constants_mapped, 0, size_t(kConstBytes) * state->const_slots);
  state->list->begin();
  g = state.release();
#ifdef _WIN32
  if (D3d12PsoCacheOn() && IsD3D12()) {
    static bool exitLogRegistered = false;
    if (!exitLogRegistered) {
      exitLogRegistered = true;
      std::atexit([] {
        if (!g || !g->device || !IsD3D12()) return;
        const auto* dev = static_cast<const plume::D3D12Device*>(g->device.get());
        const uint64_t t0 = g_prewarm_t0_ns.load();
        const uint64_t t1 = g_prewarm_end_ns.load();
        const double prewarmMs = t0 ? double((t1 ? t1 : NowNs()) - t0) / 1e6 : 0.0;
        std::fprintf(stderr, "[nr-pso] hits=%llu misses=%llu created=%llu prewarm_built=%zu prewarm_ms=%.0f\n",
                     static_cast<unsigned long long>(dev->psoCacheHits.load()),
                     static_cast<unsigned long long>(dev->psoCacheMisses.load()),
                     static_cast<unsigned long long>(dev->pipelinesCreated.load()), g_prewarm_done.load(), prewarmMs);
      });
    }
  }
#endif
  StartRecordThread();
  // GPU timers (WWE2K14_NR_GPU_TIMERS=1): default off - no query pools, no timestamps, nothing recorded.
  if (const char* gt = std::getenv("WWE2K14_NR_GPU_TIMERS")) g->gpu_timers = std::strcmp(gt, "1") == 0;
  if (const char* gi = std::getenv("WWE2K14_NR_GPU_ITEMS")) g->gpu_items = std::strcmp(gi, "1") == 0;
  if (const char* gp = std::getenv("WWE2K14_NR_GPU_PASSES")) g->gpu_passes = std::strcmp(gp, "1") == 0;
  if (const char* gd = std::getenv("WWE2K14_NR_GPU_DRAWS")) {
    g->gpu_draws = g->gpu_timers && (std::strcmp(gd, "1") == 0 || std::strcmp(gd, "2") == 0);
    g->gpu_draws_keys = g->gpu_draws && std::strcmp(gd, "2") == 0;
    if (const char* st = std::getenv("WWE2K14_NR_GPU_DRAWS_STUCK")) g_dp_stuck = size_t(std::max(0, std::atoi(st)));  // test knob
  }
  if (const char* gs = std::getenv("WWE2K14_NR_GPU_SHADER_TOP"))
    g->gpu_shader_top = g->gpu_draws && std::strcmp(gs, "1") == 0;
  if (g->gpu_shader_top) g->gpu_draws_keys = false;
  // Frame slots (WWE2K14_NR_FRAME_SLOTS=1): Vulkan only (D3D12 binds the constant ring as one root SRV by offset).
  // Slot 0 is the set created above; slots 1.. get their own constant ring and upload ring of the same size.
  // constants32 keeps one ring bound as set 4, so frame slots are disabled for that ABI.
  const char* fs = std::getenv("WWE2K14_NR_FRAME_SLOTS");
  if (fs && std::strcmp(fs, "1") == 0 && !g->abi.frame_slots_eligible) {
    std::fprintf(stderr, "[nr] frame slots: off (constants32 uses one set-4 ring)\n");
  } else if (fs && std::strcmp(fs, "1") == 0 && !IsD3D12()) {
    bool ok = true;
    for (uint32_t i = 1; i < kFrameSlots && ok; ++i) {
      FrameRes& f = g->parked[i];
      f.constants = g->device->createBuffer(RenderBufferDesc::UploadBuffer(
          uint64_t(kConstBytes) * g->const_slots, RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE));
      f.constants_mapped = f.constants ? static_cast<uint8_t*>(f.constants->map()) : nullptr;
      ok = f.constants_mapped != nullptr;
      if (!ok) break;
      std::memset(f.constants_mapped, 0, size_t(kConstBytes) * g->const_slots);
      f.constants_address = f.constants->getDeviceAddress();
      if (g->upload_ring) {
        f.upload_ring_size = g->upload_ring_size;
        f.upload_ring = g->device->createBuffer(
            RenderBufferDesc::UploadBuffer(f.upload_ring_size, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
        f.upload_ring_mapped = f.upload_ring ? static_cast<uint8_t*>(f.upload_ring->map()) : nullptr;
        ok = f.upload_ring_mapped != nullptr;
      }
    }
    g->frame_slots = ok;
    std::fprintf(stderr, "[nr] frame slots: %s\n", ok ? "on (3 frames in flight)" : "allocation failed, off");
  }
  // Card P24: WWE2K14_NR_CONST_SLOTS=N (test only) shrinks the effective constants ring to force wraps;
  // WWE2K14_NR_CONST_FENCE=1 reuses wrapped slots after per-list fence waits instead of Submit() (see ConstFenceWrap).
  if (const char* cs = std::getenv("WWE2K14_NR_CONST_SLOTS"); cs && *cs) {
    const long n = std::strtol(cs, nullptr, 10);
    if (n >= 2 && n < long(g->const_slots)) g->const_slots = uint32_t(n);
    std::fprintf(stderr, "[nr] constants ring: %u slots (WWE2K14_NR_CONST_SLOTS, test only)\n", g->const_slots);
  }
  g->const_safe = g->const_slots;
  if (const char* cf = std::getenv("WWE2K14_NR_CONST_FENCE"); cf && std::strcmp(cf, "1") == 0) {
    bool ok = !g->frame_slots;  // frame slots swap the constants ring per frame: not combined with this path
    if (ok && g->upload_ring) {  // the spare set needs its own upload ring (RingAlloc never runs without one)
      FrameRes& f = g->fence_spare;
      f.upload_ring_size = g->upload_ring_size;
      f.upload_ring = g->device->createBuffer(
          RenderBufferDesc::UploadBuffer(f.upload_ring_size, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
      f.upload_ring_mapped = f.upload_ring ? static_cast<uint8_t*>(f.upload_ring->map()) : nullptr;
      ok = f.upload_ring_mapped != nullptr;
      if (!ok) {
        f.upload_ring.reset();
        f.upload_ring_size = 0;
      }
    }
    g->const_fence = ok;
    std::fprintf(stderr, "[nr] constants fence (P24): %s\n",
                 ok ? "on" : g->frame_slots ? "off (frame slots active)" : "upload ring allocation failed, off");
  }
  // Card P22: WWE2K14_NR_PRECISE_BARRIERS=1: barrier masks from the layouts left/entered (plume) + adjacent barriers batched.
  const char* sp = std::getenv("WWE2K14_NR_SNAP_POOL");
  g_snap_pool_env = !(sp && std::strcmp(sp, "0") == 0);
  if (const char* ps = std::getenv("WWE2K14_NR_PHYS_SNAP"); !ps || std::strcmp(ps, "0") != 0) g_phys_snap_on = true;
  if (const char* pc = std::getenv("WWE2K14_NR_PHYS_SNAP_CHECK"); pc && std::strcmp(pc, "1") == 0)
    g_phys_snap_check = true;
  std::fprintf(stderr, "[nr] physical-size snapshots (U7): %s check=%s\n",
               g_phys_snap_on ? "on" : "off", g_phys_snap_check && g_phys_snap_on ? "on" : "off");
  if (g_phys_snap_check && !g_phys_snap_on)
    std::fprintf(stderr, "[nr] WWE2K14_NR_PHYS_SNAP_CHECK ignored unless WWE2K14_NR_PHYS_SNAP=1\n");
  if (const char* pb = std::getenv("WWE2K14_NR_PRECISE_BARRIERS"); pb && std::strcmp(pb, "1") == 0) g_precise_env = true;
  ApplyPreciseBarriers();
  std::fprintf(stderr, "[nr] precise barriers (P22): %s\n", g_precise_env ? "on" : "off");
  GpuListBegin(g->cmd_pool[0]);

  // Index 0: "not bound" placeholders (magenta texture in every table, default sampler).
  TextureDesc magenta;
  LevelData level;
  level.bytes = {0xFF, 0x00, 0xFF, 0xFF};
  std::string err;
  const uint32_t placeholder = CreateTexture(magenta, {level}, &err);
  if (placeholder != 0) {
    *error = "placeholder texture: " + err;
    return false;
  }
  SamplerIndex(SamplerDesc());
  InitResolve(shader_dir);
  InitSharedTextureDetile(shader_dir);
  // Card V1 bounded diagnostic (remove when V1 closes; default off): WWE2K14_NR_VRAM_BALLAST_MB=N holds N MB of
  // device-local memory that is never used, so a large card has the free budget of a small one (the driver budget and
  // the U2 steering then behave as on a 2 GB card with other programs using VRAM). Dedicated 256 MB buffers.
  if (const char* e = std::getenv("WWE2K14_NR_VRAM_BALLAST_MB"); e && *e) {
    const uint64_t want = uint64_t(std::strtoull(e, nullptr, 10)) << 20;
    uint64_t held = 0;
    while (held < want) {
      const uint64_t n = std::min<uint64_t>(want - held, 256ull << 20);
      RenderBufferDesc d = RenderBufferDesc::DefaultBuffer(n);
      d.committed = true;
      auto b = g->device->createBuffer(d);
      if (!b) break;
      g->vram_ballast.push_back(std::move(b));
      held += n;
    }
    std::fprintf(stderr, "[nr] WWE2K14_NR_VRAM_BALLAST_MB: holding %llu of %llu MB; heaps %s\n",
                 (unsigned long long)(held >> 20), (unsigned long long)(want >> 20), HeapBudgets().c_str());
  }
  return true;
}

namespace {
// Integer view format for exact-bit clears (the SDK's GetColorOwnershipTransferVulkanFormat on a device with
// integer MSAA support).
RenderFormat IntegerFormat(TargetFormat f) {
  switch (f) {
    case TargetFormat::kRG16S:
    case TargetFormat::kRG16F: return RenderFormat::R16G16_UINT;
    case TargetFormat::kRGBA16S:
    case TargetFormat::kRGBA16F: return RenderFormat::R16G16B16A16_UINT;
    case TargetFormat::kR32F: return RenderFormat::R32_UINT;
    case TargetFormat::kRG32F: return RenderFormat::R32G32_UINT;
    default: return RenderFormat::UNKNOWN;
  }
}
// Card GAP-pass2 (WWE2K14_NR_RT_VIEW_FORMATS=1, default off; read once): colour targets state their view formats.
bool RtViewFormatsOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_NR_RT_VIEW_FORMATS");
    const bool v = e && std::strcmp(e, "1") == 0;
    if (v) std::fprintf(stderr, "[nr] colour target view formats (WWE2K14_NR_RT_VIEW_FORMATS): %s\n", IsD3D12() ? "off on DXIL" : "on");
    return v;
  }();
  return on;
}
void ApplyRtViewFormats(RenderTextureDesc& desc, RenderFormat integer) {
  const auto plan = nr_rt_views::PlanFor(RtViewFormatsOn(), IsD3D12(), desc.format, integer, RenderFormat::UNKNOWN);
  desc.k14ViewFormatsKnown = plan.known;
  desc.k14ViewFormat2 = plan.second;
  if (!plan.known) return;
  static std::atomic<uint32_t> count{0};  // bounded: first few, then powers of two
  const uint32_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8 || (n & (n - 1)) == 0)
    std::fprintf(stderr, "[nr-rt-views] #%u %ux%u format=%u second_view=%u\n", n, desc.width, desc.height, uint32_t(desc.format),
                 uint32_t(plan.second));
}
RenderMultisampling Multisampling(uint32_t samples) {
  return RenderMultisampling(samples == 4   ? RenderSampleCount::COUNT_4
                             : samples == 2 ? RenderSampleCount::COUNT_2
                                            : RenderSampleCount::COUNT_1);
}
RenderFramebuffer* Framebuffer(uint32_t color, uint32_t depth) {
  // Card S3-39: one framebuffer per active resolution set of each attachment (bit 31 = the native set of a scaled surface).
  const uint32_t ck = color != kNone && g->surfaces[color].mode ? (color | 0x80000000u) : color;
  const uint32_t dk = depth != kNone && g->surfaces[depth].mode ? (depth | 0x80000000u) : depth;
  auto& fb = g->framebuffers[{ck, dk}];
  if (!fb) {
    const RenderTexture* colors[1] = {color != kNone ? g->surfaces[color].image.get() : nullptr};
    fb = g->device->createFramebuffer(RenderFramebufferDesc(colors, color != kNone ? 1 : 0,
                                                            depth != kNone ? g->surfaces[depth].image.get() : nullptr));
  }
  return fb.get();
}
}  // namespace

bool SurfaceSize(uint32_t surface, uint32_t* width, uint32_t* height) {
  if (BatchCall()) {
    const auto it = g_surface_by_handle.find(surface);
    if (it != g_surface_by_handle.end()) {
      *width = it->second->width;
      *height = it->second->height;
      return true;
    }
    return QueueSyncCall(kBatchDrainValueCall, BatchHashWords("surface_size", {surface}),
                         [=] { return SurfaceSize(surface, width, height); });
  }
  if (surface >= g->surfaces.size()) return false;
  *width = g->surfaces[surface].lw;
  *height = g->surfaces[surface].lh;
  return true;
}

#ifndef WWE2K14_NO_TEST_TOOLS
bool TestForceSurfaceRefusal(bool depth, TargetFormat format, uint32_t samples) {
  const char* value = std::getenv("WWE2K14_TEST_RT_REFUSE");
  if (!value || !*value) return false;
  const std::string_view list(value);
  for (size_t begin = 0; begin <= list.size();) {
    const size_t end = list.find(',', begin);
    const std::string_view token = list.substr(begin, end == std::string_view::npos ? end : end - begin);
    if ((token == "d24s8" && depth && format == TargetFormat::kD24S8) ||
        (token == "rgba16s" && !depth && format == TargetFormat::kRGBA16S) ||
        (token == "rg16s" && !depth && format == TargetFormat::kRG16S) ||
        (token == "msaa" && samples > 1))
      return true;
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return false;
}
#endif

uint32_t Surface(uint64_t key, uint32_t width, uint32_t height, bool depth, TargetFormat format, uint32_t samples,
                 std::string* error) {
  if (BatchCall()) {
    auto it = g_surface_reservations.find(key);
    if (it != g_surface_reservations.end()) return it->second->handle;
    auto reservation = std::make_shared<SurfaceReservation>();
    reservation->handle = g_surface_next.fetch_add(1, std::memory_order_relaxed);
    reservation->width = width;
    reservation->height = height;
    g_surface_reservations.emplace(key, reservation);
    g_surface_by_handle.emplace(reservation->handle, reservation);
    EnqueueJob([reservation, key, width, height, depth, format, samples] {
      t_reserved_surface = reservation->handle;
      std::string err;
      const uint32_t actual = Surface(key, width, height, depth, format, samples, &err);
      t_reserved_surface = kNone;
      if (actual == reservation->handle) {
        reservation->has_alt.store(SurfaceHasAlt(actual), std::memory_order_relaxed);
      } else {
        AsyncFail("surface", err.empty() ? "surface reservation mismatch" : err);
      }
      reservation->done.store(true, std::memory_order_release);
    }, BatchHashWords("surface_create", {key, width, height, uint64_t(depth), uint32_t(format), samples}),
       "surface_create");
    return reservation->handle;
  }
  ProfScope prof_scope(kProfSurface);
  auto it = g->surface_by_key.find(key);
  if (it != g->surface_by_key.end()) {
    TouchSurface(it->second);
    return it->second;
  }
  SurfaceObj t;
  if (g->mem_owners_on) t.mem_last_use_present = g->mem_present;
  t.lw = width;
  t.lh = height;
  // Card S3-31 / S3-47a (opt-in WWE2K14_D3D_SCENE_SCALE=s, 0.4..3.0; picture change): the 1280-wide screen-sized
  // pairs (the main scene / its tiled 1280x736 extent, which also carries the HUD) get a host image of s x the size.
  static const float scene_scale = [] {
    const char* e = std::getenv("WWE2K14_D3D_SCENE_SCALE");
    const float v = e ? std::strtof(e, nullptr) : 1.0f;
    return v >= 0.4f && v <= 3.0f ? v : 1.0f;
  }();
  if (scene_scale != 1.0f && width == 1280 && height >= 720 && height <= 768) {
    t.scale = scene_scale;
    width = uint32_t(std::lround(double(width) * scene_scale));
    height = uint32_t(std::lround(double(height) * scene_scale));
  }
  t.depth = depth;
  t.samples = samples;
  const RenderFormat requested_render_format = depth
      ? (format == TargetFormat::kD24S8 ? RenderFormat::D24_UNORM_S8_UINT : RenderFormat::D32_FLOAT_S8_UINT)
      : ToFormat(format);
  const RenderFormat requested_int_format = depth ? RenderFormat::UNKNOWN : IntegerFormat(format);
#ifndef WWE2K14_NO_TEST_TOOLS
  const bool forced_refusal = TestForceSurfaceRefusal(depth, format, samples);
#else
  constexpr bool forced_refusal = false;
#endif
  auto build = [&](SurfaceObj& target, uint32_t width, uint32_t height, RenderFormat render_format,
                   RenderFormat int_format, uint32_t target_samples, bool int_view_disabled,
                   bool force_image_failure, std::string* failed_step, bool* view_missing) {
    auto fail = [&](const char* step) {
      *failed_step = step;
      return false;
    };
    auto note_view = [&] {
      *view_missing = true;
      if (failed_step->empty()) *failed_step = "view";
    };
    target.width = width;
    target.height = height;
    target.samples = target_samples;
    target.format = render_format;
    target.int_format = depth ? RenderFormat::UNKNOWN : int_format;
    target.int_view_disabled = int_view_disabled;
#ifndef WWE2K14_NO_TEST_TOOLS
    if (force_image_failure) return fail("image");
#else
    (void)force_image_failure;
#endif
    const RenderMultisampling ms = Multisampling(target_samples);
    if (depth) {
      target.image = NewTexture(RenderTextureDesc::DepthTarget(width, height, target.format, ms));
      if (!target.image) return fail("image");
      target.depth_view = target.image->createTextureView(RenderTextureViewDesc::Texture2D(target.format));
      RenderTextureViewDesc sv = RenderTextureViewDesc::Texture2D(target.format);
      sv.stencilAspect = true;
      target.stencil_view = target.image->createTextureView(sv);
      if (!target.depth_view || !target.stencil_view) note_view();  // Preserve the pre-L1 non-fatal view behavior.
      target.transfer_fb = g->device->createFramebuffer(RenderFramebufferDesc(nullptr, 0, target.image.get()));
      if (!target.transfer_fb) return fail("framebuffer");
    } else {
      RenderTextureDesc color_desc = RenderTextureDesc::ColorTarget(width, height, target.format, ms);
      // [wwe13 nr] D3D12 needs a typeless resource to add the integer view; Vulkan ignores the flag.
      color_desc.typeless = IsD3D12() && target.int_format != RenderFormat::UNKNOWN;
      ApplyRtViewFormats(color_desc, target.int_format);
      target.image = NewTexture(color_desc);
      if (!target.image) return fail("image");
      if (target_samples > 1) {
        RenderTextureDesc resolved_desc = RenderTextureDesc::ColorTarget(width, height, target.format);
        ApplyRtViewFormats(resolved_desc, RenderFormat::UNKNOWN);  // read and resolved into as itself only
        target.resolved = NewTexture(resolved_desc);
        if (!target.resolved) return fail("resolved");
      }
      // [wwe13 nr] D3D12: setTexture needs an explicit view (Vulkan falls back to the image's own view).
      if (IsD3D12()) {
        target.sample_view = target.image->createTextureView(RenderTextureViewDesc::Texture2D(target.format));
        if (!target.sample_view) note_view();  // Preserve the pre-L1 non-fatal view behavior.
      }
      const RenderTexture* colors[1] = {target.image.get()};
      RenderFramebufferDesc fd(colors, 1);
      const RenderTextureView* views[1] = {nullptr};
      if (target.int_format != RenderFormat::UNKNOWN) {
        target.int_view = target.image->createTextureView(RenderTextureViewDesc::Texture2D(target.int_format));
        if (!target.int_view) note_view();  // Preserve the pre-L1 non-fatal view behavior.
        views[0] = target.int_view.get();
        fd.colorAttachmentViews = views;
      }
      target.transfer_fb = g->device->createFramebuffer(fd);
      if (!target.transfer_fb) return fail("framebuffer");
    }
    return true;
  };
  std::string failed_step;
  bool view_missing = false;
  bool built = build(t, width, height, requested_render_format, requested_int_format, samples, false,
                     forced_refusal, &failed_step, &view_missing);
  RenderFormat fallback_render_format = requested_render_format;
  RenderFormat fallback_int_format = requested_int_format;
  uint32_t fallback_samples = samples;
  bool fallback_int_view_disabled = false;
  // V152 review (11-09): no SNORM16 -> FLOAT16 fallback. Its integer clears carry raw 16-bit components (nr_shared.cpp
  // ColorClearValue) that a float image cannot hold, so the fallback would clear to wrong values. SNORM16 targets that the
  // card refuses stay refused and are named in the warning line instead.
  if (depth && format == TargetFormat::kD24S8) {
    fallback_render_format = RenderFormat::D32_FLOAT_S8_UINT;
  }
  if (samples > 1) fallback_samples = 1;
  const bool retry_available = fallback_render_format != requested_render_format ||
                               fallback_int_format != requested_int_format || fallback_samples != samples;
  bool retry_attempted = false, retry_succeeded = false, retry_view_missing = false;
  std::string retry_step;
  if (!built) {
    if (retry_available) {
      retry_attempted = true;
      SurfaceObj retry;
      retry.depth = depth;
      retry.lw = t.lw;
      retry.lh = t.lh;
      retry.scale = t.scale;
      retry.mem_last_use_present = t.mem_last_use_present;
      t = SurfaceObj{};  // Drop any partial first-attempt resources before allocating the fallback.
      retry_succeeded = build(retry, width, height, fallback_render_format, fallback_int_format, fallback_samples,
                              fallback_int_view_disabled, false, &retry_step, &retry_view_missing);
      if (retry_succeeded) {
        t = std::move(retry);
        built = true;
      }
    }
    std::ostringstream detail;
    detail << "[nr] target create failed key=" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << key
           << std::dec << " " << width << "x" << height << " depth=" << (depth ? 1 : 0)
           << " TargetFormat=" << uint32_t(format) << " original_format=" << uint32_t(requested_render_format)
           << " samples=" << samples << " forced=" << (forced_refusal ? 1 : 0)
           << " step=" << (failed_step.empty() ? "unknown" : failed_step)
           << " view_missing=" << (view_missing ? 1 : 0) << " fallback_format=" << uint32_t(fallback_render_format)
           << " fallback_samples=" << fallback_samples << " int_view_off=" << (fallback_int_view_disabled ? 1 : 0)
           << " retry_attempted=" << (retry_attempted ? 1 : 0) << " retry_succeeded=" << (retry_succeeded ? 1 : 0)
           << " retry_step=" << (retry_attempted ? (retry_step.empty() ? "unknown" : retry_step) : "not-attempted")
           << " retry_view_missing=" << (retry_view_missing ? 1 : 0);
    const std::string failure_detail = detail.str();
    if (g_surface_failure_logged.insert(key).second && built) QueueSurfaceWarning(failure_detail);
    if (!built) {
      *error = failure_detail;
      return UINT32_MAX;
    }
  }
  // Card S3-39: a scaled surface also gets its native-size set (inactive until SetSceneScaleActive(false)).
  static const bool scale_dynamic = [] {
    const char* e = std::getenv("WWE2K14_D3D_SCALE_DYNAMIC");
    return e && std::strcmp(e, "1") == 0;
  }();
  if (built && scale_dynamic && t.scale != 1.0f) {
    t.alt = std::make_unique<SurfaceObj>();
    t.alt->depth = t.depth;
    t.alt->samples = t.samples;
    t.alt->format = t.format;
    t.alt->int_format = t.int_format;
    t.alt->int_view_disabled = t.int_view_disabled;
    t.alt->scale = 1.0f;
    t.alt->lw = t.lw;
    t.alt->lh = t.lh;
    if (g->mem_owners_on) t.alt->mem_last_use_present = g->mem_present;
    std::string alt_failed_step;
    bool alt_view_missing = false;
    if (!build(*t.alt, t.lw, t.lh, t.format, t.int_format, t.samples, t.int_view_disabled, false,
               &alt_failed_step, &alt_view_missing))
      t.alt.reset();
  }
  const uint32_t handle = t_reserved_surface != kNone ? t_reserved_surface :
                          g_batch_n ? g_surface_next.fetch_add(1, std::memory_order_relaxed) :
                                      uint32_t(g->surfaces.size());
  if (handle >= g->surfaces.size()) g->surfaces.resize(size_t(handle) + 1);
  g->surfaces[handle] = std::move(t);
  g->surface_by_key[key] = handle;
  return handle;
}

// Card S3-52: frees a render target the D3D layer evicted (its slot was unused for a while and the render-target byte
// budget is full). The handle stays an empty entry (never reused); every call taking a surface rejects it (LiveSurface).
// Card S3-52b: no GPU wait. The images, views and framebuffers retire with the serial the active list will get (every
// list that can use them is submitted at or before it) and are freed once the GPU passed it (ReapSurfaces).
// WWE2K14_NR_EVICT_DRAIN=1: the old full drain (FinishGpuWork) per released surface, then immediate free.
bool ReleaseSurface(uint32_t handle) {
  if (BatchCall()) {
    const bool released = QueueSyncCall(kBatchDrainSurfaceRelease, BatchHashWords("release_surface", {handle}),
                                        [=] { return ReleaseSurface(handle); });
    if (released) {
      const auto it = g_surface_by_handle.find(handle);
      if (it != g_surface_by_handle.end()) {
        const auto key = std::find_if(g_surface_reservations.begin(), g_surface_reservations.end(),
                                      [&](const auto& kv) { return kv.second == it->second; });
        if (key != g_surface_reservations.end()) g_surface_reservations.erase(key);
        g_surface_by_handle.erase(it);
      }
    }
    return released;
  }
  ProfScope prof_scope(kProfSurface);
  if (!g || !LiveSurface(handle)) return false;
  for (const LazySnap& e : g_lazy)
    if (e.surface == handle) return false;  // card S3-15: a pending copy still reads it
  static const bool drain = [] {
    const char* e = std::getenv("WWE2K14_NR_EVICT_DRAIN");
    return e && std::strcmp(e, "1") == 0;
  }();
  State::RetiredSurface r;
  if (drain) {
    FinishGpuWork();
  } else {
    SyncRecordThread(kProfFnCount, false, kBatchDrainSurfaceRelease);
    if (g->state_elide) EndDrawPass();  // an open elided pass may be on one of its framebuffers
    if (!g->retired_surfaces.empty()) ReapSurfaces(false);
    r.serial = g->submit_serial + 1;
  }
  for (auto it = g->framebuffers.begin(); it != g->framebuffers.end();) {
    const uint32_t c = it->first.first, d = it->first.second;
    const bool uses = (c != kNone && (c & 0x7FFFFFFFu) == handle) || (d != kNone && (d & 0x7FFFFFFFu) == handle);
    if (uses && !drain) r.fbs.push_back(std::move(it->second));
    it = uses ? g->framebuffers.erase(it) : std::next(it);
  }
  for (auto it = g->surface_by_key.begin(); it != g->surface_by_key.end();) it = it->second == handle ? g->surface_by_key.erase(it) : std::next(it);
  if (!drain) {
    r.handle = handle;
    r.obj = std::move(g->surfaces[handle]);
    g->retired_surfaces.push_back(std::move(r));
  }
  g->surfaces[handle] = SurfaceObj{};
  return true;
}

// Card S3-39: swap the active resolution set of a dual surface (between frames; no pass may be open on it).
static void SwapSurfaceSet(SurfaceObj& a, SurfaceObj& b) {
  std::swap(a.image, b.image);
  std::swap(a.resolved, b.resolved);
  std::swap(a.sample_view, b.sample_view);
  std::swap(a.int_view, b.int_view);
  std::swap(a.depth_view, b.depth_view);
  std::swap(a.stencil_view, b.stencil_view);
  std::swap(a.transfer_fb, b.transfer_fb);
  std::swap(a.width, b.width);
  std::swap(a.height, b.height);
  std::swap(a.scale, b.scale);
}
bool SurfaceHasAlt(uint32_t surface) {
  if (BatchCall()) {
    const auto it = g_surface_by_handle.find(surface);
    if (it != g_surface_by_handle.end()) {
      const auto& reservation = it->second;
      if (!reservation->done.load(std::memory_order_acquire)) SyncRecordThread(kProfSurface, false, kBatchDrainSurfaceAlt);
      return reservation->done.load(std::memory_order_acquire) && reservation->has_alt.load(std::memory_order_relaxed);
    }
    return QueueSyncCall(kBatchDrainSurfaceAlt, BatchHashWords("surface_has_alt", {surface}),
                         [=] { return SurfaceHasAlt(surface); });
  }
  return g && surface < g->surfaces.size() && g->surfaces[surface].alt != nullptr;
}
uint32_t SetSceneScaleActive(bool active) {
  if (BatchCall()) return QueueSyncCall(kBatchDrainValueCall, BatchHashWords("scene_scale_active", {uint64_t(active)}),
                                        [=] { return SetSceneScaleActive(active); });
  ProfScope prof_scope(kProfSurface);
  const uint8_t want = active ? 0 : 1;
  uint32_t swapped = 0;
  for (SurfaceObj& t : g->surfaces) {
    if (!t.alt || t.mode == want) continue;
    if (!swapped && g->state_elide) EndDrawPass();
    SwapSurfaceSet(t, *t.alt);
    t.mode = want;
    ++swapped;
  }
  return swapped;
}

namespace {
// View + bindless table slot of a texture whose levels are uploaded (CreateTexture).
uint32_t FinishTexture(std::unique_ptr<Texture> tex, const TextureDesc& desc, RenderFormat format, uint32_t mip_count,
                       std::string* error) {
  RenderTextureViewDesc view = RenderTextureViewDesc::Texture2D(format);
  if (desc.cube) {
    view.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
    view.arraySize = 6;
  }
  view.mipLevels = mip_count;
  view.componentMapping = RenderComponentMapping(ToSwizzle(desc.swizzle[0]), ToSwizzle(desc.swizzle[1]),
                                                 ToSwizzle(desc.swizzle[2]), ToSwizzle(desc.swizzle[3]));
  tex->view = tex->Image()->createTextureView(view);
  if (g->mem_owners_on) tex->mem_last_use_present = g->mem_present;
  if (g->textures.empty()) {
    tex->table_index = 0;  // the placeholder
  } else if (!g->free_table_indices.empty()) {
    tex->table_index = g->free_table_indices.back();
    g->free_table_indices.pop_back();
  } else if (g->next_table_index < kTableSize) {
    tex->table_index = g->next_table_index++;
  } else {
    *error = "texture table full";
    return UINT32_MAX;
  }
  g->tables[desc.cube ? 2 : 0]->setTexture(tex->table_index, tex->Image(), RenderTextureLayout::SHADER_READ,
                                           tex->view.get());
  // Card S3-50: the handle the caller reserved for this queued job, else the next one (handles are never reused).
  uint32_t handle = t_reserved_tex;
  t_reserved_tex = kNone;
  if (handle == kNone) handle = g->tex_next.fetch_add(1, std::memory_order_relaxed);
  if (handle >= g->textures.size()) g->textures.resize(handle + 1);
  g->textures[handle] = std::move(tex);
  return handle;
}
}  // namespace

uint32_t CreateTexture(const TextureDesc& desc, const std::vector<LevelData>& levels, std::string* error) {
  if (AsyncCall()) {
    uint64_t hash = BatchHashLevels(BatchHashTextureDesc(BatchHashWords("create_texture", {}), desc), levels);
    return QueueTextureCall("create texture", hash,
                            [desc, levels](std::string* e) { return CreateTexture(desc, levels, e); });
  }
  ProfScope prof_scope(kProfCreateTexture);
  if (g->state_elide) EndDrawPass();
  auto tex = std::make_unique<Texture>();
  const RenderFormat format = ToFormat(desc.format);
  uint32_t mip_count = 1;
  for (const LevelData& l : levels) mip_count = std::max(mip_count, l.mip + 1);
  tex->texture = NewTexture(
      desc.cube ? RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D, desc.width, desc.height, 1, mip_count, 6,
                                             format, RenderTextureFlag::CUBE)
                : RenderTextureDesc::Texture2D(desc.width, desc.height, mip_count, format));
  if (!tex->texture) {
    *error = "createTexture failed";
    return UINT32_MAX;
  }
  // Upload every level through one staging buffer (512-byte aligned level offsets).
  // [wwe13 nr] D3D12: a level whose tight row pitch is not 256-byte aligned is written row by row at the aligned
  // pitch (pitches[i] != 0); Vulkan and aligned levels keep the tight copy.
  std::vector<uint64_t> offsets;
  std::vector<uint32_t> pitches;
  uint64_t total = 0;
  for (const LevelData& l : levels) {
    total = (total + 511) & ~uint64_t(511);
    offsets.push_back(total);
    const uint32_t row = IsBlock(desc.format) ? ((l.width + 3) & ~3u) : l.width;
    const uint32_t pitch = NeedsRepitch(format, row) ? AlignedPitch(format, row) : 0;
    pitches.push_back(pitch);
    total += pitch ? std::max<uint64_t>(uint64_t(pitch) * BlockRows(format, l.height), l.bytes.size()) : l.bytes.size();
  }
  // FL-4 Design A: remember the layout the loop below writes, so RefillTextureUpload can repeat it exactly.
  tex->staging_levels.resize(levels.size());
  for (size_t i = 0; i < levels.size(); ++i) {
    Texture::StagingLevel& sl = tex->staging_levels[i];
    sl.offset = offsets[i];
    sl.pitch = pitches[i];
    if (!pitches[i]) {
      sl.bytes = levels[i].bytes.size();
      continue;
    }
    const uint32_t row = IsBlock(desc.format) ? ((levels[i].width + 3) & ~3u) : levels[i].width;
    sl.tight = TightPitch(format, row);
    sl.rows = BlockRows(format, levels[i].height);
  }
  CountBuffer();
  if (g_prof) g_stats.upload_bytes += total;
  auto staging = g->device->createBuffer(RenderBufferDesc::UploadBuffer(total ? total : 4));
  uint8_t* mapped = staging ? static_cast<uint8_t*>(staging->map()) : nullptr;
  if (!mapped) {
    *error = "texture staging buffer";
    return UINT32_MAX;
  }
  for (size_t i = 0; i < levels.size(); ++i) {
    if (!pitches[i]) {
      std::memcpy(mapped + offsets[i], levels[i].bytes.data(), levels[i].bytes.size());
      continue;
    }
    const uint32_t row = IsBlock(desc.format) ? ((levels[i].width + 3) & ~3u) : levels[i].width;
    const uint64_t tight = TightPitch(format, row);
    const uint32_t rows = BlockRows(format, levels[i].height);
    for (uint32_t r = 0; r < rows && (r + 1) * tight <= levels[i].bytes.size(); ++r)
      std::memcpy(mapped + offsets[i] + uint64_t(r) * pitches[i], levels[i].bytes.data() + r * tight, tight);
  }
  staging->unmap();
  Barrier(tex->texture.get(), RenderTextureLayout::COPY_DEST);
  for (size_t i = 0; i < levels.size(); ++i) {
    const uint32_t w = levels[i].width, h = levels[i].height;
    const uint32_t row = pitches[i] ? PitchTexels(format, pitches[i]) : IsBlock(desc.format) ? ((w + 3) & ~3u) : w;
    g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(tex->texture.get(), levels[i].mip, levels[i].face),
                               RenderTextureCopyLocation::PlacedFootprint(staging.get(), format, w, h, 1, row, offsets[i]));
    ResolveCensusCopy(levels[i].bytes.size());
  }
  Barrier(tex->texture.get(), RenderTextureLayout::SHADER_READ);
  tex->staging = staging.get();  // FL-4: the copies above stay rewritable while this list is unsubmitted
  tex->create_serial = g->submit_serial;
  tex->create_cmd = g->cmd_active;
  Retire(std::move(staging));
  return FinishTexture(std::move(tex), desc, format, mip_count, error);
}

// FL-4 Design A (WWE2K14_D3D_LATE_TEX=4; see src/native/d3d/late_upload.cpp). Rewrites the staging bytes the copies
// recorded by CreateTexture will read, using the same row rules as the loop there. The caller holds the backend lock
// and does not submit; the copy is refused when the texture is gone, a level size differs, or the list that recorded
// the copies has been submitted (submit_serial/submit_cmd moved on: the bytes are already fixed).
bool RefillTextureUpload(uint32_t handle, const std::vector<LevelData>& levels, std::string* error) {
  if (BatchCall()) {
    uint64_t hash = BatchHashLevels(BatchHashWords("refill_texture", {handle}), levels);
    return QueueSyncCall(kBatchDrainTextureOrder, hash, [=] { return RefillTextureUpload(handle, levels, error); });
  }
  if (g_rq) SyncRecordThread(kProfFnCount, false, kBatchDrainTextureOrder);  // the texture's queued CreateTexture must have run
  if (!g || handle == 0 || handle >= g->textures.size() || !g->textures[handle]) {
    *error = "late upload: texture released";
    return false;
  }
  Texture& t = *g->textures[handle];
  if (!t.staging) {
    *error = "late upload: no staging";
    return false;
  }
  if (t.create_serial != g->submit_serial || t.create_cmd != g->cmd_active) {
    *error = "late upload: list submitted";
    return false;
  }
  if (t.staging_levels.size() != levels.size()) {
    *error = "late upload: level count";
    return false;
  }
  uint8_t* mapped = static_cast<uint8_t*>(t.staging->map());
  if (!mapped) {
    *error = "late upload: map failed";
    return false;
  }
  for (size_t i = 0; i < levels.size(); ++i) {
    const Texture::StagingLevel& sl = t.staging_levels[i];
    const LevelData& l = levels[i];
    if (!sl.pitch) {
      if (l.bytes.size() != sl.bytes) {
        t.staging->unmap();
        *error = "late upload: level bytes";
        return false;
      }
      std::memcpy(mapped + sl.offset, l.bytes.data(), l.bytes.size());
      continue;
    }
    for (uint32_t r = 0; r < sl.rows && uint64_t(r + 1) * sl.tight <= l.bytes.size(); ++r) {
      std::memcpy(mapped + sl.offset + uint64_t(r) * sl.pitch, l.bytes.data() + uint64_t(r) * sl.tight, sl.tight);
    }
  }
  t.staging->unmap();
  return true;
}

// FL-4 Design A: defer the early submit while a held texture upload is pending (Kick()'s call site reads it, so the
// draws keep counting and the next release submits without an extra wait). The backend does not know the reason.
void SetKickHold(bool hold) {
  if (!g) return;
  if (AsyncCall()) {  // card S3-50: in order with the queued draws and texture builds
    EnqueueJob([hold] { SetKickHold(hold); }, BatchHashWords("kick_hold", {uint64_t(hold)}), "kick_hold");
    return;
  }
  g->kick_hold.store(hold, std::memory_order_relaxed);
}

namespace {
bool RectInSurface(const SurfaceObj& t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::string* error);
}  // namespace

// Resolve reuse (WWE2K14_NR_RESOLVE_REUSE=1). A snapshot is a sampled copy of a render-target region, taken at
// the resolve (the target is overwritten later in the frame). Colour targets of RGBA8 only; a multisampled
// target is averaged first, exactly as the D3D fixed16 snapshot path reads it.
namespace {  // same anonymous namespace as the forward declaration (card S3-15)
constexpr size_t kPresentHold = 4;  // presents a direct-present view outlives (the presenter's frames in flight)
constexpr uint64_t kFsr1LogEvery = 1800;  // card S3-46b: presents per [d3d-fsr1] counter line
// Card S3-46 (WWE2K14_D3D_FSR1=1): AMD FidelityFX FSR 1 (MIT, tools/nr/xenos/fsr1.hlsl) for scaled RGBA8 snapshots.
struct Fsr1PushConstants {
  uint32_t con0[4] = {};
  uint32_t con1[4] = {};
  uint32_t con2[4] = {};
  uint32_t con3[4] = {};
};
const RenderDescriptorRange kFsr1Ranges[3] = {
    RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_TEXTURE, 2, 1)};

uint32_t Fsr1FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

// Mirrors the vendor FsrEasuConOffset constants (ffx_fsr1.h:156-224).
Fsr1PushConstants Fsr1EasuConstants(uint32_t viewport_w, uint32_t viewport_h, uint32_t image_w, uint32_t image_h,
                                     uint32_t output_w, uint32_t output_h, uint32_t offset_x, uint32_t offset_y) {
  Fsr1PushConstants p;
  const float out_rcp_x = 1.0f / float(output_w), out_rcp_y = 1.0f / float(output_h);
  const float image_rcp_x = 1.0f / float(image_w), image_rcp_y = 1.0f / float(image_h);
  p.con0[0] = Fsr1FloatBits(float(viewport_w) * out_rcp_x);
  p.con0[1] = Fsr1FloatBits(float(viewport_h) * out_rcp_y);
  p.con0[2] = Fsr1FloatBits(0.5f * float(viewport_w) * out_rcp_x - 0.5f + float(offset_x));
  p.con0[3] = Fsr1FloatBits(0.5f * float(viewport_h) * out_rcp_y - 0.5f + float(offset_y));
  p.con1[0] = Fsr1FloatBits(image_rcp_x);
  p.con1[1] = Fsr1FloatBits(image_rcp_y);
  p.con1[2] = Fsr1FloatBits(image_rcp_x);
  p.con1[3] = Fsr1FloatBits(-image_rcp_y);
  p.con2[0] = Fsr1FloatBits(-image_rcp_x);
  p.con2[1] = Fsr1FloatBits(2.0f * image_rcp_y);
  p.con2[2] = Fsr1FloatBits(image_rcp_x);
  p.con2[3] = Fsr1FloatBits(2.0f * image_rcp_y);
  p.con3[0] = Fsr1FloatBits(0.0f);
  p.con3[1] = Fsr1FloatBits(4.0f * image_rcp_y);
  return p;
}

std::shared_ptr<RenderTexture> Fsr1Upscale(RenderTexture* source, uint32_t image_w, uint32_t image_h, uint32_t x,
                                           uint32_t y, uint32_t viewport_w, uint32_t viewport_h, uint32_t output_w,
                                           uint32_t output_h, std::string* error) {
  if (!g->fsr1_on || !source || !image_w || !image_h || !viewport_w || !viewport_h || !output_w || !output_h) {
    *error = "fsr1: invalid source or extent";
    return nullptr;
  }
  const bool use_rcas = g->fsr1_sharpness >= 0.0f;  // negative stops disable RCAS.
  // Card S3-46b: the presented output comes from a ring of kFsr1Ring images (> kPresentHold, the presents a view is
  // kept); a slot is rewritten only when nothing else holds it (use_count 1: no present view / flash tap), so every
  // earlier read of it was recorded before this write on the same queue (the layout barrier orders them).
  constexpr size_t kFsr1Ring = kPresentHold + 2;
  auto ring_image = [&]() -> std::shared_ptr<RenderTexture> {
    if (g->fsr1_ring_w != output_w || g->fsr1_ring_h != output_h) {
      for (auto& r : g->fsr1_ring) g->retired_snapshots.push_back(std::move(r));
      g->fsr1_ring.assign(kFsr1Ring, nullptr);
      g->fsr1_ring_w = output_w, g->fsr1_ring_h = output_h, g->fsr1_ring_next = 0;
    }
    for (size_t n = 0; n < kFsr1Ring; ++n) {
      std::shared_ptr<RenderTexture>& r = g->fsr1_ring[(g->fsr1_ring_next + n) % kFsr1Ring];
      if (r && r.use_count() > 1) continue;
      g->fsr1_ring_next = (g->fsr1_ring_next + n + 1) % kFsr1Ring;
      if (r) {
        ++g->fsr1_stats.out_reuse;
        return r;
      }
      r.reset(NewTexture(RenderTextureDesc::Texture2D(output_w, output_h, 1, RenderFormat::R8G8B8A8_UNORM,
                                                      RenderTextureFlag::STORAGE)).release());
      ++g->fsr1_stats.out_allocs;
      return r;
    }
    ++g->fsr1_stats.out_allocs;  // every slot held (should not happen): a one-off image
    return std::shared_ptr<RenderTexture>(NewTexture(RenderTextureDesc::Texture2D(
        output_w, output_h, 1, RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::STORAGE)).release());
  };
  // With RCAS the EASU output is an intermediate: one persistent image per size (GPU work on one queue is ordered and
  // every use is fenced by the barriers below), not an allocation per frame.
  static std::shared_ptr<RenderTexture> easu_tmp;
  static uint32_t easu_tmp_w = 0, easu_tmp_h = 0;
  std::shared_ptr<RenderTexture> easu, rcas;
  if (!use_rcas) {
    easu = ring_image();  // card S3-46b: EASU writes the presented image itself
  } else if (easu_tmp && easu_tmp_w == output_w && easu_tmp_h == output_h) {
    easu = easu_tmp;
  } else {
    easu.reset(NewTexture(RenderTextureDesc::Texture2D(output_w, output_h, 1, RenderFormat::R8G8B8A8_UNORM,
                                                       RenderTextureFlag::STORAGE)).release());
    if (easu) {
      if (easu_tmp) g->retired_snapshots.push_back(std::move(easu_tmp));
      easu_tmp = easu, easu_tmp_w = output_w, easu_tmp_h = output_h;
    }
  }
  if (use_rcas) rcas = ring_image();
  if (!easu || (use_rcas && !rcas)) {
    *error = "fsr1: storage image creation failed";
    return nullptr;
  }
  auto dispatch = [&](RenderPipeline* pipeline, RenderTexture* src, RenderTexture* dst,
                       const Fsr1PushConstants& push) -> bool {
    if (g->fsr1_next == g->fsr1_sets.size()) {
      g->fsr1_sets.push_back(g->device->createDescriptorSet(RenderDescriptorSetDesc(kFsr1Ranges, 3)));
      if (!g->fsr1_sets.back()) {
        g->fsr1_sets.pop_back();
        *error = "fsr1: descriptor set creation failed";
        return false;
      }
    }
    RenderDescriptorSet* set = g->fsr1_sets[g->fsr1_next++].get();
    Barrier(src, RenderTextureLayout::SHADER_READ);
    Barrier(dst, RenderTextureLayout::GENERAL);
    set->setTexture(0, src, RenderTextureLayout::SHADER_READ);
    set->setSampler(1, g->fsr1_sampler.get());
    set->setTexture(2, dst, RenderTextureLayout::GENERAL);
    RenderCommandList* l = g->list;
    l->setPipeline(pipeline);
    l->setComputePipelineLayout(g->fsr1_layout.get());
    l->setComputeDescriptorSet(set, 0);
    l->setComputePushConstants(0, &push, 0, sizeof(push));
    l->dispatch((output_w + 15) / 16, (output_h + 15) / 16, 1);
    Barrier(dst, RenderTextureLayout::SHADER_READ);
    return true;
  };
  const Fsr1PushConstants easu_push =
      Fsr1EasuConstants(viewport_w, viewport_h, image_w, image_h, output_w, output_h, x, y);
  if (!dispatch(g->fsr1_easu_pipeline.get(), source, easu.get(), easu_push)) return nullptr;
  if (!use_rcas) return easu;
  Fsr1PushConstants rcas_push;
  rcas_push.con0[0] = Fsr1FloatBits(std::exp2(-g->fsr1_sharpness));
  if (!dispatch(g->fsr1_rcas_pipeline.get(), easu.get(), rcas.get(), rcas_push)) return nullptr;
  return rcas;
}

// Owner 2026-10-22 ceiling experiment (WWE2K14_D3D_COPY_CEILING=1, TEST ONLY - wrong pictures): keep every resolve/snapshot
// allocation and bookkeeping but skip the GPU work of the Xbox-EDRAM-model copies (snapshot resolve+copy/blit except the
// presented pair, fixed16 and depth conversions, destination-canvas seed detile), to measure the fps ceiling of removing them.
// Kept as the measuring tool of the EDRAM de-emulation work (card docs/cards/DE-6-edram-copy-ceiling.md: UHD 53.0/53.5 ->
// 57.5 fps with all classes skipped); remove together with the last EDRAM-model copy it measures.
// Value: 1 = all classes; or a bit mask s (snapshot) f (fixed16) d (depth) e (seed), e.g. "s" or "fde".
uint32_t CopyCeilingMask() {
  static const uint32_t m = [] {
    const char* e = std::getenv("WWE2K14_D3D_COPY_CEILING");
    if (!e || !*e || std::strcmp(e, "0") == 0) return 0u;
    if (std::strcmp(e, "1") == 0) return 15u;
    uint32_t v = 0;
    for (const char* c = e; *c; ++c) v |= *c == 's' ? 1u : *c == 'f' ? 2u : *c == 'd' ? 4u : *c == 'e' ? 8u : 0u;
    return v;
  }();
  return m;
}
bool CopyCeilingOn(uint32_t cls) { return (CopyCeilingMask() & cls) != 0; }
bool CopyCeilingOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_COPY_CEILING");
    const bool v = CopyCeilingMask() != 0;
    if (v) std::fprintf(stderr, "[nr] WWE2K14_D3D_COPY_CEILING=1: TEST ONLY - EDRAM-model copies skipped, pictures will be wrong\n");
    return v;
  }();
  return on;
}
std::shared_ptr<RenderTexture> CeilingImage(uint32_t w, uint32_t h, RenderFormat f) {
  std::shared_ptr<RenderTexture> image(NewTexture(RenderTextureDesc::Texture2D(w, h, 1, f)));
  if (image) Barrier(image.get(), RenderTextureLayout::SHADER_READ);
  return image;
}

// The copy part of SnapshotColor: a new image holding the (averaged) region of colour surface t.
std::shared_ptr<RenderTexture> CopySurfaceRegion(SurfaceObj& t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                                 State::SnapHi* hi, std::string* error) {
  ItemScope item("snapshot", 0, w, h, 0);
  if (g->state_elide) EndDrawPass();
  if (CopyCeilingOn(1u) && !hi) {
    std::shared_ptr<RenderTexture> image = CeilingImage(w, h, t.format);
    if (!image) *error = "snapshot: createTexture failed";
    return image;
  }
  if (t.scale != 1.0f) {
    // Card S3-31: a logical-size snapshot of a scaled surface - (MSAA: average the physical rect into t.resolved) then
    // a linear-filtered blit of the physical rect into a w x h image. Every reader sees the native size.
    const int32_t px0 = PhysEdge(t, int32_t(x), false), py0 = PhysEdge(t, int32_t(y), true);
    const int32_t px1 = std::max(PhysEdge(t, int32_t(x + w), false), px0 + 1);
    const int32_t py1 = std::max(PhysEdge(t, int32_t(y + h), true), py0 + 1);
    RenderTexture* source = t.image.get();
    // Only the 1280x720 pair (post-processing output / front-buffer source, which also carries the HUD), not the
    // 1280x736 tiled scene pairs: one EASU (+RCAS) per frame instead of one per scene snapshot.
    // Card S3-46: FSR 1 runs once per presented frame (PresentViewOfSnapshot), not per snapshot (gameplay frames take
    // ~13 snapshots of these pairs; per-snapshot EASU cost the UHD 630 17 fps, S113b/S115). Every reader keeps the
    // linear blit below; the 1280x720 pair's full snapshots also keep their physical rect for the present.
    const bool fsr1_keep = hi && g->fsr1_on && t.format == RenderFormat::R8G8B8A8_UNORM && t.scale < 1.0f && x == 0 &&
                           y == 0 && w == 1280 && h == 720 && t.lw == 1280 && t.lh == 720;
    const bool phys_snap = g_phys_snap_on && hi && !fsr1_keep && t.format == RenderFormat::R8G8B8A8_UNORM &&
                           t.scale < 1.0f && x == 0 && y == 0 && w == t.lw && h <= t.lh && px1 > px0 && py1 > py0;
    if (phys_snap) {
      RenderTexture* source = t.image.get();
      if (t.samples > 1) {
        {
          BarrierBatch bb;
          bb.Layout(t.image.get(), RenderTextureLayout::RESOLVE_SOURCE);
          bb.Layout(t.resolved.get(), RenderTextureLayout::RESOLVE_DEST);
        }
        RenderRect r(px0, py0, px1, py1);
        g->list->resolveTextureRegion(t.resolved.get(), uint32_t(px0), uint32_t(py0), t.image.get(), &r);
        source = t.resolved.get();
      }
      const uint32_t pw = uint32_t(px1 - px0), ph = uint32_t(py1 - py0);
      std::shared_ptr<RenderTexture> physical(NewTexture(RenderTextureDesc::Texture2D(pw, ph, 1, t.format)));
      if (!physical) {
        if (g_phys_snap_check) {
          ++g_stats.phys_snap_check_failures;
          std::shared_ptr<RenderTexture> logical(NewTexture(RenderTextureDesc::Texture2D(w, h, 1, t.format)));
          if (!logical) {
            *error = "snapshot physical check: physical and logical image creation failed";
            return nullptr;
          }
          {
            BarrierBatch bb;
            bb.Layout(source, RenderTextureLayout::COPY_SOURCE);
            bb.Layout(logical.get(), RenderTextureLayout::COPY_DEST);
          }
          if (!g->list->blitTexture(logical.get(), RenderBox(0, 0, int32_t(w), int32_t(h)), source,
                                    RenderBox(px0, py0, px1, py1), true)) {
            *error = "snapshot physical check: legacy logical blit unsupported";
            return nullptr;
          }
          ResolveCensusBlit(uint64_t(TightPitch(t.format, w)) * BlockRows(t.format, h));
          Barrier(logical.get(), RenderTextureLayout::SHADER_READ);
          return logical;
        }
        *error = "snapshot physical: createTexture failed";
        return nullptr;
      }
      {
        BarrierBatch bb;
        bb.Layout(source, RenderTextureLayout::COPY_SOURCE);
        bb.Layout(physical.get(), RenderTextureLayout::COPY_DEST);
      }
      const RenderBox source_box(px0, py0, px1, py1);
      g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(physical.get()),
                                 RenderTextureCopyLocation::Subresource(source), 0, 0, 0, &source_box);
      ResolveCensusCopy(uint64_t(TightPitch(t.format, pw)) * BlockRows(t.format, ph));
      Barrier(physical.get(), RenderTextureLayout::SHADER_READ);

      State::SnapHi physical_info;
      physical_info.image = physical;
      physical_info.width = pw;
      physical_info.height = ph;
      physical_info.upscale = false;
      physical_info.logical_width = w;
      physical_info.logical_height = h;
      physical_info.logical_pending = !g_phys_snap_check;
      ++g_stats.phys_snap_color;
      if (g_phys_snap_check) {
        std::shared_ptr<RenderTexture> logical(NewTexture(RenderTextureDesc::Texture2D(w, h, 1, t.format)));
        if (!logical) {
          g->retired_snapshots.push_back(std::move(physical));
          *error = "snapshot physical check: logical createTexture failed";
          return nullptr;
        }
        {
          BarrierBatch bb;
          bb.Layout(source, RenderTextureLayout::COPY_SOURCE);
          bb.Layout(logical.get(), RenderTextureLayout::COPY_DEST);
        }
        if (!g->list->blitTexture(logical.get(), RenderBox(0, 0, int32_t(w), int32_t(h)), source,
                                  RenderBox(px0, py0, px1, py1), true)) {
          g->retired_snapshots.push_back(std::move(physical));
          *error = "snapshot physical check: logical blit unsupported";
          return nullptr;
        }
        ResolveCensusBlit(uint64_t(TightPitch(t.format, w)) * BlockRows(t.format, h));
        Barrier(logical.get(), RenderTextureLayout::SHADER_READ);
        ComparePhysicalSnapshot(logical, physical, w, h, pw, ph, t.format, true);
        physical_info.logical_pending = false;
        *hi = std::move(physical_info);
        return logical;
      }
      *hi = std::move(physical_info);
      return physical;
    }
    std::shared_ptr<RenderTexture> image(NewTexture(RenderTextureDesc::Texture2D(w, h, 1, t.format)));
    if (!image) {
      *error = "snapshot: createTexture failed";
      return nullptr;
    }
    if (t.samples > 1) {
      {
        BarrierBatch bb;
        bb.Layout(t.image.get(), RenderTextureLayout::RESOLVE_SOURCE);
        bb.Layout(t.resolved.get(), RenderTextureLayout::RESOLVE_DEST);
      }
      RenderRect r(px0, py0, px1, py1);
      g->list->resolveTextureRegion(t.resolved.get(), uint32_t(px0), uint32_t(py0), t.image.get(), &r);
      source = t.resolved.get();
    }
    // Card S3-47a: a surface drawn ABOVE native size (scale > 1) snapshotted from its origin over its full width (the
    // whole surface, or the 1280x720 scene of a 1280x736 tiled pair) also keeps the physical rect 1:1 (no blit, no
    // filtering; the caller passes a fresh SnapHi). The logical-size image below stays for every other reader.
    if ((hi && t.scale > 1.0f && x == 0 && y == 0 && w == t.lw && h <= t.lh) || fsr1_keep) {
      const uint32_t pw = uint32_t(px1 - px0), ph = uint32_t(py1 - py0);
      std::shared_ptr<RenderTexture> phys;
      if (fsr1_keep) {
        // Card S3-46b: a few slots per surface, rewritten in turn by its full snapshots (only the presented one is
        // read, by the FSR present, which checks that its generation is still the slot's latest).
        ++g->fsr1_stats.keep_copies;
        std::shared_ptr<RenderTexture>& slot = t.fsr1_phys[t.fsr1_phys_next];
        t.fsr1_phys_next = uint8_t((t.fsr1_phys_next + 1) % SurfaceObj::kFsr1PhysSlots);
        if (slot) {
          const RenderTextureDesc& pd = D3D12TextureDescOf(slot.get());
          if (pd.width != pw || pd.height != ph) {
            g->fsr1_slot_gen.erase(slot.get());
            g->retired_snapshots.push_back(std::move(slot));
          }
        }
        if (!slot) {
          slot = NewTexture(RenderTextureDesc::Texture2D(pw, ph, 1, t.format));
          ++g->fsr1_stats.phys_allocs;
        }
        phys = slot;
      } else {
        phys = NewTexture(RenderTextureDesc::Texture2D(pw, ph, 1, t.format));
      }
      if (phys) {
        {
          BarrierBatch bb;
          bb.Layout(source, RenderTextureLayout::COPY_SOURCE);
          bb.Layout(phys.get(), RenderTextureLayout::COPY_DEST);
        }
        RenderBox phys_box(px0, py0, px1, py1);
        g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(phys.get()),
                                   RenderTextureCopyLocation::Subresource(source), 0, 0, 0, &phys_box);
        ResolveCensusCopy(uint64_t(TightPitch(t.format, pw)) * BlockRows(t.format, ph));
        Barrier(phys.get(), RenderTextureLayout::SHADER_READ);
        if (fsr1_keep) hi->fsr1_gen = g->fsr1_slot_gen[phys.get()] = ++g->fsr1_gen_next;
        hi->image = std::move(phys);
        hi->width = pw;
        hi->height = ph;
        hi->upscale = t.scale < 1.0f;
      }
    }
    {
      BarrierBatch bb;
      bb.Layout(source, RenderTextureLayout::COPY_SOURCE);
      bb.Layout(image.get(), RenderTextureLayout::COPY_DEST);
    }
    if (!g->list->blitTexture(image.get(), RenderBox(0, 0, int32_t(w), int32_t(h)), source, RenderBox(px0, py0, px1, py1), true)) {
      *error = "snapshot: scaled blit unsupported";
      return nullptr;
    }
    ResolveCensusBlit(uint64_t(TightPitch(t.format, w)) * BlockRows(t.format, h));
    Barrier(image.get(), RenderTextureLayout::SHADER_READ);
    return image;
  }
  const bool pool_on = g_snap_pool_env;
  std::shared_ptr<RenderTexture> image;
  if (pool_on) {
    const State::SnapKey key{w, h, t.format};
    RecordMutexLock lock(g->snap_mu, "snapshot_table_mutex");
    for (size_t i = g->snap_pool.size(); i-- > 0;) {
      if (g->snap_pool[i].k == key) {
        image = std::move(g->snap_pool[i].img);
        g->snap_pool_bytes -= g->snap_pool[i].bytes;
        g->snap_pool.erase(g->snap_pool.begin() + i);
        break;
      }
    }
    if (image) {
      if (g_prof) ++g_stats.snap_pool_hits;
    } else {
      if (g_prof) ++g_stats.snap_pool_misses;
      image = NewTexture(RenderTextureDesc::Texture2D(w, h, 1, t.format));
      if (image) g->snap_keys[image.get()] = key;
    }
    if (g_prof) g_stats.snap_pool_size = g->snap_pool.size();
  } else {
    image = NewTexture(RenderTextureDesc::Texture2D(w, h, 1, t.format));
  }
  if (!image) {
    *error = "snapshot: createTexture failed";
    return nullptr;
  }
  RenderTexture* source = t.image.get();
  bool direct = false;
  {
    BarrierBatch bb;  // k14 P22: batched (precise barriers only)
    if (t.samples > 1) {
      bb.Layout(t.image.get(), RenderTextureLayout::RESOLVE_SOURCE);
      // Card L2-B (pool on): average the region straight into the snapshot image at (0,0); t.resolved is not touched
      // (every other reader of it resolves into it first).
      // Card S3-30 (WWE2K14_D3D_SNAP_DIRECT=1): the same direct resolve without the image pool.
      static const bool snap_direct = [] {
        const char* e = std::getenv("WWE2K14_D3D_SNAP_DIRECT");
        return e && std::strcmp(e, "1") == 0;
      }();
      direct = pool_on || snap_direct;
      bb.Layout(direct ? image.get() : t.resolved.get(), RenderTextureLayout::RESOLVE_DEST);
      bb.Flush();
      RenderRect r(int32_t(x), int32_t(y), int32_t(x + w), int32_t(y + h));
      if (direct) {
        g->list->resolveTextureRegion(image.get(), 0, 0, t.image.get(), &r);
        ResolveCensusMsaaResolve(uint64_t(TightPitch(t.format, w)) * BlockRows(t.format, h));
      } else {
        g->list->resolveTextureRegion(t.resolved.get(), x, y, t.image.get(), &r);
        ResolveCensusMsaaResolve(uint64_t(TightPitch(t.format, w)) * BlockRows(t.format, h));
        bb.Layout(t.resolved.get(), RenderTextureLayout::COPY_SOURCE);
        source = t.resolved.get();
      }
    } else {
      bb.Layout(t.image.get(), RenderTextureLayout::COPY_SOURCE);
    }
    if (!direct) bb.Layout(image.get(), RenderTextureLayout::COPY_DEST);
  }
  if (direct) {
    if (g_prof) ++g_stats.snap_direct_resolve;
  } else {
    RenderBox box(int32_t(x), int32_t(y), int32_t(x + w), int32_t(y + h));
    g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(image.get()),
                               RenderTextureCopyLocation::Subresource(source), 0, 0, 0, &box);
    ResolveCensusCopy(uint64_t(TightPitch(t.format, w)) * BlockRows(t.format, h));
  }
  Barrier(image.get(), RenderTextureLayout::SHADER_READ);
  return image;
}
}  // namespace

// Card S3-50: a snapshot id for a new entry - the id the caller reserved for this queued job (t_reserved_snap), else the
// newest freed id, else the next new one (as before: free list first).
static uint32_t TakeSnapshotSlot() {
  uint32_t id;
  if (t_reserved_snap != kNone) {
    id = t_reserved_snap;
    t_reserved_snap = kNone;
  } else {
    RecordMutexLock lk(g->snap_alloc_mu, "snapshot_id_mutex");
    if (!g->free_snapshots.empty()) {
      id = g->free_snapshots.back();
      g->free_snapshots.pop_back();
    } else {
      id = g->snap_next++;
    }
  }
  if (id >= g->snapshots.size()) {
    g->snapshots.resize(id + 1);
    g->snapshot_formats.resize(id + 1, RenderFormat::UNKNOWN);
    g->snapshot_hi.resize(id + 1);
  }
  return id;
}
static void FreeSnapshotSlot(uint32_t id) {
  RecordMutexLock lk(g->snap_alloc_mu, "snapshot_id_mutex");
  g->free_snapshots.push_back(id);
}
static uint32_t NewSnapshotId(std::shared_ptr<RenderTexture> image, RenderFormat format, State::SnapHi hi = {}) {
  const uint32_t id = TakeSnapshotSlot();
  g->snapshots[id] = std::move(image);
  g->snapshot_formats[id] = format;
  g->snapshot_hi[id] = std::move(hi);
  if (id >= g->snapshot_last_use_present.size()) g->snapshot_last_use_present.resize(size_t(id) + 1);
  g->snapshot_last_use_present[id] = g->mem_present;
  return id;
}

// Card S3-15 (draw-off only, SetLazySnapshots): a colour snapshot is a PROMISE until something reads it. SnapshotColor
// records (surface, rect) and leaves the source in a read layout; the copy is made at the first reader (EnsureSnap in
// every consumer) or just before the source is written again (MaterializeSource, called by every barrier into a
// write layout - draws, clears and copies all reach the source through one). Released before either: never copied.
uint32_t SnapshotColor(uint32_t surface, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::string* error) {
  if (AsyncCall()) return QueueSnapshotCall("snapshot colour", BatchHashWords("snapshot_color", {surface, x, y, w, h}),
                                            [=](std::string* e) { return SnapshotColor(surface, x, y, w, h, e); });
  ProfScope prof_scope(kProfResolve);
  if (!LiveSurface(surface) || g->surfaces[surface].depth || !w || !h) {
    *error = "snapshot: bad colour surface";
    return kNone;
  }
  SurfaceObj& t = g->surfaces[surface];
  TouchSurface(surface);
  // Any colour format (NO_GUEST lazy resolves keep a snapshot of every copy); only RGBA8 ones can be sampled as a
  // texture or presented (TextureFromSnapshot / PresentViewOfSnapshot check the format).
  if (!RectInSurface(t, x, y, w, h, error)) return kNone;
  if (g_lazy_on) {
    if (g->state_elide) EndDrawPass();
    // Leave the source in a read layout: the next write to it must pass a barrier, which makes the copy first.
    Barrier(t.image.get(), t.samples > 1 ? RenderTextureLayout::RESOLVE_SOURCE : RenderTextureLayout::COPY_SOURCE);
    const uint32_t id = NewSnapshotId(nullptr, t.format);
    LazySnap e;
    e.surface = surface;
    e.src = t.image.get();
    e.x = x, e.y = y, e.w = w, e.h = h;
    e.ids.push_back(id);
    g_lazy.push_back(std::move(e));
    ++g_lazy_stats.created;
    return id;
  }
  State::SnapHi hi;
  std::shared_ptr<RenderTexture> image = CopySurfaceRegion(t, x, y, w, h, &hi, error);
  if (!image) return kNone;
  return NewSnapshotId(std::move(image), t.format, std::move(hi));
}

// WWE2K14_NR_SHOT_EVERY: reads an RGBA8 snapshot (w x h) back to the CPU, tight rows. One readback per call.
bool ReadSnapshot(uint32_t id, uint32_t w, uint32_t h, std::vector<uint8_t>& bytes, std::string* error) {
  if (BatchCall()) return QueueSyncCall(kBatchDrainReadSnapshot, BatchHashWords("read_snapshot", {id, w, h}),
                                        [=, &bytes] { return ReadSnapshot(id, w, h, bytes, error); });
  SyncRecordThread(kProfFnCount, false, kBatchDrainReadSnapshot);
  if (!EnsureLogicalSnap(id, error)) return false;
  if (!g || id >= g->snapshots.size() || !g->snapshots[id] || g->snapshot_formats[id] != RenderFormat::R8G8B8A8_UNORM ||
      !w || !h) {
    *error = "read snapshot: bad snapshot";
    return false;
  }
  DpScope dp;  // card D1
  if (DpOn()) dp.Begin(DpOpLabel("read_snapshot"));
  if (g->state_elide) EndDrawPass();
  RenderTexture* image = g->snapshots[id].get();
  auto rb = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(w) * h * 4));
  if (!rb) {  // k14 G37
    *error = "read snapshot: readback buffer";
    return false;
  }
  Barrier(image, RenderTextureLayout::COPY_SOURCE);
  RenderBox box(0, 0, int32_t(w), int32_t(h));
  if (!CopyTextureToTight(rb.get(), 0, RenderFormat::R8G8B8A8_UNORM, w, h, image, box)) {
    *error = "read snapshot: repitch buffer";
    return false;
  }
  Barrier(image, RenderTextureLayout::SHADER_READ);
  Submit();
  const uint8_t* src = static_cast<const uint8_t*>(rb->map());
  if (!src) {
    *error = "read snapshot: map readback";
    return false;
  }
  bytes.assign(src, src + size_t(w) * h * 4);
  rb->unmap();
  ResolveCensusReadback(uint64_t(w) * h * 4);
  return true;
}

void OfflineFrameEnd() {
  if (BatchCall()) {
    QueueSyncCall(kBatchDrainPresent, BatchHashWords("offline_frame_end", {}), [] {
      OfflineFrameEnd();
      return true;
    });
    return;
  }
  SyncRecordThread(kProfFnCount, false, kBatchDrainPresent);
  if (!g) return;
  if (g->state_elide) EndDrawPass();
  Kick();
  FrameEnd();
}

bool ReadSnapshotRaw(uint32_t id, uint32_t* format, uint32_t* w, uint32_t* h, std::vector<uint8_t>& bytes,
                     std::string* error) {
  if (BatchCall()) return QueueSyncCall(kBatchDrainReadSnapshot, BatchHashWords("read_snapshot_raw", {id}),
                                        [=, &bytes] { return ReadSnapshotRaw(id, format, w, h, bytes, error); });
  SyncRecordThread(kProfFnCount, false, kBatchDrainReadSnapshot);
  if (!EnsureLogicalSnap(id, error)) return false;
  if (!g || id >= g->snapshots.size() || !g->snapshots[id]) {
    *error = "read snapshot raw: bad snapshot";
    return false;
  }
  RenderTexture* image = g->snapshots[id].get();
  const RenderTextureDesc& d = IsD3D12() ? D3D12TextureDescOf(image) : static_cast<plume::VulkanTexture*>(image)->desc;
  const RenderFormat f = g->snapshot_formats[id];
  if (RenderFormatIsDepth(f) || RenderFormatBlockWidth(f) != 1 || d.multisampling.sampleCount != RenderSampleCount::COUNT_1 ||
      d.depth > 1 || !d.width || !d.height || !RenderFormatSize(f)) {
    *error = "read snapshot raw: unsupported snapshot (depth / multisampled / block format)";
    return false;
  }
  if (g->state_elide) EndDrawPass();
  const uint64_t total = uint64_t(d.width) * d.height * RenderFormatSize(f);
  auto rb = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(total));
  if (!rb) {
    *error = "read snapshot raw: readback buffer";
    return false;
  }
  Barrier(image, RenderTextureLayout::COPY_SOURCE);
  RenderBox box(0, 0, int32_t(d.width), int32_t(d.height));
  if (!CopyTextureToTight(rb.get(), 0, f, d.width, d.height, image, box)) {
    *error = "read snapshot raw: repitch buffer";
    return false;
  }
  Barrier(image, RenderTextureLayout::SHADER_READ);
  Submit();
  const uint8_t* src = static_cast<const uint8_t*>(rb->map());
  if (!src) {
    *error = "read snapshot raw: map readback";
    return false;
  }
  bytes.assign(src, src + total);
  rb->unmap();
  *format = uint32_t(f);
  *w = d.width;
  *h = d.height;
  return true;
}

uint32_t CreateSnapshotRaw(uint32_t format, uint32_t w, uint32_t h, const std::vector<uint8_t>& bytes, std::string* error) {
  {  // checked before any async hand-off (a queued call returns its reserved id at once)
    const RenderFormat f = RenderFormat(format);
    if (!w || !h || RenderFormatIsDepth(f) || RenderFormatBlockWidth(f) != 1 || !RenderFormatSize(f) ||
        bytes.size() != uint64_t(w) * h * RenderFormatSize(f)) {
      *error = "create snapshot raw: bad format / size";
      return kNone;
    }
  }
  if (AsyncCall()) return QueueSnapshotCall("create snapshot raw", BatchHashWords("create_snapshot_raw", {format, w, h}),
                                            [=](std::string* e) { return CreateSnapshotRaw(format, w, h, bytes, e); });
  const RenderFormat f = RenderFormat(format);
  if (!g || !w || !h || RenderFormatIsDepth(f) || RenderFormatBlockWidth(f) != 1 || !RenderFormatSize(f) ||
      bytes.size() != uint64_t(w) * h * RenderFormatSize(f)) {
    *error = "create snapshot raw: bad format / size";
    return kNone;
  }
  if (g->state_elide) EndDrawPass();
  std::shared_ptr<RenderTexture> image = NewTexture(RenderTextureDesc::Texture2D(w, h, 1, f));
  if (!image) {
    *error = "create snapshot raw: createTexture failed";
    return kNone;
  }
  Barrier(image.get(), RenderTextureLayout::SHADER_READ);
  // FillSnapshotRows uploads tight rows (Vulkan only, its comment): D3D12 needs 256-byte row pitches (review 11-18).
  if (IsD3D12() && NeedsRepitch(f, w)) {
    *error = "create snapshot raw: D3D12 row pitch not 256-aligned (not supported yet)";
    return kNone;
  }
  const uint32_t id = NewSnapshotId(std::move(image), f);
  if (id == kNone) return kNone;
  if (!FillSnapshotRows(id, w, h, bytes, error)) {
    ReleaseSnapshot(id);
    return kNone;
  }
  return id;
}

// Texture readback is also a non-draw consumer: when it requests the U7 logical extent, make a temporary logical image
// from the exact physical image bound for draws. Do not change the texture-table binding or retain this copy.
bool MakePhysicalSnapshotReadImage(Texture& texture, RenderFormat format, uint32_t w, uint32_t h,
                                   std::shared_ptr<RenderTexture>& logical, std::string* error) {
  if (!texture.phys_snap_image || w != texture.phys_snap_logical_width || h != texture.phys_snap_logical_height)
    return true;
  RenderTexture* source = texture.Image();
  if (!source) {
    *error = "read texture: physical snapshot image missing";
    return false;
  }
  const RenderTextureDesc& d = D3D12TextureDescOf(source);
  if (d.width == w && d.height == h) return true;
  if (d.format != format) {
    *error = "read texture: physical snapshot format mismatch";
    return false;
  }
  logical = std::shared_ptr<RenderTexture>(NewTexture(RenderTextureDesc::Texture2D(w, h, 1, format)));
  if (!logical) {
    *error = "read texture: logical snapshot createTexture failed";
    return false;
  }
  if (g->state_elide) EndDrawPass();
  {
    BarrierBatch bb;
    bb.Layout(source, RenderTextureLayout::COPY_SOURCE);
    bb.Layout(logical.get(), RenderTextureLayout::COPY_DEST);
  }
  const bool linear = format != RenderFormat::R32_FLOAT;
  const bool blit = g->list->blitTexture(logical.get(), RenderBox(0, 0, int32_t(w), int32_t(h)), source,
                                         RenderBox(0, 0, int32_t(d.width), int32_t(d.height)), linear);
  Barrier(source, RenderTextureLayout::SHADER_READ);
  if (!blit) {
    *error = "read texture: logical snapshot blit unsupported";
    logical.reset();
    return false;
  }
  ResolveCensusBlit(uint64_t(TightPitch(format, w)) * BlockRows(format, h));
  Barrier(logical.get(), RenderTextureLayout::SHADER_READ);
  return true;
}

// Card S3-69 B2-fix3 trace: read back the actual sampled image behind a texture-table handle, not the upload staging
// bytes or a reconstructed copy. Called only for a bounded set of DEST_TRACE comparisons.
bool ReadTexture(uint32_t handle, TexFormat format, uint32_t w, uint32_t h, std::vector<uint8_t>& bytes,
                 std::string* error) {
  if (BatchCall()) return QueueSyncCall(kBatchDrainReadTexture,
                                        BatchHashWords("read_texture", {handle, uint32_t(format), w, h}),
                                        [=, &bytes] { return ReadTexture(handle, format, w, h, bytes, error); });
  SyncRecordThread(kProfFnCount, false, kBatchDrainReadTexture);
  if (!g || handle >= g->textures.size() || !g->textures[handle] || !g->textures[handle]->Image() || !w || !h) {
    *error = "read texture: bad texture";
    return false;
  }
  Texture& texture = *g->textures[handle];
  RenderTexture* image = texture.Image();
  const RenderFormat rf = ToFormat(format);
  std::shared_ptr<RenderTexture> logical;
  if (rf == RenderFormat::UNKNOWN || !MakePhysicalSnapshotReadImage(texture, rf, w, h, logical, error)) return false;
  if (logical) image = logical.get();
  const RenderTextureDesc& d = D3D12TextureDescOf(image);
  if (d.width != w || d.height != h || d.format != rf) {
    *error = "read texture: extent/format mismatch";
    return false;
  }
  const uint64_t pitch = TightPitch(rf, w);
  const uint32_t rows = BlockRows(rf, h);
  const uint64_t size = pitch * rows;
  if (!size || size > (256ull << 20)) {
    *error = "read texture: invalid readback size";
    return false;
  }
  DpScope dp;
  if (DpOn()) dp.Begin(DpOpLabel("read_texture"));
  if (g->state_elide) EndDrawPass();
  auto rb = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(size));
  if (!rb) {
    *error = "read texture: readback buffer";
    return false;
  }
  Barrier(image, RenderTextureLayout::COPY_SOURCE);
  if (!CopyTextureToTight(rb.get(), 0, rf, w, h, image, RenderBox(0, 0, int32_t(w), int32_t(h)))) {
    *error = "read texture: copy image";
    return false;
  }
  Barrier(image, RenderTextureLayout::SHADER_READ);
  Submit();
  const uint8_t* src = static_cast<const uint8_t*>(rb->map());
  if (!src) {
    *error = "read texture: map readback";
    return false;
  }
  bytes.assign(src, src + size);
  rb->unmap();
  ResolveCensusReadback(size);
  return true;
}

bool ReadTextureLevel(uint32_t handle, TexFormat format, uint32_t level, uint32_t w, uint32_t h,
                      std::vector<uint8_t>& bytes, std::string* error) {
  if (BatchCall()) return QueueSyncCall(kBatchDrainReadTexture,
                                        BatchHashWords("read_texture_level", {handle, uint32_t(format), level, w, h}),
                                        [=, &bytes] { return ReadTextureLevel(handle, format, level, w, h, bytes, error); });
  SyncRecordThread(kProfFnCount, false, kBatchDrainReadTexture);
  if (!g || handle >= g->textures.size() || !g->textures[handle] || !g->textures[handle]->Image() || !w || !h) {
    *error = "read texture level: bad texture";
    return false;
  }
  Texture& texture = *g->textures[handle];
  RenderTexture* image = texture.Image();
  const RenderFormat rf = ToFormat(format);
  std::shared_ptr<RenderTexture> logical;
  if (rf == RenderFormat::UNKNOWN || (level == 0 && !MakePhysicalSnapshotReadImage(texture, rf, w, h, logical, error)))
    return false;
  if (logical) image = logical.get();
  const RenderTextureDesc& d = D3D12TextureDescOf(image);
  if (rf == RenderFormat::UNKNOWN || level >= 16 || std::max(1u, d.width >> level) != w ||
      std::max(1u, d.height >> level) != h || d.format != rf) {
    *error = "read texture level: extent/format mismatch";
    return false;
  }
  const uint64_t pitch = TightPitch(rf, w);
  const uint32_t rows = BlockRows(rf, h);
  const uint64_t size = pitch * rows;
  if (!size || size > (256ull << 20)) {
    *error = "read texture level: invalid readback size";
    return false;
  }
  DpScope dp;
  if (DpOn()) dp.Begin(DpOpLabel("read_texture_level"));
  if (g->state_elide) EndDrawPass();
  auto rb = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(size));
  if (!rb) {
    *error = "read texture level: readback buffer";
    return false;
  }
  Barrier(image, RenderTextureLayout::COPY_SOURCE);
  if (!CopyTextureToTight(rb.get(), 0, rf, w, h, image, RenderBox(0, 0, int32_t(w), int32_t(h)), level)) {
    *error = "read texture level: copy image";
    return false;
  }
  Barrier(image, RenderTextureLayout::SHADER_READ);
  Submit();
  const uint8_t* src = static_cast<const uint8_t*>(rb->map());
  if (!src) {
    *error = "read texture level: map readback";
    return false;
  }
  bytes.assign(src, src + size);
  rb->unmap();
  ResolveCensusReadback(size);
  return true;
}

uint32_t RetainSnapshot(uint32_t id) {
  if (AsyncCall()) return QueueSnapshotCall("retain snapshot", BatchHashWords("retain_snapshot", {id}),
                                            [id](std::string*) { return RetainSnapshot(id); });
  SyncRecordThread(kProfFnCount, false, kBatchDrainValueCall);
  for (LazySnap& e : g_lazy) {  // card S3-15: the new id shares the pending copy
    if (std::find(e.ids.begin(), e.ids.end(), id) != e.ids.end()) {
      const uint32_t nid = NewSnapshotId(nullptr, g->snapshot_formats[id]);
      e.ids.push_back(nid);
      return nid;
    }
  }
  if (id >= g->snapshots.size() || !g->snapshots[id]) return kNone;
  std::shared_ptr<RenderTexture> image = g->snapshots[id];
  State::SnapHi hi = id < g->snapshot_hi.size() ? g->snapshot_hi[id] : State::SnapHi{};
  const RenderFormat format = g->snapshot_formats[id];
  return NewSnapshotId(std::move(image), format, std::move(hi));
}

static void ReleaseSnapshotNow(uint32_t id) {
  CrowdFlushBoundary();  // a pending group may sample this snapshot's texture
  for (size_t i = 0; i < g_lazy.size(); ++i) {  // card S3-15: a pending copy nobody else holds is never made
    auto& ids = g_lazy[i].ids;
    auto it = std::find(ids.begin(), ids.end(), id);
    if (it == ids.end()) continue;
    ids.erase(it);
    if (ids.empty()) g_lazy.erase(g_lazy.begin() + i), ++g_lazy_stats.skipped;
    FreeSnapshotSlot(id);
    if (id < g->snapshot_hi.size()) g->snapshot_hi[id] = State::SnapHi{};
    return;
  }
  if (id >= g->snapshots.size() || !g->snapshots[id]) return;
  const RenderTexture* primary = g->snapshots[id].get();
  g->retired_snapshots.push_back(std::move(g->snapshots[id]));
  if (id < g->snapshot_hi.size() && g->snapshot_hi[id].image) {
    if (g->snapshot_hi[id].image.get() != primary)
      g->retired_snapshots.push_back(std::move(g->snapshot_hi[id].image));
    else
      g->snapshot_hi[id].image.reset();
    g->snapshot_hi[id] = State::SnapHi{};
  }
  FreeSnapshotSlot(id);
}
void ReleaseSnapshot(uint32_t id) {
  if (g_batch_n && g_rq) {
    if (t_on_record_thread) ReleaseSnapshotNow(id);
    else EnqueueJob([id] { ReleaseSnapshotNow(id); }, BatchHashWords("release_snapshot", {id}), "release_snapshot");
    return;
  }
  if (AsyncCall()) {
    EnqueueJob([id] { ReleaseSnapshotNow(id); }, BatchHashWords("release_snapshot", {id}), "release_snapshot");
    return;
  }
  if (g_rq) {
    g_pending_ops.push_back((2u << 30) | id);
    return;
  }
  ReleaseSnapshotNow(id);
}

bool UsesSharedDevice() { return g && g->shared_device; }

std::string FlashTapLine(uint64_t frame);  // card S3-44 diagnostic (defined below)
// Card S3-44 bounded diagnostic (WWE2K14_NR_FLASH_TRACE=1; remove when S3-44 is settled): every presented snapshot is
// blitted (linear) to a 64x36 thumbnail and read back a few presents later (fence-proven, no wait). A frame whose mean
// luma jumps away from BOTH neighbours while the neighbours agree is logged as `[nr-flash] spike`; a frame identical to
// the one two presents earlier but not to its predecessor as `[nr-flash] repeat-old`. The first 12 spike frames are
// written full size to WWE2K14_NR_OUT/flash-<frame>-{prev,spike,next}.ppm (one GPU wait each).
namespace {
constexpr uint32_t kFlashW = 64, kFlashH = 36, kFlashSlots = 16;
struct FlashSlot {
  std::unique_ptr<RenderBuffer> rb;
  std::unique_ptr<RenderTexture> thumb;
  std::shared_ptr<RenderTexture> image;  // the presented image, kept until harvested (spike dumps)
  uint64_t serial = 0, frame = 0;
  bool used = false;
};
struct FlashHist {
  uint64_t frame = 0;
  double luma = 0;
  std::vector<uint8_t> px;
  std::shared_ptr<RenderTexture> image;
};
FlashSlot g_flash[kFlashSlots];
std::vector<FlashHist> g_flash_hist;  // last 3 harvested frames, oldest first
uint64_t g_flash_frame = 0, g_flash_spikes = 0, g_flash_repeats = 0, g_flash_dumps = 0;
std::vector<uint64_t> g_flash_events;  // spike / repeat frames not yet taken by FlashTakeEvent
std::vector<std::pair<uint64_t, double>> g_flash_lumas;  // last 24 harvested (frame, luma)
bool FlashTraceOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_NR_FLASH_TRACE");
    return e && std::strcmp(e, "1") == 0;
  }();
  return on;
}
void FlashDump(const std::shared_ptr<RenderTexture>& image, uint64_t frame, const char* tag) {
  const char* out = std::getenv("WWE2K14_NR_OUT");
  if (!out || !image) return;
  const RenderTextureDesc& d = D3D12TextureDescOf(image.get());
  const uint32_t w = d.width, h = d.height;
  auto rb = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(w) * h * 4));
  if (!rb) return;  // k14 G37
  if (g->state_elide) EndDrawPass();
  Barrier(image.get(), RenderTextureLayout::COPY_SOURCE);
  if (!CopyTextureToTight(rb.get(), 0, RenderFormat::R8G8B8A8_UNORM, w, h, image.get(),
                          RenderBox(0, 0, int32_t(w), int32_t(h)))) {
    return;
  }
  Barrier(image.get(), RenderTextureLayout::SHADER_READ);
  Submit();
  const uint8_t* src = static_cast<const uint8_t*>(rb->map());
  if (!src) return;
  char name[64];
  std::snprintf(name, sizeof(name), "/flash-%06llu-%s.ppm", static_cast<unsigned long long>(frame), tag);
  if (FILE* f = std::fopen((std::string(out) + name).c_str(), "wb")) {
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < size_t(w) * h; ++i) std::fwrite(src + i * 4, 1, 3, f);
    std::fclose(f);
  }
  rb->unmap();
}
double FlashSeconds() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
double FlashLuma(const std::vector<uint8_t>& px) {
  double s = 0;
  for (size_t i = 0; i + 3 < px.size(); i += 4) s += 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2];
  return s / double(kFlashW * kFlashH);
}
// Records the thumbnail copy of `image` into the active list (before the present's Kick).
void FlashRecord(const std::shared_ptr<RenderTexture>& image) {
  FlashSlot& s = g_flash[g_flash_frame % kFlashSlots];
  if (s.used) return;  // not harvested yet (GPU far behind): skip this frame's sample
  if (!s.rb) {
    s.rb = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(kFlashW) * kFlashH * 4));
    s.thumb.reset(NewTexture(RenderTextureDesc::Texture2D(kFlashW, kFlashH, 1, RenderFormat::R8G8B8A8_UNORM)).release());
    if (!s.rb || !s.thumb) return;
  }
  const RenderTextureDesc& d = D3D12TextureDescOf(image.get());
  {
    BarrierBatch bb;
    bb.Layout(image.get(), RenderTextureLayout::COPY_SOURCE);
    bb.Layout(s.thumb.get(), RenderTextureLayout::COPY_DEST);
  }
  if (!g->list->blitTexture(s.thumb.get(), RenderBox(0, 0, int32_t(kFlashW), int32_t(kFlashH)), image.get(),
                            RenderBox(0, 0, int32_t(d.width), int32_t(d.height)), true)) {
    Barrier(image.get(), RenderTextureLayout::SHADER_READ);
    return;
  }
  Barrier(s.thumb.get(), RenderTextureLayout::COPY_SOURCE);
  CopyTextureToTight(s.rb.get(), 0, RenderFormat::R8G8B8A8_UNORM, kFlashW, kFlashH, s.thumb.get(),
                     RenderBox(0, 0, int32_t(kFlashW), int32_t(kFlashH)));
  Barrier(image.get(), RenderTextureLayout::SHADER_READ);
  s.image = image;
  s.frame = g_flash_frame;
  s.serial = 0;  // set after the Kick that submits it
  s.used = true;
}
void FlashHarvest() {
  (void)FlashSeconds();  // starts the clock at the first present
  for (;;) {
    // Oldest used slot first (frames are harvested in order).
    FlashSlot* best = nullptr;
    for (FlashSlot& s : g_flash) {
      if (s.used && s.serial && (!best || s.frame < best->frame)) best = &s;
    }
    if (!best || !GpuReachedSerial(best->serial)) return;
    FlashHist h;
    h.frame = best->frame;
    const uint8_t* p = static_cast<const uint8_t*>(best->rb->map());
    if (!p) return;
    h.px.assign(p, p + size_t(kFlashW) * kFlashH * 4);
    best->rb->unmap();
    h.luma = FlashLuma(h.px);
    g_flash_lumas.emplace_back(h.frame, h.luma);
    if (g_flash_lumas.size() > 24) g_flash_lumas.erase(g_flash_lumas.begin());
    h.image = std::move(best->image);
    best->used = false;
    g_flash_hist.push_back(std::move(h));
    if (g_flash_hist.size() > 3) g_flash_hist.erase(g_flash_hist.begin());
    if (g_flash_hist.size() < 3) continue;
    const FlashHist &a = g_flash_hist[0], &b = g_flash_hist[1], &c = g_flash_hist[2];
    if (a.frame + 1 != b.frame || b.frame + 1 != c.frame) continue;  // a sample was skipped: no verdict
    const double dab = std::fabs(b.luma - a.luma), dbc = std::fabs(b.luma - c.luma), dac = std::fabs(a.luma - c.luma);
    const double t = std::max(12.0, 0.25 * (a.luma + c.luma) * 0.5);
    if (dab > t && dbc > t && dac < t * 0.5) {
      ++g_flash_spikes;
      std::fprintf(stderr, "[nr-flash] spike frame=%llu t=%.2fs luma prev=%.1f this=%.1f next=%.1f (spikes %llu)\n",
                   static_cast<unsigned long long>(b.frame), FlashSeconds(), a.luma, b.luma, c.luma,
                   static_cast<unsigned long long>(g_flash_spikes));
      g_flash_events.push_back(b.frame);
      std::string w;
      for (const auto& fl : g_flash_lumas) {
        char t[32];
        std::snprintf(t, sizeof(t), " %llu:%.0f", static_cast<unsigned long long>(fl.first), fl.second);
        w += t;
      }
      std::fprintf(stderr, "[nr-flash]   lumas%s\n", w.c_str());
      std::fprintf(stderr, "[nr-flash]   taps prev:%s | this:%s | next:%s\n", FlashTapLine(a.frame).c_str(),
                   FlashTapLine(b.frame).c_str(), FlashTapLine(c.frame).c_str());
      if (g_flash_dumps < 12) {
        ++g_flash_dumps;
        FlashDump(a.image, b.frame, "prev");
        FlashDump(b.image, b.frame, "spike");
        FlashDump(c.image, b.frame, "next");
      }
    } else if (b.px != a.px && c.px == a.px && c.px != b.px) {
      ++g_flash_repeats;
      g_flash_events.push_back(c.frame);
      std::fprintf(stderr, "[nr-flash] repeat-old frame=%llu (equals frame %llu; repeats %llu)\n",
                   static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(a.frame),
                   static_cast<unsigned long long>(g_flash_repeats));
    }
    if (c.frame % 1800 == 0) {
      std::fprintf(stderr, "[nr-flash] frame=%llu spikes=%llu repeats=%llu luma=%.1f\n",
                   static_cast<unsigned long long>(c.frame), static_cast<unsigned long long>(g_flash_spikes),
                   static_cast<unsigned long long>(g_flash_repeats), c.luma);
    }
  }
}
}  // namespace

// Card S3-44 diagnostic: thumbnails of selected snapshots (stage taps) per present index, printed around spikes.
struct FlashTap {
  std::unique_ptr<RenderBuffer> rb;
  std::unique_ptr<RenderTexture> thumb;
  uint64_t serial = 0, frame = 0;
  uint32_t tag = 0;
  bool used = false;
};
FlashTap g_taps[512];
uint32_t g_tap_next = 0;
std::map<std::pair<uint64_t, uint32_t>, double> g_tap_luma;  // (present index, tag) -> mean luma
void FlashTapHarvest() {
  for (FlashTap& t : g_taps) {
    if (!t.used || !t.serial || !GpuReachedSerial(t.serial)) continue;
    const uint8_t* p = static_cast<const uint8_t*>(t.rb->map());
    if (!p) {
      t.used = false;
      continue;
    }
    std::vector<uint8_t> px(p, p + size_t(kFlashW) * kFlashH * 4);
    t.rb->unmap();
    g_tap_luma[{t.frame, t.tag}] = FlashLuma(px);
    t.used = false;
  }
  while (g_tap_luma.size() > 4096) g_tap_luma.erase(g_tap_luma.begin());
}
std::string FlashTapLine(uint64_t frame) {
  std::string out;
  for (auto it = g_tap_luma.lower_bound({frame, 0}); it != g_tap_luma.end() && it->first.first == frame; ++it) {
    char b[48];
    std::snprintf(b, sizeof(b), " %08X=%.0f", it->first.second, it->second);
    out += b;
  }
  return out;
}
void FlashTapSnapshot(uint32_t id, uint32_t tag) {
  if (!FlashTraceOn() || !g) return;
  if (g_async) SyncRecordThread(kProfFnCount, false, kBatchDrainSnapshotTable);  // diagnostic reads snapshot table
  if (id >= g->snapshots.size() || !g->snapshots[id]) return;
  if (g->snapshot_formats[id] != RenderFormat::R8G8B8A8_UNORM) return;
  FlashTap& t = g_taps[g_tap_next++ % 512];
  if (t.used) return;
  if (!t.rb) {
    t.rb = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(kFlashW) * kFlashH * 4));
    t.thumb.reset(NewTexture(RenderTextureDesc::Texture2D(kFlashW, kFlashH, 1, RenderFormat::R8G8B8A8_UNORM)).release());
    if (!t.rb || !t.thumb) return;
  }
  if (g->state_elide) EndDrawPass();
  RenderTexture* img = g->snapshots[id].get();
  const RenderTextureDesc& d = D3D12TextureDescOf(img);
  {
    BarrierBatch bb;
    bb.Layout(img, RenderTextureLayout::COPY_SOURCE);
    bb.Layout(t.thumb.get(), RenderTextureLayout::COPY_DEST);
  }
  g->list->blitTexture(t.thumb.get(), RenderBox(0, 0, int32_t(kFlashW), int32_t(kFlashH)), img,
                       RenderBox(0, 0, int32_t(d.width), int32_t(d.height)), true);
  Barrier(t.thumb.get(), RenderTextureLayout::COPY_SOURCE);
  CopyTextureToTight(t.rb.get(), 0, RenderFormat::R8G8B8A8_UNORM, kFlashW, kFlashH, t.thumb.get(),
                     RenderBox(0, 0, int32_t(kFlashW), int32_t(kFlashH)));
  Barrier(img, RenderTextureLayout::SHADER_READ);
  t.frame = g_flash_frame;
  t.tag = tag;
  t.serial = ~0ull;  // set to the submitting serial at the next present
  t.used = true;
}

bool FlashTakeEvent(uint64_t* frame) {
  if (g_flash_events.empty()) return false;
  *frame = g_flash_events.front();
  g_flash_events.erase(g_flash_events.begin());
  return true;
}

uint64_t PresentViewOfSnapshot(uint32_t id, const uint8_t swizzle[4], std::string* error, uint32_t* width,
                               uint32_t* height) {
  if (BatchCall()) {
    std::array<uint8_t, 4> s{};
    std::copy(swizzle, swizzle + s.size(), s.begin());
    return QueueSyncCall(kBatchDrainPresent,
                         BatchHashWords("present_view", {id, uint64_t(s[0]) | (uint64_t(s[1]) << 8) |
                                                                (uint64_t(s[2]) << 16) | (uint64_t(s[3]) << 24)}),
                         [=] { return PresentViewOfSnapshot(id, s.data(), error, width, height); });
  }
  SyncRecordThread(kProfFnCount, false, kBatchDrainPresent);
  EnsureSnap(id);
  if (!g || !g->shared_device) {
    *error = "direct present: no shared device";
    return 0;
  }
#ifndef _WIN32
  if (IsD3D12()) return 0;
#endif
  if (id >= g->snapshots.size() || !g->snapshots[id] || g->snapshot_formats[id] != RenderFormat::R8G8B8A8_UNORM) {
    *error = "direct present: bad snapshot";
    return 0;
  }
  DpScope dp;  // card D1
  if (DpOn()) dp.Begin(DpOpLabel("present_view"));
  if (g->state_elide) EndDrawPass();
  State::PresentView pv;
  pv.image = g->snapshots[id];
  // Card S3-47a: a whole-surface snapshot of a scaled surface also holds the physical image; present that one 1:1
  // (the swap pass samples the source by pixel index, so the swap size must be the image size).
  if (id < g->snapshot_hi.size() && g->snapshot_hi[id].image && !g->snapshot_hi[id].upscale) {
    pv.image = g->snapshot_hi[id].image;
    if (width) *width = g->snapshot_hi[id].width;
    if (height) *height = g->snapshot_hi[id].height;
  } else if (id < g->snapshot_hi.size() && g->snapshot_hi[id].image && g->fsr1_on) {
    // Card S3-46: the presented frame of a below-native surface - FSR 1 (EASU + RCAS) from its physical copy.
    const State::SnapHi& lo = g->snapshot_hi[id];
    // Card S3-46b: the copy lives in its surface's reused slot; a later snapshot of that surface may have rewritten it
    // (then present the logical snapshot, as without FSR).
    auto gen = g->fsr1_slot_gen.find(lo.image.get());
    if (lo.fsr1_gen && (gen == g->fsr1_slot_gen.end() || gen->second != lo.fsr1_gen)) {
      ++g->fsr1_stats.stale;
    } else {
      const RenderTextureDesc& d = IsD3D12() ? D3D12TextureDescOf(g->snapshots[id].get())
                                             : static_cast<plume::VulkanTexture*>(g->snapshots[id].get())->desc;
      std::string ferr;
      std::shared_ptr<RenderTexture> up =
          Fsr1Upscale(lo.image.get(), lo.width, lo.height, 0, 0, lo.width, lo.height, d.width, d.height, &ferr);
      if (up) {
        pv.image = std::move(up);
        ++g->fsr1_stats.upscaled;
      } else {
        ++g->fsr1_stats.failed;
      }
    }
  }
  if (g->fsr1_on && ++g->fsr1_stats.presents % kFsr1LogEvery == 0) {
    const State::Fsr1Stats& f = g->fsr1_stats;
    std::fprintf(stderr,
                 "[d3d-fsr1] presents=%llu upscaled=%llu stale=%llu failed=%llu keep_copies=%llu (%.2f/present) "
                 "phys_allocs=%llu out_allocs=%llu out_reuse=%llu\n",
                 (unsigned long long)f.presents, (unsigned long long)f.upscaled, (unsigned long long)f.stale,
                 (unsigned long long)f.failed, (unsigned long long)f.keep_copies,
                 double(f.keep_copies) / double(f.presents), (unsigned long long)f.phys_allocs,
                 (unsigned long long)f.out_allocs, (unsigned long long)f.out_reuse);
  }
  RenderTextureViewDesc vd = RenderTextureViewDesc::Texture2D(RenderFormat::R8G8B8A8_UNORM);
  vd.componentMapping = RenderComponentMapping(ToSwizzle(swizzle[0]), ToSwizzle(swizzle[1]), ToSwizzle(swizzle[2]),
                                               ToSwizzle(swizzle[3]));
  pv.view = pv.image->createTextureView(vd);
  if (!pv.view) {
    *error = "direct present: createTextureView failed";
    return 0;
  }
  Barrier(pv.image.get(), RenderTextureLayout::SHADER_READ);
  const bool flash = FlashTraceOn();  // card S3-44 diagnostic
  if (flash) FlashRecord(pv.image);
  // The presenter's swap is recorded after this on the same queue: hand the recorded work to the GPU first.
  // Card C3: the fence object of the list being submitted (stable across a cmd_pool reallocation in Kick).
  RenderCommandFence* const present_fence = g->cmd_pool[g->cmd_active].fence.get();
  Kick();
  if (flash) {
    FlashSlot& s = g_flash[g_flash_frame % kFlashSlots];
    if (s.used && s.frame == g_flash_frame && !s.serial) s.serial = g->submit_serial;
    for (FlashTap& t : g_taps) if (t.used && t.serial == ~0ull) t.serial = g->submit_serial;
    ++g_flash_frame;
  }
  FrameEnd();
  if (flash) FlashTapHarvest(), FlashHarvest();
  uint64_t handle = 0;
#ifdef _WIN32
  if (IsD3D12()) {
    // Card C3: the SDK's D3D12 presenter samples the resource itself with this view's mapping; the image was left in
    // SHADER_READ (plume: PIXEL | NON_PIXEL_SHADER_RESOURCE for the all-stages barrier above).
    const auto* d3d_view = static_cast<plume::D3D12TextureView*>(pv.view.get());
    const auto* d3d_fence = static_cast<plume::D3D12CommandFence*>(present_fence);
    handle = uint64_t(reinterpret_cast<uintptr_t>(static_cast<plume::D3D12Texture*>(pv.image.get())->d3d));
    g_d3d12_present.srv_format = uint32_t(d3d_view->format);
    g_d3d12_present.component_mapping = d3d_view->shader4ComponentMapping;
    g_d3d12_present.fence = d3d_fence ? d3d_fence->d3d : nullptr;
    g_d3d12_present.fence_value = d3d_fence && d3d_fence->fenceValue ? d3d_fence->fenceValue - 1 : 0;
    g_d3d12_present_set = handle != 0;
    // Card XB2 diagnostic (WWE2K14_NR_D3D12_MESSAGES=1, own switch; needs the SDK debug layer, cvar d3d12_debug): drain
    // the shared device's stored debug-layer messages once per present; the first 3 of each ID are printed, then a
    // per-ID count every 600 presents. Question: why the Series X shows no picture while the nr frame is correct.
    static const bool d3d12_messages = [] {
      const char* v = std::getenv("WWE2K14_NR_D3D12_MESSAGES");
      return v && std::strcmp(v, "1") == 0;
    }();
    if (d3d12_messages && g_shared_d3d12.device) {
      static std::map<int, uint64_t> seen;
      static uint64_t presents = 0;
      ID3D12InfoQueue* queue = nullptr;
      auto* dev = static_cast<ID3D12Device*>(g_shared_d3d12.device);
      if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&queue))) && queue) {
        const UINT64 count = queue->GetNumStoredMessages();
        for (UINT64 m = 0; m < count; ++m) {
          SIZE_T length = 0;
          if (FAILED(queue->GetMessage(m, nullptr, &length)) || length == 0) continue;
          std::vector<uint8_t> storage(length);
          auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
          if (FAILED(queue->GetMessage(m, message, &length))) continue;
          if (++seen[int(message->ID)] <= 3)
            std::fprintf(stderr, "[nr-d3d12-msg] present=%llu sev=%d id=%d %s\n", (unsigned long long)presents,
                         int(message->Severity), int(message->ID),
                         message->pDescription ? message->pDescription : "");
        }
        queue->ClearStoredMessages();
        queue->Release();
      } else if (presents == 0) {
        std::fprintf(stderr, "[nr-d3d12-msg] no info queue (debug layer off)\n");
      }
      if (++presents % 600 == 0) {
        std::string line;
        for (const auto& [id, n] : seen) line += " " + std::to_string(id) + "x" + std::to_string(n);
        std::fprintf(stderr, "[nr-d3d12-msg] presents=%llu ids:%s\n", (unsigned long long)presents, line.c_str());
      }
    }
  } else
#endif
  {
    (void)present_fence;
    handle = uint64_t(static_cast<plume::VulkanTextureView*>(pv.view.get())->vk);
  }
  g->present_views.push_back(std::move(pv));
  if (g->present_views.size() > kPresentHold) g->present_views.erase(g->present_views.begin());
  return handle;
}

uint32_t TextureFromSnapshot(uint32_t id, const TextureDesc& desc, std::string* error) {
  if (AsyncCall()) return QueueTextureCall("texture from snapshot",
                                           BatchHashTextureDesc(BatchHashWords("texture_from_snapshot", {id}), desc),
                                           [id, desc](std::string* e) { return TextureFromSnapshot(id, desc, e); });
  ProfScope prof_scope(kProfCreateTexture);
  EnsureSnap(id);
  const bool depth_r32f = id < g->snapshots.size() && desc.format == TexFormat::kR32F &&
                          g->snapshot_formats[id] == RenderFormat::R32_FLOAT;
  // Card P16: fixed16 copy-outs converted to 16-bit UNORM (SnapshotFixed16).
  const bool f16 = id < g->snapshots.size() &&
                   ((desc.format == TexFormat::kRGBA16 && g->snapshot_formats[id] == RenderFormat::R16G16B16A16_UNORM) ||
                    (desc.format == TexFormat::kRG16 && g->snapshot_formats[id] == RenderFormat::R16G16_UNORM));
  if (id >= g->snapshots.size() || !g->snapshots[id] || desc.cube ||
      !((desc.format == TexFormat::kRGBA8 && g->snapshot_formats[id] == RenderFormat::R8G8B8A8_UNORM) || depth_r32f || f16)) {
    *error = "snapshot texture: bad snapshot or format";
    return UINT32_MAX;
  }
  auto tex = std::make_unique<Texture>();
  // Card S3-47a: this is the whole-image reader (a view covers the whole image): use the physical companion when
  // the snapshot has one. Crop/Rows/Levels and the CPU reads keep the logical-size image.
  tex->shared = id < g->snapshot_hi.size() && g->snapshot_hi[id].image && !g->snapshot_hi[id].upscale
                    ? g->snapshot_hi[id].image : g->snapshots[id];
  if (id < g->snapshot_hi.size() && g->snapshot_hi[id].image && !g->snapshot_hi[id].upscale &&
      g->snapshot_hi[id].logical_width && g->snapshot_hi[id].logical_height) {
    tex->phys_snap_image = true;
    tex->phys_snap_logical_width = g->snapshot_hi[id].logical_width;
    tex->phys_snap_logical_height = g->snapshot_hi[id].logical_height;
  }
  TrackTextureSnapshot(tex.get(), id);
  if (g_ddiag && depth_r32f) {
    const void* key = tex->shared.get();
    auto it = g_dd_img.find(key);
    std::fprintf(stderr, "[nr-ddiag] consume seq=%llu snapshot=%u img=%p %ux%u producer_seq=%lld surface=%d min=%.6g max=%.6g nonzero=%u/%u\n",
                 (unsigned long long)++g_dd_seq, id, key, desc.width, desc.height,
                 it != g_dd_img.end() ? (long long)it->second.seq : -1LL, it != g_dd_img.end() ? int(it->second.surface) : -1,
                 it != g_dd_img.end() ? double(it->second.mn) : 0.0, it != g_dd_img.end() ? double(it->second.mx) : 0.0,
                 it != g_dd_img.end() ? it->second.nz : 0u, it != g_dd_img.end() ? it->second.n : 0u);
  }
  return FinishTexture(std::move(tex), desc,
                       depth_r32f ? RenderFormat::R32_FLOAT : f16 ? g->snapshot_formats[id] : RenderFormat::R8G8B8A8_UNORM, 1,
                       error);
}

// Card P23: a texture of the fetch size (desc.width x desc.height) holding rows 0..desc.height-1 of an RGBA8 snapshot of
// the same width, copied into a new image (a view cannot be smaller than its image: the sampled size and the normalized
// coordinates would differ). The snapshot keeps its full height for the other readers (registry, pending conversion).
// The caller guarantees the size (the registry entry: same width, more rows); plume's base image has no size to check.
// Card M2c3: see nr_backend.h.
static uint32_t AddSnapshot(std::shared_ptr<RenderTexture> image, RenderFormat format) {
  return NewSnapshotId(std::move(image), format);
}

uint32_t NewSnapshotLike(uint32_t like, uint32_t w, uint32_t h, std::string* error, bool clear) {
  if (AsyncCall()) return QueueSnapshotCall("new snapshot like",
                                            BatchHashWords("new_snapshot_like", {like, w, h, uint64_t(clear)}),
                                            [=](std::string* e) { return NewSnapshotLike(like, w, h, e, clear); });
  ProfScope prof_scope(kProfResolve);
  EnsureSnap(like);
  if (like >= g->snapshots.size() || !g->snapshots[like] || !w || !h) {
    *error = "new snapshot: bad snapshot or size";
    return kNone;
  }
  if (g->state_elide) EndDrawPass();
  const RenderFormat format = g->snapshot_formats[like];
  std::shared_ptr<RenderTexture> image = NewTexture(RenderTextureDesc::Texture2D(w, h, 1, format));
  if (!image) {
    *error = "new snapshot: createTexture failed";
    return kNone;
  }
  // Card S3-51 (WWE2K14_D3D_CANVAS_CLEAR=1): a canvas (M2c3) is filled only where resolves land; the rest of a new image
  // is undefined in Vulkan (NVIDIA tends to hand out zeroed memory, AMD does not). Clear it to transparent black first.
  static const bool canvas_clear = [] {
    const char* v = std::getenv("WWE2K14_D3D_CANVAS_CLEAR");
    return v && *v == '1';
  }();
  if (canvas_clear || clear) {
    WriteBarrier(image.get(), RenderTextureLayout::COPY_DEST);
    RenderColor c;
    for (int i = 0; i < 4; ++i) c.rgba[i] = 0.0f;
    g->list->clearTextureColor(image.get(), c);
  }
  Barrier(image.get(), RenderTextureLayout::SHADER_READ);
  return AddSnapshot(std::move(image), format);
}

// Card S3-69 (5a): whole-image CPU upload into a snapshot (the M2c3 canvas seeded from detiled guest memory).
// Mirrors CreateTexture's single-level upload (staging buffer + PlacedFootprint copy). Vulkan only: tight rows are
// fine (NeedsRepitch is D3D12-only), and D3D12 callers must use the M2c6 path instead (see dest_images MipsOn/CubeOn).
bool FillSnapshotRows(uint32_t snapshot, uint32_t w, uint32_t h, const std::vector<uint8_t>& bytes, std::string* error) {
  if (AsyncCall()) {
    const uint64_t hash = BatchHashBytes(BatchHashWords("fill_snapshot", {snapshot, w, h}), bytes.data(), bytes.size());
    return QueueBoolCall("fill snapshot", hash,
                         [=](std::string* e) { return FillSnapshotRows(snapshot, w, h, bytes, e); });
  }
  ProfScope prof_scope(kProfResolve);
  if (!EnsureLogicalSnap(snapshot, error)) return false;
  if (snapshot >= g->snapshots.size() || !g->snapshots[snapshot] || !w || !h) {
    *error = "fill snapshot: bad snapshot or empty size";
    return false;
  }
  const RenderFormat format = g->snapshot_formats[snapshot];
  const uint64_t total = uint64_t(w) * h * RenderFormatSize(format);
  if (bytes.size() != total) {
    *error = "fill snapshot: byte count mismatch";
    return false;
  }
  if (g->state_elide) EndDrawPass();
  CountBuffer();
  if (g_prof) g_stats.upload_bytes += total;
  auto staging = g->device->createBuffer(RenderBufferDesc::UploadBuffer(total ? total : 4));
  uint8_t* mapped = staging ? static_cast<uint8_t*>(staging->map()) : nullptr;
  if (!mapped) {
    *error = "fill snapshot: staging buffer";
    return false;
  }
  std::memcpy(mapped, bytes.data(), bytes.size());
  staging->unmap();
  RenderTexture* image = g->snapshots[snapshot].get();
  WriteBarrier(image, RenderTextureLayout::COPY_DEST);
  g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(image),
                             RenderTextureCopyLocation::PlacedFootprint(staging.get(), format, w, h, 1, w, 0));
  ResolveCensusCopy(total);
  Barrier(image, RenderTextureLayout::SHADER_READ);
  if (!RefreshPhysicalSnapshotDrawImage(snapshot, error)) return false;
  Retire(std::move(staging));
  return true;
}

bool CopySnapshotRegion(uint32_t dst, uint32_t dx, uint32_t dy, uint32_t src, uint32_t sx, uint32_t sy, uint32_t w,
                        uint32_t h, std::string* error) {
  if (AsyncCall())
    return QueueBoolCall("snapshot copy", BatchHashWords("snapshot_copy", {dst, dx, dy, src, sx, sy, w, h}),
                         [=](std::string* e) { return CopySnapshotRegion(dst, dx, dy, src, sx, sy, w, h, e); });
  ProfScope prof_scope(kProfResolve);
  if (!EnsureLogicalSnap(dst, error) || !EnsureLogicalSnap(src, error)) return false;
  if (dst >= g->snapshots.size() || src >= g->snapshots.size() || !g->snapshots[dst] || !g->snapshots[src] ||
      g->snapshot_formats[dst] != g->snapshot_formats[src] || !w || !h) {
    *error = "snapshot copy: bad snapshot, format mismatch or empty";
    return false;
  }
  RenderTexture* d = g->snapshots[dst].get();
  RenderTexture* s = g->snapshots[src].get();
  if (d == s) {
    *error = "snapshot copy: same image";
    return false;
  }
  if (g->state_elide) EndDrawPass();
  {
    BarrierBatch bb;
    bb.Layout(s, RenderTextureLayout::COPY_SOURCE);
    bb.Layout(d, RenderTextureLayout::COPY_DEST);
  }
  RenderBox box(int32_t(sx), int32_t(sy), int32_t(sx + w), int32_t(sy + h));
  g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(d), RenderTextureCopyLocation::Subresource(s),
                             dx, dy, 0, &box);
  ResolveCensusCopy(uint64_t(TightPitch(g->snapshot_formats[dst], w)) * BlockRows(g->snapshot_formats[dst], h));
  {
    BarrierBatch bb;
    bb.Layout(d, RenderTextureLayout::SHADER_READ);
    bb.Layout(s, RenderTextureLayout::SHADER_READ);
  }
  return RefreshPhysicalSnapshotDrawImage(dst, error);
}

uint32_t TextureFromSnapshotCrop(uint32_t id, const TextureDesc& desc, std::string* error) {
  if (AsyncCall()) return QueueTextureCall("snapshot crop",
                                           BatchHashTextureDesc(BatchHashWords("snapshot_crop", {id}), desc),
                                           [id, desc](std::string* e) { return TextureFromSnapshotCrop(id, desc, e); });
  ProfScope prof_scope(kProfCreateTexture);
  if (!EnsureLogicalSnap(id, error)) return UINT32_MAX;
  if (id >= g->snapshots.size() || !g->snapshots[id] || desc.cube || !desc.width || !desc.height) {
    *error = "snapshot crop: bad snapshot or size";
    return UINT32_MAX;
  }
  const RenderFormat f = g->snapshot_formats[id];
  const bool ok = (desc.format == TexFormat::kRGBA8 && f == RenderFormat::R8G8B8A8_UNORM) ||
                  (desc.format == TexFormat::kR32F && f == RenderFormat::R32_FLOAT) ||
                  (desc.format == TexFormat::kRGBA16 && f == RenderFormat::R16G16B16A16_UNORM) ||
                  (desc.format == TexFormat::kRG16 && f == RenderFormat::R16G16_UNORM);
  if (!ok) {
    *error = "snapshot crop: format";
    return UINT32_MAX;
  }
  if (g->state_elide) EndDrawPass();
  auto tex = std::make_unique<Texture>();
  tex->texture = NewTexture(RenderTextureDesc::Texture2D(desc.width, desc.height, 1, f));
  if (!tex->texture) {
    *error = "snapshot crop: createTexture failed";
    return UINT32_MAX;
  }
  RenderTexture* source = g->snapshots[id].get();
  {
    BarrierBatch bb;
    bb.Layout(source, RenderTextureLayout::COPY_SOURCE);
    bb.Layout(tex->texture.get(), RenderTextureLayout::COPY_DEST);
  }
  RenderBox box(0, 0, int32_t(desc.width), int32_t(desc.height));
  g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(tex->texture.get()),
                             RenderTextureCopyLocation::Subresource(source), 0, 0, 0, &box);
  ResolveCensusCopy(uint64_t(TightPitch(f, desc.width)) * BlockRows(f, desc.height));
  {
    BarrierBatch bb;
    bb.Layout(tex->texture.get(), RenderTextureLayout::SHADER_READ);
    bb.Layout(source, RenderTextureLayout::SHADER_READ);
  }
  return FinishTexture(std::move(tex), desc, f, 1, error);
}

uint32_t TextureCubeFromSnapshots(const uint32_t faces[6], const TextureDesc& desc, std::string* error) {
  if (AsyncCall()) {
    std::array<uint32_t, 6> f;
    std::copy(faces, faces + 6, f.begin());
    uint64_t hash = BatchHashTextureDesc(BatchHashWords("snapshot_cube", {}), desc);
    hash = BatchHashBytes(hash, f.data(), sizeof(f));
    return QueueTextureCall("snapshot cube", hash,
                            [f, desc](std::string* e) { return TextureCubeFromSnapshots(f.data(), desc, e); });
  }
  ProfScope prof_scope(kProfCreateTexture);
  for (int f = 0; f < 6; ++f)
    if (!EnsureLogicalSnap(faces[f], error)) return UINT32_MAX;
  if (!desc.cube || !desc.width || !desc.height) {
    *error = "snapshot cube: bad desc";
    return UINT32_MAX;
  }
  RenderFormat f = RenderFormat::UNKNOWN;
  for (int i = 0; i < 6; ++i) {
    if (faces[i] >= g->snapshots.size() || !g->snapshots[faces[i]] || (i && g->snapshot_formats[faces[i]] != f)) {
      *error = "snapshot cube: bad face snapshot or mixed formats";
      return UINT32_MAX;
    }
    f = g->snapshot_formats[faces[i]];
  }
  const bool ok = (desc.format == TexFormat::kRGBA8 && f == RenderFormat::R8G8B8A8_UNORM) ||
                  (desc.format == TexFormat::kR32F && f == RenderFormat::R32_FLOAT) ||
                  (desc.format == TexFormat::kRGBA16 && f == RenderFormat::R16G16B16A16_UNORM) ||
                  (desc.format == TexFormat::kRG16 && f == RenderFormat::R16G16_UNORM);
  if (!ok) {
    *error = "snapshot cube: format";
    return UINT32_MAX;
  }
  if (g->state_elide) EndDrawPass();
  auto tex = std::make_unique<Texture>();
  tex->texture = NewTexture(RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D, desc.width, desc.height, 1, 1,
                                                       6, f, RenderTextureFlag::CUBE));
  if (!tex->texture) {
    *error = "snapshot cube: createTexture failed";
    return UINT32_MAX;
  }
  Barrier(tex->texture.get(), RenderTextureLayout::COPY_DEST);
  RenderBox box(0, 0, int32_t(desc.width), int32_t(desc.height));
  for (uint32_t i = 0; i < 6; ++i) {
    RenderTexture* source = g->snapshots[faces[i]].get();
    Barrier(source, RenderTextureLayout::COPY_SOURCE);
    g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(tex->texture.get(), 0, i),
                               RenderTextureCopyLocation::Subresource(source), 0, 0, 0, &box);
    ResolveCensusCopy(uint64_t(TightPitch(f, desc.width)) * BlockRows(f, desc.height));
  }
  Barrier(tex->texture.get(), RenderTextureLayout::SHADER_READ);
  for (uint32_t i = 0; i < 6; ++i) Barrier(g->snapshots[faces[i]].get(), RenderTextureLayout::SHADER_READ);
  return FinishTexture(std::move(tex), desc, f, 1, error);
}

uint32_t TextureFromSnapshotLevels(const uint32_t* ids, uint32_t levels, const TextureDesc& desc, std::string* error) {
  if (AsyncCall()) {
    std::vector<uint32_t> v(ids, ids + levels);
    uint64_t hash = BatchHashTextureDesc(BatchHashWords("snapshot_levels", {levels}), desc);
    hash = BatchHashBytes(hash, v.data(), v.size() * sizeof(uint32_t));
    return QueueTextureCall("snapshot levels", hash, [v, desc](std::string* e) {
      return TextureFromSnapshotLevels(v.data(), uint32_t(v.size()), desc, e);
    });
  }
  ProfScope prof_scope(kProfCreateTexture);
  for (uint32_t i = 0; i < levels; ++i)
    if (!EnsureLogicalSnap(ids[i], error)) return UINT32_MAX;
  if (desc.cube || !desc.width || !desc.height || !levels || levels > 16) {
    *error = "snapshot levels: bad desc";
    return UINT32_MAX;
  }
  RenderFormat f = RenderFormat::UNKNOWN;
  for (uint32_t i = 0; i < levels; ++i) {
    if (ids[i] >= g->snapshots.size() || !g->snapshots[ids[i]] || (i && g->snapshot_formats[ids[i]] != f)) {
      *error = "snapshot levels: bad level snapshot or mixed formats";
      return UINT32_MAX;
    }
    f = g->snapshot_formats[ids[i]];
  }
  const bool ok = (desc.format == TexFormat::kRGBA8 && f == RenderFormat::R8G8B8A8_UNORM) ||
                  (desc.format == TexFormat::kR32F && f == RenderFormat::R32_FLOAT) ||
                  (desc.format == TexFormat::kRGBA16 && f == RenderFormat::R16G16B16A16_UNORM) ||
                  (desc.format == TexFormat::kRG16 && f == RenderFormat::R16G16_UNORM);
  if (!ok) {
    *error = "snapshot levels: format";
    return UINT32_MAX;
  }
  if (g->state_elide) EndDrawPass();
  auto tex = std::make_unique<Texture>();
  tex->texture = NewTexture(RenderTextureDesc::Texture2D(desc.width, desc.height, levels, f));
  if (!tex->texture) {
    *error = "snapshot levels: createTexture failed";
    return UINT32_MAX;
  }
  Barrier(tex->texture.get(), RenderTextureLayout::COPY_DEST);
  for (uint32_t i = 0; i < levels; ++i) {
    RenderTexture* source = g->snapshots[ids[i]].get();
    Barrier(source, RenderTextureLayout::COPY_SOURCE);
    RenderBox box(0, 0, int32_t(std::max(1u, desc.width >> i)), int32_t(std::max(1u, desc.height >> i)));
    g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(tex->texture.get(), i, 0),
                               RenderTextureCopyLocation::Subresource(source), 0, 0, 0, &box);
    ResolveCensusCopy(uint64_t(TightPitch(f, std::max(1u, desc.width >> i))) *
                      BlockRows(f, std::max(1u, desc.height >> i)));
  }
  Barrier(tex->texture.get(), RenderTextureLayout::SHADER_READ);
  for (uint32_t i = 0; i < levels; ++i) Barrier(g->snapshots[ids[i]].get(), RenderTextureLayout::SHADER_READ);
  return FinishTexture(std::move(tex), desc, f, levels, error);
}

void LogD3D12SnapshotTextureCopy() {
  if (!IsD3D12()) return;
  static std::atomic<bool> logged{false};
  if (logged.exchange(true, std::memory_order_relaxed)) return;
  std::fprintf(stderr, "[nr] D3D12 snapshot->texture copy enabled\n");
  std::fflush(stderr);
}

bool CopySnapshotRectsToTexture(uint32_t texture, uint32_t snapshot, const int32_t* rects, uint32_t count,
                                std::string* error) {
  if (AsyncCall()) {
    std::vector<int32_t> r(rects, rects + size_t(count) * 4);
    uint64_t hash = BatchHashBytes(BatchHashWords("snapshot_rects", {texture, snapshot, count}), r.data(),
                                   r.size() * sizeof(int32_t));
    return QueueBoolCall("snapshot rects", hash, [texture, snapshot, r](std::string* e) {
      return CopySnapshotRectsToTexture(texture, snapshot, r.data(), uint32_t(r.size() / 4), e);
    });
  }
  ProfScope prof_scope(kProfCreateTexture);
  if (!EnsureLogicalSnap(snapshot, error)) return false;
  if (texture >= g->textures.size() || !g->textures[texture] || !g->textures[texture]->Image() ||
      snapshot >= g->snapshots.size() || !g->snapshots[snapshot]) {
    *error = "snapshot rects: bad texture or snapshot";
    return false;
  }
  RenderTexture* dst = g->textures[texture]->Image();
  RenderTexture* src = g->snapshots[snapshot].get();
  const RenderTextureDesc& dst_desc = D3D12TextureDescOf(dst);
  if (dst_desc.format != g->snapshot_formats[snapshot]) {
    *error = "snapshot rects: format mismatch";
    return false;
  }
  if (g->state_elide) EndDrawPass();
  {
    BarrierBatch bb;
    bb.Layout(src, RenderTextureLayout::COPY_SOURCE);
    bb.Layout(dst, RenderTextureLayout::COPY_DEST);
  }
  bool copied = false;
  for (uint32_t i = 0; i < count; ++i) {
    const int32_t* r = rects + 4 * i;
    if (r[2] <= r[0] || r[3] <= r[1]) continue;
    if (copied) WriteBarrier(dst, RenderTextureLayout::COPY_DEST);
    RenderBox box(r[0], r[1], r[2], r[3]);
    g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(dst), RenderTextureCopyLocation::Subresource(src),
                               uint32_t(r[0]), uint32_t(r[1]), 0, &box);
    ResolveCensusCopy(uint64_t(TightPitch(g->snapshot_formats[snapshot], uint32_t(r[2] - r[0]))) *
                      BlockRows(g->snapshot_formats[snapshot], uint32_t(r[3] - r[1])));
    copied = true;
  }
  {
    BarrierBatch bb;
    bb.Layout(dst, RenderTextureLayout::SHADER_READ);
    bb.Layout(src, RenderTextureLayout::SHADER_READ);
  }
  if (copied) LogD3D12SnapshotTextureCopy();
  return true;
}

// Card S3-33h: copies a snapshot (w x h from its origin) into mip `level` of a texture of the same format.
bool CopySnapshotToTextureLevel(uint32_t texture, uint32_t snapshot, uint32_t level, uint32_t w, uint32_t h,
                                std::string* error) {
  if (AsyncCall())
    return QueueBoolCall("snapshot level", BatchHashWords("snapshot_level", {texture, snapshot, level, w, h}),
                         [=](std::string* e) { return CopySnapshotToTextureLevel(texture, snapshot, level, w, h, e); });
  ProfScope prof_scope(kProfCreateTexture);
  if (!EnsureLogicalSnap(snapshot, error)) return false;
  if (texture >= g->textures.size() || !g->textures[texture] || !g->textures[texture]->Image() ||
      snapshot >= g->snapshots.size() || !g->snapshots[snapshot] || !w || !h) {
    *error = "snapshot level: bad texture or snapshot";
    return false;
  }
  RenderTexture* dst = g->textures[texture]->Image();
  RenderTexture* src = g->snapshots[snapshot].get();
  const RenderTextureDesc& dst_desc = D3D12TextureDescOf(dst);
  if (dst_desc.format != g->snapshot_formats[snapshot] || level >= dst_desc.mipLevels) {
    *error = "snapshot level: format or level mismatch";
    return false;
  }
  if (g->state_elide) EndDrawPass();
  {
    BarrierBatch bb;
    bb.Layout(src, RenderTextureLayout::COPY_SOURCE);
    bb.Layout(dst, RenderTextureLayout::COPY_DEST);
  }
  RenderBox box(0, 0, int32_t(w), int32_t(h));
  g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(dst, level), RenderTextureCopyLocation::Subresource(src),
                             0, 0, 0, &box);
  ResolveCensusCopy(uint64_t(TightPitch(g->snapshot_formats[snapshot], w)) * BlockRows(g->snapshot_formats[snapshot], h));
  {
    BarrierBatch bb;
    bb.Layout(dst, RenderTextureLayout::SHADER_READ);
    bb.Layout(src, RenderTextureLayout::SHADER_READ);
  }
  LogD3D12SnapshotTextureCopy();
  return true;
}

// Card S3-69 (5c): as CopySnapshotToTextureLevel, into array layer / cube face `layer`.
bool CopySnapshotToTextureLayer(uint32_t texture, uint32_t snapshot, uint32_t level, uint32_t layer, uint32_t w,
                                uint32_t h, std::string* error) {
  if (AsyncCall())
    return QueueBoolCall("snapshot layer", BatchHashWords("snapshot_layer", {texture, snapshot, level, layer, w, h}),
                         [=](std::string* e) { return CopySnapshotToTextureLayer(texture, snapshot, level, layer, w, h, e); });
  ProfScope prof_scope(kProfCreateTexture);
  if (!EnsureLogicalSnap(snapshot, error)) return false;
  if (texture >= g->textures.size() || !g->textures[texture] || !g->textures[texture]->Image() ||
      snapshot >= g->snapshots.size() || !g->snapshots[snapshot] || !w || !h) {
    *error = "snapshot layer: bad texture or snapshot";
    return false;
  }
  RenderTexture* dst = g->textures[texture]->Image();
  RenderTexture* src = g->snapshots[snapshot].get();
  const RenderTextureDesc& dst_desc = D3D12TextureDescOf(dst);
  if (dst_desc.format != g->snapshot_formats[snapshot] || level >= dst_desc.mipLevels ||
      layer >= dst_desc.arraySize) {
    *error = "snapshot layer: format, level or layer mismatch";
    return false;
  }
  if (g->state_elide) EndDrawPass();
  {
    BarrierBatch bb;
    bb.Layout(src, RenderTextureLayout::COPY_SOURCE);
    bb.Layout(dst, RenderTextureLayout::COPY_DEST);
  }
  RenderBox box(0, 0, int32_t(w), int32_t(h));
  g->list->copyTextureRegion(RenderTextureCopyLocation::Subresource(dst, level, layer),
                             RenderTextureCopyLocation::Subresource(src), 0, 0, 0, &box);
  ResolveCensusCopy(uint64_t(TightPitch(g->snapshot_formats[snapshot], w)) * BlockRows(g->snapshot_formats[snapshot], h));
  {
    BarrierBatch bb;
    bb.Layout(dst, RenderTextureLayout::SHADER_READ);
    bb.Layout(src, RenderTextureLayout::SHADER_READ);
  }
  LogD3D12SnapshotTextureCopy();
  return true;
}

static void ReleaseTextureNow(uint32_t handle) {
  ProfScope prof_scope(kProfReleaseTexture);
  if (handle == 0 || handle >= g->textures.size() || !g->textures[handle]) return;
  // Pending commands may still sample it: the texture and its table index live until the next wait.
  g->pending_free_indices.push_back(g->textures[handle]->table_index);
  g->retired_textures.push_back(std::move(g->textures[handle]));
}
// With the record thread the release is queued behind the draws that may still sample the texture; the retire lists are
// only touched by the thread that runs the queue (all other public calls wait for it first).
void ReleaseTexture(uint32_t handle) {
  if (g_batch_n && g_rq) {
    if (t_on_record_thread) ReleaseTextureNow(handle);
    else EnqueueJob([handle] { ReleaseTextureNow(handle); }, BatchHashWords("release_texture", {handle}), "release_texture");
    return;
  }
  if (AsyncCall()) {
    EnqueueJob([handle] { ReleaseTextureNow(handle); }, BatchHashWords("release_texture", {handle}), "release_texture");
    return;
  }
  if (g_rq) {
    g_pending_ops.push_back((1u << 30) | handle);
    return;
  }
  ReleaseTextureNow(handle);
}

// Card U2c: instanced crowd group handed to the draw body (null = ordinary single draw).
struct CrowdInst {
  uint32_t n = 1;
  const uint8_t* vs_blocks = nullptr;
  RenderShader* vs = nullptr;
  bool verify_ring = false;
  uint32_t ring_blocks_checked = 0;
};
static bool ExecuteDrawBody(const Draw& d, std::string* error, CrowdInst* inst);
static bool ExecuteDrawNow(const Draw& d, std::string* error);
static bool ExecuteDrawBody(const Draw& d, std::string* error, CrowdInst* inst) {
  ProfScope prof_scope(kProfExecuteDraw);
  const int rec_reuse = g->rec_reuse.load(std::memory_order_relaxed);
  const uint32_t vb_direct_mode = d.vb_direct_mode;
  // Apply D3D VBC stores before any early return (the worker counts them as completed).
  if (d.cache_flush) {
    for (auto& kv : g->vb_cache) Retire(std::move(kv.second.buf));
    g->vb_cache.clear();
  }
  auto cache_store = [&](uint32_t id, const uint8_t* bytes, size_t n, size_t full) {
    CountBuffer();
    auto buffer = g->device->createBuffer(
        RenderBufferDesc::VertexBuffer(full, RenderHeapType::UPLOAD, RenderBufferFlag::INDEX));
    void* dst = buffer ? buffer->map() : nullptr;
    if (!dst) {  // k14 G37: drop the entry so the draw that needs it fails "vertex cache entry missing"
      auto it = g->vb_cache.find(id);
      if (it != g->vb_cache.end()) {
        Retire(std::move(it->second.buf));
        g->vb_cache.erase(it);
      }
      return;
    }
    std::memcpy(dst, bytes, n);
    buffer->unmap();
    auto& slot = g->vb_cache[id];
    Retire(std::move(slot.buf));
    slot.buf = std::move(buffer);
    slot.size = full;
    if (g_prof) g_stats.upload_bytes += n;
  };
  for (const VertexBuffer& vb : d.Buffers()) {
    if (vb.cache_id && vb.cache_upload) cache_store(vb.cache_id, vb.data.data(), vb.data.size(), vb.cache_size);
  }
  if (d.index_cache_id && d.index_cache_upload) {
    cache_store(d.index_cache_id, d.Indices().data(), d.Indices().size(), d.index_cache_bytes);
  }
  if ((d.color != kNone && !LiveSurface(d.color)) || (d.depth != kNone && !LiveSurface(d.depth)) ||
      (d.color == kNone && d.depth == kNone)) {
    *error = "bad surfaces";
    return false;
  }
  SurfaceObj* color = d.color != kNone ? &g->surfaces[d.color] : nullptr;
  SurfaceObj* depth = d.depth != kNone ? &g->surfaces[d.depth] : nullptr;
  if (g_ddiag && d.depth != kNone) {
    DdSurf& ds = g_dd_s[d.depth];
    ds.draw = ++g_dd_seq;
    ++ds.draws_since_clear;
    ++ds.draws;
  }
  if (g->mem_owners_on) {
    if (d.color != kNone) TouchSurface(d.color);
    if (d.depth != kNone) TouchSurface(d.depth);
    g->mem_ring_last_use_present = g->mem_present;
  }
  const uint32_t samples = color ? color->samples : depth->samples;
  if (color && depth && color->samples != depth->samples) {
    *error = "colour / depth sample count mismatch";
    return false;
  }
  RenderShader* vs = inst ? inst->vs : Shader(d.vs_key, d.vs_spirv);
  RenderShader* ps = Shader(d.ps_key, d.ps_spirv);
  if (!vs) {
    *error = "no vertex shader";
    return false;
  }

  // Constants + descriptor indices (exec_consts).
  uint64_t push[3];
  SubTimer t_consts(kProfExecConsts);
  uint8_t shared[1024];
  std::memcpy(shared, d.shared, sizeof(shared));
  for (const TextureBinding& b : d.Textures()) {
    if (b.slot >= 32 || b.texture >= g->textures.size() || !g->textures[b.texture]) {
      *error = "bad texture binding";
      return false;
    }
    if (g->mem_owners_on) TouchTexture(g->textures[b.texture].get());
    const uint32_t index = g->textures[b.texture]->table_index;
    const uint32_t sampler = SamplerIndex(b.sampler);
    std::memcpy(shared + (b.dimension * 128 + b.slot * 4), &index, 4);
    std::memcpy(shared + (3 * 128 + b.slot * 4), &sampler, 4);
  }
  if (inst) {
    // One ring slot per instance; the derived VS offsets the vertex-constant base by i*kConstBytes.
    bool wrapped;
    const uint32_t first_slot = ConstSlot(&wrapped);
    for (uint32_t i = 1; i < inst->n; ++i) ConstSlot(&wrapped);
    const uint64_t slot_offset = uint64_t(first_slot) * kConstBytes + kGuard;
    uint8_t* c = g->constants_mapped + slot_offset;
    crowd::PackVsBlocks(c, kConstBytes, inst->vs_blocks, inst->n);
    const uint64_t base = g->constants_address + slot_offset;
    std::memcpy(c + 4096, d.ps_constants, 4096);
    std::memcpy(c + 8192, shared, 1024);
    push[0] = base;
    push[1] = base + 4096;
    push[2] = base + 8192;
    inst->ring_blocks_checked = 0;
    if (inst->verify_ring)
      inst->ring_blocks_checked = crowd::VerifyVsBlocks(g->constants_mapped + (push[0] - g->constants_address),
                                                        kConstBytes, inst->vs_blocks, inst->n);
  } else if (!g->const_dedupe) {
    // Today's behaviour exactly: one ring slot per draw, all three blocks written unconditionally.
    bool wrapped;
    const uint64_t slot_offset = uint64_t(ConstSlot(&wrapped)) * kConstBytes + kGuard;
    uint8_t* c = g->constants_mapped + slot_offset;
    std::memcpy(c, d.vs_constants, 4096);
    const uint64_t base = g->constants_address + slot_offset;
    std::memcpy(c + 4096, d.ps_constants, 4096);
    push[1] = base + 4096;
    std::memcpy(c + 8192, shared, 1024);
    push[0] = base;
    push[2] = base + 8192;
  } else {
    // Constants dedupe (WWE2K14_NR_CONST_DEDUPE=1): each of the three blocks is compared against the
    // last written copy (memcmp) and, if unchanged AND still valid, its prior address is re-pushed
    // without a write. When at least one block changed, a slot is allocated exactly as today and only
    // the changed blocks are written into it; when none changed, no slot is allocated at all.
    const uint8_t* src[3] = {d.vs_constants, d.ps_constants, shared};
    const uint32_t sizes[3] = {4096, 4096, 1024};
    const uint64_t off[3] = {0, 4096, 8192};
    bool changed[3];
    for (int i = 0; i < 3; ++i)
      changed[i] = !g->dedupe[i].valid || std::memcmp(g->dedupe[i].bytes, src[i], sizes[i]) != 0;
    if (changed[0] || changed[1] || changed[2]) {
      bool wrapped;
      const uint64_t slot_offset = uint64_t(ConstSlot(&wrapped)) * kConstBytes + kGuard;
      // A wrap invalidated every cached address (their slots will be rewritten): write all three.
      if (wrapped) changed[0] = changed[1] = changed[2] = true;
      uint8_t* c = g->constants_mapped + slot_offset;
      const uint64_t base = g->constants_address + slot_offset;
      for (int i = 0; i < 3; ++i) {
        if (changed[i]) {
          std::memcpy(c + off[i], src[i], sizes[i]);
          std::memcpy(g->dedupe[i].bytes, src[i], sizes[i]);
          g->dedupe[i].address = base + off[i];
          g->dedupe[i].valid = true;
          if (g_prof) ++g_stats.const_blocks_written;
        } else if (g_prof) {
          ++g_stats.const_blocks_reused;
        }
      }
    } else if (g_prof) {
      g_stats.const_blocks_reused += 3;
    }
    push[0] = g->dedupe[0].address;
    push[1] = g->dedupe[1].address;
    push[2] = g->dedupe[2].address;
  }
  if (g->const_fence) {
    ConstRef(push[0]);
    ConstRef(push[1]);
    ConstRef(push[2]);
  }
  t_consts.stop();

  // Pipeline (exec_pipeline).
  SubTimer t_pipeline(kProfExecPipeline);
  RenderGraphicsPipelineDesc p;
  p.pipelineLayout = g->layout.get();
  p.vertexShader = vs;
  p.pixelShader = ps;
  RenderSpecConstant spec(0, d.spec_constants);
  p.specConstants = &spec;
  p.specConstantsCount = 1;
  p.primitiveTopology = RenderPrimitiveTopology(d.topology);
  p.cullMode = RenderCullMode(d.cull);
  p.frontFace = d.front_ccw ? RenderFrontFace::COUNTER_CLOCKWISE : RenderFrontFace::CLOCKWISE;
  p.depthClipEnabled = !d.depth_clamp;
  p.depthEnabled = depth && (d.depth_test || d.depth_write);
  p.depthWriteEnabled = d.depth_write;
  p.depthFunction = d.depth_test ? RenderComparisonFunction(d.depth_compare) : RenderComparisonFunction::ALWAYS;
  p.stencilEnabled = depth && d.stencil;
  p.stencilReadMask = d.stencil_read;
  p.stencilWriteMask = d.stencil_write;
  // D3D12's generic stencil-reference path follows the reference assignment and Plume's OMSetStencilRef:
  // <wwe13-recomp-nr>/src/nr/nr_backend.cpp:1789 and
  // <this repo>/dependencies/nr-plume/plume/plume_d3d12.cpp:2520-2527.
  p.stencilReference = d.stencil_ref;
  auto face = [](const StencilFace& f) {
    RenderStencilFaceDesc r;
    r.failOp = RenderStencilOp(f.fail);
    r.depthFailOp = RenderStencilOp(f.depth_fail);
    r.passOp = RenderStencilOp(f.pass);
    r.compareFunction = RenderComparisonFunction(f.compare);
    return r;
  };
  p.stencilFrontFace = face(d.stencil_front);
  p.stencilBackFace = face(d.stencil_back);
  p.renderTargetCount = color ? 1 : 0;
  p.renderTargetFormat[0] = color ? color->format : RenderFormat::UNKNOWN;
  p.multisampling = Multisampling(samples);
  p.sampleShadingEnabled = d.sample_shading && samples > 1;
  RenderBlendDesc blend = RenderBlendDesc::Copy();
  blend.blendEnabled = d.blend.enabled;
  blend.srcBlend = RenderBlend(d.blend.src);
  blend.dstBlend = RenderBlend(d.blend.dst);
  blend.blendOp = RenderBlendOperation(d.blend.op);
  blend.srcBlendAlpha = RenderBlend(d.blend.src_a);
  blend.dstBlendAlpha = RenderBlend(d.blend.dst_a);
  blend.blendOpAlpha = RenderBlendOperation(d.blend.op_a);
  blend.renderTargetWriteMask = d.blend.write_mask;
  p.renderTargetBlend[0] = blend;
  p.depthTargetFormat = depth ? depth->format : RenderFormat::UNKNOWN;
  std::vector<RenderInputSlot> slots;
  for (uint32_t i = 0; i < d.Buffers().size(); ++i) slots.emplace_back(i, d.Buffers()[i].stride_bytes);
  std::vector<RenderInputElement> elements;
  for (const VertexAttribute& a : d.Attributes()) {
    elements.emplace_back("TEXCOORD", a.location, a.location, ToFormat(a.format), a.binding, a.offset_bytes);
    if (elements.back().format == RenderFormat::UNKNOWN) {
      *error = "unsupported vertex format";
      return false;
    }
  }
  p.inputSlots = slots.data();
  p.inputSlotsCount = uint32_t(slots.size());
  p.inputElements = elements.data();
  p.inputElementsCount = uint32_t(elements.size());
  // Cache key from the fields set above only (M4): the raw bytes of a RenderGraphicsPipelineDesc copy
  // included uninitialised padding (stack garbage), so equal states could miss and build duplicates.
  PipelineKey pk;
  std::memset(&pk, 0, sizeof(pk));
  pk.vs = uint64_t(reinterpret_cast<uintptr_t>(vs));
  pk.ps = uint64_t(reinterpret_cast<uintptr_t>(ps));
  pk.vs_key = d.vs_key;
  pk.ps_key = d.ps_key;
  pk.spec = d.spec_constants;
  pk.topology = uint32_t(p.primitiveTopology);
  pk.cull = uint32_t(p.cullMode);
  pk.front_ccw = p.frontFace == RenderFrontFace::COUNTER_CLOCKWISE;
  pk.depth_clip = p.depthClipEnabled;
  pk.depth_enabled = p.depthEnabled;
  pk.depth_write = p.depthWriteEnabled;
  pk.depth_func = uint32_t(p.depthFunction);
  pk.stencil_enabled = p.stencilEnabled;
  pk.stencil_read = p.stencilReadMask;
  pk.stencil_write = p.stencilWriteMask;
  pk.stencil_ref = p.stencilReference;
  for (int i = 0; i < 2; ++i) {
    const RenderStencilFaceDesc& f = i ? p.stencilBackFace : p.stencilFrontFace;
    uint32_t* o = i ? pk.stencil_back : pk.stencil_front;
    o[0] = uint32_t(f.passOp);
    o[1] = uint32_t(f.failOp);
    o[2] = uint32_t(f.depthFailOp);
    o[3] = uint32_t(f.compareFunction);
  }
  pk.rt_count = p.renderTargetCount;
  pk.rt_format = uint32_t(p.renderTargetFormat[0]);
  pk.depth_format = uint32_t(p.depthTargetFormat);
  pk.samples = samples;
  pk.sample_shading = p.sampleShadingEnabled;
  pk.blend_enabled = blend.blendEnabled;
  pk.blend[0] = uint32_t(blend.srcBlend);
  pk.blend[1] = uint32_t(blend.dstBlend);
  pk.blend[2] = uint32_t(blend.blendOp);
  pk.blend[3] = uint32_t(blend.srcBlendAlpha);
  pk.blend[4] = uint32_t(blend.dstBlendAlpha);
  pk.blend[5] = uint32_t(blend.blendOpAlpha);
  pk.write_mask = blend.renderTargetWriteMask;
  RenderPipeline* pipeline = nullptr;
  if (g->draw_fast) {
    // Fast path (WWE2K14_NR_DRAW_FAST=1): build the key bytes into a reusable member buffer (no heap on
    // the hit path), hash with XXH3-64 and look up the unordered_map; a hash hit is confirmed against
    // the stored full key bytes, so a collision can never return the wrong pipeline. A miss or collision
    // falls through to today's std::map create path below, on the same key bytes.
    std::string& key = g->pipeline_key_buf;
    BuildPipelineKey(key, pk, slots, elements);
    const uint64_t h = XXH3_64bits(key.data(), key.size());
    auto hit = g->pipeline_hash.find(h);
    if (hit != g->pipeline_hash.end()) {
      if (hit->second.key.size() == key.size() &&
          std::memcmp(hit->second.key.data(), key.data(), key.size()) == 0) {
        pipeline = hit->second.pipeline;
      } else if (g_prof) {
        ++g_stats.pipeline_collisions;  // hash hit, bytes differ (true collision; expect 0)
      }
    }
    if (!pipeline) {
      // Miss or collision: today's create path (std::map lookup by the same key bytes), then record the
      // hash for the next draw. A genuine miss creates and caches; a collision leaves the existing hash
      // entry untouched (that key always takes the std::map path).
      auto pit = g->pipelines.find(key);
      if (pit == g->pipelines.end()) {
        const uint64_t pt0 = g_prof ? NowNs() : 0;
        std::unique_ptr<RenderPipeline> created;
        bool adopted = false;
        CreateOrAdoptPipeline(key, p, &created, &adopted);
        if (g_prof) {
          if (adopted) ++g_stats.pipelines_adopted;
          else {
            ++g_stats.pipelines;
            g_stats.pipeline_ns += NowNs() - pt0;
          }
        }
        if (created && g_prewarm && !adopted) RecordMainPipeline(pk, d, slots, elements);
        // A failed pipeline is cached as null (no re-create every draw); the check below fails the draw.
        pit = g->pipelines.emplace(key, std::move(created)).first;
      }
      pipeline = pit->second.get();
      if (!pipeline) {
        *error = "createGraphicsPipeline failed";
        return false;
      }
      g->pipeline_hash.emplace(h, PipelineEntry{key, pipeline});
    }
  } else {
    std::string key;
    BuildPipelineKey(key, pk, slots, elements);
    auto pit = g->pipelines.find(key);
    if (pit == g->pipelines.end()) {
      const uint64_t pt0 = g_prof ? NowNs() : 0;
      std::unique_ptr<RenderPipeline> created;
      bool adopted = false;
      CreateOrAdoptPipeline(key, p, &created, &adopted);
      if (g_prof) {
        if (adopted) ++g_stats.pipelines_adopted;
        else {
          ++g_stats.pipelines;
          g_stats.pipeline_ns += NowNs() - pt0;
        }
      }
      if (created && g_prewarm && !adopted) RecordMainPipeline(pk, d, slots, elements);
      // A failed pipeline is cached as null (no re-create every draw); the check below fails the draw.
      pit = g->pipelines.emplace(key, std::move(created)).first;
    }
    pipeline = pit->second.get();
    if (!pipeline) {
      *error = "createGraphicsPipeline failed";
      return false;
    }
  }
  t_pipeline.stop();

  // Geometry (upload buffers per draw, retired until the next GPU wait). With
  // WWE2K14_NR_UPLOAD_RING=1, vertex and index uploads sub-allocate from one persistent,
  // persistently-mapped upload ring, so setVertexBuffers/setIndexBuffer below just carry a
  // (ringBuffer, offset) reference; the per-draw createBuffer path is byte-identical and remains
  // the fallback (ring disabled, or a ring allocation/growth failed).
  SubTimer t_upload(kProfExecUpload);
  std::vector<std::unique_ptr<RenderBuffer>> vbufs;
  std::vector<RenderVertexBufferView> views;
  const bool ring_ok = g->upload_ring && g->upload_ring_mapped;
  bool vb_direct_draw = false, vb_direct_mismatch = false;
  uint64_t vb_direct_bytes = 0, vb_direct_mismatch_bytes = 0;
  for (const VertexBuffer& vb : d.Buffers()) {
    if (vb.cache_id) {
      auto it = g->vb_cache.find(vb.cache_id);
      if (it == g->vb_cache.end() || it->second.size != vb.cache_size) {
        *error = "vertex cache entry missing";
        for (auto& b : vbufs) Retire(std::move(b));
        return false;
      }
      views.emplace_back(RenderBufferReference(it->second.buf.get()), uint32_t(vb.cache_size));
      continue;
    }
    const bool direct_context = vb.direct_src && vb_direct_mode && !g_rq && !g_batch_n && !t_on_record_thread && !inst;
    const size_t direct_size = vb.direct_src ? vb.direct_bytes : vb.data.size();
    const size_t direct_vb_size = size_t(vb.prefix_bytes) + direct_size;
    const bool direct_ring_room = direct_context && ring_ok && RingHasRoom(direct_vb_size, kRingVertexAlign);
    const bool direct = direct_ring_room;
    if (direct_context && !direct_ring_room) ++g_vb_direct_stats.not_ring_room;
    ByteBuffer fallback_data;
    const uint8_t* upload_data = vb.data.data();
    size_t upload_bytes = vb.data.size();
    if (direct) {
      upload_bytes = vb.direct_bytes;
      vb_direct_draw = true;
      vb_direct_bytes += vb.direct_bytes;
    } else if (vb.direct_src && vb.data.empty() && vb.direct_bytes) {
      // Mode 1 skipped the vector copy while the ring had room; restore legacy bytes if the ring is no longer usable.
      fallback_data.resize(vb.direct_bytes);
      nr::vbswap::CopySwapDwords(fallback_data.data(), vb.direct_src, vb.direct_bytes, vb.direct_endian);
      upload_data = fallback_data.data();
      upload_bytes = fallback_data.size();
    }
    const size_t vb_size = size_t(vb.prefix_bytes) + upload_bytes;  // bytes [0, prefix) are never read
    if (g_prof) g_stats.upload_bytes += upload_bytes;
    const uint64_t off = ring_ok ? RingAlloc(vb_size, kRingVertexAlign) : UINT64_MAX;
    if (off != UINT64_MAX) {
      if (direct) {
        uint8_t* dst = g->upload_ring_mapped + off + vb.prefix_bytes;
        nr::vbswap::CopySwapDwordsToSpan(dst, vb.direct_src, vb.direct_bytes, vb.direct_endian);
        if (vb_direct_mode == 2 &&
            (vb.data.size() != vb.direct_bytes || std::memcmp(dst, vb.data.data(), vb.direct_bytes) != 0)) {
          vb_direct_mismatch = true;
          vb_direct_mismatch_bytes += vb.direct_bytes;
        }
      } else {
        std::memcpy(g->upload_ring_mapped + off + vb.prefix_bytes, upload_data, upload_bytes);
      }
      views.emplace_back(RenderBufferReference(g->upload_ring.get(), off), uint32_t(vb_size));
    } else {
      CountBuffer();
      auto buffer = g->device->createBuffer(RenderBufferDesc::VertexBuffer(vb_size, RenderHeapType::UPLOAD));
      uint8_t* dst = buffer ? static_cast<uint8_t*>(buffer->map()) : nullptr;
      if (!dst) {  // k14 G37
        *error = "vertex buffer map failed (out of memory?)";
        for (auto& b : vbufs) Retire(std::move(b));
        return false;
      }
      std::memcpy(dst + vb.prefix_bytes, upload_data, upload_bytes);
      buffer->unmap();
      views.emplace_back(RenderBufferReference(buffer.get()), uint32_t(vb_size));
      vbufs.push_back(std::move(buffer));
      if (g_prof && ring_ok) ++g_stats.ring_fallbacks;  // only a real ring failure, not ring off
    }
  }
  if (vb_direct_draw) {
    ++g_vb_direct_stats.direct_draws;
    g_vb_direct_stats.direct_bytes += vb_direct_bytes;
    if (vb_direct_mode == 2) {
      ++g_vb_direct_stats.check_draws;
      g_vb_direct_stats.check_bytes += vb_direct_bytes;
      if (vb_direct_mismatch) {
        ++g_vb_direct_stats.mismatch_draws;
        g_vb_direct_stats.mismatch_bytes += vb_direct_mismatch_bytes;
      }
    }
  }
  const bool indexed = !d.Indices().empty() || d.index_cache_id != 0;
  std::unique_ptr<RenderBuffer> ibuf;
  RenderBuffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  uint32_t index_bytes = uint32_t(d.Indices().size());
  if (d.index_cache_id) {
    auto it = g->vb_cache.find(d.index_cache_id);
    if (it == g->vb_cache.end() || it->second.size != d.index_cache_bytes) {
      *error = "index cache entry missing";
      for (auto& b : vbufs) Retire(std::move(b));
      return false;
    }
    index_buffer = it->second.buf.get();
    index_offset = 0;
    index_bytes = d.index_cache_bytes;
  } else if (indexed) {
    if (g_prof) g_stats.upload_bytes += d.Indices().size();
    const uint64_t off = ring_ok ? RingAlloc(d.Indices().size(), d.index32 ? kRingIndexAlign32 : kRingIndexAlign16)
                                 : UINT64_MAX;
    if (off != UINT64_MAX) {
      std::memcpy(g->upload_ring_mapped + off, d.Indices().data(), d.Indices().size());
      index_buffer = g->upload_ring.get();
      index_offset = off;
    } else {
      CountBuffer();
      ibuf = g->device->createBuffer(RenderBufferDesc::IndexBuffer(d.Indices().size(), RenderHeapType::UPLOAD));
      void* dst = ibuf ? ibuf->map() : nullptr;
      if (!dst) {  // k14 G37
        *error = "index buffer map failed (out of memory?)";
        for (auto& b : vbufs) Retire(std::move(b));
        return false;
      }
      std::memcpy(dst, d.Indices().data(), d.Indices().size());
      ibuf->unmap();
      index_buffer = ibuf.get();
      index_offset = 0;
      if (g_prof && ring_ok) ++g_stats.ring_fallbacks;  // only a real ring failure, not ring off
    }
  }
  t_upload.stop();

  RenderCommandList* l = g->list;
  SubTimer t_record(kProfExecRecord);
  RenderFramebuffer* fb = Framebuffer(d.color, d.depth);
  // State elision (WWE2K14_NR_STATE_ELIDE=1): `continued` is true only when the previous draw left this
  // exact framebuffer's render pass open. EndDrawPass() (at every non-draw recording entry point) and the
  // per-list reset in Submit()/Kick() clear Elide().fb, so any other recording in between makes it false.
  const bool continued = g->state_elide && (Elide().fb == fb);
  const bool dp_on = DpOn();  // card D1 (WWE2K14_NR_GPU_DRAWS=1)
  if (g->state_elide) {
    const bool color_ok = !color || CurrentLayout(color->image.get()) == RenderTextureLayout::COLOR_WRITE;
    const bool depth_ok = !depth || CurrentLayout(depth->image.get()) == RenderTextureLayout::DEPTH_WRITE;
    if (continued && color_ok && depth_ok) {
      // Share the open pass: in-subpass ordering replaces the per-pass same-layout barriers.
    } else {
      if (dp_on) DpOpen(DpOpLabel("pass-switch"));  // card D1: barriers + framebuffer change (+ the previous pass's end)
      BarrierBatch bb;  // k14 P22: batched (precise barriers only)
      if (color) bb.Write(color->image.get(), RenderTextureLayout::COLOR_WRITE);
      if (depth) bb.Write(depth->image.get(), RenderTextureLayout::DEPTH_WRITE);
    }
  } else {
    if (dp_on) DpOpen(DpOpLabel("pass-switch"));  // card D1
    BarrierBatch bb;  // k14 P22: batched (precise barriers only)
    if (color) bb.Write(color->image.get(), RenderTextureLayout::COLOR_WRITE);
    if (depth) bb.Write(depth->image.get(), RenderTextureLayout::DEPTH_WRITE);
  }
  if (g->state_elide) {
    if (!continued) {
      GpuPassBegin(color, depth);
      l->setFramebuffer(fb);
      Elide().fb = fb;
      if (g_prof || g_phys_snap_on) ++g_stats.elide_passes;
    }
  } else {
    l->setFramebuffer(fb);
  }
  // Redundant-bind elision (pipeline, layout, 4 sets, viewport, scissor); push constants and vertex/index
  // buffers change per draw and are always re-set.
  if (g->state_elide) {
    if (Elide().pipeline != pipeline) {
      l->setPipeline(pipeline);
      Elide().pipeline = pipeline;
    } else if (g_prof) ++g_stats.elide_binds_skipped;
  } else {
    l->setPipeline(pipeline);
  }
  if (g->state_elide) {
    if (Elide().graphics_layout != g->layout.get()) {
      l->setGraphicsPipelineLayout(g->layout.get());
      Elide().graphics_layout = g->layout.get();
    } else if (g_prof) ++g_stats.elide_binds_skipped;
  } else {
    l->setGraphicsPipelineLayout(g->layout.get());
  }
  for (uint32_t s = 0; s < 4; ++s) {
    if (g->state_elide) {
      if (Elide().sets[s] != g->tables[s].get()) {
        l->setGraphicsDescriptorSet(g->tables[s].get(), s);
        Elide().sets[s] = g->tables[s].get();
      } else if (g_prof) ++g_stats.elide_binds_skipped;
    } else {
      l->setGraphicsDescriptorSet(g->tables[s].get(), s);
    }
  }
  if (g->abi.bind_constants_set_every_draw) {
    // Set 4 binding 0 is one descriptor over the whole constants ring; it never changes but is bound
    // on every draw (the packet's rule, independent of state elision).
    RenderDescriptorSet* const_set = g->const32_set.get();
    if (rec_reuse) {
      ElideState& e = Elide();
      const auto probe = rec_reuse::Probe(e.sets[4] != nullptr, uintptr_t(e.sets[4]),
                                          rec_reuse == 2 && e.check_set4 != nullptr,
                                          uintptr_t(e.check_set4), uintptr_t(const_set));
      if (probe.hit) ++g_rec_reuse_stats.hits;
      if (rec_reuse == 2) {
        ++g_rec_reuse_stats.checks;
        if (probe.mismatch) {
          ++g_rec_reuse_stats.mismatches;
          if (g_rec_reuse_stats.logged++ < 20)
          std::fprintf(stderr,
                       "[nr-rec-reuse] CHECK mismatch: cached set-4 bind decision differs from always-bind reference\n");
        }
      }
      if (!probe.hit || rec_reuse == 2) l->setGraphicsDescriptorSet(const_set, 4);
      e.sets[4] = const_set;
      if (rec_reuse == 2) e.check_set4 = const_set;
    } else {
      l->setGraphicsDescriptorSet(const_set, 4);
    }
  }
  l->setGraphicsPushConstants(0, push, 0, sizeof(push));
  if (g->shader_format != RenderShaderFormat::SPIRV) {
    // D3D12: root SRV (t0, space4) = constants ring base; pushed values are byte offsets into it.
    // Re-bound every draw: SetGraphicsRootSignature (layout changes) clears root descriptors.
    assert(push[0] <= 0xFFFFFFFFull && push[1] <= 0xFFFFFFFFull && push[2] <= 0xFFFFFFFFull);
    l->setGraphicsRootDescriptor(RenderBufferReference(g->constants.get(), 0), 0);
  }
  if (!views.empty()) l->setVertexBuffers(0, views.data(), uint32_t(views.size()), slots.data());
  // Card S3-31: a scaled target maps the logical viewport / scissor (same edge rule as clears and copies).
  float vpf[6];
  int32_t scr[4];
  std::memcpy(vpf, d.viewport, sizeof(vpf));
  std::memcpy(scr, d.scissor, sizeof(scr));
  {
    const uint32_t ts = d.color != kNone ? d.color : d.depth;
    if (ts < g->surfaces.size() && g->surfaces[ts].scale != 1.0f) {
      const SurfaceObj& t = g->surfaces[ts];
      const float sx = float(t.width) / float(t.lw), sy = float(t.height) / float(t.lh);
      vpf[0] *= sx; vpf[2] *= sx; vpf[1] *= sy; vpf[3] *= sy;
      const int32_t x0 = PhysEdge(t, scr[0], false), y0 = PhysEdge(t, scr[1], true);
      const int32_t x1 = PhysEdge(t, scr[0] + scr[2], false), y1 = PhysEdge(t, scr[1] + scr[3], true);
      scr[0] = x0; scr[1] = y0; scr[2] = std::max(x1 - x0, 0); scr[3] = std::max(y1 - y0, 0);
    }
  }
  RenderViewport vp(vpf[0], vpf[1], vpf[2], vpf[3], vpf[4], vpf[5]);
  if (g->state_elide) {
    ElideState& e = Elide();
    if (!(e.has_viewport && std::memcmp(e.viewport, vpf, sizeof(e.viewport)) == 0)) {
      l->setViewports(&vp, 1);
      e.has_viewport = true;
      std::memcpy(e.viewport, vpf, sizeof(e.viewport));
    } else if (g_prof) ++g_stats.elide_binds_skipped;
  } else {
    l->setViewports(&vp, 1);
  }
  RenderRect sc(scr[0], scr[1], scr[0] + scr[2], scr[1] + scr[3]);
  if (g->state_elide) {
    ElideState& e = Elide();
    if (!(e.has_scissor && std::memcmp(e.scissor, scr, sizeof(e.scissor)) == 0)) {
      l->setScissors(&sc, 1);
      e.has_scissor = true;
      std::memcpy(e.scissor, scr, sizeof(e.scissor));
    } else if (g_prof) ++g_stats.elide_binds_skipped;
  } else {
    l->setScissors(&sc, 1);
  }
  // Card D1: the draw's own interval (the render pass begins lazily in plume's draw call: its load is in here).
  if (dp_on) DpOpen(DpDrawLabel(d.ps_key, d.vs_key, color, depth, d.count));
  if (indexed) {
    RenderIndexBufferView iv(RenderBufferReference(index_buffer, index_offset), index_bytes,
                             d.index32 ? RenderFormat::R32_UINT : RenderFormat::R16_UINT);
    l->setIndexBuffer(&iv);
    l->drawIndexedInstanced(d.count, inst ? inst->n : 1, 0, 0, 0);
  } else {
    l->drawInstanced(d.count, inst ? inst->n : 1, 0, 0);
  }
  if (!g->state_elide) l->setFramebuffer(nullptr);
  if (dp_on) DpClose();
  for (auto& b : vbufs) Retire(std::move(b));
  Retire(std::move(ibuf));
  // Early submit: hand the list to the GPU without waiting and start a fresh list. With state elision a
  // render pass may still be open here; Kick() -> SubmitActive() -> end() ends it (endActiveRenderPass).
  // FL-4 Design A (SetKickHold): while a late texture upload is held, the handover waits for its release boundary; the
  // draws keep counting, so the first draw after the hold clears kicks (one early submit deferred, none lost).
  static const FixAltKnob early_submit_knob = FixAltKnobFor("WWE2K14_NR_EARLY_SUBMIT");
  static const int64_t normal_kick_every = early_submit_knob.cached && early_submit_knob.cached->has_normal
                                               ? early_submit_knob.cached->normal
                                               : 0;
  const bool fix_alt_on = g->fix_alt_on;
  const int64_t kick_every = fix_alt_on ? FixAltInt(early_submit_knob, normal_kick_every) : normal_kick_every;
  if (fix_alt_on || kick_every > 0) {
    const uint32_t draws_since_kick = ++g->draws_since_kick;
    if (kick_every > 0 && uint64_t(draws_since_kick) >= uint64_t(kick_every) &&
        !g->kick_hold.load(std::memory_order_relaxed))
      Kick();
  }
  t_record.stop();
  return true;
}

// ---- Card U2c: crowd draw grouping (WWE2K14_D3D_CROWD_GROUP) ----
// Consecutive crowd PS draws that differ only in vertex constants become one ordered instanced draw. The derived VS
// variant offsets the vertex-constant base by SV_InstanceID * 17408; all other Draw state must compare equal.
void CrowdReportFailure(uint32_t ordinal, const std::string& error);
namespace {
constexpr uint64_t kCrowdPsKey = 0xF5C9EF1CFA228C6Eull;
constexpr uint32_t kCrowdMaxInstances = 64;
static_assert(kConstBytes == 17408, "the derived crowd VS variants use the 17408-byte constant-ring stride");
std::atomic<int> g_crowd_frame_mode{0};
int CrowdMode() { return g_crowd_frame_mode.load(std::memory_order_acquire); }
using nr::backend::crowd::CrowdWhy;
using namespace nr::backend::crowd;

struct CrowdCounters {
  std::atomic<uint64_t> in{0}, out{0}, groups{0}, grouped{0}, singles{0}, declined{0}, inelig{0};
  std::atomic<uint64_t> ring_checks{0}, ring_mismatch{0}, pic_checks{0}, pic_mismatch{0}, pic_skipped{0};
  std::atomic<uint64_t> why[kCwCount];
  std::atomic<uint64_t> max_n{0};
  std::atomic<uint64_t> inelig_why[4]{}, uncached_starts{0}, cached_starts{0};
  std::atomic<uint64_t> card_in{0}, card_out{0}, card_groups{0}, card_grouped{0}, card_why[kCwCount];
} g_cw;
// WWE2K14_D3D_CROWD_CARDS=1: the flat crowd cards join the grouping too. Default off; a FIX_ALT knob, latched per frame in
// CrowdFrameStart. The variant comes from the shader folder or, on the legacy constants ABI only, from
// WWE2K14_D3D_CROWD_CARDS_DIR.
// =2 adds the entrance crowd keys (not yet validated in a game run).
std::atomic<int> g_crowd_cards{0};
bool CrowdCardsOn() { return g_crowd_cards.load(std::memory_order_acquire) != 0; }
bool CrowdEntranceOn() { return g_crowd_cards.load(std::memory_order_acquire) == 2; }
CrowdGroup g_crowd;
std::mutex g_crowd_log_mu;
std::vector<std::string> g_crowd_log;

void CrowdLog(const std::string& line) {
  std::fprintf(stderr, "%s\n", line.c_str());
  std::lock_guard<std::mutex> lock(g_crowd_log_mu);
  if (g_crowd_log.size() < 64) g_crowd_log.push_back(line);
}

RenderShader* CrowdVariant(uint64_t vs_key) {
  auto it = g->crowd_vs.find(vs_key);
  if (it != g->crowd_vs.end()) return it->second.get();
  std::unique_ptr<RenderShader> shader;
  if (!g->shader_dir.empty() && g->shader_format == RenderShaderFormat::SPIRV) {
    char name[48];
    std::snprintf(name, sizeof(name), "/%016llX.vs.crowd.spv", (unsigned long long)vs_key);
    std::ifstream f(g->shader_dir + name, std::ios::binary);
    if (!f && (vs_key == kCardVsKey || vs_key == kEntranceCardVsKey || vs_key == kEntranceFigureVsKey) && !g->abi.constants32) {
      const char* dir = std::getenv("WWE2K14_D3D_CROWD_CARDS_DIR");
      if (dir && *dir) f = std::ifstream(std::string(dir) + name, std::ios::binary);
    }
    if (f) {
      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (!bytes.empty()) shader = g->device->createShader(bytes.data(), bytes.size(), "main", g->shader_format);
    }
    std::fprintf(stderr, "[nr-crowd] VS %016llX instanced variant %s\n", (unsigned long long)vs_key,
                 shader ? "loaded" : "NOT available (draws stay separate)");
  }
  RenderShader* raw = shader.get();
  g->crowd_vs[vs_key] = std::move(shader);
  return raw;
}

bool CrowdTargetFormat(const SurfaceObj& t, TargetFormat* out) {
  if (t.depth) {
    *out = t.format == RenderFormat::D24_UNORM_S8_UINT ? TargetFormat::kD24S8 : TargetFormat::kD24FS8;
    return t.format == RenderFormat::D24_UNORM_S8_UINT || t.format == RenderFormat::D32_FLOAT_S8_UINT;
  }
  for (TargetFormat f : {TargetFormat::kRGBA8, TargetFormat::kRGBA16F, TargetFormat::kR32F, TargetFormat::kRG16F,
                         TargetFormat::kRG32F, TargetFormat::kRG16S, TargetFormat::kRGBA16S}) {
    if (ToFormat(f) == t.format) {
      *out = f;
      return true;
    }
  }
  return false;
}

// CHECK mode: render the group both ways onto two scratch pairs seeded from the real colour/depth/stencil state. A pass is
// counted only if the draws painted pixels and both results match byte-for-byte; an unpainted result stays inconclusive.
void CrowdPictureCheck(Draw& first, const std::vector<uint8_t>& blocks, CrowdInst inst) {
  std::string err;
  auto skip = [&](const char* why) {
    g_cw.pic_skipped.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[nr-crowd-check] skipped: %s\n", why);
  };
  static bool scratch_capacity_reserved = false;
  if (!scratch_capacity_reserved) {
    g->surfaces.reserve(std::max<size_t>(1024, g->surfaces.size() + 4));
    scratch_capacity_reserved = true;
  }
  if (!LiveSurface(first.color)) return skip("no colour target");
  const SurfaceObj cs = [&] {
    SurfaceObj t;
    const SurfaceObj& src = g->surfaces[first.color];
    t.format = src.format; t.samples = src.samples; t.lw = src.lw; t.lh = src.lh; t.depth = false; t.scale = src.scale;
    return t;
  }();
  SurfaceObj ds;
  const bool has_depth = first.depth != kNone && LiveSurface(first.depth);
  if (has_depth) {
    const SurfaceObj& src = g->surfaces[first.depth];
    ds.format = src.format; ds.samples = src.samples; ds.lw = src.lw; ds.lh = src.lh; ds.depth = true;
  }
  if (g->surfaces[first.color].alt || (has_depth && g->surfaces[first.depth].alt)) return skip("dynamic-scale alt set");
  TargetFormat cf, df = TargetFormat::kD24S8;
  if (!CrowdTargetFormat(cs, &cf) || (has_depth && !CrowdTargetFormat(ds, &df))) return skip("target format");
  const uint64_t tag = (0xC2ull << 56) | (uint64_t(uint32_t(cf)) << 44) | (uint64_t(uint32_t(df)) << 40) |
                       (uint64_t(cs.samples & 0xFF) << 32) | (uint64_t(cs.lw & 0xFFF) << 16) | (cs.lh & 0xFFF);
  uint32_t color[2], depth[2] = {kNone, kNone};
  for (int i = 0; i < 2; ++i) {
    color[i] = Surface(tag | (uint64_t(1 + i) << 48), cs.lw, cs.lh, false, cf, cs.samples, &err);
    if (color[i] == kNone) return skip("scratch colour surface");
    if (has_depth) {
      depth[i] = Surface(tag | (uint64_t(3 + i) << 48), ds.lw, ds.lh, true, df, ds.samples, &err);
      if (depth[i] == kNone) return skip("scratch depth surface");
    }
  }
  const uint32_t pw = g->surfaces[color[0]].width, ph = g->surfaces[color[0]].height;
  std::vector<uint8_t> pre;
  if (g->surfaces[first.color].width != pw || g->surfaces[first.color].height != ph) return skip("scratch size differs from target");
  if (!ReadColor(first.color, 0, 0, pw, ph, pre, &err)) return skip("pre-state readback");
  EndDrawPass();
  {
    auto seed = [&](uint32_t dst_handle, uint32_t src_handle) {
      SurfaceObj& src = g->surfaces[src_handle];
      SurfaceObj& dst = g->surfaces[dst_handle];
      Barrier(src.image.get(), RenderTextureLayout::COPY_SOURCE);
      Barrier(dst.image.get(), RenderTextureLayout::COPY_DEST);
      g->list->copyTexture(dst.image.get(), src.image.get());
    };
    for (int i = 0; i < 2; ++i) {
      seed(color[i], first.color);
      if (has_depth) seed(depth[i], first.depth);
    }
  }
  Draw tmp = first;
  tmp.color = color[0];
  tmp.depth = depth[0];
  inst.verify_ring = false;
  if (!ExecuteDrawBody(tmp, &err, &inst)) return skip("instanced scratch draw failed");
  tmp.color = color[1];
  tmp.depth = depth[1];
  for (uint32_t i = 0; i < inst.n; ++i) {
    std::memcpy(tmp.vs_constants, blocks.data() + size_t(i) * 4096, 4096);
    if (!ExecuteDrawBody(tmp, &err, nullptr)) return skip("separate scratch draw failed");
  }
  std::vector<uint8_t> a, b;
  if (!ReadColor(color[0], 0, 0, pw, ph, a, &err) || !ReadColor(color[1], 0, 0, pw, ph, b, &err) || a.size() != b.size())
    return skip("readback");
  const PictureResult result = ComparePictures(pre, a, b, size_t(pw) * ph);
  if (result.verdict == PictureResult::kInconclusive) {
    g_cw.pic_skipped.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[nr-crowd-check] group n=%u: both renderings changed 0 pixels; inconclusive\n", inst.n);
    return;
  }
  g_cw.pic_checks.fetch_add(1, std::memory_order_relaxed);
  if (result.diff_bytes) g_cw.pic_mismatch.fetch_add(1, std::memory_order_relaxed);
  std::fprintf(stderr,
               "[nr-crowd-check] group n=%u vs=%016llX tris=%u scratch %ux%u (host %ux%u) x%u fmt=%u: diff_bytes=%llu max_diff=%u painted_px=%llu%s\n",
               inst.n, (unsigned long long)first.vs_key, first.count / 3, cs.lw, cs.lh, pw, ph, cs.samples, unsigned(cf),
               (unsigned long long)result.diff_bytes, result.max_diff, (unsigned long long)result.painted_px,
               result.diff_bytes ? " MISMATCH" : "");
  if (result.diff_bytes && result.first_diff != SIZE_MAX)
    std::fprintf(stderr, "[nr-crowd-check] first diff at byte %zu: %u vs %u\n", result.first_diff, a[result.first_diff], b[result.first_diff]);
}

bool CrowdRun(std::string* error) {
  CrowdGroup& group = g_crowd;
  const uint32_t n = group.n;
  group.n = 0;
  if (n == 0) return true;
  const bool is_card = IsCrowdCard(group.first.ps_key, group.first.vs_key) || IsEntranceCrowd(group.first.ps_key, group.first.vs_key);
  if (n == 1) {
    g_cw.singles.fetch_add(1, std::memory_order_relaxed);
    g_cw.out.fetch_add(1, std::memory_order_relaxed);
    if (is_card) g_cw.card_out.fetch_add(1, std::memory_order_relaxed);
    return ExecuteDrawBody(group.first, error, nullptr);
  }
  const int mode = CrowdMode();
  RenderShader* variant = mode == 3 ? nullptr : CrowdVariant(group.first.vs_key);
  CrowdInst inst;
  inst.n = n;
  inst.vs_blocks = group.vs.data();
  inst.vs = variant;
  inst.verify_ring = mode == 2;
  if (variant && mode == 2) {
    const uint64_t index = g_cw.groups.load(std::memory_order_relaxed);
    if (index < 12 || index % 31 == 0) CrowdPictureCheck(group.first, group.vs, inst);
  }
  const uint32_t available = g->const_next >= g->const_slots ? g->const_slots : g->const_slots - g->const_next;
  if (variant && !g->const_dedupe && n <= available) {
    if (!ExecuteDrawBody(group.first, error, &inst)) return false;
    if (inst.verify_ring) {
      g_cw.ring_checks.fetch_add(inst.n, std::memory_order_relaxed);
      g_cw.ring_mismatch.fetch_add(inst.n - inst.ring_blocks_checked, std::memory_order_relaxed);
    }
    g_cw.out.fetch_add(1, std::memory_order_relaxed);
    g_cw.groups.fetch_add(1, std::memory_order_relaxed);
    g_cw.grouped.fetch_add(n, std::memory_order_relaxed);
    if (is_card) {
      g_cw.card_out.fetch_add(1, std::memory_order_relaxed);
      g_cw.card_groups.fetch_add(1, std::memory_order_relaxed);
      g_cw.card_grouped.fetch_add(n, std::memory_order_relaxed);
    }
    uint64_t maximum = g_cw.max_n.load(std::memory_order_relaxed);
    while (n > maximum && !g_cw.max_n.compare_exchange_weak(maximum, n, std::memory_order_relaxed)) {}
    return true;
  }
  if (mode != 3) g_cw.declined.fetch_add(n, std::memory_order_relaxed);
  for (uint32_t i = 0; i < n; ++i) {
    std::memcpy(group.first.vs_constants, group.vs.data() + size_t(i) * 4096, 4096);
    g_cw.out.fetch_add(1, std::memory_order_relaxed);
    if (is_card) g_cw.card_out.fetch_add(1, std::memory_order_relaxed);
    if (!ExecuteDrawBody(group.first, error, nullptr)) return false;
    if (i == 0) {
      group.first.cache_flush = false;
      if (!group.first.shared_payload) {
        for (VertexBuffer& vb : group.first.buffers)
          if (vb.cache_upload) vb.cache_upload = false, vb.data.clear();
        if (group.first.index_cache_upload) group.first.index_cache_upload = false, group.first.indices.clear();
      }
    }
  }
  return true;
}

bool CrowdSubmit(const Draw& draw, std::string* error) {
  CrowdGroup& group = g_crowd;
  const bool card_draw = (CrowdCardsOn() && IsCrowdCard(draw.ps_key, draw.vs_key)) ||
                         (CrowdEntranceOn() && IsEntranceCrowd(draw.ps_key, draw.vs_key));
  const bool crowd_draw = draw.ps_key == kCrowdPsKey || card_draw;
  if (crowd_draw) g_cw.in.fetch_add(1, std::memory_order_relaxed);
  if (card_draw) g_cw.card_in.fetch_add(1, std::memory_order_relaxed);
  if (group.n) {
    if (draw.const_used_mode == 2 && group.first.full_float_constants && draw.full_float_constants &&
        crowd_draw && group.n < kCrowdMaxInstances) {
      const bool used_join = CrowdCompare(group.first, draw) == kCwJoined;
      const bool full_join = CrowdCompare(group.first, draw, false, true) == kCwJoined;
      if (used_join != full_join) ConstUsedRecordCrowdDifference();
    }
    const CrowdWhy why = group.TryJoin(draw, crowd_draw, kCrowdMaxInstances);
    g_cw.why[why].fetch_add(1, std::memory_order_relaxed);
    if (card_draw && draw.ps_key == group.first.ps_key) g_cw.card_why[why].fetch_add(1, std::memory_order_relaxed);
    if (why == kCwJoined) return true;
    const uint32_t ordinal = group.first.ordinal;
    std::string err;
    if (!CrowdRun(&err)) CrowdReportFailure(ordinal, err);
  }
  if (!crowd_draw) return ExecuteDrawBody(draw, error, nullptr);
  const int mode = CrowdMode();
  const int why_not = !g->state_elide ? 1 : (!draw.vs_spirv || draw.vs_spirv->empty()) ? 2 :
                      (mode != 3 && !CrowdVariant(draw.vs_key)) ? 3 : 0;
  if (why_not) {
    g_cw.inelig_why[why_not].fetch_add(1, std::memory_order_relaxed);
    g_cw.inelig.fetch_add(1, std::memory_order_relaxed);
    g_cw.out.fetch_add(1, std::memory_order_relaxed);
    if (card_draw) g_cw.card_out.fetch_add(1, std::memory_order_relaxed);
    return ExecuteDrawBody(draw, error, nullptr);
  }
  (CrowdGeometryCached(draw) ? g_cw.cached_starts : g_cw.uncached_starts).fetch_add(1, std::memory_order_relaxed);
  group.Start(draw);
  return true;
}
}  // namespace

static bool ExecuteDrawNow(const Draw& draw, std::string* error) {
  if (CrowdMode() == 0) return ExecuteDrawBody(draw, error, nullptr);
  return CrowdSubmit(draw, error);
}

void CrowdFlushBoundary() {
  if (!g_crowd.n) return;
  const uint32_t ordinal = g_crowd.first.ordinal;
  std::string err;
  if (!CrowdRun(&err)) CrowdReportFailure(ordinal, err);
}

int CrowdFrameStart() {
  // Called by OnBeforeSwap after the CP has passed the worker swap gate and under BackendGuard. Sample once per frame;
  // when it changes, first drain queued old-frame draws and flush their pending run while the old mode is still latched.
  static const FixAltKnob knob = FixAltKnobFor("WWE2K14_D3D_CROWD_GROUP");
  const int mode = int(FixAltInt(knob, 1));
  static const FixAltKnob cards_knob = FixAltKnobFor("WWE2K14_D3D_CROWD_CARDS");
  const int cards = int(FixAltInt(cards_knob, 0));
  if (cards != g_crowd_cards.load(std::memory_order_acquire)) {
    SyncRecordThread(kProfFnCount, true, kBatchDrainFrameTail);
    CrowdFlushBoundary();
    g_crowd_cards.store(cards, std::memory_order_release);
  }
  const int old = g_crowd_frame_mode.load(std::memory_order_acquire);
  if (old == mode) return -1;
  SyncRecordThread(kProfFnCount, true, kBatchDrainFrameTail);
  CrowdFlushBoundary();
  g_crowd_frame_mode.store(mode, std::memory_order_release);
  return mode;
}

std::vector<std::string> CrowdTakeLog() {
  std::lock_guard<std::mutex> lock(g_crowd_log_mu);
  std::vector<std::string> out;
  out.swap(g_crowd_log);
  return out;
}

std::vector<std::string> SurfaceTakeWarnings() {
  std::lock_guard<std::mutex> lock(g_surface_warning_mutex);
  std::vector<std::string> out;
  out.swap(g_surface_warnings);
  return out;
}

void CrowdFrame(uint32_t swap) {
  const int mode = CrowdMode();
  if (mode == 0) return;
  static uint32_t last_swap = 0;
  static uint64_t previous[12] = {};
  if (swap < last_swap + 120) return;
  const uint32_t frames = swap - last_swap;
  last_swap = swap;
  const uint64_t current[12] = {g_cw.in, g_cw.out, g_cw.groups, g_cw.grouped, g_cw.singles, g_cw.declined, g_cw.inelig,
                                g_cw.ring_checks, g_cw.ring_mismatch, g_cw.pic_checks, g_cw.pic_mismatch, g_cw.pic_skipped};
  auto delta = [&](int i) { return double(current[i] - previous[i]); };
  const double frame_div = frames ? frames : 1;
  const double group_count = delta(2);
  char line[1024];
  std::snprintf(line, sizeof(line),
                "[nr-crowd] swap=%u mode=%d crowd_draws_in/f=%.1f draws_out/f=%.1f groups/f=%.1f grouped_draws/f=%.1f avg_n=%.2f max_n=%llu "
                "singles/f=%.1f declined/f=%.1f ineligible/f=%.1f ring_checks=%llu ring_mismatch=%llu pic_checks=%llu pic_mismatch=%llu "
                "pic_skipped=%llu inelig_why{noelide=%llu nospirv=%llu novariant=%llu} starts/f{cached=%.1f uncached=%.1f} breaks/f{geom=%.1f tex=%.1f psc=%.1f shared=%.1f pass=%.1f state=%.1f other=%.1f full=%.1f}",
                swap, mode, delta(0) / frame_div, delta(1) / frame_div, group_count / frame_div, delta(3) / frame_div,
                group_count > 0 ? delta(3) / group_count : 0.0, (unsigned long long)g_cw.max_n.load(), delta(4) / frame_div,
                delta(5) / frame_div, delta(6) / frame_div, (unsigned long long)current[7], (unsigned long long)current[8],
                (unsigned long long)current[9], (unsigned long long)current[10], (unsigned long long)current[11],
                (unsigned long long)g_cw.inelig_why[1], (unsigned long long)g_cw.inelig_why[2],
                (unsigned long long)g_cw.inelig_why[3], double(g_cw.cached_starts) / frame_div,
                double(g_cw.uncached_starts) / frame_div, double(g_cw.why[kCwGeometry]) / frame_div,
                double(g_cw.why[kCwTexture]) / frame_div, double(g_cw.why[kCwPsConst]) / frame_div,
                double(g_cw.why[kCwShared]) / frame_div, double(g_cw.why[kCwPass]) / frame_div,
                double(g_cw.why[kCwState]) / frame_div, double(g_cw.why[kCwOtherDraw]) / frame_div,
                double(g_cw.why[kCwFull]) / frame_div);
  CrowdLog(line);
  static uint64_t card_prev[4] = {};
  const uint64_t card_now[4] = {g_cw.card_in, g_cw.card_out, g_cw.card_groups, g_cw.card_grouped};
  if (card_now[0] != card_prev[0]) {
    const uint64_t cg = card_now[2] - card_prev[2];
    std::snprintf(line, sizeof(line),
                  "[nr-crowd-cards] swap=%u cards_in/f=%.1f draws_out/f=%.1f groups/f=%.1f grouped/f=%.1f avg_n=%.2f breaks/f{geom=%.1f tex=%.1f psc=%.1f shared=%.1f pass=%.1f state=%.1f full=%.1f}",
                  swap, double(card_now[0] - card_prev[0]) / frame_div, double(card_now[1] - card_prev[1]) / frame_div,
                  double(cg) / frame_div, double(card_now[3] - card_prev[3]) / frame_div,
                  cg ? double(card_now[3] - card_prev[3]) / double(cg) : 0.0, double(g_cw.card_why[kCwGeometry]) / frame_div,
                  double(g_cw.card_why[kCwTexture]) / frame_div, double(g_cw.card_why[kCwPsConst]) / frame_div,
                  double(g_cw.card_why[kCwShared]) / frame_div, double(g_cw.card_why[kCwPass]) / frame_div,
                  double(g_cw.card_why[kCwState]) / frame_div, double(g_cw.card_why[kCwFull]) / frame_div);
    CrowdLog(line);
    for (int i = 0; i < 4; ++i) card_prev[i] = card_now[i];
    for (auto& why : g_cw.card_why) why.store(0, std::memory_order_relaxed);
  }
  for (int i = 0; i < 12; ++i) previous[i] = current[i];
  for (auto& why : g_cw.why) why.store(0, std::memory_order_relaxed);
  g_cw.cached_starts.store(0, std::memory_order_relaxed);
  g_cw.uncached_starts.store(0, std::memory_order_relaxed);
}

// ---- record thread (WWE2K14_NR_RECORD_THREAD=1) ----
namespace {
std::mutex g_rq_fail_mutex;
std::vector<DrawFailure> g_rq_failures;
void AsyncFail(const char* what, const std::string& error) {
  RecordMutexLock lk(g_rq_fail_mutex, "async_failure_mutex");
  if (g_rq_failures.size() < 4096) g_rq_failures.push_back({0, std::string("async ") + what + ": " + error});
}
Draw g_inline_draw;  // the one slot used when the record thread is off
std::unique_ptr<Draw> g_swap_draw;  // U9: private producer ownership, only allocated when opted in
}  // namespace
namespace {
void CrowdReportFailureImpl(uint32_t ordinal, const std::string& error) {
  std::fprintf(stderr, "[nr-crowd] FAILED group starting at draw %u: %s\n", ordinal, error.c_str());
  RecordMutexLock lk(g_rq_fail_mutex, "async_failure_mutex");
  if (g_rq_failures.size() < 4096) g_rq_failures.push_back({ordinal, "crowd group: " + error});
}
}  // namespace
void CrowdReportFailure(uint32_t ordinal, const std::string& error) { CrowdReportFailureImpl(ordinal, error); }
namespace {

void RecordOne(Draw& d) {
  const uint64_t pd_jobs = draw_profile::Now();
  if (!t_on_record_thread) {
    t_on_record_thread = true;
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "NR Record");
#endif
  }
  struct ClearWatchdogState {
    ~ClearWatchdogState() {
      g_record_consumer_phase.store(0, std::memory_order_relaxed);
      g_record_consumer_item.store(0, std::memory_order_relaxed);
      g_record_consumer_job_hash.store(0, std::memory_order_relaxed);
      g_record_consumer_operation.store(nullptr, std::memory_order_release);
    }
  } clear_watchdog_state;
  if (!d.pre_ops.empty() || !d.pre_jobs.empty() || d.jobs_only) CrowdFlushBoundary();
  g_record_consumer_phase.store(1, std::memory_order_relaxed);
  g_record_consumer_ordinal.store(d.ordinal, std::memory_order_relaxed);
  g_record_consumer_item.store(0, std::memory_order_relaxed);
  g_record_consumer_job_hash.store(0, std::memory_order_relaxed);
  g_record_consumer_operation.store(d.jobs_only ? "jobs_only_slot" : "record_slot", std::memory_order_release);
  for (size_t i = 0; i < d.pre_ops.size(); ++i) {
    const uint32_t op = d.pre_ops[i];
    const bool texture = (op >> 30) == 1;
    g_record_consumer_phase.store(2, std::memory_order_relaxed);
    g_record_consumer_item.store(uint32_t(i), std::memory_order_relaxed);
    g_record_consumer_job_hash.store(op, std::memory_order_relaxed);
    g_record_consumer_operation.store(texture ? "release_texture" : "release_snapshot", std::memory_order_release);
    BatchStreamEvent(false, BatchReleaseHash(op));
    if ((op >> 30) == 1) ReleaseTextureNow(op & 0x3FFFFFFFu);
    else ReleaseSnapshotNow(op & 0x3FFFFFFFu);
  }
  for (size_t i = 0; i < d.pre_jobs.size(); ++i) {
    g_record_consumer_phase.store(3, std::memory_order_relaxed);
    g_record_consumer_item.store(uint32_t(i), std::memory_order_relaxed);
    g_record_consumer_job_hash.store(i < d.pre_job_hashes.size() ? d.pre_job_hashes[i] : UINT64_MAX,
                                     std::memory_order_relaxed);
    g_record_consumer_operation.store(i < d.pre_job_names.size() && d.pre_job_names[i] ? d.pre_job_names[i] : "async_job",
                                      std::memory_order_release);
    BatchStreamEvent(false, i < d.pre_job_hashes.size() ? d.pre_job_hashes[i] : UINT64_MAX);
    d.pre_jobs[i]();  // card S3-50: the calls queued before this draw, in call order
  }
  d.pre_jobs.clear();
  if (draw_profile::On()) draw_profile::RecordAdd(draw_profile::kPreJobs, draw_profile::Now() - pd_jobs);
  if (d.jobs_only) return;
  g_record_consumer_phase.store(4, std::memory_order_relaxed);
  g_record_consumer_item.store(0, std::memory_order_relaxed);
  g_record_consumer_job_hash.store(0, std::memory_order_relaxed);
  g_record_consumer_operation.store("draw", std::memory_order_release);
  BatchStreamEvent(false, BatchHashDraw(d));
  std::string err;
  if (g_prof) ++g_stats.rec_draws;
  const uint64_t pd_execute = draw_profile::Now();
  t_pd_record_execute = draw_profile::On();
  const bool execute_ok = ExecuteDrawNow(d, &err);
  t_pd_record_execute = false;
  if (draw_profile::On()) draw_profile::RecordAdd(draw_profile::kExecute, draw_profile::Now() - pd_execute);
  if (!execute_ok) {
    if (g_prof) ++g_stats.rec_failed;
    RecordMutexLock lk(g_rq_fail_mutex, "async_failure_mutex");
    if (g_rq_failures.size() < 4096) g_rq_failures.push_back({d.ordinal, err});
  }
}

void SyncRecordThread(uint32_t fn, bool tail, BatchDrainKind kind) {
  // Card S3-50: queued calls run on the record thread (or on an idle-thread flush); a wait there would wait for itself.
  if (!g_rq || t_on_record_thread) return;
  if (g_batch_n) {
    // The producer's current Draw is private until SubmitDraw; pending backend calls can therefore be published first
    // as jobs-only entries without exposing a partially translated draw to the consumer.
    FlushPendingRecordOps();
    if (g_rq->Idle()) {
      CountBatchDrain(kind, false, 0);
      return;
    }
    if (fn > kProfFnCount) fn = kProfFnCount;
    if (!tail) ++g_stats.rec_barrier[fn];
    const uint64_t t0 = g_prof ? NowNs() : 0;
    const uint64_t batch_t0 = g_batch_stats ? NowNs() : 0;
    g_backend_wait.store(4, std::memory_order_relaxed);
    g_rq->Sync();
    g_backend_wait.store(0, std::memory_order_relaxed);
    CountBatchDrain(kind, true, g_batch_stats ? NowNs() - batch_t0 : 0);
    if (g_prof) {
      const uint64_t ns = NowNs() - t0;
      if (tail) {
        g_stats.rec_tail_ns += ns;
      } else {
        g_stats.rec_wait_ns += ns;
        g_stats.rec_barrier_ns[fn] += ns;
        g_stats.total_ns += ns;
        ++g_stats.rec_syncs;
      }
    }
    return;
  }
  struct FlushPending {  // after the wait the record thread is idle: run the pending releases here
    ~FlushPending() {
      if (g_pending_ops.empty() && g_pending_jobs.empty()) return;
      std::vector<uint32_t> ops;
      ops.swap(g_pending_ops);
      std::vector<std::function<void()>> jobs;
      jobs.swap(g_pending_jobs);
      g_pending_job_hashes.clear();
      g_pending_job_names.clear();
      t_on_record_thread = true;  // the release's ProfScope must not wait again; the thread is idle
      for (uint32_t op : ops) {
        if ((op >> 30) == 1) ReleaseTextureNow(op & 0x3FFFFFFFu);
        else ReleaseSnapshotNow(op & 0x3FFFFFFFu);
      }
      for (auto& job : jobs) job();  // card S3-50: queued calls not yet handed over (record thread idle: run here)
      t_on_record_thread = false;
    }
  } flush_pending;
  if (g_rq->Idle()) return;
  if (fn > kProfFnCount) fn = kProfFnCount;
  if (!tail) ++g_stats.rec_barrier[fn];
  const uint64_t t0 = g_prof ? NowNs() : 0;
  g_backend_wait.store(4, std::memory_order_relaxed);
  g_rq->Sync();
  g_backend_wait.store(0, std::memory_order_relaxed);
  if (g_prof) {
    const uint64_t ns = NowNs() - t0;
    if (tail) {
      g_stats.rec_tail_ns += ns;
    } else {
      g_stats.rec_wait_ns += ns;
      g_stats.rec_barrier_ns[fn] += ns;
      g_stats.total_ns += ns;  // the renderer thread was busy waiting: part of nr_ms, not of any frontend category
      ++g_stats.rec_syncs;
    }
  }
}

void StartRecordThread() {
  const char* rt = std::getenv("WWE2K14_NR_RECORD_THREAD");
  const char* batch_env = std::getenv("WWE2K14_NR_RECORD_BATCH");
  if (batch_env && *batch_env) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(batch_env, &end, 10);
    if (end != batch_env && *end == '\0' && parsed != 0) g_batch_n = uint32_t(std::min(parsed, 64ul));
  }
  const char* batch_stats_env = std::getenv("WWE2K14_NR_RECORD_BATCH_STATS");
  g_batch_stats = g_batch_n && batch_stats_env && std::strcmp(batch_stats_env, "1") == 0;
  const char* batch_check_env = std::getenv("WWE2K14_NR_RECORD_BATCH_CHECK");
  g_batch_check = g_batch_n && batch_check_env && std::strcmp(batch_check_env, "1") == 0;
  // "1" = on (inline when the process may use only one CPU, where a second thread can only cost time); "2" = force on.
  const bool thread_mode = rt && (std::strcmp(rt, "1") == 0 || std::strcmp(rt, "2") == 0 || std::strcmp(rt, "3") == 0);
  if ((!thread_mode && !g_batch_n) || g_rq) return;
  g_async = g_batch_n != 0 || std::strcmp(rt ? rt : "", "3") == 0;  // batch mode also queues ordered backend calls
#if defined(__linux__)
  cpu_set_t set;
  if ((g_batch_n || (rt && std::strcmp(rt, "1") == 0)) && sched_getaffinity(0, sizeof(set), &set) == 0 && CPU_COUNT(&set) < 2) {
    std::fprintf(stderr, "[nr] record thread: only one CPU available to this process, recording inline\n");
    g_batch_n = 0;
    g_async = false;
    return;
  }
#endif
  g_surface_reservations.clear();
  g_surface_by_handle.clear();
  g_surface_next = g ? uint32_t(g->surfaces.size()) : 0;
  g_rq = new RecordQueue<Draw, kRecordSlotsMax>(RecordOne);
  g_rq_watchdog.store(g_rq, std::memory_order_release);
  if (RecordCpuDiagnosticOn())
    g_record_cpu_previous_ns.store(ReadRecordThreadCpuNs(), std::memory_order_relaxed);
  std::atexit([] {
    if (g_rq) {
      if (g_batch_n) FlushPendingRecordOps();
      g_rq->Stop();
    }
  });
  if (g_batch_n)
    std::fprintf(stderr, "[nr] record thread on (128 draw slots, batch=%u, ordered async backend calls, stats=%u, check=%u)\n",
                 g_batch_n, g_batch_stats ? 1 : 0, g_batch_check ? 1 : 0);
  else
    std::fprintf(stderr, "[nr] record thread on (128 draw slots%s)\n", g_async ? ", async backend calls (S3-50)" : "");
}
}  // namespace

bool GetRecordBatchWatchdogState(RecordBatchWatchdogState* state) {
  if (!state) return false;
  *state = {};
  RecordQueue<Draw, kRecordSlotsMax>* rq = g_rq_watchdog.load(std::memory_order_acquire);
  if (!rq) return false;
  const RecordQueueWatchdogState q = rq->WatchdogState();
  state->head = q.published;
  state->committed = q.committed;
  state->tail = q.consumed;
  state->spurious_recoveries = q.spurious_recoveries;
  state->executing_slot = q.executing;
  state->queue_state = q.worker_state;
  state->producer_blocked = q.producer_blocked;
  state->producer_sync_waiting = q.producer_sync_waiting;
  state->phase = g_record_consumer_phase.load(std::memory_order_acquire);
  state->current_ordinal = g_record_consumer_ordinal.load(std::memory_order_relaxed);
  state->current_item = g_record_consumer_item.load(std::memory_order_relaxed);
  state->current_job_hash = g_record_consumer_job_hash.load(std::memory_order_relaxed);
  state->consumer_wait = g_record_consumer_wait.load(std::memory_order_acquire);
  state->backend_wait_site = g_backend_wait.load(std::memory_order_relaxed);
  state->wait_serial = g_record_consumer_wait_serial.load(std::memory_order_relaxed);
  state->operation = g_record_consumer_operation.load(std::memory_order_acquire);
  state->wait_name = g_record_consumer_wait_name.load(std::memory_order_acquire);
  return true;
}

void ConstUsedRecordCopy(uint32_t copied, uint32_t cleared, uint32_t mismatches) {
  g_const_used_copied.fetch_add(copied, std::memory_order_relaxed);
  g_const_used_cleared.fetch_add(cleared, std::memory_order_relaxed);
  g_const_used_mismatches.fetch_add(mismatches, std::memory_order_relaxed);
}
void ConstUsedRecordBatchDifference() {
  g_const_used_batch_differences.fetch_add(1, std::memory_order_relaxed);
}
void ConstUsedRecordCrowdDifference() {
  g_const_used_crowd_differences.fetch_add(1, std::memory_order_relaxed);
}
ConstUsedStats ConstUsedTakeStats() {
  ConstUsedStats result;
  result.copied_registers = g_const_used_copied.exchange(0, std::memory_order_relaxed);
  result.cleared_registers = g_const_used_cleared.exchange(0, std::memory_order_relaxed);
  result.used_row_mismatches = g_const_used_mismatches.exchange(0, std::memory_order_relaxed);
  result.batch_hash_differences = g_const_used_batch_differences.exchange(0, std::memory_order_relaxed);
  result.crowd_decision_differences = g_const_used_crowd_differences.exchange(0, std::memory_order_relaxed);
  return result;
}
void ConstUsedFrameBoundary() {
  SyncRecordThread(kProfFnCount, true, kBatchDrainFrameTail);
  CrowdFlushBoundary();
}

bool ExecuteDraw(const Draw& d, std::string* error) { return ExecuteDrawNow(d, error); }

void SetV17cFrame(uint32_t frame) {
  if (!g || !g->v17c_ab_on) return;
  const int mode = wwe2k14::d3d::v17c_ab::ModeForFrame(frame);
  if (mode < 0 || mode == g->v17c_ab_mode) return;

  // No pending draw may cross a scheduled mode boundary with a stale descriptor-cache decision.
  SyncRecordThread(kProfFnCount, true, kBatchDrainFrameTail);
  CrowdFlushBoundary();
  g->rec_reuse.store(mode ? g->v17c_rec_reuse_mode : 0, std::memory_order_relaxed);
  g->vb_direct_mode.store(mode ? g->v17c_vb_direct_mode : 0u, std::memory_order_relaxed);
  for (CmdEntry& entry : g->cmd_pool) {
    entry.elide.sets[4] = nullptr;
    entry.elide.check_set4 = nullptr;
  }
  g->v17c_ab_mode = mode;
  g_vb_direct_stats = VbDirectCounts{};
  g_rec_reuse_stats = RecReuseCounts{};
}

uint32_t DirectVertexUploadMode() { return VbDirectMode(); }

bool CanDirectVertexUpload(size_t allocation_bytes, uint64_t ps_key, uint32_t mode, bool* skip_legacy_copy) {
  if (skip_legacy_copy) *skip_legacy_copy = false;
  if (!mode) {
    ++g_vb_direct_stats.not_mode_off;
    return false;
  }
  if (g_batch_n) {
    ++g_vb_direct_stats.not_batch;
    return false;
  }
  if (g_rq || t_on_record_thread) {
    ++g_vb_direct_stats.not_record_queue;
    return false;
  }
  if (ps_key == kCrowdPsKey || (ps_key == kCardPsKey && CrowdCardsOn()) ||
      ((ps_key == kEntranceCardPsKey || ps_key == kEntranceFigurePsKey) && CrowdEntranceOn())) {
    ++g_vb_direct_stats.not_crowd_candidate;
    return false;
  }
  if (!g || !RingHasRoom(allocation_bytes, kRingVertexAlign)) {
    ++g_vb_direct_stats.not_ring_room;
    return false;
  }
  if (skip_legacy_copy) *skip_legacy_copy = mode == 1;
  return true;
}

DirectVertexUploadStats GetDirectVertexUploadStats() {
  const VbDirectCounts& s = g_vb_direct_stats;
  return {VbDirectMode(), s.direct_draws, s.direct_bytes, s.check_draws, s.check_bytes, s.mismatch_draws,
          s.mismatch_bytes, s.not_record_queue, s.not_batch, s.not_crowd_candidate, s.not_ring_room, s.not_mode_off};
}

RecReuseStats GetRecReuseStats() {
  return {uint32_t(g ? g->rec_reuse.load(std::memory_order_relaxed) : 0), g_rec_reuse_stats.hits,
          g_rec_reuse_stats.checks, g_rec_reuse_stats.mismatches};
}

Draw* AcquireDraw() {
  if (g_rq && g_batch_n) {
    if (draw_swap::Mode()) {
      if (!g_swap_draw) g_swap_draw = std::make_unique<Draw>();
      g_swap_draw->Reset();
      g_slot_held = true;
      return g_swap_draw.get();
    }
    g_inline_draw.Reset();
    g_slot_held = true;
    return &g_inline_draw;
  }
  Draw* d = g_rq ? g_rq->Acquire() : &g_inline_draw;
  d->Reset();
  g_slot_held = g_rq != nullptr;
  return d;
}

bool SubmitDraw(Draw* d, std::string* error) {
  if (!g_rq) return ExecuteDrawNow(*d, error);
  const uint64_t pd_attach = draw_profile::Now();
  d->pre_ops.swap(g_pending_ops);
  g_pending_ops.clear();
  d->pre_jobs.swap(g_pending_jobs);
  d->pre_job_hashes.swap(g_pending_job_hashes);
  d->pre_job_names.swap(g_pending_job_names);
  g_pending_jobs.clear();
  g_pending_job_hashes.clear();
  g_pending_job_names.clear();
  g_slot_held = false;
  draw_profile::Add(draw_profile::kAttachJobs, pd_attach);
  if (g_batch_n) {
    Draw* slot = AcquireRecordSlot(true);
    bool swapped = false;
    if (draw_swap::Mode() && d == g_swap_draw.get()) {
      const uint64_t pd_swap = draw_profile::Now();
      const auto result = draw_swap::Handoff(*g_rq, slot, g_swap_draw, draw_swap::Mode());
      draw_profile::Add(draw_profile::kTakeFrom, pd_swap);
      draw_profile::producer.checks += result.checked;
      draw_profile::producer.mismatches += result.mismatch;
      draw_profile::producer.swaps += result.swapped;
      draw_profile::producer.fallbacks += !result.swapped;
      slot = result.submitted;
      swapped = result.swapped;
    }
    if (!swapped) {
      const uint64_t pd_reset = draw_profile::Now();
      slot->Reset();
      draw_profile::Add(draw_profile::kSlotReset, pd_reset);
      const uint64_t pd_take = draw_profile::Now();
      slot->TakeFrom(*d);
      draw_profile::Add(draw_profile::kTakeFrom, pd_take);
    }
    BatchStreamProducerSlot(*slot);
    if (!swapped) {
      const uint64_t pd_reset = draw_profile::Now();
      d->Reset();
      draw_profile::Add(draw_profile::kProducerReset, pd_reset);
    }
    // On swap, `d` belongs to the queue. Do not touch it after publication. The previous slot
    // is now private and is reset by the NEXT AcquireDraw (or AbandonDraw).
    const uint64_t pd_publish = draw_profile::Now();
    g_rq->SubmitBatch(true, g_batch_n);
    draw_profile::Add(draw_profile::kPublish, pd_publish);
    return true;
  }
  g_rq->Submit();
  return true;
}

void AbandonDraw() {
  if (!g_batch_n || !g_rq) return;
  g_slot_held = false;
  if (draw_swap::Mode() && g_swap_draw) g_swap_draw->Reset();
  else g_inline_draw.Reset();
}

std::vector<DrawFailure> TakeDrawFailures() {
  SyncRecordThread(kProfFnCount, true, kBatchDrainFrameTail);
  std::lock_guard<std::mutex> lk(g_rq_fail_mutex);
  std::vector<DrawFailure> out;
  out.swap(g_rq_failures);
  return out;
}

bool RecordThreadActive() { return g_rq != nullptr; }
bool TakeRecordThreadCpuDeltaNs(uint64_t* delta_ns) {
  if (!delta_ns || !RecordCpuDiagnosticOn() || !g_rq) return false;
  const uint64_t cpu_now = ReadRecordThreadCpuNs();
  if (!cpu_now) return false;
  const uint64_t cpu_previous = g_record_cpu_previous_ns.exchange(cpu_now, std::memory_order_relaxed);
  if (!cpu_previous || cpu_now < cpu_previous) return false;
  *delta_ns = cpu_now - cpu_previous;
  return true;
}
uintptr_t RecordThreadNativeHandle() {
  // Linux: pthread_t. Windows (clang-cl/MSVC STL): std::thread::native_handle() is a HANDLE.
  return g_rq ? (uintptr_t)g_rq->worker_native_handle() : 0;
}
unsigned long RecordThreadHandle() {
#if defined(__linux__)
  return g_rq ? static_cast<unsigned long>(g_rq->worker_native_handle()) : 0;
#else
  return 0;
#endif
}

namespace {
// Copies/resolves must stay inside the source surface: a rectangle past its edge makes the MSAA resolve
// blit write outside its target, which on NVIDIA is a GPU exception (Xid 13 "WIDTH CT Violation") that
// kills the channel - the next fence wait then hangs the game (Wine gates win-g02/g04/g05). Refuse it.
bool RectInSurface(const SurfaceObj& t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::string* error) {
  if (uint64_t(x) + w <= t.lw && uint64_t(y) + h <= t.lh) return true;  // logical coordinates (card S3-31)
  *error = "rect " + std::to_string(x) + "," + std::to_string(y) + " " + std::to_string(w) + "x" +
           std::to_string(h) + " outside the " + std::to_string(t.width) + "x" + std::to_string(t.height) +
           " surface";
  return false;
}
}  // namespace

bool ReadColor(uint32_t surface, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint8_t>& bytes,
               std::string* error) {
  if (BatchCall()) return QueueSyncCall(kBatchDrainPublicReadColor,
                                        BatchHashWords("read_color", {surface, x, y, w, h}),
                                        [=, &bytes] { return ReadColor(surface, x, y, w, h, bytes, error); });
  ProfScope prof_scope(kProfReadColor);
  if (g->state_elide) EndDrawPass();
  if (!LiveSurface(surface) || g->surfaces[surface].depth) {
    *error = "bad colour surface";
    return false;
  }
  SurfaceObj& t = g->surfaces[surface];
  TouchSurface(surface);
  if (!RectInSurface(t, x, y, w, h, error)) return false;
  const uint32_t bpp = t.format == RenderFormat::R16G16B16A16_FLOAT || t.format == RenderFormat::R32G32_FLOAT ||
                               t.format == RenderFormat::R16G16B16A16_SNORM
                           ? 8
                           : 4;
  CountBuffer();
  if (g_prof) g_stats.readback_bytes += uint64_t(w) * h * bpp;
  auto rb = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(w) * h * bpp));
  if (!rb) {  // k14 G37
    *error = "read target: readback buffer";
    return false;
  }
  RenderTexture* source = t.image.get();
  if (t.samples > 1) {
    // Average the samples (vkCmdResolveImage) into the 1x copy, then read that.
    {
      BarrierBatch bb;  // k14 P22: batched (precise barriers only)
      bb.Layout(t.image.get(), RenderTextureLayout::RESOLVE_SOURCE);
      bb.Layout(t.resolved.get(), RenderTextureLayout::RESOLVE_DEST);
    }
    RenderRect r(int32_t(x), int32_t(y), int32_t(x + w), int32_t(y + h));
    g->list->resolveTextureRegion(t.resolved.get(), x, y, t.image.get(), &r);
    ResolveCensusMsaaResolve(uint64_t(TightPitch(t.format, w)) * BlockRows(t.format, h));
    Barrier(t.resolved.get(), RenderTextureLayout::COPY_SOURCE);
    source = t.resolved.get();
  } else {
    Barrier(t.image.get(), RenderTextureLayout::COPY_SOURCE);
  }
  RenderBox box(int32_t(x), int32_t(y), int32_t(x + w), int32_t(y + h));
  if (!CopyTextureToTight(rb.get(), 0, t.format, w, h, source, box)) {  // [wwe13 nr] D3D12 pitch rule
    *error = "readback repitch buffer";
    return false;
  }
  Submit();
  const uint8_t* src = static_cast<const uint8_t*>(rb->map());
  if (!src) {
    *error = "read target: map readback";
    return false;
  }
  bytes.assign(src, src + size_t(w) * h * bpp);
  rb->unmap();
  ResolveCensusReadback(uint64_t(w) * h * bpp);
  return true;
}

// Card S3-28 (WWE2K14_D3D_CLEAR_IMAGE=1): a clear of the whole image is recorded as an image clear outside any render
// pass (vkCmdClearColorImage / vkCmdClearDepthStencilImage) instead of opening a pass for vkCmdClearAttachments; the
// driver can use its fast-clear path. Same values, same image, same ordering (full write barrier before).
static bool ClearImageOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_CLEAR_IMAGE");
    return e && std::strcmp(e, "1") == 0;
  }();
  return on;
}
static bool WholeImage(const SurfaceObj& t, int32_t x, int32_t y, int32_t w, int32_t h) {
  return x == 0 && y == 0 && w > 0 && h > 0 && uint32_t(w) == t.width && uint32_t(h) == t.height;
}

bool ClearColor(uint32_t surface, int32_t x, int32_t y, int32_t w, int32_t h, const float rgba[4], bool integer_view,
                std::string* error) {
  if (AsyncCall()) {
    std::array<float, 4> c = {rgba[0], rgba[1], rgba[2], rgba[3]};
    const uint64_t hash = BatchHashWords("clear_color", {surface, uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h),
                                                          BatchFloatBits(c[0]), BatchFloatBits(c[1]),
                                                          BatchFloatBits(c[2]), BatchFloatBits(c[3]),
                                                          uint64_t(integer_view)});
    return QueueBoolCall("clear colour", hash,
                         [=](std::string* e) { return ClearColor(surface, x, y, w, h, c.data(), integer_view, e); });
  }
  ProfScope prof_scope(kProfClear);
  if (g->state_elide) EndDrawPass();
  if (!LiveSurface(surface) || g->surfaces[surface].depth) {
    *error = "bad colour surface";
    return false;
  }
  SurfaceObj& t = g->surfaces[surface];
  TouchSurface(surface);
  // The SNORM16 fallback is a floating-point image with no integer view; apply its clear through that image's float view.
  if (integer_view && t.int_view_disabled) integer_view = false;
  if (integer_view && t.int_format == RenderFormat::UNKNOWN) {
    *error = "no integer view";
    return false;
  }
  if (t.scale != 1.0f) {  // card S3-31: logical -> physical
    const int32_t x0 = PhysEdge(t, x, false), y0 = PhysEdge(t, y, true);
    w = PhysEdge(t, x + w, false) - x0;
    h = PhysEdge(t, y + h, true) - y0;
    x = x0;
    y = y0;
  }
  if (ClearImageOn() && !integer_view && WholeImage(t, x, y, w, h)) {
    WriteBarrier(t.image.get(), RenderTextureLayout::COPY_DEST);
    RenderColor c;
    for (int i = 0; i < 4; ++i) c.rgba[i] = rgba[i];
    if (g->list->clearTextureColor(t.image.get(), c)) return true;
  }
  WriteBarrier(t.image.get(), RenderTextureLayout::COLOR_WRITE);
  // The transfer framebuffer renders through the integer view (when the format has one).
  g->list->setFramebuffer(integer_view ? t.transfer_fb.get() : Framebuffer(surface, kNone));
  RenderRect r(x, y, x + w, y + h);
  RenderColor c;
  for (int i = 0; i < 4; ++i) c.rgba[i] = rgba[i];  // raw bits for an integer view (VkClearColorValue union)
  g->list->clearColor(0, c, &r, 1);
  g->list->setFramebuffer(nullptr);
  return true;
}

bool ClearDepthStencil(uint32_t surface, int32_t x, int32_t y, int32_t w, int32_t h, bool depth, float z,
                       bool stencil, uint32_t s, std::string* error) {
  if (AsyncCall())
    return QueueBoolCall("clear depth",
                         BatchHashWords("clear_depth", {surface, uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h),
                                                         uint64_t(depth), BatchFloatBits(z), uint64_t(stencil), s}),
                         [=](std::string* e) { return ClearDepthStencil(surface, x, y, w, h, depth, z, stencil, s, e); });
  ProfScope prof_scope(kProfClear);
  if (g->state_elide) EndDrawPass();
  if (!LiveSurface(surface) || !g->surfaces[surface].depth) {
    *error = "bad depth surface";
    return false;
  }
  SurfaceObj& t = g->surfaces[surface];
  TouchSurface(surface);
  if (g_ddiag && depth) {
    DdSurf& ds = g_dd_s[surface];
    ds.clear = ++g_dd_seq;
    ds.draws_since_clear = 0;
    ++ds.clears;
  }
  if (t.scale != 1.0f) {  // card S3-31: logical -> physical
    const int32_t x0 = PhysEdge(t, x, false), y0 = PhysEdge(t, y, true);
    w = PhysEdge(t, x + w, false) - x0;
    h = PhysEdge(t, y + h, true) - y0;
    x = x0;
    y = y0;
  }
  if (ClearImageOn() && WholeImage(t, x, y, w, h)) {
    WriteBarrier(t.image.get(), RenderTextureLayout::COPY_DEST);
    if (g->list->clearTextureDepthStencil(t.image.get(), depth, stencil, z, s)) return true;
  }
  WriteBarrier(t.image.get(), RenderTextureLayout::DEPTH_WRITE);
  g->list->setFramebuffer(t.transfer_fb.get());
  RenderRect r(x, y, x + w, y + h);
  g->list->clearDepthStencil(depth, stencil, z, s, &r, 1);
  g->list->setFramebuffer(nullptr);
  return true;
}

// Shared staging upload used by the resolve LUT and texture mirror/page-table setup.
namespace {
bool FillBuffer(RenderBuffer* buffer, uint64_t bytes, const void* data, uint8_t fill) {
  CountBuffer();
  if (g_prof) g_stats.upload_bytes += bytes;
  auto staging = g->device->createBuffer(RenderBufferDesc::UploadBuffer(bytes));
  uint8_t* m = staging ? static_cast<uint8_t*>(staging->map()) : nullptr;
  if (!m) return false;  // k14 G37: the destination keeps its previous contents
  if (data) std::memcpy(m, data, bytes);
  else std::memset(m, fill, bytes);
  staging->unmap();
  BufferBarrier(buffer);
  CopyBarrier(buffer, true);  // [wwe13 nr] D3D12 only
  g->list->copyBufferRegion(RenderBufferReference(buffer), RenderBufferReference(staging.get()), bytes);
  ResolveCensusCopy(bytes);
  BufferBarrier(buffer);
  Retire(std::move(staging));
  return true;
}
}  // namespace

namespace {
const RenderDescriptorRange kResolveRanges[4] = {
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 0, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 1, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 2, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 3, 1)};
// D3D depth snapshot: depth + stencil textures, then the staging buffer + page table.
const RenderDescriptorRange kResolveDepthRanges[4] = {
    RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 1, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 2, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 3, 1)};
// Card S3-38: depth -> R32F storage image (depth view, stencil view, the output image, the page table).
const RenderDescriptorRange kDepthImgRanges[4] = {
    RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 1, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_TEXTURE, 2, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 3, 1)};
// Fixed16 fused image conversion: the source image, destination staging buffer, LUT and page table.
const RenderDescriptorRange kResolveTexRanges[4] = {
    RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 1, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 2, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 3, 1)};
const RenderDescriptorRange kF16LeanRanges[2] = {
    RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::STRUCTURED_BUFFER, 1, 1)};
const RenderDescriptorRange kDetileRanges[3] = {
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 0, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 1, 1),
    RenderDescriptorRange(RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 2, 1)};
constexpr uint32_t kGuestPages = 0x20000000u >> 12;  // 512 MB physical
constexpr uint32_t kNoSlot = UINT32_MAX;
constexpr uint64_t kResolveLutEntries = 65536 + 256;

// Initialize host-image copyout pipelines used by D3D snapshots and their shared LUT.
void InitResolve(const std::string& shader_dir) {
  if (const char* lean = std::getenv("WWE2K14_D3D_DEPTH_LEAN")) {
    g->depth_lean_on = std::strcmp(lean, "1") == 0;
    g->depth_lean_check = std::strcmp(lean, "check") == 0;
  }
  const bool dxil_snapshot_routes = g->shader_format != RenderShaderFormat::SPIRV;
  if (dxil_snapshot_routes) {
    g->depth_lean_on = false;
    g->depth_lean_check = false;
    std::fprintf(stderr,
                 "[nr] depth lean/image route: off on DXIL (device-removed bisect 2026-10-31), using buffer route\n");
  }
  if (g->depth_lean_on || g->depth_lean_check) {
    std::fprintf(stderr, "[nr] depth lean (G5): %s\n", g->depth_lean_on ? "on" : "check (1/16)");
  }
  RenderDescriptorSetDesc set(kResolveRanges, 4);
  RenderPushConstantRange push(0, 0, 0, sizeof(ResolveParams), RenderShaderStageFlag::COMPUTE);
  g->resolve_layout = g->device->createPipelineLayout(RenderPipelineLayoutDesc(&push, 1, &set, 1));
  const RenderBufferFlags storage = RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS;
  g->resolve_lut = g->device->createBuffer(RenderBufferDesc::DefaultBuffer(kResolveLutEntries * 4, storage));
  // Depth snapshots share a descriptor layout with depth-to-R32F, without loading the packet renderer's
  // EDRAM-buffer resolve variants.
  {
    RenderDescriptorSetDesc dset(kResolveDepthRanges, 4);
    RenderPushConstantRange dpush(0, 0, 0, sizeof(ResolveParams), RenderShaderStageFlag::COMPUTE);
    g->resolve_depth_layout = g->device->createPipelineLayout(RenderPipelineLayoutDesc(&dpush, 1, &dset, 1));
  }
  // D3D depth snapshots: float24 depth -> R32F. Optional.
  if (g->resolve_depth_layout) {
    for (int v = 0; v < 2; ++v) {
      std::ifstream f(LibShaderPath(shader_dir, v ? "edram_depth_to_r32f_1x" : "edram_depth_to_r32f"), std::ios::binary);
      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (bytes.empty()) continue;
      auto& cs = v ? g->depth_r32f_cs_1x : g->depth_r32f_cs;
      cs = g->device->createShader(bytes.data(), bytes.size(), "main", g->shader_format);
      if (cs) {
        (v ? g->depth_r32f_pipeline_1x : g->depth_r32f_pipeline) = g->device->createComputePipeline(
            RenderComputePipelineDesc(g->resolve_depth_layout.get(), cs.get(), 8, 8, 1));
      }
    }
  }
  if (const char* e = std::getenv("WWE2K14_D3D_FSR1"); e && std::strcmp(e, "1") == 0) {
    const char* status = "off (unsupported shader format)";
    if (const char* sharp = std::getenv("WWE2K14_D3D_FSR1_SHARP")) {
      char* end = nullptr;
      const float value = std::strtof(sharp, &end);
      if (end != sharp && *end == '\0' && std::isfinite(value)) g->fsr1_sharpness = value;
    }
    if (g->shader_format == RenderShaderFormat::SPIRV) {
      status = "off (missing FSR1 EASU or RCAS shader)";
      std::ifstream fe(LibShaderPath(shader_dir, "edram_fsr1_easu"), std::ios::binary);
      std::ifstream fr(LibShaderPath(shader_dir, "edram_fsr1_rcas"), std::ios::binary);
      std::vector<uint8_t> easu_bytes((std::istreambuf_iterator<char>(fe)), std::istreambuf_iterator<char>());
      std::vector<uint8_t> rcas_bytes((std::istreambuf_iterator<char>(fr)), std::istreambuf_iterator<char>());
      if (!easu_bytes.empty() && !rcas_bytes.empty()) {
        status = "off (shader or pipeline creation failed)";
        g->fsr1_easu_cs = g->device->createShader(easu_bytes.data(), easu_bytes.size(), "main", g->shader_format);
        g->fsr1_rcas_cs = g->device->createShader(rcas_bytes.data(), rcas_bytes.size(), "main", g->shader_format);
        RenderDescriptorSetDesc dset(kFsr1Ranges, 3);
        RenderPushConstantRange push(0, 0, 0, sizeof(Fsr1PushConstants), RenderShaderStageFlag::COMPUTE);
        g->fsr1_layout = g->device->createPipelineLayout(RenderPipelineLayoutDesc(&push, 1, &dset, 1));
        RenderSamplerDesc sampler;
        sampler.magFilter = RenderFilter::LINEAR;
        sampler.minFilter = RenderFilter::LINEAR;
        sampler.mipmapMode = RenderMipmapMode::NEAREST;
        sampler.addressU = RenderTextureAddressMode::CLAMP;
        sampler.addressV = RenderTextureAddressMode::CLAMP;
        sampler.addressW = RenderTextureAddressMode::CLAMP;
        sampler.borderColor = RenderBorderColor::TRANSPARENT_BLACK;
        sampler.anisotropyEnabled = false;
        sampler.maxAnisotropy = 1;
        sampler.mipLODBias = 0.0f;
        sampler.minLOD = 0.0f;
        sampler.maxLOD = 0.0f;
        g->fsr1_sampler = g->device->createSampler(sampler);
        if (g->fsr1_easu_cs && g->fsr1_rcas_cs && g->fsr1_layout && g->fsr1_sampler) {
          g->fsr1_easu_pipeline = g->device->createComputePipeline(
              RenderComputePipelineDesc(g->fsr1_layout.get(), g->fsr1_easu_cs.get(), 8, 8, 1));
          g->fsr1_rcas_pipeline = g->device->createComputePipeline(
              RenderComputePipelineDesc(g->fsr1_layout.get(), g->fsr1_rcas_cs.get(), 8, 8, 1));
        }
        g->fsr1_on = g->fsr1_easu_pipeline && g->fsr1_rcas_pipeline && g->fsr1_sampler;
        if (g->fsr1_on) status = "on";
      }
    }
    std::fprintf(stderr, "[nr] FSR1 scaled snapshots: %s\n", status);
  }
  const char* di = std::getenv("WWE2K14_D3D_DEPTH_IMG");
  const bool depth_img_requested = !dxil_snapshot_routes && di && std::strcmp(di, "1") == 0;
  if (depth_img_requested || g->depth_lean_on || g->depth_lean_check) {
    RenderDescriptorSetDesc dset(kDepthImgRanges, 4);
    RenderPushConstantRange dpush(0, 0, 0, sizeof(ResolveParams), RenderShaderStageFlag::COMPUTE);
    g->depth_img_layout = g->device->createPipelineLayout(RenderPipelineLayoutDesc(&dpush, 1, &dset, 1));
    for (int v = 0; v < 2 && g->depth_img_layout; ++v) {
      std::ifstream f(LibShaderPath(shader_dir, v ? "edram_depth_to_r32f_img_1x" : "edram_depth_to_r32f_img"), std::ios::binary);
      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (bytes.empty()) continue;
      auto& cs = v ? g->depth_img_cs_1x : g->depth_img_cs;
      cs = g->device->createShader(bytes.data(), bytes.size(), "main", g->shader_format);
      if (cs) {
        (v ? g->depth_img_pipeline_1x : g->depth_img_pipeline) = g->device->createComputePipeline(
            RenderComputePipelineDesc(g->depth_img_layout.get(), cs.get(), 8, 8, 1));
      }
    }
    std::fprintf(stderr, "[nr] depth snapshot direct image (S3-38/G5): %s\n",
                 g->depth_img_pipeline && g->depth_img_pipeline_1x ? "on" : "off (missing depth_to_r32f_img variants)");
  }
  // Card P16: fixed16 copy-out as a linear UNORM image. Optional; the frontend asks (F16_REUSE).
  {
    std::ifstream f(LibShaderPath(shader_dir, "edram_resolve_color_linear"), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!bytes.empty() && g->resolve_layout) {
      g->f16_linear_cs = g->device->createShader(bytes.data(), bytes.size(), "main", g->shader_format);
      if (g->f16_linear_cs) {
        g->f16_linear_pipeline = g->device->createComputePipeline(
            RenderComputePipelineDesc(g->resolve_layout.get(), g->f16_linear_cs.get(), 8, 8, 1));
      }
    }
  }
  // The old snapshot-to-guest-memory colour conversion is intentionally not loaded; D3D uses host-image snapshots.
  if (dxil_snapshot_routes) {
    std::fprintf(stderr, "[nr] fixed16 snapshot fused (S3-23): off on DXIL (device removed)\n");
  } else if (const char* ff = std::getenv("WWE2K14_D3D_F16_FUSED"); ff && std::strcmp(ff, "1") == 0) {
    std::ifstream f(LibShaderPath(shader_dir, "edram_resolve_color_tex_linear"), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!bytes.empty()) {
      g->f16_tex_cs = g->device->createShader(bytes.data(), bytes.size(), "main", g->shader_format);
      RenderDescriptorSetDesc tset(kResolveTexRanges, 4);
      RenderPushConstantRange tpush(0, 0, 0, sizeof(ResolveParams), RenderShaderStageFlag::COMPUTE);
      g->f16_tex_layout = g->device->createPipelineLayout(RenderPipelineLayoutDesc(&tpush, 1, &tset, 1));
      if (g->f16_tex_cs && g->f16_tex_layout) {
        g->f16_tex_pipeline = g->device->createComputePipeline(
            RenderComputePipelineDesc(g->f16_tex_layout.get(), g->f16_tex_cs.get(), 8, 8, 1));
      }
    }
    std::fprintf(stderr, "[nr] fixed16 snapshot fused (S3-23): %s\n",
                 g->f16_tex_pipeline ? "on" : "off (no fixed16 fused shader)");
  }
  const char* f16_lean = std::getenv("WWE2K14_D3D_F16_LEAN");
  if (f16_lean && std::strcmp(f16_lean, "1") == 0) g->f16_lean_mode = 1;
  else if (f16_lean && std::strcmp(f16_lean, "check") == 0) g->f16_lean_mode = 2;
  if (g->f16_lean_mode && g->shader_format != RenderShaderFormat::SPIRV) {
    g->f16_lean_mode = 0;
    std::fprintf(stderr, "[nr] fixed16 lean fragment: not available on DXIL, using linear compute\n");
  }
  if (g->f16_lean_mode && g->shader_format == RenderShaderFormat::SPIRV) {
    auto read_shader = [&](const char* name) {
      std::ifstream f(LibShaderPath(shader_dir, name), std::ios::binary);
      return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const std::vector<uint8_t> vs_bytes = read_shader("edram_f16_lean_vs");
    const std::vector<uint8_t> ps_bytes = read_shader("edram_f16_lean_ps");
    if (!vs_bytes.empty() && !ps_bytes.empty()) {
      g->f16_lean_vs = g->device->createShader(vs_bytes.data(), vs_bytes.size(), "vs_main", g->shader_format);
      g->f16_lean_ps = g->device->createShader(ps_bytes.data(), ps_bytes.size(), "ps_main", g->shader_format);
      RenderDescriptorSetDesc tset(kF16LeanRanges, 2);
      RenderPushConstantRange tpush(0, 0, 0, sizeof(ResolveParams), RenderShaderStageFlag::PIXEL);
      g->f16_lean_layout = g->device->createPipelineLayout(RenderPipelineLayoutDesc(&tpush, 1, &tset, 1));
      if (g->f16_lean_vs && g->f16_lean_ps && g->f16_lean_layout) {
        RenderGraphicsPipelineDesc p;
        p.pipelineLayout = g->f16_lean_layout.get();
        p.vertexShader = g->f16_lean_vs.get();
        p.pixelShader = g->f16_lean_ps.get();
        p.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        p.cullMode = RenderCullMode::NONE;
        p.multisampling = Multisampling(1);
        p.renderTargetCount = 1;
        p.renderTargetBlend[0] = RenderBlendDesc::Copy();
        p.renderTargetFormat[0] = RenderFormat::R16G16_UNORM;
        g->f16_lean_rg_pipeline = g->device->createGraphicsPipeline(p);
        p.renderTargetFormat[0] = RenderFormat::R16G16B16A16_UNORM;
        g->f16_lean_rgba_pipeline = g->device->createGraphicsPipeline(p);
      }
    }
  }
  const bool f16_lean_ready = g->f16_lean_layout && g->f16_lean_rg_pipeline && g->f16_lean_rgba_pipeline;
  std::fprintf(stderr, "[nr] fixed16 lean fragment (WWE2K14_D3D_F16_LEAN): %s\n",
               !g->f16_lean_mode ? "off" : f16_lean_ready ? (g->f16_lean_mode == 2 ? "check" : "on")
                                                            : "requested but shader/pipeline unavailable");
}

// Shared resolve-binding table and texture-detile setup, independent of EDRAM emulation and resolve shader availability.
void InitSharedTextureDetile(const std::string& shader_dir) {
  g->shader_dir = shader_dir;
  const RenderBufferFlags storage = RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS;
  // D3D resolve shaders retain their shared g_pt descriptor; this table remains all "no slot" and is not guest input.
  g->pt_cpu.assign(kGuestPages, kNoSlot);
  g->mirror_pt = g->device->createBuffer(RenderBufferDesc::DefaultBuffer(uint64_t(kGuestPages) * 4, storage));
  g->pt_dirty = g->mirror_pt != nullptr;
  if (!g->mirror_pt) return;  // resolve/detile descriptors require their shared page-table binding
  // Shared texture detile (optional shader-backed path).
  std::ifstream f(LibShaderPath(shader_dir, "edram_detile"), std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.empty()) return;
  g->detile_cs = g->device->createShader(bytes.data(), bytes.size(), "main", g->shader_format);
  RenderDescriptorSetDesc dset(kDetileRanges, 3);
  RenderPushConstantRange dpush(0, 0, 0, sizeof(DetileParams), RenderShaderStageFlag::COMPUTE);
  g->detile_layout = g->device->createPipelineLayout(RenderPipelineLayoutDesc(&dpush, 1, &dset, 1));
  if (!g->detile_cs || !g->detile_layout) return;
  g->detile_pipeline = g->device->createComputePipeline(
      RenderComputePipelineDesc(g->detile_layout.get(), g->detile_cs.get(), 64, 1, 1));
}

void FlushPageTable() {
  if (!g->pt_dirty) return;
  if (FillBuffer(g->mirror_pt.get(), uint64_t(kGuestPages) * 4, g->pt_cpu.data(), 0)) g->pt_dirty = false;
}

// Grows `buffer` to at least `bytes` (the old one is retired: earlier commands of the batch may use it).
bool EnsureBuffer(std::unique_ptr<RenderBuffer>& buffer, uint64_t& size, uint64_t bytes, bool readback) {
  if (buffer && size >= bytes) return true;
  uint64_t n = 1 << 20;
  while (n < bytes) n <<= 1;
  CountBuffer();
  auto b = readback ? g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(n))
                    : g->device->createBuffer(RenderBufferDesc::DefaultBuffer(
                          n, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
  if (!b) return false;
  Retire(std::move(buffer));
  buffer = std::move(b);
  size = n;
  return true;
}
uint64_t g_resolve_rb_size = 0;

// G5: take a retired depth output image from the existing wait-gated snapshot pool, or allocate and register one.
std::shared_ptr<RenderTexture> DepthLeanImage(uint32_t w, uint32_t h) {
  const State::SnapKey key{w, h, RenderFormat::R32_FLOAT, true};
  {
    RecordMutexLock lock(g->snap_mu, "snapshot_table_mutex");
    for (size_t i = g->snap_pool.size(); i-- > 0;) {
      if (g->snap_pool[i].k == key) {
        std::shared_ptr<RenderTexture> image = std::move(g->snap_pool[i].img);
        g->snap_pool_bytes -= g->snap_pool[i].bytes;
        g->snap_pool.erase(g->snap_pool.begin() + i);
        return image;
      }
    }
  }
  std::shared_ptr<RenderTexture> image(
      NewTexture(RenderTextureDesc::Texture2D(w, h, 1, RenderFormat::R32_FLOAT, RenderTextureFlag::STORAGE)));
  if (image) {
    RecordMutexLock lock(g->snap_mu, "snapshot_table_mutex");
    g->snap_keys[image.get()] = key;
  }
  return image;
}

RenderPipeline* DepthImagePipeline(const SurfaceObj& t) {
  return t.samples > 1 ? g->depth_img_pipeline.get() : g->depth_img_pipeline_1x.get();
}

bool DispatchDepthImage(SurfaceObj& t, const ResolveParams& ps, RenderPipeline* pipeline,
                        const std::shared_ptr<RenderTexture>& image, uint32_t w, uint32_t h,
                        const char* what, std::string* error) {
  if (!pipeline || !image) {
    *error = std::string(what) + ": image pipeline or output unavailable";
    return false;
  }
  FlushPageTable();
  RenderBufferStructuredView pview(4);
  if (g->depth_img_next == g->depth_img_sets.size()) {
    g->depth_img_sets.push_back(g->device->createDescriptorSet(RenderDescriptorSetDesc(kDepthImgRanges, 4)));
    if (!g->depth_img_sets.back()) g->depth_img_sets.pop_back();
  }
  if (g->depth_img_next >= g->depth_img_sets.size()) {
    *error = std::string(what) + ": descriptor set creation failed";
    return false;
  }
  RenderDescriptorSet* set = g->depth_img_sets[g->depth_img_next++].get();
  set->setTexture(0, t.image.get(), RenderTextureLayout::SHADER_READ, t.depth_view.get());
  set->setTexture(1, t.image.get(), RenderTextureLayout::SHADER_READ, t.stencil_view.get());
  set->setTexture(2, image.get(), RenderTextureLayout::GENERAL);
  set->setBuffer(3, g->mirror_pt.get(), uint64_t(kGuestPages) * 4, &pview);
  {
    BarrierBatch bb;
    bb.Layout(t.image.get(), RenderTextureLayout::SHADER_READ);
    bb.Layout(image.get(), RenderTextureLayout::GENERAL);
  }
  RenderCommandList* list = g->list;
  list->setPipeline(pipeline);
  list->setComputePipelineLayout(g->depth_img_layout.get());
  list->setComputeDescriptorSet(set, 0);
  list->setComputePushConstants(0, &ps, 0, sizeof(ps));
  list->dispatch((w + 7) / 8, (h + 7) / 8, 1);
  ResolveCensusCompute();
  Barrier(image.get(), RenderTextureLayout::SHADER_READ);
  return true;
}

// The reference route: uint bits in a staging buffer, then a tightly packed copy into an R32_FLOAT image.
std::shared_ptr<RenderTexture> DepthBufferImage(SurfaceObj& t, const ResolveParams& p, const ResolveParams& ps,
                                                RenderPipeline* pipeline, std::string* error) {
  std::shared_ptr<RenderTexture> image(NewTexture(RenderTextureDesc::Texture2D(p.w, p.h, 1, RenderFormat::R32_FLOAT)));
  const uint64_t bytes = uint64_t(p.w) * p.h * 4;
  CountBuffer();
  auto staging = g->device->createBuffer(
      RenderBufferDesc::DefaultBuffer(bytes, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
  if (!image || !staging) {
    *error = "depth r32f: image/staging creation failed";
    return nullptr;
  }
  PrimeComputeOutput(staging.get());
  FlushPageTable();
  RenderBufferStructuredView view(4);
  if (g->resolve_depth_next == g->resolve_depth_sets.size()) {
    g->resolve_depth_sets.push_back(g->device->createDescriptorSet(RenderDescriptorSetDesc(kResolveDepthRanges, 4)));
    if (!g->resolve_depth_sets.back()) {
      g->resolve_depth_sets.pop_back();
      *error = "depth r32f: descriptor set creation failed";
      return nullptr;
    }
  }
  RenderDescriptorSet* set = g->resolve_depth_sets[g->resolve_depth_next++].get();
  set->setTexture(0, t.image.get(), RenderTextureLayout::SHADER_READ, t.depth_view.get());
  set->setTexture(1, t.image.get(), RenderTextureLayout::SHADER_READ, t.stencil_view.get());
  set->setBuffer(2, staging.get(), bytes, &view);
  set->setBuffer(3, g->mirror_pt.get(), uint64_t(kGuestPages) * 4, &view);
  Barrier(t.image.get(), RenderTextureLayout::SHADER_READ);
  RenderCommandList* list = g->list;
  list->setPipeline(pipeline);
  list->setComputePipelineLayout(g->resolve_depth_layout.get());
  list->setComputeDescriptorSet(set, 0);
  list->setComputePushConstants(0, &ps, 0, sizeof(ps));
  list->dispatch((p.w + 7) / 8, (p.h + 7) / 8, 1);
  ResolveCensusCompute();
  {
    BarrierBatch bb;
    bb.Buffer(staging.get());
    bb.Layout(image.get(), RenderTextureLayout::COPY_DEST);
  }
  CopyBarrier(staging.get(), false);  // [wwe13 nr] D3D12 only
  if (!CopyTightToTexture(image.get(), 0, 0, staging.get(), 0, RenderFormat::R32_FLOAT, p.w, p.h, p.w, __LINE__)) {
    *error = "depth r32f: repitch buffer";
    return nullptr;
  }
  if (const char* dump0 = std::getenv("WWE2K14_NR_DEPTH_R32F_DUMP"); (dump0 && *dump0) || g_ddiag) {
    const char* dump = dump0 && *dump0 ? dump0 : nullptr;
    if (EnsureBuffer(g->resolve_rb, g_resolve_rb_size, bytes, true)) {
      CopyBarrier(staging.get(), false);  // [wwe13 nr] D3D12 only
      g->list->copyBufferRegion(RenderBufferReference(g->resolve_rb.get()), RenderBufferReference(staging.get(), 0), bytes);
      ResolveCensusCopy(bytes);
      Submit();
      ResolveCensusReadback(bytes);
      const uint8_t* src = static_cast<const uint8_t*>(g->resolve_rb->map());
      if (dump) {
        if (FILE* f = src ? std::fopen(dump, "wb") : nullptr) {
          std::fwrite(src, 1, bytes, f);
          std::fclose(f);
        }
      }
      if (g_ddiag && src) {
        const float* fv = reinterpret_cast<const float*>(src);
        const uint64_t n = uint64_t(p.w) * p.h;
        float mn = 3.4e38f, mx = -3.4e38f;
        uint64_t nz = 0;
        for (uint64_t i = 0; i < n; ++i) {
          const float v = fv[i];
          if (v != v) continue;
          mn = std::min(mn, v);
          mx = std::max(mx, v);
          if (v != 0.0f) ++nz;
        }
        const uint32_t sid = g_dd_cur_surface;
        const DdSurf ds = sid != 0xFFFFFFFFu ? g_dd_s[sid] : DdSurf{};
        const uint64_t seq = ++g_dd_seq;
        g_dd_img[image.get()] = DdRes{seq, sid, mn, mx, uint32_t(nz), uint32_t(n)};
        std::fprintf(stderr,
                     "[nr-ddiag] resolve seq=%llu surface=%u fmt=%s surf=%ux%u logical=%ux%u scale=%.2f samples=%u out=%ux%u x0=%u y0=%u "
                     "since_clear=%llu(draws_since_last_clear) last_clear_seq=%llu last_draw_seq=%llu total_draws=%llu total_clears=%llu "
                     "min=%.6g max=%.6g nonzero=%llu/%llu img=%p\n",
                     (unsigned long long)seq, sid, t.format == RenderFormat::D24_UNORM_S8_UINT ? "D24S8" : "D32S8", t.width,
                     t.height, t.lw, t.lh, double(t.scale), t.samples, p.w, p.h, p.x0, p.y0,
                     (unsigned long long)ds.draws_since_clear, (unsigned long long)ds.clear, (unsigned long long)ds.draw,
                     (unsigned long long)ds.draws, (unsigned long long)ds.clears, double(mn), double(mx),
                     (unsigned long long)nz, (unsigned long long)n, (const void*)image.get());
      }
      g->resolve_rb->unmap();
    }
  }
  Barrier(image.get(), RenderTextureLayout::SHADER_READ);
  Retire(std::move(staging));
  return image;
}

void DepthLeanCheckFailure(const char* reason) {
  ++g->depth_lean_check_failed;
  std::fprintf(stderr,
               "[d3d-depth-lean-check] compared=%llu equal=%llu different=%llu failed=%llu reason=%s\n",
               (unsigned long long)g->depth_lean_check_compared, (unsigned long long)g->depth_lean_check_equal,
               (unsigned long long)g->depth_lean_check_different, (unsigned long long)g->depth_lean_check_failed,
               reason);
}

std::shared_ptr<RenderTexture> DepthLeanCheckImage(SurfaceObj& t, const ResolveParams& p, const ResolveParams& ps,
                                                   RenderPipeline* pipeline, RenderPipeline* image_pipeline,
                                                   std::string* error) {
  std::shared_ptr<RenderTexture> reference = DepthBufferImage(t, p, ps, pipeline, error);
  if (!reference) return nullptr;
  const uint64_t bytes = uint64_t(p.w) * p.h * 4;
  std::shared_ptr<RenderTexture> candidate(
      NewTexture(RenderTextureDesc::Texture2D(p.w, p.h, 1, RenderFormat::R32_FLOAT, RenderTextureFlag::STORAGE)));
  auto ref_readback = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
  auto candidate_readback = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
  if (!candidate || !ref_readback || !candidate_readback || !image_pipeline) {
    DepthLeanCheckFailure("comparison resources or image pipeline unavailable");
    return reference;
  }
  std::string image_error;
  if (!DispatchDepthImage(t, ps, image_pipeline, candidate, p.w, p.h, "depth lean check", &image_error)) {
    DepthLeanCheckFailure(image_error.c_str());
    return reference;
  }
  Barrier(reference.get(), RenderTextureLayout::COPY_SOURCE);
  Barrier(candidate.get(), RenderTextureLayout::COPY_SOURCE);
  const RenderBox box(0, 0, int32_t(p.w), int32_t(p.h));
  const bool ref_copy = CopyTextureToTight(ref_readback.get(), 0, RenderFormat::R32_FLOAT, p.w, p.h, reference.get(), box);
  const bool candidate_copy =
      CopyTextureToTight(candidate_readback.get(), 0, RenderFormat::R32_FLOAT, p.w, p.h, candidate.get(), box);
  Barrier(reference.get(), RenderTextureLayout::SHADER_READ);
  Submit();  // readbacks are mapped only after the queue proves both copy-outs complete
  if (!ref_copy || !candidate_copy) {
    DepthLeanCheckFailure("texture readback copy failed");
    return reference;
  }
  const uint8_t* ref_bytes = static_cast<const uint8_t*>(ref_readback->map());
  const uint8_t* candidate_bytes = static_cast<const uint8_t*>(candidate_readback->map());
  const bool mapped = ref_bytes && candidate_bytes;
  bool equal = false;
  if (mapped) equal = std::memcmp(ref_bytes, candidate_bytes, size_t(bytes)) == 0;
  if (ref_bytes) ref_readback->unmap();
  if (candidate_bytes) candidate_readback->unmap();
  if (!mapped) {
    DepthLeanCheckFailure("readback map failed");
    return reference;
  }
  ++g->depth_lean_check_compared;
  if (equal) ++g->depth_lean_check_equal;
  else ++g->depth_lean_check_different;
  std::fprintf(stderr,
               "[d3d-depth-lean-check] compared=%llu equal=%llu different=%llu failed=%llu bytes=%llu\n",
               (unsigned long long)g->depth_lean_check_compared, (unsigned long long)g->depth_lean_check_equal,
               (unsigned long long)g->depth_lean_check_different, (unsigned long long)g->depth_lean_check_failed,
               (unsigned long long)bytes);
  return reference;
}

}  // namespace

bool ResolveAvailable() { return g && g->resolve_layout && g->resolve_lut; }

bool SetResolveLut(const std::vector<uint32_t>& lut, std::string* error) {
  if (BatchCall()) {
    uint64_t hash = BatchHashBytes(BatchHashWords("resolve_lut", {uint64_t(lut.size())}), lut.data(),
                                   lut.size() * sizeof(uint32_t));
    return QueueSyncCall(kBatchDrainResolveLut, hash, [=] { return SetResolveLut(lut, error); });
  }
  SyncRecordThread(kProfFnCount, false, kBatchDrainResolveLut);
  DpScope dp;  // card D1
  if (DpOn()) dp.Begin(DpOpLabel("resolve_lut"));
  if (g->state_elide) EndDrawPass();
  if (!ResolveAvailable() || lut.size() != kResolveLutEntries) {
    *error = "resolve LUT";
    return false;
  }
  if (!FillBuffer(g->resolve_lut.get(), kResolveLutEntries * 4, lut.data(), 0)) {
    *error = "resolve LUT: staging buffer";
    return false;
  }
  return true;
}

std::shared_ptr<RenderTexture> AcquireF16LeanImage(uint32_t w, uint32_t h, RenderFormat format,
                                                   RenderFramebuffer** framebuffer, std::string* error) {
  const State::SnapKey key{w, h, format};
  std::shared_ptr<RenderTexture> image;
  RecordMutexLock lock(g->snap_mu, "snapshot_table_mutex");
  for (size_t i = g->f16_lean_pool.size(); i-- > 0;) {
    if (g->f16_lean_pool[i].k == key) {
      image = std::move(g->f16_lean_pool[i].img);
      g->f16_lean_pool_bytes -= g->f16_lean_pool[i].bytes;
      g->f16_lean_pool.erase(g->f16_lean_pool.begin() + i);
      break;
    }
  }
  if (!image) {
    RenderTextureDesc lean_desc = RenderTextureDesc::Texture2D(w, h, 1, format, RenderTextureFlag::RENDER_TARGET);
    ApplyRtViewFormats(lean_desc, RenderFormat::UNKNOWN);  // a snapshot image: sampled, copied and drawn as itself only
    image = std::shared_ptr<RenderTexture>(NewTexture(lean_desc));
    if (!image) {
      *error = "f16 lean: pooled target image creation failed";
      return nullptr;
    }
    const RenderTexture* colors[1] = {image.get()};
    std::unique_ptr<RenderFramebuffer> fb = g->device->createFramebuffer(RenderFramebufferDesc(colors, 1));
    if (!fb) {
      *error = "f16 lean: target framebuffer creation failed";
      return nullptr;
    }
    g->f16_lean_keys.emplace(image.get(), key);
    g->f16_lean_fbs.emplace(image.get(), std::move(fb));
  }
  auto fit = g->f16_lean_fbs.find(image.get());
  if (fit == g->f16_lean_fbs.end() || !fit->second) {
    *error = "f16 lean: pooled image has no framebuffer";
    g->f16_lean_keys.erase(image.get());
    return nullptr;
  }
  *framebuffer = fit->second.get();
  return image;
}

void PoolF16LeanImage(std::shared_ptr<RenderTexture> image) {
  if (!image || image.use_count() != 1) return;
  RecordMutexLock lock(g->snap_mu, "snapshot_table_mutex");
  auto fit = g->f16_lean_keys.find(image.get());
  if (fit == g->f16_lean_keys.end()) return;
  State::F16PoolEntry e;
  e.k = fit->second;
  e.bytes = uint64_t(e.k.w) * e.k.h * RenderFormatSize(e.k.f);
  e.img = std::move(image);
  e.mem_last_use_present = g->mem_present;
  g->f16_lean_pool_bytes += e.bytes;
  g->f16_lean_pool.push_back(std::move(e));
  while (g->f16_lean_pool.size() > kSnapPoolMax || g->f16_lean_pool_bytes > SnapPoolMaxBytes()) {
    const RenderTexture* old = g->f16_lean_pool.front().img.get();
    g->f16_lean_pool_bytes -= g->f16_lean_pool.front().bytes;
    g->f16_lean_keys.erase(old);
    g->f16_lean_fbs.erase(old);
    g->f16_lean_pool.erase(g->f16_lean_pool.begin());
  }
}

bool CompareF16LeanImages(RenderTexture* lean, RenderTexture* legacy, uint32_t w, uint32_t h, RenderFormat format,
                          uint64_t sample, std::string* error) {
  if (g->state_elide) EndDrawPass();
  const uint64_t bytes = uint64_t(TightPitch(format, w)) * BlockRows(format, h);
  std::unique_ptr<RenderBuffer> lean_rb = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
  std::unique_ptr<RenderBuffer> legacy_rb = g->device->createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
  if (!bytes || !lean_rb || !legacy_rb) {
    Submit();  // the lean draw is already recorded; wait before CHECK recycles its output image
    *error = "f16 lean check: readback buffer allocation failed";
    ++g_f16_lean_check.readback_failed;
    return false;
  }
  Barrier(lean, RenderTextureLayout::COPY_SOURCE);
  Barrier(legacy, RenderTextureLayout::COPY_SOURCE);
  const RenderBox box(0, 0, int32_t(w), int32_t(h));
  const bool lean_copy = CopyTextureToTight(lean_rb.get(), 0, format, w, h, lean, box);
  const bool legacy_copy = CopyTextureToTight(legacy_rb.get(), 0, format, w, h, legacy, box);
  Barrier(lean, RenderTextureLayout::SHADER_READ);
  Barrier(legacy, RenderTextureLayout::SHADER_READ);
  Submit();  // both image copies complete before the buffers are mapped or released
  if (!lean_copy || !legacy_copy) {
    *error = "f16 lean check: image copy to readback failed";
    ++g_f16_lean_check.readback_failed;
    return false;
  }
  const uint8_t* lean_bytes = static_cast<const uint8_t*>(lean_rb->map());
  const uint8_t* legacy_bytes = static_cast<const uint8_t*>(legacy_rb->map());
  if (!lean_bytes || !legacy_bytes) {
    *error = "f16 lean check: readback map failed";
    ++g_f16_lean_check.readback_failed;
    if (lean_bytes) lean_rb->unmap();
    if (legacy_bytes) legacy_rb->unmap();
    return false;
  }
  ++g_f16_lean_check.compared;
  const bool equal = std::memcmp(lean_bytes, legacy_bytes, size_t(bytes)) == 0;
  if (equal) {
    ++g_f16_lean_check.equal;
  } else {
    ++g_f16_lean_check.different;
    if (!g_f16_lean_check.has_first_difference) {
      const uint32_t channels = format == RenderFormat::R16G16_UNORM ? 2u : 4u;
      const uint32_t count = w * h * channels;
      uint32_t component = 0;
      for (; component < count; ++component) {
        uint16_t a = 0, b = 0;
        std::memcpy(&a, legacy_bytes + component * 2, sizeof(a));
        std::memcpy(&b, lean_bytes + component * 2, sizeof(b));
        if (a != b) {
          g_f16_lean_check.has_first_difference = true;
          g_f16_lean_check.first_sample = sample;
          g_f16_lean_check.first_x = (component / channels) % w;
          g_f16_lean_check.first_y = (component / channels) / w;
          g_f16_lean_check.first_channel = component % channels;
          g_f16_lean_check.first_legacy = a;
          g_f16_lean_check.first_lean = b;
          break;
        }
      }
    }
  }
  lean_rb->unmap();
  legacy_rb->unmap();
  if (g_f16_lean_check.compared == 1 || (g_f16_lean_check.compared & 63u) == 0 || !equal) {
    if (g_f16_lean_check.has_first_difference) {
      std::fprintf(stderr,
                   "[nr-f16-lean-check] sample=%llu compared=%llu equal=%llu different=%llu readback_failed=%llu "
                   "first_diff_sample=%llu xy=%u,%u channel=%u legacy=%04X lean=%04X\n",
                   (unsigned long long)sample, (unsigned long long)g_f16_lean_check.compared,
                   (unsigned long long)g_f16_lean_check.equal, (unsigned long long)g_f16_lean_check.different,
                   (unsigned long long)g_f16_lean_check.readback_failed,
                   (unsigned long long)g_f16_lean_check.first_sample, g_f16_lean_check.first_x,
                   g_f16_lean_check.first_y, g_f16_lean_check.first_channel, g_f16_lean_check.first_legacy,
                   g_f16_lean_check.first_lean);
    } else {
      std::fprintf(stderr,
                   "[nr-f16-lean-check] sample=%llu compared=%llu equal=%llu different=%llu readback_failed=%llu "
                   "first_diff=none\n",
                   (unsigned long long)sample, (unsigned long long)g_f16_lean_check.compared,
                   (unsigned long long)g_f16_lean_check.equal, (unsigned long long)g_f16_lean_check.different,
                   (unsigned long long)g_f16_lean_check.readback_failed);
    }
  }
  return true;
}

bool DepthSnapshotAvailable() { return g && g->depth_r32f_pipeline && g->resolve_depth_layout; }

bool Fixed16SnapshotAvailable() {
  if (!g || !g->resolve_lut || !g->resolve_layout) return false;
  if (g->f16_lean_mode) {
    const bool lean_ready = g->f16_lean_layout && g->f16_lean_rg_pipeline && g->f16_lean_rgba_pipeline;
    return lean_ready && (g->f16_lean_mode != 2 || g->f16_linear_pipeline);
  }
  return bool(g->f16_linear_pipeline);
}

uint32_t SnapshotFixed16Lean(uint32_t surface, uint32_t x, uint32_t y, const ResolveParams& p, SurfaceObj& t,
                             RenderFormat format, bool check, uint64_t sample,
                             std::shared_ptr<RenderTexture> legacy_image, std::string* error) {
  const uint32_t w = p.w, h = p.h;
  RenderFramebuffer* fb = nullptr;
  std::shared_ptr<RenderTexture> image = AcquireF16LeanImage(w, h, format, &fb, error);
  if (!image) return kNone;
  if (g->f16_lean_next == g->f16_lean_sets.size()) {
    g->f16_lean_sets.push_back(g->device->createDescriptorSet(RenderDescriptorSetDesc(kF16LeanRanges, 2)));
    if (!g->f16_lean_sets.back()) {
      g->f16_lean_sets.pop_back();
      *error = "f16 lean: descriptor set creation failed";
      PoolF16LeanImage(std::move(image));
      return kNone;
    }
  }
  RenderDescriptorSet* set = g->f16_lean_sets[g->f16_lean_next++].get();
  RenderBufferStructuredView lut_view(4);
  set->setBuffer(1, g->resolve_lut.get(), kResolveLutEntries * 4, &lut_view);

  // Preserve the driver's resolve operation and its averaging before reading the source in the fragment shader.
  RenderTexture* source = t.image.get();
  if (t.samples > 1) {
    {
      BarrierBatch bb;
      bb.Layout(t.image.get(), RenderTextureLayout::RESOLVE_SOURCE);
      bb.Layout(t.resolved.get(), RenderTextureLayout::RESOLVE_DEST);
    }
    const int32_t rx0 = PhysEdge(t, int32_t(x), false), ry0 = PhysEdge(t, int32_t(y), true);
    RenderRect r(rx0, ry0, std::max(PhysEdge(t, int32_t(x + w), false), rx0 + 1),
                 std::max(PhysEdge(t, int32_t(y + h), true), ry0 + 1));
    g->list->resolveTextureRegion(t.resolved.get(), uint32_t(rx0), uint32_t(ry0), t.image.get(), &r);
    ResolveCensusMsaaResolve(uint64_t(TightPitch(t.format, uint32_t(r.right - r.left))) *
                             BlockRows(t.format, uint32_t(r.bottom - r.top)));
    source = t.resolved.get();
  }
  set->setTexture(0, source, RenderTextureLayout::SHADER_READ);
  ResolveParams q = p;
  q.x0 = x;
  q.y0 = y;
  q.flags = 0;
  if (t.scale != 1.0f) {
    q.flags |= 0x400u;
    q.dx = uint32_t(std::lround(double(t.scale) * 65536.0));
    q.dy = (t.width - 1) | ((t.height - 1) << 16);
  }
  {
    BarrierBatch bb;
    bb.Layout(source, RenderTextureLayout::SHADER_READ);
    bb.Write(image.get(), RenderTextureLayout::COLOR_WRITE);
  }
  RenderCommandList* l = g->list;
  l->setFramebuffer(fb);
  l->setPipeline(p.src_bpp == 8 ? g->f16_lean_rgba_pipeline.get() : g->f16_lean_rg_pipeline.get());
  l->setGraphicsPipelineLayout(g->f16_lean_layout.get());
  l->setGraphicsDescriptorSet(set, 0);
  l->setGraphicsPushConstants(0, &q, 0, sizeof(q));
  const RenderViewport viewport(0.0f, 0.0f, float(w), float(h));
  const RenderRect scissor(0, 0, int32_t(w), int32_t(h));
  l->setViewports(viewport);
  l->setScissors(scissor);
  l->drawInstanced(3, 1, 0, 0);
  l->setFramebuffer(nullptr);
  if (g->state_elide) {
    Elide().fb = nullptr;
    InvalidateElideBinds();
  }
  Barrier(image.get(), RenderTextureLayout::SHADER_READ);

  if (check) {
    std::string check_error;
    if (!CompareF16LeanImages(image.get(), legacy_image.get(), w, h, format, sample, &check_error))
      std::fprintf(stderr, "[nr-f16-lean-check] sample=%llu readback failed: %s\n",
                   (unsigned long long)sample, check_error.c_str());
    PoolF16LeanImage(std::move(image));  // CHECK's output is not a snapshot; Submit/readback above made reuse safe.
    return legacy_image ? NewSnapshotId(std::move(legacy_image), format) : kNone;
  }
  return NewSnapshotId(std::move(image), format);
}

// Card P16: a fixed16 colour copy-out (p.flags & 2, src_bpp 4 or 8) as the texel values a k_16_16(_16_16) fetch of
// the copy reads after detiling: the source rectangle (4x: host average) -> tight buffer -> the
// resolve LUT conversion written linearly (edram_resolve_color_linear.spv) -> a new R16G16(B16A16)_UNORM image,
// returned as a snapshot id. kNone on failure (the caller keeps the guest-byte path).
uint32_t SnapshotFixed16(uint32_t surface, uint32_t x, uint32_t y, const ResolveParams& p, std::string* error) {
  if (AsyncCall()) return QueueSnapshotCall("snapshot fixed16",
                                            BatchMixWords(BatchHashWords("snapshot_fixed16", {surface, x, y}),
                                                          {BatchHashResolveParams(p)}),
                                            [=](std::string* e) { return SnapshotFixed16(surface, x, y, p, e); });
  ProfScope prof_scope(kProfResolve);
  ItemScope item("f16-image", p.dst_start, p.w, p.h, p.flags);
  if (g->state_elide) EndDrawPass();
  if (!Fixed16SnapshotAvailable() || !LiveSurface(surface) || g->surfaces[surface].depth || !p.w || !p.h ||
      !(p.flags & 2u) || (p.flags & 5u) || (p.src_bpp != 4 && p.src_bpp != 8) || p.bpp_log2 != (p.src_bpp == 8 ? 3u : 2u)) {
    *error = "f16 image: bad surface or params";
    return kNone;
  }
  SurfaceObj& t = g->surfaces[surface];
  TouchSurface(surface);
  const uint32_t w = p.w, h = p.h;
  if (!RectInSurface(t, x, y, w, h, error)) return kNone;
  if (g->shader_format == RenderShaderFormat::DXIL && !g->f16_tex_pipeline && t.scale != 1.0f) {
    *error = "f16 image: scaled surface needs WWE2K14_D3D_F16_FUSED=1 (card S3-31)";
    return kNone;
  }
  const RenderFormat iformat = p.src_bpp == 8 ? RenderFormat::R16G16B16A16_UNORM : RenderFormat::R16G16_UNORM;
  if (CopyCeilingOn(2u)) {  // ceiling experiment: no MSAA average / LUT conversion
    std::shared_ptr<RenderTexture> image = CeilingImage(w, h, iformat);
    return image ? NewSnapshotId(std::move(image), iformat) : kNone;
  }
  if (g->f16_lean_mode) {
    bool check = false;
    uint64_t sample = 0;
    std::shared_ptr<RenderTexture> legacy_image;
    if (g->f16_lean_mode == 2) {
      check = (g->f16_lean_check_ordinal++ & 15u) == 0;
      if (check) {
        sample = ++g_f16_lean_check.sampled;
        const int saved_mode = g->f16_lean_mode;
        g->f16_lean_mode = 0;
        const uint32_t legacy_id = SnapshotFixed16(surface, x, y, p, error);
        g->f16_lean_mode = saved_mode;
        if (legacy_id == kNone || legacy_id >= g->snapshots.size() || !g->snapshots[legacy_id]) {
          ++g_f16_lean_check.readback_failed;
          std::fprintf(stderr, "[nr-f16-lean-check] sample=%llu legacy path failed: %s\n",
                       (unsigned long long)sample, error->c_str());
          if (legacy_id != kNone) ReleaseSnapshotNow(legacy_id);
          return kNone;
        }
        legacy_image = g->snapshots[legacy_id];
        ReleaseSnapshotNow(legacy_id);  // hold only the image for the comparison; it is not exposed as a snapshot
      }
    }
    return SnapshotFixed16Lean(surface, x, y, p, t, iformat, check, sample, std::move(legacy_image), error);
  }
  const uint64_t bytes = uint64_t(w) * h * p.src_bpp;
  std::shared_ptr<RenderTexture> image(NewTexture(RenderTextureDesc::Texture2D(w, h, 1, iformat)));
  CountBuffer();
  auto staging = g->device->createBuffer(
      RenderBufferDesc::DefaultBuffer(bytes, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
  if (!image || !staging || !EnsureBuffer(g->f16_src, g->f16_src_size, bytes, false)) {
    *error = "f16 image: image/buffer creation failed";
    return kNone;
  }
  PrimeComputeOutput(staging.get());
  // Source texels from the same host-image surface used by D3D reads.
  RenderTexture* source = t.image.get();
  if (t.samples > 1) {
    {
      BarrierBatch bb;  // k14 P22: batched (precise barriers only)
      bb.Layout(t.image.get(), RenderTextureLayout::RESOLVE_SOURCE);
      bb.Layout(t.resolved.get(), RenderTextureLayout::RESOLVE_DEST);
    }
    // Card S3-31: a scaled surface resolves its physical rect (same coordinates in t.resolved).
    const int32_t rx0 = PhysEdge(t, int32_t(x), false), ry0 = PhysEdge(t, int32_t(y), true);
    RenderRect r(rx0, ry0, std::max(PhysEdge(t, int32_t(x + w), false), rx0 + 1),
                 std::max(PhysEdge(t, int32_t(y + h), true), ry0 + 1));
    g->list->resolveTextureRegion(t.resolved.get(), uint32_t(rx0), uint32_t(ry0), t.image.get(), &r);
    ResolveCensusMsaaResolve(uint64_t(TightPitch(t.format, uint32_t(r.right - r.left))) *
                             BlockRows(t.format, uint32_t(r.bottom - r.top)));
    source = t.resolved.get();
  }
  if (g->f16_tex_pipeline) {
    // Card S3-23: the LUT conversion reads the source rectangle straight from the image (as LEVER3's SRC_TEXTURE: SNORM
    // texel -> int16 code, exact) and writes the staging rows; the image -> buffer copy and its barrier are gone.
    ResolveParams q = p;
    q.x0 = x;
    q.y0 = y;
    q.flags = (p.flags & ~0x300u) | ((p.src_bpp == 8 ? 1u : 2u) << 8);
    if (t.scale != 1.0f) {  // card S3-31: nearest physical texel of each logical one (resolve.hlsl LINEAR_OUT)
      q.flags |= 0x400u;
      q.dx = uint32_t(std::lround(double(t.scale) * 65536.0));
      q.dy = (t.width - 1) | ((t.height - 1) << 16);
    }
    FlushPageTable();
    RenderBufferStructuredView view(4);
    if (g->resolve_tex_next == g->resolve_tex_sets.size()) {
      g->resolve_tex_sets.push_back(g->device->createDescriptorSet(RenderDescriptorSetDesc(kResolveTexRanges, 4)));
      if (!g->resolve_tex_sets.back()) {
        g->resolve_tex_sets.pop_back();
        *error = "f16 image: descriptor set creation failed";
        return kNone;
      }
    }
    RenderDescriptorSet* set = g->resolve_tex_sets[g->resolve_tex_next++].get();
    set->setTexture(0, source, RenderTextureLayout::SHADER_READ);
    set->setBuffer(1, staging.get(), bytes, &view);
    set->setBuffer(2, g->resolve_lut.get(), kResolveLutEntries * 4, &view);
    set->setBuffer(3, g->mirror_pt.get(), uint64_t(kGuestPages) * 4, &view);
    Barrier(source, RenderTextureLayout::SHADER_READ);
    RenderCommandList* l = g->list;
    l->setPipeline(g->f16_tex_pipeline.get());
    l->setComputePipelineLayout(g->f16_tex_layout.get());
    l->setComputeDescriptorSet(set, 0);
    l->setComputePushConstants(0, &q, 0, sizeof(q));
    l->dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ResolveCensusCompute();
  } else {
  if (t.scale != 1.0f) {
    *error = "f16 image: scaled surface needs WWE2K14_D3D_F16_FUSED=1 (card S3-31)";
    return kNone;
  }
  {
    BarrierBatch bb;  // k14 P22: COPY_SOURCE + destination buffer batched (precise barriers only)
    bb.Layout(source, RenderTextureLayout::COPY_SOURCE);
    bb.Buffer(g->f16_src.get());
  }
  CopyBarrier(g->f16_src.get(), true);  // [wwe13 nr] D3D12 only
  RenderBox box(int32_t(x), int32_t(y), int32_t(x + w), int32_t(y + h));
  if (!CopyTextureToTight(g->f16_src.get(), 0, t.format, w, h, source, box)) {
    *error = "f16 image: repitch buffer";
    return kNone;
  }
  CopyDone(g->f16_src.get());
  FlushPageTable();
  RenderBufferStructuredView view(4);
  if (g->resolve_next == g->resolve_sets.size()) {
    g->resolve_sets.push_back(g->device->createDescriptorSet(RenderDescriptorSetDesc(kResolveRanges, 4)));
    if (!g->resolve_sets.back()) {
      g->resolve_sets.pop_back();
      *error = "f16 image: descriptor set creation failed";
      return kNone;
    }
  }
  RenderDescriptorSet* set = g->resolve_sets[g->resolve_next++].get();
  set->setBuffer(0, g->f16_src.get(), g->f16_src_size, &view);
  set->setBuffer(1, staging.get(), bytes, &view);
  set->setBuffer(2, g->resolve_lut.get(), kResolveLutEntries * 4, &view);
  set->setBuffer(3, g->mirror_pt.get(), uint64_t(kGuestPages) * 4, &view);
  BufferBarrier(g->f16_src.get());
  RenderCommandList* l = g->list;
  l->setPipeline(g->f16_linear_pipeline.get());
  l->setComputePipelineLayout(g->resolve_layout.get());
  l->setComputeDescriptorSet(set, 0);
  l->setComputePushConstants(0, &p, 0, sizeof(p));
  l->dispatch((w + 7) / 8, (h + 7) / 8, 1);
  ResolveCensusCompute();
  }  // card S3-23 else
  {
    BarrierBatch bb;  // k14 P22: batched (precise barriers only)
    bb.Buffer(staging.get());
    bb.Layout(image.get(), RenderTextureLayout::COPY_DEST);
  }
  CopyBarrier(staging.get(), false);  // [wwe13 nr] D3D12 only
  if (!CopyTightToTexture(image.get(), 0, 0, staging.get(), 0, iformat, w, h, w, __LINE__)) {
    *error = "f16 image: repitch buffer";
    return kNone;
  }
  Barrier(image.get(), RenderTextureLayout::SHADER_READ);
  Retire(std::move(staging));
  return NewSnapshotId(std::move(image), iformat);
}

// LEVER3B: the float24 depth of a flat depth surface as the R32F texel values a k_24_8_FLOAT fetch of its depth
// copy-out would read (see depth_to_r32f.hlsl): compute into a staging buffer, copy to a new R32F image, which
// becomes a snapshot id (format R32_FLOAT; TextureFromSnapshot / ReleaseSnapshot). `p` = the ResolveParams of the
// resolve (w, h, x0, y0, select, ms used). kNone on failure (the caller keeps the mirror path).
uint32_t SnapshotDepthR32F(uint32_t surface, const ResolveParams& p, std::string* error) {
  if (AsyncCall()) return QueueSnapshotCall("snapshot depth",
                                            BatchMixWords(BatchHashWords("snapshot_depth", {surface}),
                                                          {BatchHashResolveParams(p)}),
                                            [=](std::string* e) { return SnapshotDepthR32F(surface, p, e); });
  ProfScope prof_scope(kProfResolve);
  ItemScope item("depth-r32f", p.dst_start, p.w, p.h, 0);
  if (g->state_elide) EndDrawPass();
  if (!g->resolve_depth_layout || !LiveSurface(surface) || !g->surfaces[surface].depth || !p.w || !p.h) {
    *error = "depth r32f: bad surface";
    return kNone;
  }
  if (CopyCeilingOn(4u)) {  // ceiling experiment: no depth -> R32F compute
    std::shared_ptr<RenderTexture> image = CeilingImage(p.w, p.h, RenderFormat::R32_FLOAT);
    return image ? NewSnapshotId(std::move(image), RenderFormat::R32_FLOAT) : kNone;
  }
  SurfaceObj& t = g->surfaces[surface];
  TouchSurface(surface);
  g_dd_cur_surface = surface;
  RenderPipeline* pipeline = t.samples > 1 ? g->depth_r32f_pipeline.get() : g->depth_r32f_pipeline_1x.get();
  RenderPipeline* image_pipeline = DepthImagePipeline(t);
  // Card S3-31: a scaled depth surface is read at the nearest physical texel of each logical one (flags 0x400; dx =
  // scale in 16.16, dy = last physical x | last physical y << 16); the output stays logical-size.
  ResolveParams ps = p;
  if (t.scale != 1.0f) {
    ps.flags |= 0x400u;
    ps.dx = uint32_t(std::lround(double(t.scale) * 65536.0));
    ps.dy = (t.width - 1) | ((t.height - 1) << 16);
  }
  if (!pipeline) {
    *error = "depth r32f: no shader for this sample count";
    return kNone;
  }
  if (!RectInSurface(t, 0, 0, p.w, p.h, error)) return kNone;
  const bool phys_depth = g_phys_snap_on && !g->depth_lean_check && t.scale < 1.0f && p.x0 == 0 && p.y0 == 0 &&
                          p.w == t.lw && p.h <= t.lh;
  if (phys_depth) {
    const int32_t px0 = PhysEdge(t, int32_t(p.x0), false), py0 = PhysEdge(t, int32_t(p.y0), true);
    const int32_t px1 = std::max(PhysEdge(t, int32_t(p.x0 + p.w), false), px0 + 1);
    const int32_t py1 = std::max(PhysEdge(t, int32_t(p.y0 + p.h), true), py0 + 1);
    const uint32_t pw = uint32_t(px1 - px0), ph = uint32_t(py1 - py0);
    if (pw && ph) {
      ResolveParams physical_p = p;
      physical_p.w = pw;
      physical_p.h = ph;
      physical_p.x0 = uint32_t(px0);
      physical_p.y0 = uint32_t(py0);
      ResolveParams physical_ps = physical_p;
      physical_ps.flags &= ~0x400u;  // output texels address the physical depth surface directly

      auto render_depth = [&](const ResolveParams& output, const ResolveParams& shader,
                              std::string* render_error) -> std::shared_ptr<RenderTexture> {
        if (g->depth_lean_on) {
          if (!image_pipeline) {
            *render_error = "depth lean: storage-image pipeline unavailable";
            return nullptr;
          }
          std::shared_ptr<RenderTexture> result = DepthLeanImage(output.w, output.h);
          if (!result || !DispatchDepthImage(t, shader, image_pipeline, result, output.w, output.h,
                                             "depth lean", render_error)) {
            if (render_error->empty()) *render_error = "depth lean: output image creation failed";
            return nullptr;
          }
          return result;
        }
        if (image_pipeline) {
          std::shared_ptr<RenderTexture> result(NewTexture(RenderTextureDesc::Texture2D(
              output.w, output.h, 1, RenderFormat::R32_FLOAT, RenderTextureFlag::STORAGE)));
          std::string image_error;
          if (result && DispatchDepthImage(t, shader, image_pipeline, result, output.w, output.h, "depth image",
                                           &image_error))
            return result;
        }
        return DepthBufferImage(t, output, shader, pipeline, render_error);
      };

      std::shared_ptr<RenderTexture> logical, physical;
      if (g_phys_snap_check) {
        logical = render_depth(p, ps, error);
        if (!logical) return kNone;
      }
      std::string physical_error;
      physical = render_depth(physical_p, physical_ps, &physical_error);
      if (!physical) {
        if (g_phys_snap_check) {
          ++g_stats.phys_snap_check_failures;
          return NewSnapshotId(std::move(logical), RenderFormat::R32_FLOAT);
        }
        *error = physical_error.empty() ? "depth physical: output creation failed" : physical_error;
        return kNone;
      }
      State::SnapHi hi;
      hi.image = physical;
      hi.width = pw;
      hi.height = ph;
      hi.upscale = false;
      hi.logical_width = p.w;
      hi.logical_height = p.h;
      hi.logical_pending = !g_phys_snap_check;
      ++g_stats.phys_snap_depth;
      if (g_phys_snap_check) {
        ComparePhysicalSnapshot(logical, physical, p.w, p.h, pw, ph, RenderFormat::R32_FLOAT, false);
        return NewSnapshotId(std::move(logical), RenderFormat::R32_FLOAT, std::move(hi));
      }
      return NewSnapshotId(std::move(physical), RenderFormat::R32_FLOAT, std::move(hi));
    }
  }
  if (g->depth_lean_on) {
    if (!image_pipeline) {
      *error = "depth lean: storage-image pipeline unavailable";
      return kNone;
    }
    std::shared_ptr<RenderTexture> image = DepthLeanImage(p.w, p.h);
    if (!image || !DispatchDepthImage(t, ps, image_pipeline, image, p.w, p.h, "depth lean", error)) {
      if (error->empty()) *error = "depth lean: output image creation failed";
      return kNone;
    }
    return NewSnapshotId(std::move(image), RenderFormat::R32_FLOAT);
  }
  if (g->depth_lean_check && ((g->depth_lean_check_seq++ & 15u) == 0)) {
    std::shared_ptr<RenderTexture> image =
        DepthLeanCheckImage(t, p, ps, pipeline, image_pipeline, error);
    return image ? NewSnapshotId(std::move(image), RenderFormat::R32_FLOAT) : kNone;
  }
  // Card S3-38 remains independently opt-in. G5 check mode always returns the default buffer path.
  if (!g->depth_lean_check && image_pipeline) {
    std::shared_ptr<RenderTexture> image(
        NewTexture(RenderTextureDesc::Texture2D(p.w, p.h, 1, RenderFormat::R32_FLOAT, RenderTextureFlag::STORAGE)));
    std::string image_error;
    if (image && DispatchDepthImage(t, ps, image_pipeline, image, p.w, p.h, "depth image", &image_error)) {
      return NewSnapshotId(std::move(image), RenderFormat::R32_FLOAT);
    }
  }
  std::shared_ptr<RenderTexture> image = DepthBufferImage(t, p, ps, pipeline, error);
  return image ? NewSnapshotId(std::move(image), RenderFormat::R32_FLOAT) : kNone;
}

bool DetileAvailable() { return g && g->detile_pipeline; }

bool DetileDepth24Available() {
  if (!DetileAvailable()) return false;
  std::ifstream f(g->shader_dir + "/edram_detile.depth24", std::ios::binary);
  return f.good();
}

// Card S3-8: submit everything recorded so far; returns the serial the GPU must reach (0 = nothing to wait for).
uint64_t KickForSync() {
  if (BatchCall()) return QueueSyncCall(kBatchDrainKick, BatchHashWords("kick_for_sync", {}), [] { return KickForSync(); });
  if (!g || !g->list) return 0;
  SyncRecordThread(kProfFnCount, false, kBatchDrainKick);
  if (g->state_elide) EndDrawPass();
  Kick();
  return g->submit_serial;
}
// Card S3-8: non-blocking check that every list up to `serial` has finished; lists found signalled are retired exactly as
// WaitSerial would (their fence wait returns at once). Caller holds the backend lock (briefly).
bool GpuReachedSerial(uint64_t serial) {
  if (!g) return true;
  bool all = true;
  for (CmdEntry& e : g->cmd_pool) {
    if (!e.in_flight || e.serial > serial) continue;
    if (g->queue->k14IsCommandFenceSignalled(e.fence.get())) {
      g->queue->waitForCommandFence(e.fence.get());  // signalled: returns at once and resets the fence
      e.in_flight = false;
      GpuHarvest(e);
    } else {
      all = false;
    }
  }
  return all;
}
// Card S3-8: hand everything recorded so far to the GPU and wait until it has finished (no ring reset).
void FinishGpuWork() {
  if (BatchCall()) return QueueSyncCall(kBatchDrainFinish, BatchHashWords("finish_gpu_work", {}), [] { FinishGpuWork(); });
  if (!g || !g->list) return;
  SyncRecordThread(kProfFnCount, false, kBatchDrainFinish);
  if (g->state_elide) EndDrawPass();
  Kick();
  WaitSerial(g->submit_serial);
}
bool DetileSnapshotGpu(uint32_t snapshot, const GpuLevel& level, uint32_t source_bytes, const uint8_t* source,
                       std::vector<uint8_t>* dump_level, SeedDetileStats* stats, std::string* error) {
  if (BatchCall()) {
    const GpuLevel& lv = level;
    uint64_t hash = BatchHashWords("detile_snapshot_gpu", {snapshot, lv.width, lv.height, lv.mip, lv.face,
                                                           source_bytes, lv.p.src_base, lv.p.row_pitch,
                                                           lv.p.pitch_blocks, lv.p.off_x, lv.p.off_y, lv.p.bw,
                                                           lv.p.bh, lv.p.bpb_log2, lv.p.tiled, lv.p.endian,
                                                           lv.p.mode, lv.p.out_offset, lv.p.out_size,
                                                           uint64_t(dump_level != nullptr)});
    hash = BatchHashBytes(hash, source, source_bytes);
    return QueueSyncCall(kBatchDrainDetile, hash,
                         [=] { return DetileSnapshotGpu(snapshot, level, source_bytes, source, dump_level, stats, error); });
  }
  ProfScope prof_scope(kProfSeedDetile);
  if (stats) *stats = SeedDetileStats{};
  if (g->state_elide) EndDrawPass();
  if (!EnsureLogicalSnap(snapshot, error)) return false;
  if (!DetileAvailable() || snapshot >= g->snapshots.size() || !g->snapshots[snapshot] || !source || !source_bytes ||
      !level.width || !level.height || !level.p.out_size || level.p.out_offset != 0) {
    *error = "seed detile: unavailable or invalid input";
    return false;
  }
  if (CopyCeilingOn(8u)) return true;  // ceiling experiment: canvas left unseeded
  if ((level.p.mode == 2 || level.p.mode == 3) && !DetileDepth24Available()) {
    *error = "seed detile: depth24 shader unavailable";
    return false;
  }
  const uint64_t source_end = uint64_t(level.p.src_base) + source_bytes;
  if (source_end > uint64_t(kGuestPages) * 4096 || source_end <= level.p.src_base) {
    *error = "seed detile: source range outside guest physical memory";
    return false;
  }
  const uint32_t first_page = level.p.src_base >> 12;
  const uint32_t source_offset = level.p.src_base & 4095;
  const uint64_t page_bytes = (uint64_t(source_offset) + source_bytes + 4095) & ~uint64_t(4095);
  const uint32_t page_count = uint32_t(page_bytes >> 12);
  const uint64_t mapping_bytes = uint64_t(page_count) * sizeof(uint32_t);
  const uint64_t padded_upload_bytes = uint64_t(page_count) * 4096;
  if (!page_count || first_page + page_count > kGuestPages || page_bytes != padded_upload_bytes) {
    *error = "seed detile: invalid page span";
    return false;
  }
  const RenderFormat format = g->snapshot_formats[snapshot];
  const uint64_t expected_bytes = uint64_t(level.width) * level.height * RenderFormatSize(format);
  if (expected_bytes != level.p.out_size || expected_bytes > UINT32_MAX) {
    *error = "seed detile: snapshot format/size mismatch";
    return false;
  }
  if (!EnsureBuffer(g->dest_seed_pool, g->dest_seed_pool_size, padded_upload_bytes, false) ||
      !EnsureBuffer(g->dest_seed_out, g->dest_seed_out_size, expected_bytes, false)) {
    *error = "seed detile: scratch buffer allocation";
    return false;
  }
  if (!g->dest_seed_pt) {
    CountBuffer();
    g->dest_seed_pt = g->device->createBuffer(RenderBufferDesc::DefaultBuffer(
        uint64_t(kGuestPages) * 4, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
    if (!g->dest_seed_pt) {
      *error = "seed detile: page table allocation";
      return false;
    }
  }

  const uint64_t upload_start = NowNs();
  CountBuffer();
  if (g_prof) g_stats.upload_bytes += padded_upload_bytes + mapping_bytes;
  auto input = g->device->createBuffer(RenderBufferDesc::UploadBuffer(padded_upload_bytes + mapping_bytes));
  uint8_t* mapped = input ? static_cast<uint8_t*>(input->map()) : nullptr;
  if (!mapped) {
    *error = "seed detile: source upload staging";
    return false;
  }
  std::memset(mapped, 0, size_t(padded_upload_bytes));  // pad the last page without reading beyond the canvas range
  std::memcpy(mapped + source_offset, source, source_bytes);
  uint32_t* slots = reinterpret_cast<uint32_t*>(mapped + padded_upload_bytes);
  for (uint32_t i = 0; i < page_count; ++i) slots[i] = i;
  input->unmap();

  BufferBarrier(g->dest_seed_pool.get());
  CopyBarrier(g->dest_seed_pool.get(), true);
  g->list->copyBufferRegion(RenderBufferReference(g->dest_seed_pool.get()), RenderBufferReference(input.get()),
                            padded_upload_bytes);
  ResolveCensusCopy(padded_upload_bytes);
  BufferBarrier(g->dest_seed_pool.get());
  BufferBarrier(g->dest_seed_pt.get());
  CopyBarrier(g->dest_seed_pt.get(), true);
  g->list->copyBufferRegion(RenderBufferReference(g->dest_seed_pt.get(), uint64_t(first_page) * 4),
                            RenderBufferReference(input.get(), padded_upload_bytes), mapping_bytes);
  ResolveCensusCopy(mapping_bytes);
  BufferBarrier(g->dest_seed_pt.get());
  Retire(std::move(input));
  if (stats) {
    stats->upload_ns = NowNs() - upload_start;
    stats->source_bytes = source_bytes;
    stats->padded_upload_bytes = padded_upload_bytes;
  }

  if (g->detile_next == g->detile_sets.size()) {
    g->detile_sets.push_back(g->device->createDescriptorSet(RenderDescriptorSetDesc(kDetileRanges, 3)));
    if (!g->detile_sets.back()) {
      *error = "seed detile: descriptor allocation";
      return false;
    }
  }
  RenderDescriptorSet* set = g->detile_sets[g->detile_next++].get();
  RenderBufferStructuredView view(4);
  set->setBuffer(0, g->dest_seed_pool.get(), g->dest_seed_pool_size, &view);
  set->setBuffer(1, g->dest_seed_pt.get(), uint64_t(kGuestPages) * 4, &view);
  set->setBuffer(2, g->dest_seed_out.get(), g->dest_seed_out_size, &view);
  BufferBarrier(g->dest_seed_pool.get());
  BufferBarrier(g->dest_seed_pt.get());
  BufferBarrier(g->dest_seed_out.get());
  RenderCommandList* l = g->list;
  l->setPipeline(g->detile_pipeline.get());
  l->setComputePipelineLayout(g->detile_layout.get());
  l->setComputeDescriptorSet(set, 0);
  const uint32_t items = level.p.mode == 0 ? (level.p.out_size + 3) / 4 : level.p.bw * level.p.bh;
  const uint64_t dispatch_start = NowNs();
  {
    ItemScope gpu_item("dest-seed-detile", level.p.src_base, level.width, level.height, level.p.mode);
    l->setComputePushConstants(0, &level.p, 0, sizeof(level.p));
    if (items) {
      l->dispatch((items + 63) / 64, 1, 1);
      ResolveCensusCompute();
    }
  }
  if (stats) stats->dispatch_submit_ns = NowNs() - dispatch_start;
  BufferBarrier(g->dest_seed_out.get());

  if (dump_level) {
    if (!EnsureBuffer(g->resolve_rb, g_resolve_rb_size, expected_bytes, true)) {
      *error = "seed detile: readback buffer allocation";
      return false;
    }
    CopyBarrier(g->dest_seed_out.get(), false);
    g->list->copyBufferRegion(RenderBufferReference(g->resolve_rb.get()), RenderBufferReference(g->dest_seed_out.get()),
                              expected_bytes);
    ResolveCensusCopy(expected_bytes);
    Submit();
    if (g_prof) g_stats.readback_bytes += expected_bytes;
    const uint8_t* result = static_cast<const uint8_t*>(g->resolve_rb->map());
    if (!result) {
      *error = "seed detile: readback map";
      return false;
    }
    dump_level->assign(result, result + expected_bytes);
    g->resolve_rb->unmap();
    ResolveCensusReadback(expected_bytes);
  }

  RenderTexture* image = g->snapshots[snapshot].get();
  Barrier(image, RenderTextureLayout::COPY_DEST);
  CopyBarrier(g->dest_seed_out.get(), false);
  if (!CopyTightToTexture(image, 0, 0, g->dest_seed_out.get(), 0, format, level.width, level.height, level.width,
                          __LINE__)) {
    *error = "seed detile: copy into canvas failed";
    return false;
  }
  Barrier(image, RenderTextureLayout::SHADER_READ);
  return RefreshPhysicalSnapshotDrawImage(snapshot, error);
}

// Card S3-57: background prewarm of draw-pipeline keys read at startup. Creates shader modules and pipelines through
// the device only (no descriptor, command or memory work); with adoption on, shader objects use the live shader cache.
namespace {
struct RecordReader {
  const std::string& r;
  size_t at = 0;
  bool Get(void* out, size_t n) {
    if (r.size() - at < n) return false;
    std::memcpy(out, r.data() + at, n);
    at += n;
    return true;
  }
  uint32_t U32(bool* ok) {
    uint32_t v = 0;
    if (!Get(&v, sizeof(v))) *ok = false;
    return v;
  }
};

void PrewarmPipelines(std::vector<std::string> records, std::vector<const std::vector<uint8_t>*> spirv) {
  const uint64_t t0 = NowNs();
  std::unordered_map<uint64_t, const std::vector<uint8_t>*> by_hash;
  for (const std::vector<uint8_t>* v : spirv) {
    if (v && !v->empty()) by_hash.emplace(XXH3_64bits(v->data(), v->size()), v);
  }
  std::unordered_map<uint64_t, std::unique_ptr<RenderShader>> legacy_shaders;
  auto shader = [&](uint64_t key, uint64_t h) -> RenderShader* {
    auto bytes = by_hash.find(h);
    if (bytes == by_hash.end()) return nullptr;
    if (g_prewarm_adopt) return Shader(key, bytes->second);
    auto it = legacy_shaders.find(h);
    if (it != legacy_shaders.end()) return it->second.get();
    auto created = g->device->createShader(bytes->second->data(), bytes->second->size(), "main", g->shader_format);
    return (legacy_shaders[h] = std::move(created)).get();
  };
  static auto* keep = new std::vector<std::unique_ptr<RenderPipeline>>();  // leaked like g (no renderer teardown)
  size_t built = 0, skipped = 0, skipped_live = 0;
  // Card R14 (780M stalls): parse every record first (shader modules created here, one thread), then build the
  // pipelines on several threads. A deque keeps each job's desc pointers (spec, slots, elements) stable.
  struct Job {
    RenderGraphicsPipelineDesc p;
    RenderSpecConstant spec;
    PipelineKey pk;
    std::vector<RenderInputSlot> slots;
    std::vector<RenderInputElement> elements;
  };
  std::deque<Job> jobs;
  for (const std::string& rec : records) {
    if (g_prewarm_stop) break;
    RecordReader in{rec};
    bool ok = true;
    const uint32_t scope = in.U32(&ok);
    Job& job = jobs.emplace_back();
    RenderGraphicsPipelineDesc& p = job.p;
    RenderSpecConstant& spec = job.spec;
    PipelineKey& pk = job.pk;
    std::vector<RenderInputSlot>& slots = job.slots;
    std::vector<RenderInputElement>& elements = job.elements;
    if (ok && scope == 0) {
      ok = in.Get(&pk, sizeof(pk));
      const uint32_t n = ok ? in.U32(&ok) : 0;
      for (uint32_t i = 0; ok && i < n; ++i) {
        const uint32_t index = in.U32(&ok), stride = in.U32(&ok), cls = in.U32(&ok);
        slots.emplace_back(index, stride, RenderInputSlotClassification(cls));
      }
      const uint32_t m = ok ? in.U32(&ok) : 0;
      for (uint32_t i = 0; ok && i < m; ++i) {
        const uint32_t location = in.U32(&ok), format = in.U32(&ok), slot = in.U32(&ok), offset = in.U32(&ok);
        elements.emplace_back("TEXCOORD", location, location, RenderFormat(format), slot, offset);
      }
      // As ExecuteDrawNow (same desc fields, same values; the key stores the final ones).
      p.pipelineLayout = g->layout.get();
      p.vertexShader = ok ? shader(pk.vs_key, pk.vs) : nullptr;
      p.pixelShader = ok && pk.ps ? shader(pk.ps_key, pk.ps) : nullptr;
      ok = ok && p.vertexShader && (!pk.ps || p.pixelShader);
      spec = RenderSpecConstant(0, pk.spec);
      p.specConstants = &spec;
      p.specConstantsCount = 1;
      p.primitiveTopology = RenderPrimitiveTopology(pk.topology);
      p.cullMode = RenderCullMode(pk.cull);
      p.frontFace = pk.front_ccw ? RenderFrontFace::COUNTER_CLOCKWISE : RenderFrontFace::CLOCKWISE;
      p.depthClipEnabled = pk.depth_clip;
      p.depthEnabled = pk.depth_enabled;
      p.depthWriteEnabled = pk.depth_write;
      p.depthFunction = RenderComparisonFunction(pk.depth_func);
      p.stencilEnabled = pk.stencil_enabled;
      p.stencilReadMask = uint8_t(pk.stencil_read);
      p.stencilWriteMask = uint8_t(pk.stencil_write);
      p.stencilReference = pk.stencil_ref;
      for (int i = 0; i < 2; ++i) {
        const uint32_t* o = i ? pk.stencil_back : pk.stencil_front;
        RenderStencilFaceDesc& f = i ? p.stencilBackFace : p.stencilFrontFace;
        f.passOp = RenderStencilOp(o[0]);
        f.failOp = RenderStencilOp(o[1]);
        f.depthFailOp = RenderStencilOp(o[2]);
        f.compareFunction = RenderComparisonFunction(o[3]);
      }
      p.renderTargetCount = pk.rt_count;
      p.renderTargetFormat[0] = RenderFormat(pk.rt_format);
      p.multisampling = Multisampling(pk.samples);
      p.sampleShadingEnabled = pk.sample_shading;
      RenderBlendDesc blend = RenderBlendDesc::Copy();
      blend.blendEnabled = pk.blend_enabled;
      blend.srcBlend = RenderBlend(pk.blend[0]);
      blend.dstBlend = RenderBlend(pk.blend[1]);
      blend.blendOp = RenderBlendOperation(pk.blend[2]);
      blend.srcBlendAlpha = RenderBlend(pk.blend[3]);
      blend.dstBlendAlpha = RenderBlend(pk.blend[4]);
      blend.blendOpAlpha = RenderBlendOperation(pk.blend[5]);
      blend.renderTargetWriteMask = uint8_t(pk.write_mask);
      p.renderTargetBlend[0] = blend;
      p.depthTargetFormat = RenderFormat(pk.depth_format);
      p.inputSlots = slots.data();
      p.inputSlotsCount = uint32_t(slots.size());
      p.inputElements = elements.data();
      p.inputElementsCount = uint32_t(elements.size());
      // Portable records store shader-content hashes in these two fields; live keys store the actual shader pointers.
      pk.vs = uint64_t(reinterpret_cast<uintptr_t>(p.vertexShader));
      pk.ps = p.pixelShader ? uint64_t(reinterpret_cast<uintptr_t>(p.pixelShader)) : 0;
    } else {
      ok = false;
    }
    if (!ok) {
      jobs.pop_back();
      ++skipped;
    }
  }
  g_prewarm_total = jobs.size() + skipped;
  g_prewarm_done = skipped;
  // Before play (g_prewarm_block): the game waits in WaitPipelinePrewarm, nothing else creates pipelines or saves the
  // cache, so the builders run in parallel without the create mutex (vkCreateGraphicsPipelines and the VkPipelineCache
  // are thread-safe; the mutex only kept the background prewarm from delaying the draw path). In the background mode
  // one builder takes the mutex per create, as before.
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  const unsigned threads = g_prewarm_block ? std::clamp(hw > 2 ? hw - 2 : 1u, 1u, 8u) : 1u;
  std::atomic<size_t> next{0}, built_n{0}, failed_n{0}, skipped_live_n{0};
  std::mutex keep_mu;
  auto builder = [&] {
    for (size_t i; !g_prewarm_stop && (i = next.fetch_add(1)) < jobs.size();) {
      Job& job = jobs[i];
      std::string key;
      BuildPipelineKey(key, job.pk, job.slots, job.elements);
      std::unique_ptr<RenderPipeline> pipeline;
      bool created_successfully = false, skip_live = false;
      auto key_is_present = [&] {
        return g_live_pipeline_keys.find(key) != g_live_pipeline_keys.end() ||
               g_warm_pipelines.find(key) != g_warm_pipelines.end();
      };
      if (g_prewarm_block) {
        if (g_prewarm_adopt) {
          std::lock_guard<std::mutex> lk(g_pipeline_create_mu);
          skip_live = key_is_present();
        }
        if (!skip_live) {
          pipeline = g->device->createGraphicsPipeline(job.p);
          created_successfully = bool(pipeline);
        }
        if (pipeline && g_prewarm_adopt) {
          std::lock_guard<std::mutex> lk(g_pipeline_create_mu);
          if (key_is_present()) {
            skip_live = true;
            created_successfully = false;
          } else {
            const bool inserted = g_warm_pipelines.emplace(key, std::move(pipeline)).second;
            if (!inserted) {
              skip_live = true;
              created_successfully = false;
            }
          }
        }
      } else {
        std::lock_guard<std::mutex> lk(g_pipeline_create_mu);  // one create at a time with the draw path / cache save
        if (g_prewarm_adopt && key_is_present()) {
          skip_live = true;
        } else {
          pipeline = g->device->createGraphicsPipeline(job.p);
          created_successfully = bool(pipeline);
          if (pipeline && g_prewarm_adopt) {
            const bool inserted = g_warm_pipelines.emplace(key, std::move(pipeline)).second;
            if (!inserted) {
              skip_live = true;
              created_successfully = false;
            }
          }
        }
      }
      if (skip_live) {
        ++skipped_live_n;
      } else if (created_successfully) {
        if (!g_prewarm_adopt) {
          std::lock_guard<std::mutex> lk(keep_mu);
          keep->push_back(std::move(pipeline));
        }
        ++built_n;
      } else {
        ++failed_n;
      }
      ++g_prewarm_done;
    }
  };
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < threads; ++t) pool.emplace_back(builder);
  builder();
  for (std::thread& t : pool) t.join();
  built = built_n;
  skipped += failed_n;
  skipped_live = skipped_live_n;
  size_t adoptable = 0;
  {
    std::lock_guard<std::mutex> lk(g_pipeline_create_mu);
    adoptable = g_warm_pipelines.size();
  }
  std::fprintf(stderr,
               "[nr-prewarm] list=%zu built=%zu skipped=%zu adoptable=%zu skipped_live=%zu ms=%.0f threads=%u %s%s\n",
               records.size(), built, skipped, adoptable, skipped_live, double(NowNs() - t0) / 1e6, threads,
               g_prewarm_block ? "before play" : "background", g_prewarm_stop ? " (stopped at exit)" : "");
  g_prewarm_end_ns = NowNs();
  g_prewarm_running = false;
}
}  // namespace

void MaybeStartPipelinePrewarm(const std::function<std::vector<const std::vector<uint8_t>*>()>& shaders) {
  if (!g_prewarm || g_prewarm_started || !g) return;
  g_prewarm_started = true;
  std::vector<std::string> records;
  {
    std::lock_guard<std::mutex> lk(g_prewarm_mu);
    records.assign(g_prewarm_records.begin(), g_prewarm_records.begin() + g_prewarm_loaded);
  }
  std::fprintf(stderr, "[nr-prewarm] list=%zu built=0 skipped=0 ms=0 (start): %s\n", records.size(),
               g_prewarm_file.c_str());
  if (records.empty()) return;
  g_prewarm_running = true;
  g_prewarm_t0_ns = NowNs();
  std::atexit([] { g_prewarm_stop = true; });
  std::thread(PrewarmPipelines, std::move(records), shaders()).detach();
}

PrewarmStatus GetPrewarmStatus() {
  PrewarmStatus st;
  st.running = g_prewarm_running.load();
  st.before_play = g_prewarm_block;
  st.total = g_prewarm_total.load();
  st.done = g_prewarm_done.load();
  const uint64_t t0 = g_prewarm_t0_ns.load();
  const uint64_t t1 = g_prewarm_end_ns.load();
  st.seconds = t0 ? double((t1 ? t1 : NowNs()) - t0) / 1e9 : 0.0;
  return st;
}

void WaitPipelinePrewarm() {
  if (!g_prewarm_block) return;
  while (g_prewarm_running && !g_prewarm_stop) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

}  // namespace nr::backend
