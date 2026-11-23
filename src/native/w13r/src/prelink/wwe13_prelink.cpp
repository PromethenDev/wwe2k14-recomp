// wwe13_prelink: build tool for wwe13_d3d12.exe (WWE '12 PC's wwe12_prelink). Links every spec-constant variant of the game's
// DXIL shaders ahead of time, so the exe never needs DXC (dxcompiler.dll / dxil.dll, whose
// Microsoft licence we do not ship under) at run time and never links on the render thread.
//
// The shader cache (generated/native/shader_cache.cpp, linked into this tool) holds each shader's
// DXIL; a shader with a non-zero specConstantsMask is a DXIL library with an unresolved
// g_SpecConstants() export. guest_shaders.cpp looks a variant up by (hash, specConstants & mask),
// so every subset of every mask is linked here (WWE '12: masks of at most 2 bits, ~2 000
// variants). The output is re:Blue's linked_shader_cache.h format: a table sorted by (hash,
// value) and the miniz (zlib) compressed concatenation of the linked DXIL.
//
// Usage: wwe13_prelink <out linked_shader_cache.cpp>   (dxcompiler.dll and dxil.dll beside it)

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <zstd.h>
#include <miniz.h>

#include "gpu/shaders/dxc_link.h"
#include "gpu/shaders/shader_cache.h"

namespace {

struct Variant {
  uint64_t hash;
  uint32_t value;
  std::vector<uint8_t> dxil;
};

}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: wwe13_prelink <out.cpp>\n");
    return 2;
  }
  std::vector<uint8_t> cache(g_dxilCacheDecompressedSize);
  const size_t n = ZSTD_decompress(cache.data(), cache.size(), g_compressedDxilCache,
                                   g_dxilCacheCompressedSize);
  if (ZSTD_isError(n) || n != cache.size()) {
    std::fprintf(stderr, "wwe13_prelink: DXIL cache decompression failed\n");
    return 1;
  }

  // Every (shader, mask subset) to link.
  std::vector<Variant> variants;
  std::vector<const ShaderCacheEntry *> owners;
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
    const ShaderCacheEntry &e = g_shaderCacheEntries[i];
    if (e.specConstantsMask == 0)
      continue;
    for (uint32_t sub = e.specConstantsMask;; sub = (sub - 1) & e.specConstantsMask) {
      variants.push_back({e.hash, sub, {}});
      owners.push_back(&e);
      if (sub == 0)
        break;
    }
  }

  // The g_SpecConstants() libraries, one per value.
  std::mutex lib_mutex;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> libs;
  const auto spec_lib = [&](uint32_t value) -> std::vector<uint8_t> {
    std::lock_guard lock(lib_mutex);
    for (auto &l : libs)
      if (l.first == value)
        return l.second;
    libs.emplace_back(value, bd::gpu::CompileSpecConstantLib(value));
    return libs.back().second;
  };

  // The cache does not say whether a shader is a vertex or a pixel shader: the link fails for the
  // wrong profile (signature elements of the other stage), so the pixel profile is tried first.
  std::atomic<size_t> next{0}, failed{0}, pixel{0};
  const auto work = [&] {
    for (size_t i; (i = next.fetch_add(1)) < variants.size();) {
      Variant &v = variants[i];
      const ShaderCacheEntry &e = *owners[i];
      const std::vector<uint8_t> lib = spec_lib(v.value);
      if (lib.empty()) {
        ++failed;
        continue;
      }
      v.dxil = bd::gpu::LinkSpecConstantLib(cache.data() + e.dxilOffset, e.dxilSize, lib.data(),
                                            lib.size(), L"ps_6_0");
      if (!v.dxil.empty()) {
        ++pixel;
        continue;
      }
      v.dxil = bd::gpu::LinkSpecConstantLib(cache.data() + e.dxilOffset, e.dxilSize, lib.data(),
                                            lib.size(), L"vs_6_0");
      if (v.dxil.empty()) {
        ++failed;
        std::fprintf(stderr, "wwe13_prelink: link failed hash=0x%016llX value=0x%X\n",
                     (unsigned long long)v.hash, v.value);
      }
    }
  };
  std::vector<std::thread> threads;
  const unsigned count = std::max(1u, std::thread::hardware_concurrency());
  for (unsigned t = 0; t < count; ++t)
    threads.emplace_back(work);
  for (auto &t : threads)
    t.join();

  variants.erase(std::remove_if(variants.begin(), variants.end(),
                                [](const Variant &v) { return v.dxil.empty(); }),
                 variants.end());
  std::sort(variants.begin(), variants.end(), [](const Variant &a, const Variant &b) {
    return a.hash != b.hash ? a.hash < b.hash : a.value < b.value;
  });

  // D3D12 refuses unsigned DXIL (zero container hash: DXC ran without dxil.dll / libdxil.so) and every
  // pipeline using it fails at run time, so an unsigned variant fails the build here instead.
  size_t unsigned_count = 0;
  for (const Variant &v : variants) {
    static const uint8_t zero[16] = {};
    if (v.dxil.size() < 20 || std::memcmp(v.dxil.data() + 4, zero, 16) == 0)
      ++unsigned_count;
  }
  if (unsigned_count) {
    std::fprintf(stderr, "wwe13_prelink: %zu of %zu linked shaders are unsigned (validator dxil not loaded)\n",
                 unsigned_count, variants.size());
    return 1;
  }

  std::vector<uint8_t> blob;
  std::vector<uint32_t> offsets;
  for (const Variant &v : variants) {
    offsets.push_back(uint32_t(blob.size()));
    blob.insert(blob.end(), v.dxil.begin(), v.dxil.end());
  }
  mz_ulong packed_size = mz_compressBound(mz_ulong(blob.size()));
  std::vector<uint8_t> packed(packed_size);
  if (mz_compress2(packed.data(), &packed_size, blob.data(), mz_ulong(blob.size()), 9) != MZ_OK) {
    std::fprintf(stderr, "wwe13_prelink: compression failed\n");
    return 1;
  }
  packed.resize(packed_size);

  FILE *f = std::fopen(argv[1], "wb");
  if (!f) {
    std::fprintf(stderr, "wwe13_prelink: cannot write %s\n", argv[1]);
    return 1;
  }
  std::fprintf(f, "// Generated by wwe13_prelink (port/src/prelink): do not edit.\n");
  std::fprintf(f, "#include \"gpu/shaders/linked_shader_cache.h\"\n\n");
  std::fprintf(f, "LinkedShaderCacheEntry g_linkedShaderCacheEntries[%zu] = {\n",
               std::max<size_t>(variants.size(), 1));
  for (size_t i = 0; i < variants.size(); ++i)
    std::fprintf(f, "\t{ 0x%016llXull, %u, %u, %zu },\n", (unsigned long long)variants[i].hash,
                 variants[i].value, offsets[i], variants[i].dxil.size());
  if (variants.empty())
    std::fprintf(f, "\t{ 0, 0, 0, 0 },\n");
  std::fprintf(f, "};\nconst size_t g_linkedShaderCacheEntryCount = %zu;\n\n", variants.size());
  std::fprintf(f, "const u8 g_compressedLinkedDxilCache[] = {");
  for (size_t i = 0; i < packed.size(); ++i)
    std::fprintf(f, "%s%u,", (i % 40) ? "" : "\n", packed[i]);
  std::fprintf(f, "\n};\nconst size_t g_linkedDxilCacheCompressedSize = %zu;\n", packed.size());
  std::fprintf(f, "const size_t g_linkedDxilCacheDecompressedSize = %zu;\n", blob.size());
  std::fclose(f);

  std::printf("wwe13_prelink: %zu variants linked (%zu pixel, %zu vertex, %zu failed), "
              "%zu bytes -> %zu compressed\n",
              variants.size(), size_t(pixel), variants.size() - size_t(pixel), size_t(failed),
              blob.size(), packed.size());
  return failed ? 1 : 0;
}
