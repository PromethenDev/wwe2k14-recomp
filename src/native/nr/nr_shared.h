// Native renderer (nr) - shared, input-neutral pieces (Stage 2 WP0, decision 0006).
//
// Lifted verbatim (behaviour-neutral) out of the private nr::Frontend class in nr_frontend.cpp so that the packet-path
// frontend and the D3D-state renderer (src/native/d3d) execute the SAME code: shader contract set + key lookup,
// vertex format typing, clear-value decode, texture format/swizzle/level decode, sampler decode, VB/IB cache and
// the texture write-watch. Nothing here reads a Xenos register file or the GPU command stream; every input is a
// parameter. Not thread safe unless stated (TextureWatch's page arrays are atomics, as before).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/system/xmemory.h>
#include <xxhash.h>

#include "nr_backend.h"

namespace nr::shared {

namespace nb = nr::backend;

// ---- endian / tiling helpers ----------------------------------------------------------------------------------
inline uint16_t Swap16(uint16_t v) { return uint16_t(v >> 8 | v << 8); }
inline uint32_t Swap32(uint32_t v) { return __builtin_bswap32(v); }
// Guest bytes -> host order for a xenos::Endian / Endian128 value (0 none, 1 8in16, 2 8in32, 3 16in32), whole dwords.
void SwapDwords(uint8_t* p, size_t bytes, uint32_t endian);
// SwapDwords fused with the copy out of guest memory: one pass, the endian switch outside the loop.
void CopySwapDwords(uint8_t* dst, const uint8_t* src, size_t bytes, uint32_t endian);
inline uint32_t SwapDword(uint32_t v, uint32_t endian) {
  switch (endian) {
    case 1: return (v & 0x00FF00FFu) << 8 | (v & 0xFF00FF00u) >> 8;
    case 2: return Swap32(v);
    case 3: return v << 16 | v >> 16;
    default: return v;
  }
}
// texture_util::GetTiledOffset2D arithmetic, inlinable. `pitch_aligned` must already be aligned to 32.
inline int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch_aligned, uint32_t bytes_per_block_log2) {
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch_aligned >> 5)) << (bytes_per_block_log2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// ---- shader contracts -----------------------------------------------------------------------------------------
// FNV-1a-64 over the guest-order (big-endian) microcode dwords: the manifest key. Vertex shaders are stored under
// `key ^ 1` (the stage discriminator).
uint64_t Fnv1a64BigEndian(const uint32_t* dwords, size_t count);

struct VFetch {
  uint32_t location, usage, fetch, stride, offset, format, is_signed, integer;
};
struct ShaderEntry {
  // These byte vectors hold the selected backend's format (SPIR-V by default, DXIL when D3D12 is selected).
  std::vector<uint8_t> spirv;
  std::vector<uint8_t> spirv_f24;   // pixel shaders: float24-depth variant (tools/nr/f24-variant.py)
  std::vector<uint8_t> spirv_f24c;  // card P8: the same with SV_DepthLessEqual (WWE2K14_NR_F24_CONSERVATIVE)
  std::vector<uint8_t> spirv_b, spirv_f24_b, spirv_f24c_b;  // card P27: the same from the second set (empty: none)
  // D3D12 DXIL alpha-test specialization-constant variants (SPEC_CONSTANT_ALPHA_TEST=1).
  std::vector<uint8_t> spirv_s2, spirv_f24_s2, spirv_s2_b, spirv_f24_s2_b;
  std::vector<VFetch> vfetch;
  std::set<uint32_t> texture_slots;           // microcode fetch slots
  std::map<uint32_t, uint32_t> texture_dims;  // slot -> XenosRecomp TextureDimension (0 1D,1 2D,2 3D,3 cube)
  std::string source;
  std::string unsupported;  // a construct the converted shader does not reproduce
  // Q quality proof is lazy, worker-only, and lives with this immutable entry.
  // A reload constructs a new entry; there is no address-keyed stale cache.
  mutable uint8_t quality_proof_state = 0;
  mutable uint32_t quality_proof_reasons = 0, quality_proof_samples = 0, quality_proof_variants = 0;
};

struct RuntimeShaderWaitStats {
  uint64_t waits = 0;
  uint64_t wait_ms = 0;
};

// The selected bytecode/contract set from a shader directory (SPIR-V or DXIL manifest + HLSL contracts). One process-wide
// instance (Get()) so every renderer path sees the same contracts; Load() is idempotent for the same arguments.
class ShaderSet {
 public:
  static ShaderSet& Get();
  // Returns the number of entries. dir_b may be empty (no second set). A second call with identical arguments
  // is a no-op; different arguments reload.
  size_t Load(const std::filesystem::path& dir, const std::filesystem::path& dir_b, bool f24_conservative);
  // Entry for a microcode hash (Fnv1a64BigEndian) and stage, or null.
  const ShaderEntry* Find(uint64_t fnv, bool vertex) const { return FindRaw(fnv ^ (vertex ? 1ull : 0ull)); }
  // Entry by the stored (stage-discriminated) manifest key, or null.
  const ShaderEntry* FindRaw(uint64_t stored_key) const {
    auto it = map_.find(stored_key);
    return it == map_.end() ? nullptr : &it->second;
  }
  size_t size() const { return map_.size(); }
  // Card RT1: adds a runtime-translated contract (worker thread only; existing entries are never replaced and
  // element pointers stay valid). Returns the entry stored under the key.
  const ShaderEntry* Insert(uint64_t fnv, bool vertex, ShaderEntry&& e) {
    return &map_.try_emplace(fnv ^ (vertex ? 1ull : 0ull), std::move(e)).first->second;
  }
  bool UsesDxil() const { return dxil_; }
  bool FormatMatchesBackend() const { return saw_manifest_row_ && format_matches_backend_; }
  // Card S3-57: every non-empty shader bytecode variant (SPIR-V or DXIL, read-only; vectors live until reload).
  template <typename F>
  void ForEachSpirv(F&& f) const {
    for (const auto& kv : map_) {
      for (const std::vector<uint8_t>* v : {&kv.second.spirv, &kv.second.spirv_f24, &kv.second.spirv_f24c,
                                            &kv.second.spirv_b, &kv.second.spirv_f24_b, &kv.second.spirv_f24c_b,
                                            &kv.second.spirv_s2, &kv.second.spirv_f24_s2, &kv.second.spirv_s2_b,
                                            &kv.second.spirv_f24_s2_b}) {
        if (!v->empty()) f(*v);
      }
    }
  }
  uint64_t f24c_loaded() const { return f24c_loaded_; }
  uint64_t set_b_loaded() const { return set_b_loaded_; }
  uint64_t set_b_missing() const { return set_b_missing_; }

 private:
  std::unordered_map<uint64_t, ShaderEntry> map_;
  std::filesystem::path dir_, dir_b_;
  bool f24c_ = false, loaded_ = false, dxil_ = false, saw_manifest_row_ = false, format_matches_backend_ = true;
  uint64_t f24c_loaded_ = 0, set_b_loaded_ = 0, set_b_missing_ = 0;
};

// ---- card RT1: runtime contracts for shaders the set lacks ----------------------------------------------------
// Default on (WWE2K14_NR_RUNTIME_SHADERS=0 disables); needs the translator tools (<exe dir>/shadertools or
// WWE2K14_NR_RUNTIME_SHADERS_TOOLS); SPIR-V backends only. All calls on the thread that owns the ShaderSet lookups.
// Cache root for translated contracts (<userdata>/cache/nr/runtime-shaders); env WWE2K14_NR_RUNTIME_SHADERS_CACHE wins.
void SetRuntimeShaderCacheDir(const std::filesystem::path& dir);
// Shutdown began (call before the logger shuts down): the detached translator stops logging and publishing.
void RuntimeShaderShutdown();
// A draw found no contract for `fnv`: returns a runtime contract when one is ready (translated earlier or in the disk
// cache; inserted into `set`), else queues once per key and waits up to WWE2K14_RTS_WAIT_MS (default 250, 0 disables)
// for that exact key/stage. On timeout/failure, returns null; only the first pending draw waits for a key.
const ShaderEntry* RuntimeShaderResolve(ShaderSet& set, uint64_t fnv, bool vertex, const uint32_t* host_dwords,
                                        size_t count, RuntimeShaderWaitStats* wait_stats = nullptr);
// CHECK mode (WWE2K14_NR_RUNTIME_SHADERS_CHECK=1): translate a key that HAS a shipped contract once and compare.
bool RuntimeShaderCheckEnabled();
void RuntimeShaderCheck(uint64_t fnv, bool vertex, const uint32_t* host_dwords, size_t count, const ShaderEntry* shipped);

// Host vertex format for one fetch, with the converter's typing rule: packed 10_11_11/11_11_10 -> uint (shader
// unpacks); float formats -> float; other formats: integer -> UINT/SINT, fraction -> UNORM/SNORM.
bool VertexFormat(const VFetch& f, nb::VtxFormat* out, bool* half_words);

// ---- render-target / clear values -----------------------------------------------------------------------------
bool HostTargetFormat(rex::graphics::xenos::ColorRenderTargetFormat f, nb::TargetFormat* out);
// Resolve-clear values (RB_DEPTH_CLEAR / RB_COLOR_CLEAR(_LO)) as the host clear takes them.
float DepthClearZ(uint32_t depth_format, uint32_t rb_depth_clear);
bool ColorClearValue(rex::graphics::xenos::ColorRenderTargetFormat cf, uint32_t clear, uint32_t clear_lo,
                     float rgba[4], bool* integer, std::string* why);

// ---- samplers and textures ------------------------------------------------------------------------------------
// The SDK's fetch-constant -> texture-key decoding is a protected TextureCache member; this never-instantiated
// subclass only re-exports it.
struct TextureKeys : rex::graphics::TextureCache {
  using rex::graphics::TextureCache::BindingInfoFromFetchConstant;
  using rex::graphics::TextureCache::TextureKey;
};

// Host sampler for fetch constant `fetch` used by fetch slot `slot`: the fetch instruction's override (looked up in
// the vertex/pixel shaders' texture bindings, either may be null), else the fetch constant. Reads the SDK's
// anisotropic_override cvar as before.
nb::SamplerDesc SamplerFromFetch(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, uint32_t slot,
                                 const rex::graphics::Shader* vs, const rex::graphics::Shader* ps);

// Host format decision for a valid, single-level-set guest texture (TextureFor guest-texture branch).
struct TexFormatPlan {
  nb::TexFormat format = nb::TexFormat::kRGBA8;
  uint32_t block = 1, bpb = 4;  // block edge in texels, bytes per block
  bool one_component = false, two_component = false, depth24 = false, depth_float = false, rgba4 = false;
  bool any_signed = false;
};
// false: unsupported; *why is the reason WITHOUT the "slot N " prefix (e.g. "gamma/biased texture sign").
bool PlanTextureFormat(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, const TextureKeys::TextureKey& key,
                       TexFormatPlan* out, std::string* why);
// The fetch constant with sampler-only fields cleared, used as the D3D texture cache key.
rex::graphics::xenos::xe_gpu_texture_fetch_t TextureKeyFetch(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                                                             const TextureKeys::TextureKey& key, bool nosampler);
// Host swizzle for nb::TextureDesc (one/two component formats replicate R / G).
void TextureSwizzle(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, const TexFormatPlan& plan,
                    uint8_t swizzle[4]);
// Block offset of `level` inside its stored memory, as the SDK's VulkanTextureCache::LoadTexture: every level >=
// packed_level sits inside the packed mip tail - level 0 too when the tail starts there (fetch packed_mips with the
// shortest side <= 16: a 256x16 DXT1 base is at block row 4, the rows above it hold its smaller mips and padding).
inline void PackedLevelOffset(const rex::graphics::texture_util::TextureGuestLayout& layout, uint32_t width,
                              uint32_t height, rex::graphics::xenos::TextureFormat format, uint32_t level,
                              uint32_t* x_blocks, uint32_t* y_blocks) {
  *x_blocks = *y_blocks = 0;
  if (layout.packed_level == UINT32_MAX || level < layout.packed_level) return;
  uint32_t z_blocks = 0;
  rex::graphics::texture_util::GetPackedMipOffset(width, height, 1, format, level, *x_blocks, *y_blocks, z_blocks);
}
// Per-level geometry shared by the CPU and GPU detile paths.
struct LevelGeom {
  uint32_t lw, lh, bw, bh, address, off_x, off_y, pitch_blocks, bpb_log2;
  const rex::graphics::texture_util::TextureGuestLayout::Level* lay;
};
LevelGeom ComputeLevelGeom(const TextureKeys::TextureKey& key, const rex::graphics::texture_util::TextureGuestLayout& layout,
                           const TexFormatPlan& plan, uint32_t level);
// GPU detile description of one level/face (guest base address in gl.p.src_base).
nb::GpuLevel MakeGpuLevel(const TextureKeys::TextureKey& key, const TexFormatPlan& plan, const LevelGeom& g,
                          uint32_t level, uint32_t face);
// CPU detile of one level/face from `src` (the level's guest bytes at its own base). false: 8bpp with 16in32 swap
// (unsupported). data.bytes receives the host-order texels.
bool DetileLevelCpu(const TextureKeys::TextureKey& key, const TexFormatPlan& plan, const LevelGeom& g, uint32_t level,
                    uint32_t face, const uint8_t* src, nb::LevelData* data);
// RR1 (docs/tasks/RR-record-replay.md): optional observer of the guest bytes DetileLevelCpu / TextureHash read (host
// pointer + length; a superset at 32-row macro-tile granularity for tiled levels). Null (default) = no cost but one load.
using ReadObserver = void (*)(const uint8_t* host, uint64_t len);
void SetReadObserver(ReadObserver fn);
// XXH3-128 over the cache key plus the guest bytes a texture build reads (exact base and mip extents).
XXH128_hash_t TextureHash(XXH3_state_t* st, const std::string& cache_key, const TextureKeys::TextureKey& key,
                          const rex::graphics::texture_util::TextureGuestLayout& layout, rex::memory::Memory* memory);

// ---- texture / buffer write watch -----------------------------------------------------------------------------
// Per 4 KB guest page write generation (monotonic) + one-shot armed bit (cleared by the write that fires the SDK's
// physical-memory invalidation callback). The OWNER of the callback registration calls MarkWritten from that
// callback (and for writes that bypass the fault watch); Arm() sets the bits and enables the SDK watch, calling
// `ensure_registered` first (the owner registers its callback there, once).
class TextureWatch {
 public:
  static constexpr uint32_t kPages = 0x20000000u >> 12;
  using EnsureFn = void (*)(void* ctx);
  void SetEnsureRegistered(EnsureFn fn, void* ctx) { ensure_ = fn; ensure_ctx_ = ctx; }
  // Arms the one-shot watch over [start, start+length). MUST run BEFORE the bytes are hashed/read.
  void Arm(rex::memory::Memory* memory, uint32_t start, size_t length);
  // Advances every page's generation and clears the armed bits. bypass selects the profile counter.
  void MarkWritten(uint32_t start, uint32_t length, bool bypass);
  // Sum of generations over {base_start, base_end, mip_start, mip_end} (empty spans skipped).
  uint64_t GenSum(const std::array<uint32_t, 4>& range) const;
  // Generation sum valid (gen_valid), every page still armed and the sum unchanged.
  bool Clean(const std::array<uint32_t, 4>& range, bool gen_valid, uint64_t gen_sum) const;
  uint32_t Gen(uint32_t page) const { return gen_[page].load(std::memory_order_relaxed); }
  bool Armed(uint32_t page) const { return armed_[page].load(std::memory_order_relaxed) != 0; }
  std::atomic<uint64_t> watch_faults{0};  // pages armed when a write arrived (fault watch), per frame
  std::atomic<uint64_t> bypass_pages{0};  // pages marked written without a fault, per frame

 private:
  std::unique_ptr<std::atomic<uint32_t>[]> gen_{new std::atomic<uint32_t>[kPages]()};
  std::unique_ptr<std::atomic<uint8_t>[]> armed_{new std::atomic<uint8_t>[kPages]()};
  EnsureFn ensure_ = nullptr;
  void* ensure_ctx_ = nullptr;
};

// ---- vertex / index buffer cache --------------------------------------------------------------------------------
// Static vertex / index buffers kept in the backend. Key = everything that decides the host bytes. A key is cached on
// its SECOND sighting. Validity = the TextureWatch generation sum over the buffer's pages, armed BEFORE the bytes are
// read. A buffer found written again within a few frames of its last build 3 times is dynamic and left to the normal
// path for 240 frames.
class VbCache {
 public:
  struct Key {
    std::array<uint32_t, 12> k{};
    bool operator==(const Key& o) const { return k == o.k; }
  };
  struct KeyHash {
    size_t operator()(const Key& v) const {
      uint64_t h = 1469598103934665603ull;
      for (uint32_t x : v.k) h = (h ^ x) * 1099511628211ull;
      return size_t(h ^ (h >> 29));
    }
  };
  struct Entry {
    uint32_t id = 0, bytes = 0, first_pg = 0, last_pg = 0, strikes = 0;
    uint64_t gen_sum = 0, last_build = 0, dyn_until = 0;
    bool built = false, backend_has = false, counted = false;
  };
  enum { kNo = 0, kHit = 1, kBuild = 2 };
  explicit VbCache(TextureWatch* watch) : watch_(watch) {}
  // kHit: reuse e->id. kBuild: pages armed and gen_sum captured - the caller reads the bytes NOW and sends them with
  // the upload flag. kNo: normal path. `frame` is the caller's frame counter; trust_replay is measurement-only.
  int Lookup(rex::memory::Memory* memory, const Key& key, uint32_t phys_start, uint32_t bytes, bool index,
             uint64_t frame, bool trust_replay, Entry** out);
  // V17d (Claude 11-03): read-only probe - true iff Lookup(key, ...) would return kHit right now (no state change).
  bool WouldHit(const Key& key, uint32_t phys_start, uint32_t bytes, uint64_t frame, uint32_t* id) const;
  int Probe(const Key& key, uint32_t phys_start, uint32_t bytes, uint64_t frame, uint32_t* id) const;
  // Drops everything when over `max_bytes` and sets `flush` (the next submitted draw carries cache_flush).
  void EvictIfFull(uint64_t max_bytes);
  void ResetFrameStats() {
    vbc_hit = vbc_build = vbc_first = vbc_dyn = vbc_hit_bytes = vbc_build_bytes = 0;
    ibc_hit = ibc_build = ibc_first = ibc_dyn = ibc_hit_bytes = 0;
  }
  size_t entries() const { return map_.size(); }
  uint64_t bytes = 0;
  uint32_t next_id = 1;
  bool flush = false;
  uint64_t vbc_hit = 0, vbc_build = 0, vbc_first = 0, vbc_dyn = 0, vbc_hit_bytes = 0, vbc_build_bytes = 0;
  uint64_t ibc_hit = 0, ibc_build = 0, ibc_first = 0, ibc_dyn = 0, ibc_hit_bytes = 0;

 private:
  bool Clean(const Entry& e) const;
  TextureWatch* watch_;
  std::unordered_map<Key, Entry, KeyHash> map_;
};

}  // namespace nr::shared
