/**
 * @file    gpu/native_texture_mirror.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/native_texture_mirror.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <cstdio>
#include <string>

#include <fmt/format.h>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/guest_texel_write.h"
#include "gpu/host_resource_heap.h"
#include "gpu/texture_upload.h"
#if defined(SVR_NATIVE_RENDERER)
#include "svr_geometry.h"
#include "wwe12_texture_packs.h"
#endif

namespace bd::gpu {

namespace {

namespace tu = rex::graphics::texture_util;
namespace tc = rex::graphics::texture_conversion;
namespace rg = rex::graphics;
namespace xe = rex::graphics::xenos;

std::mutex g_mirror_mutex;
std::unordered_map<u32, std::unique_ptr<GuestTexture>> g_native_mirrors;

// Asset name per live mirror, normalized to a lowercased basename, so the
// engine can find the loaded instance of a texture it serves and rewrite it.
// The sequence number distinguishes allocations that reuse a VA.
struct NamedNativeTexture {
  std::string name;
  u64 seq = 0;
};
std::unordered_map<u32, NamedNativeTexture> g_native_names;
u64 g_native_name_seq = 0;

std::string NormalizeTextureName(std::string_view raw) {
  const size_t slash = raw.find_last_of("\\/");
  if (slash != std::string_view::npos)
    raw.remove_prefix(slash + 1);
  const size_t dot = raw.rfind('.');
  if (dot != std::string_view::npos)
    raw = raw.substr(0, dot);
  std::string out(raw);
  for (char &c : out)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Evicted (guest thread) but not yet freed. Held per slot until the post-fence
// drain so freeing the texture + rewriting its descriptor can't race an
// in-flight command list.
std::vector<std::unique_ptr<GuestTexture>> g_pending_native_destroy[kNumFrames];

// The name is diagnostic only and BD passes 0 for unnamed allocations, so an
// absent or unmapped name still logs something the line can be read by.
const char *TextureName(u32 name_va) {
  const char *name = bd::mem::str(name_va);
  return *name ? name : "?";
}

// Derived once from the fetch constant and the format's block geometry.
struct MirrorLayout {
  xe::TextureFormat format = xe::TextureFormat::k_8_8_8_8;
  xe::Endian endian = xe::Endian::kNone;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 texels_per_edge = 1;
  u32 bytes_per_block = 1;
  u32 bpb_log2 = 0;
  u32 block_w = 0;
  u32 block_h = 0;
  // Tiled mip-0 footprint in blocks, padded to the 32-block macro tile.
  u32 aligned_w = 0;
  u32 aligned_h = 0;
  u32 staging_row_bytes = 0; // 256-aligned for the D3D12 copy rule
  u32 row_width_texels = 0;
  // A dimension <= 16 sits offset inside the level-0 tile, not at its origin.
  int pack_off_x = 0;
  int pack_off_y = 0;
  // Linear (untiled) storage: rows of blocks at linear_row_pitch bytes. source_bytes bounds the
  // base level's guest footprint either way.
  bool tiled = true;
  u32 linear_row_pitch = 0;
  u32 source_bytes = 0;
  // Distance between cube faces in the guest data.
  u32 face_stride = 0;
};

// Guest memory a texture fetch constant's address names. Blue Dragon's D3D stores virtual addresses
// there. SvR's texture headers hold CPU addresses in a physical alias, which the XDK converts with
// GPU_CONVERT_CPU_TO_GPU_ADDRESS (SvrGpuAddress: the 0xE0000000 alias, 4 KB pages, maps 4 KB
// further on); converting is a no-op for an address that already is a GPU one.
template <typename T> const T *FetchMemory(u32 address) {
#if defined(SVR_NATIVE_RENDERER)
  if (!address)
    return nullptr;
  return REX_KERNEL_MEMORY()->TranslatePhysical<const T *>(SvrGpuAddress(address));
#else
  return bd::mem::at<const T>(address);
#endif
}

// Byte offset of base-level block (bx, by) in the guest data.
i32 BaseBlockOffset(const MirrorLayout &L, int bx, int by);

u32 FetchWidth(const xe::xe_gpu_texture_fetch_t &f) {
  return (f.dimension == xe::DataDimension::k3D ? f.size_3d.width
                                                : f.size_2d.width) +
         1u;
}

u32 FetchHeight(const xe::xe_gpu_texture_fetch_t &f) {
  return (f.dimension == xe::DataDimension::k3D ? f.size_3d.height
                                                : f.size_2d.height) +
         1u;
}

u32 FetchDepth(const xe::xe_gpu_texture_fetch_t &f) {
  return f.dimension == xe::DataDimension::k3D ? f.size_3d.depth + 1u : 1u;
}

bool ReadFetch(u32 guest_va, xe::xe_gpu_texture_fetch_t &fetch) {
  const auto *d3dtex = bd::mem::try_at<const D3DTexture>(guest_va);
  if (!d3dtex)
    return false;
  u32 fc[6];
  for (int i = 0; i < 6; ++i) {
    fc[i] = u32(d3dtex->Format.dword[i]);
  }
  std::memcpy(&fetch, fc, sizeof(fetch));
  return true;
}

MirrorLayout MakeLayout(const xe::xe_gpu_texture_fetch_t &fetch) {
  MirrorLayout L;
  L.format = fetch.format;
  L.endian = fetch.endianness;
  L.width = FetchWidth(fetch);
  L.height = FetchHeight(fetch);
  L.depth = FetchDepth(fetch);

  const rg::FormatInfo *info = rg::FormatInfo::Get(L.format);
  L.texels_per_edge = info->block_width;
  L.bytes_per_block = info->bytes_per_block();
  L.bpb_log2 = rex::log2_floor(L.bytes_per_block);
  L.block_w = rex::align(L.width, L.texels_per_edge) / L.texels_per_edge;
  L.block_h = rex::align(L.height, L.texels_per_edge) / L.texels_per_edge;
  L.aligned_w = rex::align(L.block_w, 32u);
  L.aligned_h = rex::align(L.block_h, 32u);
  L.staging_row_bytes = rex::align(L.block_w * L.bytes_per_block, 256u);
  L.row_width_texels =
      (L.staging_row_bytes / L.bytes_per_block) * L.texels_per_edge;

  if (fetch.packed_mips) {
    u32 px = 0, py = 0, pz = 0;
    tu::GetPackedMipOffset(L.width, L.height, 1u, L.format, 0u, px, py, pz);
    L.pack_off_x = static_cast<int>(px);
    L.pack_off_y = static_cast<int>(py);
  }
  L.tiled = fetch.tiled != 0;
  L.face_stride = L.aligned_w * L.aligned_h * L.bytes_per_block;
  if (L.tiled) {
    L.source_bytes = L.aligned_w * L.aligned_h * L.bytes_per_block;
#if defined(SVR_NATIVE_RENDERER)
    // The tiled base level is laid out with the fetch constant's pitch, like the mip levels
    // (BuildMippedMirror), not with the width rounded up: WWE '12's eye/teeth textures have a wider
    // pitch, and gathering them with the width's turned their base level black.
    if (fetch.dimension == xe::DataDimension::k2DOrStacked) {
      const tu::TextureGuestLayout base_layout = tu::GetGuestTextureLayout(
          xe::DataDimension::k2DOrStacked, fetch.pitch, L.width, L.height, 1u,
          /*is_tiled=*/true, L.format, fetch.packed_mips != 0u, /*has_base=*/true, 0u);
      const u32 pitch_blocks = base_layout.base.row_pitch_bytes / L.bytes_per_block;
      if (pitch_blocks >= L.block_w && base_layout.base.level_data_extent_bytes) {
        L.aligned_w = pitch_blocks;
        L.source_bytes = base_layout.base.level_data_extent_bytes;
      }
    }
#endif
  } else {
    // Cube faces are array slices of the level (6 of them, each its own stride).
    const bool cube = fetch.dimension == xe::DataDimension::kCube;
    const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
        cube ? xe::DataDimension::kCube : xe::DataDimension::k2DOrStacked, fetch.pitch,
        L.width, L.height, cube ? 6u : 1u,
        /*is_tiled=*/false, L.format, fetch.packed_mips != 0u,
        /*has_base=*/true, 0u);
    L.linear_row_pitch = layout.base.row_pitch_bytes;
    L.source_bytes =
        cube ? layout.base.array_slice_data_extent_bytes : layout.base.level_data_extent_bytes;
    if (cube)
      L.face_stride = layout.base.array_slice_stride_bytes;
  }
  return L;
}

i32 BaseBlockOffset(const MirrorLayout &L, int bx, int by) {
  if (L.tiled)
    return tu::GetTiledOffset2D(bx, by, L.aligned_w, L.bpb_log2);
  return by * i32(L.linear_row_pitch) + bx * i32(L.bytes_per_block);
}

size_t FaceStagingSize(const MirrorLayout &L) {
  return size_t(L.staging_row_bytes) * size_t(L.block_h);
}

// Gathers each untiled block from its tiled offset. 'out' is zero-filled by
// the caller, so an out-of-range gather simply leaves its block zero.
void UntileFace(const MirrorLayout &L, const u8 *face_src, u8 *out, int off_x,
                int off_y) {
  const u32 source_size = L.source_bytes;
  const int bpb = static_cast<int>(L.bytes_per_block);
  for (int by = 0; by < static_cast<int>(L.block_h); ++by) {
    for (int bx = 0; bx < static_cast<int>(L.block_w); ++bx) {
      const i32 so = BaseBlockOffset(L, off_x + bx, off_y + by);
      if (so < 0 || u32(so) + L.bytes_per_block > source_size)
        continue;
      u8 *db =
          out + size_t(by) * L.staging_row_bytes + size_t(bx) * size_t(bpb);
      tc::CopySwapBlock(L.endian, db, face_src + so, size_t(bpb));
    }
  }
}

// The tiled footprint pads W/H to 32 blocks and depth to 4.
GuestTexture *BuildVolumeMirror(const MirrorLayout &L, const u8 *src,
                                u32 guest_va) {
  const size_t slice_size = FaceStagingSize(L);
  const size_t src_size = size_t(L.aligned_w) * size_t(L.aligned_h) *
                          size_t(rex::align(L.depth, 4u)) *
                          size_t(L.bytes_per_block);

  std::vector<u8> staging(slice_size * L.depth, 0u);
  for (int z = 0; z < static_cast<int>(L.depth); ++z) {
    for (int by = 0; by < static_cast<int>(L.block_h); ++by) {
      for (int bx = 0; bx < static_cast<int>(L.block_w); ++bx) {
        const i32 so = tu::GetTiledOffset3D(bx, by, z, L.aligned_w, L.aligned_h,
                                            L.bpb_log2);
        if (so < 0 || size_t(so) + L.bytes_per_block > src_size)
          continue;
        u8 *db = staging.data() + size_t(z) * slice_size +
                 size_t(by) * L.staging_row_bytes +
                 size_t(bx) * L.bytes_per_block;
        tc::CopySwapBlock(L.endian, db, src + size_t(so), L.bytes_per_block);
      }
    }
  }

  GuestTexture *tex = BuildBCMirrorTextureVolume(
      L.width, L.height, L.depth, u32(L.format), staging.data(), staging.size(),
      L.staging_row_bytes, L.row_width_texels);
  if (!tex) {
    BD_WARN("native texture va=0x{:08X}: volume mirror build failed "
            "({}x{}x{} fmt=0x{:X})",
            guest_va, L.width, L.height, L.depth, u32(L.format));
    return nullptr;
  }

  // A tfetch2D shader on a 3D resource samples slice 0, and SetTexture routes
  // the volume into both index tables.
  GuestTexture *slice0 = BuildBCMirrorTexture(
      L.width, L.height, u32(L.format), staging.data(), slice_size,
      L.staging_row_bytes, L.row_width_texels);
  if (slice0) {
    tex->companion2D.reset(slice0);
  } else {
    BD_WARN("native texture va=0x{:08X}: slice-0 2D companion build failed "
            "({}x{} fmt=0x{:X}), 3D-as-2D fetch will read null",
            guest_va, L.width, L.height, u32(L.format));
  }
  return tex;
}

// 6 faces packed consecutively from the base address, per-face stride being
// the exact tiled mip-0 footprint with no mip tail or padding between faces.
GuestTexture *BuildCubeMirror(const MirrorLayout &L, const u8 *src,
                              u32 guest_va) {
  // Tiled: the exact mip-0 footprint per face; linear: the layout's array slice stride.
  const size_t face_stride = L.face_stride;
  const size_t face_size = FaceStagingSize(L);

  std::vector<u8> face_bufs[6];
  const u8 *face_ptrs[6];
  for (int face = 0; face < 6; ++face) {
    face_bufs[face].assign(face_size, 0u);
    UntileFace(L, src + size_t(face) * face_stride, face_bufs[face].data(),
               L.pack_off_x, L.pack_off_y);
    face_ptrs[face] = face_bufs[face].data();
  }

  GuestTexture *tex = BuildBCMirrorTextureCube(
      L.width, L.height, u32(L.format), face_ptrs, face_size,
      L.staging_row_bytes, L.row_width_texels);
  if (!tex) {
    BD_WARN("native texture va=0x{:08X}: cube mirror build failed "
            "({}x{} fmt=0x{:X})",
            guest_va, L.width, L.height, u32(L.format));
  }
  return tex;
}

// Levels 1..mip_max live at mip_address, with a packed tail for the <=16-texel
// levels. nullptr leaves the caller on the single-level path.
GuestTexture *BuildMippedMirror(const MirrorLayout &L,
                                const xe::xe_gpu_texture_fetch_t &fetch,
                                std::vector<u8> &base_staging) {
  const auto *mip_src = FetchMemory<u8>(fetch.mip_address << 12);
  const u32 mip_max_level = std::min<u32>(
      fetch.mip_max_level, rex::log2_floor(std::max(L.width, L.height)));
  if (mip_max_level < 1u || !mip_src)
    return nullptr;

  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      xe::DataDimension::k2DOrStacked, fetch.pitch, L.width, L.height, 1u,
      L.tiled, L.format,
      /*has_packed_levels=*/fetch.packed_mips != 0u, /*has_base=*/true,
      mip_max_level);

  // packed_level 0 means the base is in the tail too, so there is no chain.
  if (layout.packed_level == 0u)
    return nullptr;

  std::vector<std::vector<u8>> level_bufs(mip_max_level);
  std::vector<BCMipLevel> mip_levels;
  mip_levels.reserve(mip_max_level + 1u);
  mip_levels.push_back({base_staging.data(), base_staging.size(), L.width,
                        L.height, L.row_width_texels});

  for (u32 lvl = 1u; lvl <= mip_max_level; ++lvl) {
    const u32 lw = std::max(L.width >> lvl, 1u);
    const u32 lh = std::max(L.height >> lvl, 1u);
    const int lwb = int(rex::align(lw, L.texels_per_edge) / L.texels_per_edge);
    const int lhb = int(rex::align(lh, L.texels_per_edge) / L.texels_per_edge);
    const u32 dst_row = rex::align(u32(lwb) * L.bytes_per_block, 256u);
    const u32 dst_row_width_texels =
        (dst_row / L.bytes_per_block) * L.texels_per_edge;

    const bool packed =
        layout.packed_level != UINT32_MAX && lvl >= layout.packed_level;
    const u32 storage_level = packed ? layout.packed_level : lvl;
    const tu::TextureGuestLayout::Level &sl = layout.mips[storage_level];
    const u32 pitch_blocks = sl.row_pitch_bytes / L.bytes_per_block;
    const u8 *level_src = mip_src + layout.mip_offsets_bytes[storage_level];

    u32 px = 0, py = 0, pz = 0;
    if (packed) {
      tu::GetPackedMipOffset(L.width, L.height, 1u, L.format, lvl, px, py, pz);
    }

    std::vector<u8> &dst = level_bufs[lvl - 1u];
    dst.assign(size_t(dst_row) * size_t(lhb), 0u);
    for (int by = 0; by < lhb; ++by) {
      for (int bx = 0; bx < lwb; ++bx) {
        const i32 so =
            L.tiled ? tu::GetTiledOffset2D(i32(px) + bx, i32(py) + by,
                                           pitch_blocks, L.bpb_log2)
                    : (i32(py) + by) * i32(sl.row_pitch_bytes) +
                          (i32(px) + bx) * i32(L.bytes_per_block);
        if (so < 0 || u32(so) + L.bytes_per_block > sl.level_data_extent_bytes)
          continue;
        u8 *db =
            dst.data() + size_t(by) * dst_row + size_t(bx) * L.bytes_per_block;
        tc::CopySwapBlock(L.endian, db, level_src + so, L.bytes_per_block);
      }
    }
    mip_levels.push_back(
        {dst.data(), dst.size(), lw, lh, dst_row_width_texels});
  }

  return BuildBCMirrorTexture2DMips(L.width, L.height, u32(L.format),
                                           mip_levels.data(),
                                           u32(mip_levels.size()));
}

// BD ships static reflection cubes (cube_*) as a 2D DXT atlas of six square
// faces left to right, but the water/glass fetch is a hardware cube (RefEnv,
// fetch slot 5), which would otherwise read the null placeholder. Slice the
// untiled mip-0 atlas into the TextureCube companion that slot publishes.
void AttachCubeAtlasCompanion(const MirrorLayout &L, GuestTexture *tex,
                              const std::vector<u8> &staging, u32 guest_va) {
  const u32 face_blocks_w =
      rex::align(L.height, L.texels_per_edge) / L.texels_per_edge;
  const u32 face_visible_row = face_blocks_w * L.bytes_per_block;
  const u32 face_row_bytes = rex::align(face_visible_row, 256u);
  const u32 face_size = face_row_bytes * L.block_h;
  const u32 face_row_width_texels =
      (face_row_bytes / L.bytes_per_block) * L.texels_per_edge;

  std::vector<u8> face_bufs[6];
  const u8 *face_ptrs[6];
  for (u32 f = 0; f < 6u; ++f) {
    face_bufs[f].assign(face_size, 0u);
    for (u32 by = 0; by < L.block_h; ++by) {
      const u8 *sb = staging.data() + size_t(by) * L.staging_row_bytes +
                     size_t(f) * face_visible_row;
      std::memcpy(face_bufs[f].data() + size_t(by) * face_row_bytes, sb,
                  face_visible_row);
    }
    face_ptrs[f] = face_bufs[f].data();
  }

  GuestTexture *cube = BuildBCMirrorTextureCube(
      L.height, L.height, u32(L.format), face_ptrs, face_size, face_row_bytes,
      face_row_width_texels);
  if (cube) {
    tex->companionCube.reset(cube);
  } else {
    BD_WARN("native texture va=0x{:08X}: cube_* atlas companion build failed "
            "({}x{} fmt=0x{:X}), water/glass cube fetch reads null",
            guest_va, L.height, L.height, u32(L.format));
  }
}

#if defined(SVR_NATIVE_RENDERER)
// 16-bit packed colour formats have no host format: expanded to RGBA8 (one level). Xenos names
// components from the high bits down, so x (red) is in the low bits, as in k_2_10_10_10.
bool IsPacked16(xe::TextureFormat format) {
  return format == xe::TextureFormat::k_1_5_5_5 || format == xe::TextureFormat::k_5_6_5 ||
         format == xe::TextureFormat::k_4_4_4_4;
}

GuestTexture *BuildPacked16Mirror(const MirrorLayout &L, const std::vector<u8> &staging) {
  const u32 row = rex::align(L.width * 4u, 256u);
  std::vector<u8> rgba(size_t(row) * L.height, 0u);
  const auto expand5 = [](u32 c) { return u8((c << 3) | (c >> 2)); };
  const auto expand6 = [](u32 c) { return u8((c << 2) | (c >> 4)); };
  for (u32 y = 0; y < L.height; ++y) {
    for (u32 x = 0; x < L.width; ++x) {
      // After the endian swap the texel is in GPU (little-endian) byte order.
      const u8 *t = staging.data() + size_t(y) * L.staging_row_bytes + size_t(x) * 2u;
      const u32 v = u32(t[0]) | (u32(t[1]) << 8);
      u8 *o = rgba.data() + size_t(y) * row + size_t(x) * 4u;
      switch (L.format) {
      case xe::TextureFormat::k_1_5_5_5:
        o[0] = expand5(v & 31u);
        o[1] = expand5((v >> 5) & 31u);
        o[2] = expand5((v >> 10) & 31u);
        o[3] = (v & 0x8000u) ? 255 : 0;
        break;
      case xe::TextureFormat::k_5_6_5:
        o[0] = expand5(v & 31u);
        o[1] = expand6((v >> 5) & 63u);
        o[2] = expand5((v >> 11) & 31u);
        o[3] = 255;
        break;
      default:  // k_4_4_4_4
        o[0] = u8((v & 15u) * 17u);
        o[1] = u8(((v >> 4) & 15u) * 17u);
        o[2] = u8(((v >> 8) & 15u) * 17u);
        o[3] = u8(((v >> 12) & 15u) * 17u);
        break;
      }
    }
  }
  return BuildBCMirrorTexture(L.width, L.height, u32(xe::TextureFormat::k_8_8_8_8), rgba.data(),
                              rgba.size(), row, row / 4u);
}
#endif

#if defined(SVR_NATIVE_RENDERER)
// Dev aid: SVR_DUMP_TEXTURES=<dir> writes each 2D texture's decoded base level as a .dds there
// (DXT1/3/5 and 8:8:8:8 only), to look at what the mirror made of it.
void DumpBaseLevel(const MirrorLayout &L, const xe::xe_gpu_texture_fetch_t &fetch,
                   const std::vector<u8> &staging, u32 guest_va) {
  static const char *dir = std::getenv("SVR_DUMP_TEXTURES");
  if (!dir || !*dir)
    return;
  u32 fourcc = 0;
  switch (L.format) {
  case xe::TextureFormat::k_DXT1: fourcc = 0x31545844; break;    // DXT1
  case xe::TextureFormat::k_DXT2_3: fourcc = 0x33545844; break;  // DXT3
  case xe::TextureFormat::k_DXT4_5: fourcc = 0x35545844; break;  // DXT5
  case xe::TextureFormat::k_8_8_8_8: break;
  default: return;
  }
  static std::atomic<u32> s_count{0};
  if (s_count.fetch_add(1) >= 20000)
    return;
  // Unique per mirror build: the game reuses texture objects (same VA) for other textures. The
  // base address matches the fetch constants [drawlog] prints (dword 1, bits 12+).
  const std::string path = fmt::format("{}/{:04}_{:08X}_base{:08X}_{}x{}_f{}_p{}_m{}.dds", dir,
                                       s_count.load(), guest_va, fetch.base_address << 12,
                                       L.width, L.height, u32(L.format), u32(fetch.pitch),
                                       u32(fetch.mip_max_level));
  FILE *f = std::fopen(path.c_str(), "wb");
  if (!f)
    return;
  u32 h[32] = {};
  h[0] = 0x20534444;  // "DDS "
  h[1] = 124;
  h[2] = 0x1 | 0x2 | 0x4 | 0x1000;
  h[3] = L.height;
  h[4] = L.width;
  h[5] = L.block_w * L.bytes_per_block;
  h[19] = 32;
  if (fourcc) {
    h[20] = 0x4;
    h[21] = fourcc;
  } else {
    h[20] = 0x41;
    h[22] = 32;
    h[23] = 0x000000FF;
    h[24] = 0x0000FF00;
    h[25] = 0x00FF0000;
    h[26] = 0xFF000000;
  }
  h[27] = 0x1000;
  std::fwrite(h, 4, 32, f);
  const u32 row = L.block_w * L.bytes_per_block;
  for (u32 by = 0; by < L.block_h; ++by)
    std::fwrite(staging.data() + size_t(by) * L.staging_row_bytes, 1, row, f);
  std::fclose(f);
}
#endif

#if defined(SVR_NATIVE_RENDERER)
// Set by Build2DMirror when a texture pack replaced the texture: its pixels are already as they
// look, so GetOrCreateNativeMirror leaves the fetch swizzle out.
thread_local bool t_replaced = false;

// A texture pack's replacement (wwe12_texture_packs.h) as a mirror, at its own size, with its levels.
GuestTexture *BuildReplacementMirror(const wwe12::texture_packs::Replacement &r) {
  using F = wwe12::texture_packs::Format;
  const xe::TextureFormat format = r.format == F::kBC1   ? xe::TextureFormat::k_DXT1
                                   : r.format == F::kBC2 ? xe::TextureFormat::k_DXT2_3
                                   : r.format == F::kBC3 ? xe::TextureFormat::k_DXT4_5
                                                         : xe::TextureFormat::k_8_8_8_8;
  const u32 edge = r.format == F::kRGBA8 ? 1u : 4u;
  const u32 bpb = r.format == F::kRGBA8 ? 4u : r.format == F::kBC1 ? 8u : 16u;
  std::vector<std::vector<u8>> rows(r.levels.size());
  std::vector<BCMipLevel> levels;
  for (size_t l = 0; l < r.levels.size(); ++l) {
    const u32 lw = std::max(r.width >> l, 1u), lh = std::max(r.height >> l, 1u);
    const u32 bw = (lw + edge - 1) / edge, bh = (lh + edge - 1) / edge;
    const size_t tight = size_t(bw) * bpb, pitch = rex::align<size_t>(tight, 256);
    if (r.levels[l].size() < tight * bh)
      break;
    rows[l].assign(pitch * bh, 0u);  // rows 256-byte aligned, as the uploader wants
    for (u32 y = 0; y < bh; ++y)
      std::memcpy(&rows[l][y * pitch], &r.levels[l][y * tight], tight);
    levels.push_back({rows[l].data(), rows[l].size(), lw, lh, u32(pitch / bpb) * edge});
  }
  if (levels.empty())
    return nullptr;
  return BuildBCMirrorTexture2DMips(r.width, r.height, u32(format), levels.data(), u32(levels.size()));
}
#endif

GuestTexture *Build2DMirror(const MirrorLayout &L, const u8 *src,
                            const xe::xe_gpu_texture_fetch_t &fetch,
                            u32 guest_va, u32 name_va, bool dxt) {
  std::vector<u8> staging(FaceStagingSize(L), 0u);
  UntileFace(L, src, staging.data(), L.pack_off_x, L.pack_off_y);
#if defined(SVR_NATIVE_RENDERER)
  DumpBaseLevel(L, fetch, staging, guest_va);
  // Texture dumps and packs (wwe12_texture_packs.h): the top level's content hash.
  if ((dxt || L.format == xe::TextureFormat::k_8_8_8_8) && wwe12::texture_packs::Active()) {
    using F = wwe12::texture_packs::Format;
    const F format = L.format == xe::TextureFormat::k_DXT1     ? F::kBC1
                     : L.format == xe::TextureFormat::k_DXT2_3 ? F::kBC2
                     : L.format == xe::TextureFormat::k_DXT4_5 ? F::kBC3
                                                               : F::kRGBA8;
    const u64 hash = wwe12::texture_packs::HashBlocks(staging.data(), L.staging_row_bytes, L.block_w,
                                                      L.block_h, L.bytes_per_block);
    wwe12::texture_packs::Dump(hash, L.width, L.height, format, staging.data(), L.staging_row_bytes,
                               fetch.swizzle);
    if (const auto r = wwe12::texture_packs::Find(hash)) {
      if (GuestTexture *tex = BuildReplacementMirror(*r)) {
        t_replaced = true;
        return tex;
      }
    }
  }
#endif
#if defined(SVR_NATIVE_RENDERER)
  if (IsPacked16(L.format)) {
    GuestTexture *tex = BuildPacked16Mirror(L, staging);
    if (!tex)
      BD_WARN("native texture va=0x{:08X}: 16-bit mirror build failed ({}x{} fmt=0x{:X})",
              guest_va, L.width, L.height, u32(L.format));
    return tex;
  }
#endif

#if defined(SVR_NATIVE_RENDERER)
  // Dev switch: SVR_MIRROR_NO_MIPS=1 mirrors the base level only (to isolate mip decoding).
  static const bool no_mips = [] {
    const char *env = std::getenv("SVR_MIRROR_NO_MIPS");
    return env && *env == '1';
  }();
  GuestTexture *tex = no_mips ? nullptr : BuildMippedMirror(L, fetch, staging);
#else
  GuestTexture *tex = BuildMippedMirror(L, fetch, staging);
#endif
  if (!tex) {
    tex = BuildBCMirrorTexture(L.width, L.height, u32(L.format),
                                      staging.data(), staging.size(),
                                      L.staging_row_bytes, L.row_width_texels);
  }
  if (!tex) {
    BD_WARN("native texture va=0x{:08X}: 2D mirror build failed "
            "({}x{} fmt=0x{:X})",
            guest_va, L.width, L.height, u32(L.format));
    return nullptr;
  }

  const bool is_cube_atlas =
      std::strncmp(bd::mem::str(name_va), "cube_", 5) == 0 && dxt &&
      L.height > 0u && L.width == L.height * 6u;
  if (is_cube_atlas) {
    AttachCubeAtlasCompanion(L, tex, staging, guest_va);
  }
  return tex;
}

#if defined(SVR_NATIVE_RENDERER)
// Xenos fetch constants carry a component swizzle, 3 bits per result channel (0-3 = source
// X/Y/Z/W, 4 = 0, 5 = 1); 0x688 is plain RGBA. SvR 2009 binds some textures with other swizzles
// (BGRA logos came out with red and blue swapped). The mirror keeps the data as stored and its
// view applies the swizzle; the bindless descriptor, written when the mirror was built, is
// pointed at that view.
void ApplyFetchSwizzle(GuestTexture *tex, const xe::xe_gpu_texture_fetch_t &fetch, u32 guest_va) {
  constexpr u32 kRgba = 0x688;
  const u32 swizzle = fetch.swizzle;
  if (swizzle == kRgba || !tex->texture || tex->format == plume::RenderFormat::UNKNOWN)
    return;
  using S = plume::RenderSwizzle;
  static constexpr S kSource[8] = {S::R, S::G, S::B, S::A, S::ZERO, S::ONE, S::IDENTITY, S::IDENTITY};
  plume::RenderTextureViewDesc view_desc;
  view_desc.format = tex->format;
  view_desc.dimension = tex->viewDimension != plume::RenderTextureViewDimension::UNKNOWN
                            ? tex->viewDimension
                            : plume::RenderTextureViewDimension::TEXTURE_2D;
  view_desc.mipLevels = tex->mipLevels ? tex->mipLevels : 1;
  view_desc.componentMapping = plume::RenderComponentMapping(
      kSource[swizzle & 7], kSource[(swizzle >> 3) & 7], kSource[(swizzle >> 6) & 7],
      kSource[(swizzle >> 9) & 7]);
  auto view = tex->texture->createTextureView(view_desc);
  if (!view)
    return;
  tex->textureView = std::move(view);
  auto &s = state();
  if (tex->descriptorIndex != kInvalidDescriptorIndex && s.texture_descriptor_set)
    s.texture_descriptor_set->setTexture(tex->descriptorIndex, tex->texture,
                                         plume::RenderTextureLayout::SHADER_READ,
                                         tex->textureView.get());
  static std::unordered_map<u32, bool> logged;
  if (logged.emplace(swizzle, true).second)
    BD_INFO("native texture va=0x{:08X}: fetch swizzle 0x{:03X} applied (fmt=0x{:X})", guest_va,
            swizzle, u32(fetch.format));
}
#endif

} // namespace

GuestTexture *GetOrCreateNativeMirror(u32 guest_va, u32 name_va) {
  if (!guest_va)
    return nullptr;
  std::lock_guard<std::mutex> lock(g_mirror_mutex);

  auto it = g_native_mirrors.find(guest_va);
  if (it != g_native_mirrors.end()) {
    // From the bdAllocRenderBuffer hook a hit means the engine re-malloc'd a
    // freed texture struct at this VA: the previous occupant was released via
    // the preload table path (hcgTextureListRelease) that we do not hook,
    // leaving this mirror stale. From NativeTextureReplace it is the rewritten
    // payload wanting a fresh mirror. Either way, evict and rebuild.
    g_pending_native_destroy[Video::RetireSlot("native mirror")].push_back(
        std::move(it->second));
    g_native_mirrors.erase(it);
  }

  xe::xe_gpu_texture_fetch_t fetch;
  if (!ReadFetch(guest_va, fetch)) {
    BD_WARN("native texture: guest VA 0x{:08X} does not translate", guest_va);
    return nullptr;
  }

  const bool dxt = (fetch.format == xe::TextureFormat::k_DXT1 ||
                    fetch.format == xe::TextureFormat::k_DXT2_3 ||
                    fetch.format == xe::TextureFormat::k_DXT4_5);
  // Uncompressed k_8_8_8_8 (color ramps / UI atlases BD stores raw, e.g. the
  // cloud sky LUT dg03_cbr01). 2D, and cubes: WWE '13 binds 64x64 tiled k_8_8_8_8 cubes at
  // fetch slot 11 in entrances (11-21: they fell back to the green marker). The cube path gathers
  // per block, so 4-byte blocks need nothing else. Volumes still assume DXT blocks.
  const bool rgba8 = fetch.format == xe::TextureFormat::k_8_8_8_8 &&
                     (fetch.dimension <= xe::DataDimension::k2DOrStacked ||
                      fetch.dimension == xe::DataDimension::kCube);
#if defined(SVR_NATIVE_RENDERER)
  // SvR also samples single-channel k_8 (the Bink Y/cR/cB planes), k_8_8, 16-bit and float
  // formats (lookup tables) and 16-bit packed colour (icons, expanded to RGBA8), 2D only.
  const bool plain8 =
      (fetch.format == xe::TextureFormat::k_8 || fetch.format == xe::TextureFormat::k_8_8 ||
       fetch.format == xe::TextureFormat::k_16 || fetch.format == xe::TextureFormat::k_16_16 ||
       fetch.format == xe::TextureFormat::k_16_16_16_16 ||
       fetch.format == xe::TextureFormat::k_16_FLOAT ||
       fetch.format == xe::TextureFormat::k_16_16_FLOAT ||
       fetch.format == xe::TextureFormat::k_16_16_16_16_FLOAT ||
       fetch.format == xe::TextureFormat::k_32_FLOAT ||
       fetch.format == xe::TextureFormat::k_32_32_FLOAT ||
       fetch.format == xe::TextureFormat::k_32_32_32_32_FLOAT || IsPacked16(fetch.format)) &&
      fetch.dimension <= xe::DataDimension::k2DOrStacked;
  // SvR textures are linear: 2D and cube mirrors read linear data; volumes still assume tiled.
  const bool tiling_ok = fetch.tiled || fetch.dimension <= xe::DataDimension::k2DOrStacked ||
                         fetch.dimension == xe::DataDimension::kCube;
#else
  const bool plain8 = false;
  const bool tiling_ok = fetch.tiled != 0;
#endif
  const bool supported_dim = fetch.dimension != xe::DataDimension::k1D;
  // DXT ships endian kNone/k8in16, and k_8_8_8_8 ships k8in32 (full word swap).
  const bool endian_ok =
      fetch.endianness <= (dxt ? xe::Endian::k8in16 : xe::Endian::k8in32);

  // Non-tiled is rejected outside SvR: Blue Dragon's textures are all tiled.
  if (fetch.type != xe::FetchConstantType::kTexture || !supported_dim ||
      !(dxt || rgba8 || plain8) || !endian_ok || !tiling_ok) {
    BD_WARN("native texture rejected name='{}' va=0x{:08X} type={} dim={} "
            "fmt=0x{:X} endian={} tiled={} {}x{}x{}",
            TextureName(name_va), guest_va, u32(fetch.type),
            u32(fetch.dimension), u32(fetch.format), u32(fetch.endianness),
            u32(fetch.tiled), FetchWidth(fetch), FetchHeight(fetch),
            FetchDepth(fetch));
    return nullptr;
  }

  // The XPhysicalAllocEx VA the engine wrote through, so the heap-aware
  // accessor matches how the data was written.
  const auto *src = FetchMemory<u8>(fetch.base_address << 12);
  if (!src) {
    BD_WARN("native texture va=0x{:08X}: base address 0x{:08X} does not "
            "translate",
            guest_va, fetch.base_address << 12);
    return nullptr;
  }

  const MirrorLayout layout = MakeLayout(fetch);
#if defined(SVR_NATIVE_RENDERER)
  // Diagnostic: a base level that is all zero while the mip chain holds data (WWE '12's eyes turn
  // black when the base level is sampled).
  {
    static std::atomic<u32> s_logged{0};
    const u32 base_bytes = std::min<u32>(layout.source_bytes, 1u << 20);
    bool base_zero = base_bytes > 0;
    for (u32 i = 0; i < base_bytes && base_zero; ++i)
      base_zero = src[i] == 0;
    const u8 *mips = fetch.mip_address ? FetchMemory<u8>(fetch.mip_address << 12) : nullptr;
    bool mips_data = false;
    for (u32 i = 0; mips && i < 4096 && !mips_data; ++i)
      mips_data = mips[i] != 0;
    if (base_zero && mips_data && s_logged.fetch_add(1) < 64)
      BD_WARN("native texture va=0x{:08X} name='{}': base level empty, mips not ({}x{} fmt 0x{:X} "
              "tiled {} packed {} pitch {} base 0x{:08X} mips 0x{:08X} levels {})",
              guest_va, TextureName(name_va), layout.width, layout.height, u32(fetch.format),
              u32(fetch.tiled), u32(fetch.packed_mips), u32(fetch.pitch),
              fetch.base_address << 12, fetch.mip_address << 12, u32(fetch.mip_max_level) + 1);
  }
#endif
  GuestTexture *tex = nullptr;
#if defined(SVR_NATIVE_RENDERER)
  t_replaced = false;
#endif
  switch (fetch.dimension) {
  case xe::DataDimension::k3D:
    tex = BuildVolumeMirror(layout, src, guest_va);
    break;
  case xe::DataDimension::kCube:
    tex = BuildCubeMirror(layout, src, guest_va);
#if defined(SVR_NATIVE_RENDERER)
    // Diagnostic (11-21): whether an uncompressed cube holds data in guest memory when it is
    // first built, or is all zero (then the game fills it some other way, e.g. a GPU resolve).
    if (tex && fetch.format == xe::TextureFormat::k_8_8_8_8) {
      static std::atomic<u32> s_cube_logged{0};
      if (s_cube_logged.fetch_add(1) < 24) {
        const size_t n = std::min<size_t>(size_t(layout.face_stride) * 6u, 1u << 20);
        size_t nonzero = 0;
        for (size_t i = 0; i < n; ++i)
          nonzero += src[i] != 0;
        BD_INFO("native texture va=0x{:08X}: rgba8 cube {}x{} tiled {} swizzle 0x{:03X}, {} of {} "
                "bytes non-zero",
                guest_va, layout.width, layout.height, u32(fetch.tiled), u32(fetch.swizzle),
                nonzero, n);
      }
    }
#endif
    break;
  default:
    tex = Build2DMirror(layout, src, fetch, guest_va, name_va, dxt);
    break;
  }
  if (!tex)
    return nullptr;
#if defined(SVR_NATIVE_RENDERER)
  if (!t_replaced)
    ApplyFetchSwizzle(tex, fetch, guest_va);
#endif

  auto stored = std::unique_ptr<GuestTexture>(tex);
  GuestTexture *raw = stored.get();
  g_native_mirrors.emplace(guest_va, std::move(stored));
  // A rebuild through NativeTextureReplace passes no name and keeps the entry
  // the allocation registered, sequence included.
  if (name_va) {
    const char *name = bd::mem::str(name_va);
    if (*name)
      g_native_names[guest_va] = {NormalizeTextureName(name),
                                  ++g_native_name_seq};
  }
  return raw;
}

TextureContent TextureContent::Scan(u32 guest_va) {
  const TextureContent whole;
  xe::xe_gpu_texture_fetch_t fetch;
  if (!ReadFetch(guest_va, fetch) ||
      fetch.type != xe::FetchConstantType::kTexture ||
      fetch.dimension != xe::DataDimension::k2DOrStacked ||
      !rg::FormatInfo::Get(fetch.format))
    return whole;

  const MirrorLayout L = MakeLayout(fetch);
  if (!L.width || !L.height || !L.bytes_per_block || !L.texels_per_edge)
    return whole;

  const u32 base = fetch.base_address << 12;
  const u32 blocks =
      fetch.tiled ? L.aligned_w * L.aligned_h : L.block_w * L.block_h;
  const auto *src = bd::mem::try_at<const u8>(base);
  // Both ends, since try_at validates an address and not a range.
  if (!src || !bd::mem::try_at<const u8>(base + blocks * L.bytes_per_block - 1))
    return whole;

  const auto block_at = [&](u32 bx, u32 by) -> const u8 * {
    if (!fetch.tiled)
      return src + (size_t(by) * L.block_w + bx) * L.bytes_per_block;
    const i32 offset =
        tu::GetTiledOffset2D(i32(bx), i32(by), L.aligned_w, L.bpb_log2);
    return offset < 0 ? nullptr : src + size_t(offset);
  };

  const u8 *field = block_at(0, 0);
  if (!field)
    return whole;

  u32 min_x = L.block_w;
  u32 min_y = L.block_h;
  u32 max_x = 0;
  u32 max_y = 0;
  for (u32 by = 0; by < L.block_h; ++by) {
    for (u32 bx = 0; bx < L.block_w; ++bx) {
      const u8 *block = block_at(bx, by);
      if (!block || std::memcmp(block, field, L.bytes_per_block) == 0)
        continue;
      min_x = std::min(min_x, bx);
      max_x = std::max(max_x, bx);
      min_y = std::min(min_y, by);
      max_y = std::max(max_y, by);
    }
  }
  if (min_x > max_x || min_y > max_y)
    return whole;

  TextureContent out;
  out.u0 = float(min_x * L.texels_per_edge) / float(L.width);
  out.v0 = float(min_y * L.texels_per_edge) / float(L.height);
  out.u1 =
      std::min(1.0f, float((max_x + 1) * L.texels_per_edge) / float(L.width));
  out.v1 =
      std::min(1.0f, float((max_y + 1) * L.texels_per_edge) / float(L.height));
  return out;
}

bool GetNativeMirrorSource(u32 guest_va, NativeMirrorSource &out) {
  xe::xe_gpu_texture_fetch_t fetch;
  if (!ReadFetch(guest_va, fetch))
    return false;
  std::memcpy(out.fetch, &fetch, sizeof(out.fetch));
  if (fetch.type != xe::FetchConstantType::kTexture ||
      !rg::FormatInfo::Get(fetch.format))
    return false;
  const u32 width = FetchWidth(fetch);
  const u32 height = FetchHeight(fetch);
  const u32 depth = FetchDepth(fetch);
  const u32 mip_max_level = std::min<u32>(
      fetch.mip_max_level, rex::log2_floor(std::max(width, height)));
  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      fetch.dimension == xe::DataDimension::k3D ? xe::DataDimension::k3D
                                                : xe::DataDimension::k2DOrStacked,
      fetch.pitch, width, height,
      fetch.dimension == xe::DataDimension::kCube ? 6u : depth,
      fetch.tiled != 0, fetch.format, fetch.packed_mips != 0u,
      /*has_base=*/true, mip_max_level);
  // Physical memory is 512 MB; a range is cut at its end rather than read past it.
  const auto clamp = [](u32 address, size_t size) -> size_t {
    const u32 offset = SvrGpuAddress(address) & 0x1FFFFFFFu;
    return std::min<size_t>(size, 0x20000000u - offset);
  };
  out.base = FetchMemory<u8>(fetch.base_address << 12);
  out.base_size =
      out.base ? clamp(fetch.base_address << 12, layout.base.level_data_extent_bytes) : 0;
  if (mip_max_level && fetch.mip_address) {
    out.mips = FetchMemory<u8>(fetch.mip_address << 12);
    out.mips_size =
        out.mips ? clamp(fetch.mip_address << 12, layout.mips_total_extent_bytes) : 0;
  }
  return out.base != nullptr;
}

GuestTexture *ResolveGuestTexture(u32 guest_va) {
  if (!guest_va)
    return nullptr;

  if (auto *host = HostResourceHeap::FromGuest<GuestTexture>(guest_va)) {
    return host;
  }

  // Pure lookup, since mirrors are built eagerly at alloc time. Building on
  // demand would mis-decode arbitrary bytes as a fetch constant and overread
  // small allocations.
  {
    std::lock_guard<std::mutex> lock(g_mirror_mutex);
    auto it = g_native_mirrors.find(guest_va);
    if (it != g_native_mirrors.end())
      return it->second.get();
  }
  return nullptr;
}

void EvictNativeTexture(u32 guest_va) {
  if (!guest_va)
    return;
  std::lock_guard<std::mutex> lock(g_mirror_mutex);
  auto it = g_native_mirrors.find(guest_va);
  if (it == g_native_mirrors.end())
    return;
  // Defer teardown to the post-fence drain, and remove from the registry now so
  // a same-VA re-alloc builds a fresh mirror.
  g_pending_native_destroy[Video::RetireSlot("native mirror")].push_back(
      std::move(it->second));
  g_native_mirrors.erase(it);
  g_native_names.erase(guest_va);
}

std::vector<NativeTextureRef> NativeTexturesByName(std::string_view name) {
  const std::string wanted = NormalizeTextureName(name);
  std::vector<NativeTextureRef> out;
  std::lock_guard<std::mutex> lock(g_mirror_mutex);
  for (const auto &[va, entry] : g_native_names) {
    if (entry.name == wanted)
      out.push_back({va, entry.seq});
  }
  return out;
}

bool NativeTextureReplace(u32 guest_va, const u8 *blob, size_t size) {
  if (!blob || size < 2048u)
    return false;
  const auto *d3dtex = bd::mem::at<const D3DTexture>(guest_va);
  if (!d3dtex)
    return false;

  u32 fc[6];
  for (int i = 0; i < 6; ++i)
    fc[i] = u32(d3dtex->Format.dword[i]);
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fc, sizeof(fetch));

  u32 blobFc[6];
  for (int i = 0; i < 6; ++i) {
    const u8 *w = blob + 4u + 28u + u32(i) * 4u;
    blobFc[i] = (u32(w[0]) << 24) | (u32(w[1]) << 16) | (u32(w[2]) << 8) |
                u32(w[3]);
  }
  xe::xe_gpu_texture_fetch_t blobFetch;
  std::memcpy(&blobFetch, blobFc, sizeof(blobFetch));

  // Same geometry or nothing: the physical allocation was sized for the live
  // texture, and the mirror build below trusts the live fetch constant.
  if (blobFetch.format != fetch.format || blobFc[2] != fc[2] ||
      blobFetch.pitch != fetch.pitch || !fetch.tiled ||
      fetch.dimension != xe::DataDimension::k2DOrStacked || fetch.mip_address) {
    BD_WARN("native texture replace refused va=0x{:08X}: geometry mismatch",
            guest_va);
    return false;
  }

  const MirrorLayout L = MakeLayout(fetch);
  const size_t payload =
      size_t(L.aligned_w) * size_t(L.aligned_h) * size_t(L.bytes_per_block);
  const u32 blobPayload = (u32(blob[0]) << 24) | (u32(blob[1]) << 16) |
                          (u32(blob[2]) << 8) | u32(blob[3]);
  if (blobPayload != payload || size < 2048u + payload) {
    BD_WARN("native texture replace refused va=0x{:08X}: payload {} vs {}",
            guest_va, blobPayload, payload);
    return false;
  }

  auto *dst = bd::mem::at<u8>(fetch.base_address << 12);
  if (!dst)
    return false;
  std::memcpy(dst, blob + 2048, payload);
  return GetOrCreateNativeMirror(guest_va) != nullptr;
}

void DrainEvictedNativeTextures(u32 slot) {
  std::vector<std::unique_ptr<GuestTexture>> dead;
  {
    std::lock_guard<std::mutex> lock(g_mirror_mutex);
    dead.swap(g_pending_native_destroy[slot]);
  }
  // Lock released before NotifyTextureDestroyed, which takes Video's mutex.
  for (auto &tex : dead) {
    Video::NotifyTextureDestroyed(tex.get());
    // Only fence(slot) was awaited, and the other in-flight slot may still
    // reference the image/view, so park them instead of freeing here.
    ParkTextureGPUObjects(tex.get());
  }
}

#if defined(SVR_NATIVE_RENDERER)
bool ReadGuestTextureBaseRaw(u32 guest_va, std::vector<u8> &raw, u32 &pitch_blocks) {
  xe::xe_gpu_texture_fetch_t fetch{};
  if (!ReadFetch(guest_va, fetch))
    return false;
  const MirrorLayout L = MakeLayout(fetch);
  const auto *src = FetchMemory<u8>(u32(fetch.base_address) << 12);
  if (!src)
    return false;
  raw.assign(src, src + L.source_bytes);
  pitch_blocks = L.tiled ? L.aligned_w : 0;
  return true;
}

bool WriteGuestTextureBase(u32 guest_va, const u8 *rgba, u32 row_pitch, u32 width, u32 height,
                           std::string *out_range) {
  xe::xe_gpu_texture_fetch_t fetch{};
  // Packed mips only move the small levels into a shared tail; a base level wider and taller
  // than 16 texels keeps its own place (the created superstar's 736x1280 portrait, 11 levels).
  if (!rgba || !ReadFetch(guest_va, fetch) ||
      fetch.dimension != xe::DataDimension::k2DOrStacked ||
      (fetch.packed_mips && (width <= 16 || height <= 16))) {
    if (out_range)
      *out_range = fmt::format("not a 2D texture (dimension {}, packed mips {})",
                               u32(fetch.dimension), u32(fetch.packed_mips));
    return false;
  }
  if (fetch.format != xe::TextureFormat::k_8_8_8_8 &&
      fetch.format != xe::TextureFormat::k_4_4_4_4) {
    if (out_range)
      *out_range = fmt::format("format {} not handled", u32(fetch.format));
    return false;
  }
  const MirrorLayout L = MakeLayout(fetch);
  if (L.width != width || L.height != height) {
    if (out_range)
      *out_range = fmt::format("header now {}x{}", L.width, L.height);
    return false;
  }
  if (out_range)
    *out_range = fmt::format("phys {:08X}-{:08X} fmt {} {}", u32(fetch.base_address) << 12,
                             (u32(fetch.base_address) << 12) + L.source_bytes, u32(fetch.format),
                             fetch.tiled ? "tiled" : "linear");
  // Writable view of the same memory the mirror reads.
  auto *dst = const_cast<u8 *>(FetchMemory<u8>(fetch.base_address << 12));
  if (!dst)
    return false;
  const bool packed16 = fetch.format == xe::TextureFormat::k_4_4_4_4;
  for (u32 y = 0; y < height; ++y) {
    const u8 *row = rgba + size_t(y) * row_pitch;
    for (u32 x = 0; x < width; ++x) {
      const u8 *t = row + size_t(x) * 4u;
      // GPU (little-endian) texel, then the fetch's endian swap. What the CPU reads is a
      // D3DFMT_A8R8G8B8 / A4R4G4B4 render target resolve: blue in the low bits (written red
      // first, custom attires came out with blue skin).
      u8 texel[4];
      if (packed16) {
        const u32 v = u32(t[2] >> 4) | (u32(t[1] >> 4) << 4) | (u32(t[0] >> 4) << 8) |
                      (u32(t[3] >> 4) << 12);
        texel[0] = u8(v);
        texel[1] = u8(v >> 8);
      } else {
        texel[0] = t[2];
        texel[1] = t[1];
        texel[2] = t[0];
        texel[3] = t[3];
      }
      // Inline tiling and swap (guest_texel_write.h): same bytes, no DLL calls per texel.
      const i32 offset =
          L.tiled ? texel_write::TiledOffset2D(i32(x), i32(y), L.aligned_w, L.bpb_log2)
                  : i32(y) * i32(L.linear_row_pitch) + i32(x) * i32(L.bytes_per_block);
      if (offset < 0 || u32(offset) + L.bytes_per_block > L.source_bytes)
        continue;
      if (!texel_write::CopySwapSmall(u32(L.endian), dst + offset, texel, L.bytes_per_block))
        tc::CopySwapBlock(L.endian, dst + offset, texel, L.bytes_per_block);
    }
  }
  return true;
}
#endif

} // namespace bd::gpu

// Midasm hook at global scope, externed by the generated recompiler dispatch,
// so name and signature must match the hook config. Fires in
// hcgTextureListRelease's refcount-zero block when the name-refcounted
// preload table releases a native texture's last reference. hcgTextureCacheAdd
// registered flt_82DBB148[k] with bdAllocRenderBuffer's return (== the
// g_native_mirrors key) and unk_82DBB170[k] with the XPhysical allocation. That
// free path calls XPhysicalFree directly, bypassing LoadTexture__vf03, so the
// host mirror must be evicted here.
//   r25 = flt_82DBB148[k] = mirror key   r26 = unk_82DBB170[k] = XPhysical
//   marker
void bdPreloadTableFreeTextureHook(PPCRegister &r25, PPCRegister &r26) {
  if (r26.u32)
    bd::gpu::EvictNativeTexture(r25.u32);
}
