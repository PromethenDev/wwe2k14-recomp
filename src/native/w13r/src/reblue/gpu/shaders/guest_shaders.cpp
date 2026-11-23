/**
 * @file    gpu/shaders/guest_shaders.cpp
 * @brief   Turning a guest shader into a host one: cache lookup, DXIL/SPIR-V
 *          decompression, and spec constant linking.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/device.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>
#include <rex/hash.h>
#include <zstd.h>

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/host_resource_heap.h"
#include "gpu/pipeline/pso_recorder.h"
#include "gpu/shaders/shader_cache.h"

#if defined(REBLUE_D3D11)
#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
// The game's shaders as DXBC, one per spec-constant subset (WWE '12 PC's
// tools/convert_shaders_d3d11.py, run at build time: dxbc_shader_cache.cpp in the build folder),
// zlib-compressed, sorted by (hash, constants).
struct Wwe12DxbcEntry {
  uint64_t hash;
  uint32_t specConstants;
  uint32_t offset;
  uint32_t size;
};
extern const Wwe12DxbcEntry g_wwe12DxbcEntries[];
extern const size_t g_wwe12DxbcEntryCount;
extern const size_t g_wwe12DxbcRawSize;
extern const size_t g_wwe12DxbcPackedSize;
extern const unsigned char g_wwe12DxbcPacked[];
#elif defined(REBLUE_D3D12)
#define MINIZ_HEADER_FILE_ONLY
#include "gpu/shaders/linked_shader_cache.h"
#include "gpu/shaders/shader_linker.h"
bool Wwe13DxcAvailable();  // native/wwe13_dxc.cpp
#include <miniz.h>
#else
#include <cstdio>
#include <smolv.h>
#endif

#include <atomic>
#include <cstring>
#include <rex/cvar.h>

// Android : désactivé par défaut. La capacité SPV_KHR_float_controls ajoutée empêche toutes les
// pipelines du jeu de se construire sur Adreno 720 (pilote d'origine Vulkan 1.3.128) : écran noir.
// Le bug des yeux et des dents qu'elle corrige est propre à Intel sur PC.
#if defined(__ANDROID__)
#define WWE12_INF_NAN_DEFAULT false
#else
#define WWE12_INF_NAN_DEFAULT true
#endif
REXCVAR_DEFINE_BOOL(svr_preserve_inf_nan, WWE12_INF_NAN_DEFAULT, "SvR",
                    "Ask the GPU driver to keep infinities and NaN exact in game shaders (Intel "
                    "otherwise drops the Xbox-style clamps: black eyes and teeth).");
REXCVAR_DEFINE_BOOL(svr_wait_for_shader, true, "SvR",
                    "A draw whose shader is still converting waits for it (up to 2 s) instead of being skipped");

REXCVAR_DEFINE_BOOL(svr_name_shaders, false, "SvR",
                    "Dev: name each game shader guest_<hash> in its SPIR-V, for RenderDoc.");

namespace bd::gpu {

namespace {
// WWE13_BUILTIN_SPIRV (wwe13_native.exe built with the dumped shaders, as WWE '12 PC): the game's
// SPIR-V comes from the generated shader_cache.cpp built into the exe. A shader missing from it falls
// back to the run-time conversion (WWE13_RUNTIME_SHADERS), which needs dxcompiler.dll beside the exe.
#if !defined(WWE13_RUNTIME_SHADERS) || defined(WWE13_BUILTIN_SPIRV)
#define WWE13_HAS_BUILTIN_CACHE 1
[[maybe_unused]] bool IsBuiltinEntry(const ShaderCacheEntry *entry) {
  return entry >= g_shaderCacheEntries && entry < g_shaderCacheEntries + g_shaderCacheEntryCount;
}
#endif

ShaderCacheEntry *FindShaderCacheEntry(u64 hash) {
#if defined(WWE13_HAS_BUILTIN_CACHE)
  auto *end = g_shaderCacheEntries + g_shaderCacheEntryCount;
  auto *result = std::lower_bound(
      g_shaderCacheEntries, end, hash,
      [](const ShaderCacheEntry &lhs, u64 rhs) { return lhs.hash < rhs; });
  if (result != end && result->hash == hash)
    return result;
#endif
#if defined(WWE13_RUNTIME_SHADERS)
  return Wwe13FindShaderEntry(hash);
#else
  return nullptr;
#endif
}

// Guards GuestShader::shader / linkedShaders lookups+inserts only. DXC link
// and createShader run unlocked so precache workers compile in parallel.
std::mutex g_shader_link_mutex;

#if defined(REBLUE_D3D11)
std::once_flag g_dxbc_cache_once;
std::unique_ptr<u8[]> g_dxbc_cache;

const u8 *DxbcCache() {
  std::call_once(g_dxbc_cache_once, [] {
    auto buf = std::make_unique<u8[]>(g_wwe12DxbcRawSize);
    mz_ulong out_len = static_cast<mz_ulong>(g_wwe12DxbcRawSize);
    if (mz_uncompress(buf.get(), &out_len, g_wwe12DxbcPacked,
                      static_cast<mz_ulong>(g_wwe12DxbcPackedSize)) != MZ_OK ||
        out_len != g_wwe12DxbcRawSize) {
      BD_ERROR("DXBC shader cache decompression failed");
      return;
    }
    g_dxbc_cache = std::move(buf);
  });
  return g_dxbc_cache.get();
}

const Wwe12DxbcEntry *FindDxbcEntry(u64 hash, u32 masked) {
  const Wwe12DxbcEntry *first = g_wwe12DxbcEntries;
  const Wwe12DxbcEntry *end = first + g_wwe12DxbcEntryCount;
  const auto *it = std::lower_bound(
      first, end, std::pair{hash, masked},
      [](const Wwe12DxbcEntry &lhs, const std::pair<u64, u32> &rhs) {
        if (lhs.hash != rhs.first)
          return lhs.hash < rhs.first;
        return lhs.specConstants < rhs.second;
      });
  return (it != end && it->hash == hash && it->specConstants == masked) ? it : nullptr;
}
#elif defined(REBLUE_D3D12)
// The linked-in DXIL shader cache ships ZSTD-compressed. Decompress it once on
// first shader resolve.
std::once_flag g_dxil_cache_once;
std::unique_ptr<u8[]> g_dxil_cache;

const u8 *DxilCache() {
  std::call_once(g_dxil_cache_once, [] {
    g_dxil_cache = std::make_unique<u8[]>(g_dxilCacheDecompressedSize);
    const size_t n =
        ZSTD_decompress(g_dxil_cache.get(), g_dxilCacheDecompressedSize,
                        g_compressedDxilCache, g_dxilCacheCompressedSize);
    if (ZSTD_isError(n) || n != g_dxilCacheDecompressedSize) {
      BD_ERROR("DXIL shader cache decompression failed ({} of {} bytes)", n,
               g_dxilCacheDecompressedSize);
      g_dxil_cache.reset();
    }
  });
  return g_dxil_cache.get();
}

// Build-time pre-linked spec constant variants (reblue_prelink), miniz
// (deflate) compressed, decompressed once on first spec shader resolve.
std::once_flag g_linked_dxil_cache_once;
std::unique_ptr<u8[]> g_linked_dxil_cache;

const u8 *LinkedDxilCache() {
  std::call_once(g_linked_dxil_cache_once, [] {
    if (g_linkedShaderCacheEntryCount == 0)
      return;
    auto buf = std::make_unique<u8[]>(g_linkedDxilCacheDecompressedSize);
    mz_ulong out_len = static_cast<mz_ulong>(g_linkedDxilCacheDecompressedSize);
    if (mz_uncompress(buf.get(), &out_len, g_compressedLinkedDxilCache,
                      static_cast<mz_ulong>(g_linkedDxilCacheCompressedSize)) !=
            MZ_OK ||
        out_len != g_linkedDxilCacheDecompressedSize) {
      BD_ERROR("linked shader cache decompression failed");
      return;
    }
    g_linked_dxil_cache = std::move(buf);
    BD_DEBUG("linked shader cache: {} prelinked spec-constant variant(s)",
             g_linkedShaderCacheEntryCount);
  });
  return g_linked_dxil_cache.get();
}

const LinkedShaderCacheEntry *FindLinkedShaderCacheEntry(u64 hash, u32 masked) {
  const LinkedShaderCacheEntry *first = g_linkedShaderCacheEntries;
  const LinkedShaderCacheEntry *end = first + g_linkedShaderCacheEntryCount;
  const auto *it = std::lower_bound(
      first, end, std::pair{hash, masked},
      [](const LinkedShaderCacheEntry &lhs, const std::pair<u64, u32> &rhs) {
        if (lhs.hash != rhs.first)
          return lhs.hash < rhs.first;
        return lhs.specConstants < rhs.second;
      });
  return (it != end && it->hash == hash && it->specConstants == masked)
             ? it
             : nullptr;
}
#else
// XenosRecomp clamps rcp/rsq/log results to +-FLT_MAX, as the Xbox does, so that 1/0 never
// reaches a multiply as an infinity. Vulkan lets a driver assume no Inf/NaN unless the module asks
// otherwise: Intel's compiler then drops those clamps, the infinity survives and Inf * 0 gives NaN.
// WWE '12's face layers hit that over the eye sockets and blend NaN, black eyes and teeth (and the
// referee's hair) on Intel GPUs. Declares SignedZeroInfNanPreserve for 32-bit floats (capability,
// SPV_KHR_float_controls, execution mode on the entry point) in a decoded module.
void PreserveInfNan(std::vector<u8> &bytes) {
  constexpr u32 kOpExtension = 10, kOpEntryPoint = 15, kOpExecutionMode = 16, kOpCapability = 17;
  constexpr u32 kCapSignedZeroInfNanPreserve = 4466, kModeSignedZeroInfNanPreserve = 4461;
  if (bytes.size() < 20 || bytes.size() % 4)
    return;
  std::vector<u32> w(bytes.size() / 4);
  std::memcpy(w.data(), bytes.data(), bytes.size());
  if (w[0] != 0x07230203u)
    return;
  size_t after_caps = 0, after_modes = 0;
  u32 entry = 0;
  bool has_cap = false;
  for (size_t i = 5; i < w.size();) {
    const u32 op = w[i] & 0xFFFFu, count = w[i] >> 16;
    if (count == 0 || i + count > w.size())
      return;
    if (op == kOpCapability) {
      has_cap |= w[i + 1] == kCapSignedZeroInfNanPreserve;
      after_caps = i + count;
    } else if (op == kOpEntryPoint && !entry) {
      entry = w[i + 2];
      after_modes = i + count;
    } else if (op == kOpExecutionMode || (op == kOpEntryPoint && entry)) {
      after_modes = i + count;
    } else if (entry && after_modes) {
      break;  // past the entry points and execution modes
    }
    i += count;
  }
  if (has_cap || !after_caps || !entry || !after_modes)
    return;
  // Inserted from the back so the earlier position stays valid.
  const u32 mode[] = {(4u << 16) | kOpExecutionMode, entry, kModeSignedZeroInfNanPreserve, 32};
  w.insert(w.begin() + std::ptrdiff_t(after_modes), std::begin(mode), std::end(mode));
  static const char kExt[] = "SPV_KHR_float_controls";  // 22 chars + NUL, padded to 6 words
  u32 ext[7] = {(7u << 16) | kOpExtension};
  std::memcpy(&ext[1], kExt, sizeof(kExt));
  const u32 cap[] = {(2u << 16) | kOpCapability, kCapSignedZeroInfNanPreserve};
  w.insert(w.begin() + std::ptrdiff_t(after_caps), std::begin(ext), std::end(ext));
  w.insert(w.begin() + std::ptrdiff_t(after_caps), std::begin(cap), std::end(cap));
  bytes.resize(w.size() * 4);
  std::memcpy(bytes.data(), w.data(), bytes.size());
  static std::atomic<u32> s_done{0};
  const u32 n = s_done.fetch_add(1) + 1;
  if (n == 1 || n % 200 == 0)
    BD_INFO("[shaders] Inf/NaN preserved in {} module(s)", n);
}

// Dev (svr_name_shaders): names the entry point "guest_<hash>" (OpName), so RenderDoc shows which
// guest shader a pipeline runs; used to find the draws behind a graphics option (the crowd...).
// Debug info only: no effect on the code. Inserted after the entry points, execution modes and
// any OpString/OpSource, where SPIR-V wants OpName.
void NameShader(std::vector<u8> &bytes, u64 hash) {
  constexpr u32 kOpSourceContinued = 2, kOpSource = 3, kOpSourceExtension = 4, kOpName = 5,
                kOpString = 7, kOpEntryPoint = 15, kOpExecutionMode = 16;
  if (bytes.size() < 20 || bytes.size() % 4)
    return;
  std::vector<u32> w(bytes.size() / 4);
  std::memcpy(w.data(), bytes.data(), bytes.size());
  if (w[0] != 0x07230203u)
    return;
  size_t at = 0;
  u32 entry = 0;
  for (size_t i = 5; i < w.size();) {
    const u32 op = w[i] & 0xFFFFu, count = w[i] >> 16;
    if (count == 0 || i + count > w.size())
      return;
    if (op == kOpEntryPoint) {
      if (!entry)
        entry = w[i + 2];
      at = i + count;
    } else if (op == kOpExecutionMode || (entry && (op == kOpString || op == kOpSource ||
                                                    op == kOpSourceContinued ||
                                                    op == kOpSourceExtension))) {
      at = i + count;
    } else if (entry) {
      break;
    }
    i += count;
  }
  if (!entry || !at)
    return;
  char name[24];  // "guest_" + 16 hex + NUL = 23 chars, padded to 6 words
  std::snprintf(name, sizeof(name), "guest_%016llX", static_cast<unsigned long long>(hash));
  u32 op[8] = {(8u << 16) | kOpName, entry};
  std::memcpy(&op[2], name, sizeof(name));
  w.insert(w.begin() + std::ptrdiff_t(at), std::begin(op), std::end(op));
  bytes.resize(w.size() * 4);
  std::memcpy(bytes.data(), w.data(), bytes.size());
}

#if defined(WWE13_HAS_BUILTIN_CACHE)
// The SPIR-V half of the same generated cache: ZSTD-compressed, per-entry
// modules smol-v encoded. Decompressed once on first shader resolve.
std::once_flag g_spirv_cache_once;
std::unique_ptr<u8[]> g_spirv_cache;

const u8 *SpirvCache() {
  std::call_once(g_spirv_cache_once, [] {
    g_spirv_cache = std::make_unique<u8[]>(g_spirvCacheDecompressedSize);
    const size_t n =
        ZSTD_decompress(g_spirv_cache.get(), g_spirvCacheDecompressedSize,
                        g_compressedSpirvCache, g_spirvCacheCompressedSize);
    if (ZSTD_isError(n) || n != g_spirvCacheDecompressedSize) {
      BD_ERROR("SPIR-V shader cache decompression failed ({} of {} bytes)", n,
               g_spirvCacheDecompressedSize);
      g_spirv_cache.reset();
    } else {
      BD_INFO("Native shaders: {} built into the executable (SPIR-V, {} KB)",
              g_shaderCacheEntryCount, g_spirvCacheDecompressedSize / 1024);
    }
  });
  return g_spirv_cache.get();
}
#endif  // WWE13_HAS_BUILTIN_CACHE
#endif

// X360 exports BD's bloom bright mask into an LDR EDRAM tile, saturating it at
// 1.0 before the gaussian blur, but reblue's FP16 posteff chain keeps the raw
// mask (wc01 authors BLOOM BriMulti=50 -> exports ~59) and the blur spreads it
// into giant white blobs. These two masks are substituted by copies whose
// export is clamped to [0,1].
// SvR 2008: no shader substitutions yet (re:Blue swaps in clamped copies of Blue Dragon's
// bloom bright-pass shaders here, because FP16 render targets don't saturate like EDRAM).
bool BloomMaskClampBlob(u64, const void *&, size_t &) { return false; }

// Publishes the first shader built for gs, a concurrent builder's copy is
// dropped before any caller can observe it.
plume::RenderShader *PublishShader(GuestShader *gs,
                                   std::unique_ptr<plume::RenderShader> sh) {
  std::lock_guard lock(g_shader_link_mutex);
  if (!gs->shader)
    gs->shader = std::move(sh);
  return gs->shader.get();
}

} // namespace

GuestShader *CreateShader(const be_u32 *function, ResourceType type) {
  // The cache key hashes the whole container, matching how XenosRecomp keyed
  // the entries it generated.
  const auto *container = reinterpret_cast<const ShaderContainer *>(function);
  const u32 hash_len = container->virtualSize + container->physicalSize;
  const u64 hash = XXH3_64bits(function, hash_len);
#if defined(WWE13_NATIVE)
  // Build-time shader caches (D3D12 / D3D11, as WWE '12 PC): WWE13_DUMP_SHADERS=<folder> writes every
  // container the game creates to <folder>\<hash>.xsc, once. WWE '13's archives are compressed, so the
  // containers are taken when the game creates them (as WWE 2K15 PC does). Never shipped or committed.
  if (static const char *dump_dir = std::getenv("WWE13_DUMP_SHADERS"); dump_dir && *dump_dir) {
    static std::mutex dump_mutex;
    static std::unordered_set<u64> dumped;
    std::lock_guard lock(dump_mutex);
    if (dumped.insert(hash).second) {
      char path[1024];
      std::snprintf(path, sizeof(path), "%s\\%016llX.xsc", dump_dir, static_cast<unsigned long long>(hash));
      if (FILE *f = std::fopen(path, "wb")) {
        std::fwrite(function, 1, hash_len, f);
        std::fclose(f);
      }
    }
  }
#endif
  ShaderCacheEntry *entry = FindShaderCacheEntry(hash);
  auto *shader = HostResourceHeap::Alloc<GuestShader>(type);
  if (entry) {
    if (entry->guestShader == nullptr) {
      entry->guestShader = shader;
    }
    shader->shaderCacheEntry = entry;
  } else {
#if defined(WWE13_RUNTIME_SHADERS)
    Wwe13RequestShader(hash, reinterpret_cast<const u8 *>(function), hash_len, shader);
#else
    BD_WARN("Shader cache miss: hash=0x{:016X} len={} type={}", hash, hash_len,
            static_cast<u32>(type));
#endif
  }
  // Let boot cache replay enqueue any pending PSO that was waiting on this
  // shader's microcode hash now that its host object exists.
  OnShaderCreated(hash);
  return shader;
}

// For a guest container the game altered after loading (WWE '12): the cache
// key was matched by other means, so bind the entry by its known hash.
// nullptr when the hash is not in the cache.
bool ShaderCacheHas(u64 hash) { return FindShaderCacheEntry(hash) != nullptr; }

GuestShader *CreateShaderFromHash(u64 hash, ResourceType type) {
  ShaderCacheEntry *entry = FindShaderCacheEntry(hash);
  if (!entry)
    return nullptr;
  auto *shader = HostResourceHeap::Alloc<GuestShader>(type);
  if (entry->guestShader == nullptr)
    entry->guestShader = shader;
  shader->shaderCacheEntry = entry;
  OnShaderCreated(hash);
  return shader;
}

plume::RenderShader *GetOrLinkShader(GuestShader *gs, u32 specConstants) {
  if (!gs)
    return nullptr;
  // hcg*ShaderCreateByHlsl path: shader populated directly, no cache entry.
  if (gs->shader)
    return gs->shader.get();
  const ShaderCacheEntry *entry = gs->shaderCacheEntry;
#if defined(WWE13_RUNTIME_SHADERS)
  // Converted at run time: wait for it rather than skip the draw (see Wwe13WaitShader).
  if (!entry && REXCVAR_GET(svr_wait_for_shader)) {
    Wwe13WaitShader(gs, 2000);
    entry = gs->shaderCacheEntry;
  }
#endif
  if (!entry)
    return nullptr;
  auto *device = Video::HostDevice();
  if (!device)
    return nullptr;

  const void *clampBlob = nullptr;
  size_t clampSize = 0;
  if (BloomMaskClampBlob(entry->hash, clampBlob, clampSize)) {
    return PublishShader(gs, device->createShader(clampBlob, clampSize, "main",
                                                  kHostShaderFormat));
  }

#if !defined(REBLUE_D3D12)
  // Vulkan: one decoded SPIR-V module serves every spec constant value. The
  // XenosRecomp SPIR-V keeps g_SpecConstants as specialization constant id 0,
  // supplied per pipeline by the pipeline cache, and no link step exists.
  (void)specConstants;
  const u8 *smol = nullptr;
#if defined(WWE13_HAS_BUILTIN_CACHE)
  if (IsBuiltinEntry(entry)) {
    const u8 *cache = SpirvCache();
    if (!cache)
      return nullptr;
    smol = cache + entry->spirvOffset;
  }
#endif
#if defined(WWE13_RUNTIME_SHADERS)
  if (!smol)
    smol = Wwe13ShaderSpirv(entry);
#endif
  if (!smol)
    return nullptr;
  std::vector<u8> spirv(smolv::GetDecodedBufferSize(smol, entry->spirvSize));
  if (spirv.empty() ||
      !smolv::Decode(smol, entry->spirvSize, spirv.data(), spirv.size())) {
    BD_ERROR("GetOrLinkShader: SPIR-V decode failed (hash=0x{:016X})",
             entry->hash);
    return nullptr;
  }
  if (REXCVAR_GET(svr_preserve_inf_nan))
    PreserveInfNan(spirv);
  if (REXCVAR_GET(svr_name_shaders))
    NameShader(spirv, entry->hash);
  return PublishShader(gs,
                       device->createShader(spirv.data(), spirv.size(), "main",
                                            plume::RenderShaderFormat::SPIRV));
#elif defined(REBLUE_D3D11)
  const u8 *cache = DxbcCache();
  if (!cache)
    return nullptr;
  const u32 masked = specConstants & entry->specConstantsMask;
  {
    std::lock_guard lock(g_shader_link_mutex);
    auto it = gs->linkedShaders.find(masked);
    if (it != gs->linkedShaders.end())
      return it->second.get();
  }
  const Wwe12DxbcEntry *dxbc = FindDxbcEntry(entry->hash, masked);
  if (!dxbc) {
    BD_WARN("GetOrLinkShader: no DXBC variant, not drawn (hash=0x{:016X} mask=0x{:X})",
            entry->hash, masked);
    return nullptr;
  }
  auto sh = device->createShader(cache + dxbc->offset, dxbc->size, "main",
                                 plume::RenderShaderFormat::DXBC);
  std::lock_guard lock(g_shader_link_mutex);
  auto [it, inserted] = gs->linkedShaders.try_emplace(masked, std::move(sh));
  return it->second.get();
#else
  const u8 *cache = DxilCache();
  if (!cache)
    return nullptr;

  const u32 masked = specConstants & entry->specConstantsMask;
  if (entry->specConstantsMask != 0) {
    std::lock_guard lock(g_shader_link_mutex);
    auto it = gs->linkedShaders.find(masked);
    if (it != gs->linkedShaders.end())
      return it->second.get();
  }

  // Miss: build outside the lock. DXC (per-call instances) and D3D12 shader
  // creation are free-threaded, so precache workers link in parallel and a
  // render thread miss waits only on its own build.
  if (entry->specConstantsMask == 0) {
    // The cache DXIL is already a complete vs/ps.
    return PublishShader(
        gs, device->createShader(cache + entry->dxilOffset, entry->dxilSize,
                                 "main", plume::RenderShaderFormat::DXIL));
  }

  // Spec constant shaders ship as DXIL libraries with an unresolved
  // g_SpecConstants() export. reblue_prelink bakes every mask subset of every
  // shipped shader, so the pre-linked lookup is the norm. The runtime DXC link
  // below only covers a prelink/shader cache skew.
  std::unique_ptr<plume::RenderShader> sh;
  const LinkedShaderCacheEntry *pre =
      FindLinkedShaderCacheEntry(entry->hash, masked);
  const u8 *linkedCache = pre ? LinkedDxilCache() : nullptr;
  if (pre && linkedCache) {
    sh = device->createShader(linkedCache + pre->dxilOffset, pre->dxilSize,
                              "main", plume::RenderShaderFormat::DXIL);
  } else {
    // As WWE '12 PC: the shipped exe has no DXC (wwe13_prelink links every variant at build
    // time). Without it a missing variant is not drawn, logged once.
    if (!Wwe13DxcAvailable()) {
      static std::mutex s_missing_mutex;
      static std::unordered_set<u64> s_missing;
      std::lock_guard missing_lock(s_missing_mutex);
      if (s_missing.insert(entry->hash ^ (u64(masked) << 56)).second)
        BD_WARN("GetOrLinkShader: no prelinked variant and no DXC, not drawn "
                "(hash=0x{:016X} mask=0x{:X})",
                entry->hash, masked);
      return nullptr;
    }
    BD_WARN("GetOrLinkShader: no prelinked variant, runtime DXC link "
            "(hash=0x{:016X} mask=0x{:X})",
            entry->hash, masked);
    std::vector<u8> linked =
        LinkSpecConstant(cache + entry->dxilOffset, entry->dxilSize,
                         gs->type == ResourceType::PixelShader, masked);
    if (linked.empty())
      return nullptr;
    sh = device->createShader(linked.data(), linked.size(), "main",
                              plume::RenderShaderFormat::DXIL);
  }
  std::lock_guard lock(g_shader_link_mutex);
  auto [it, inserted] = gs->linkedShaders.try_emplace(masked, std::move(sh));
  return it->second.get();
#endif
}

GuestShader *FindGuestShaderByHash(u64 hash) {
  ShaderCacheEntry *entry = FindShaderCacheEntry(hash);
  if (!entry || !entry->guestShader)
    return nullptr;
  return entry->guestShader;
}

} // namespace bd::gpu
