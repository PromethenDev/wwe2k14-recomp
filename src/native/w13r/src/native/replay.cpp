// WWE '13 native-renderer replay (replay.h). Runs a recording.w13r back through the
// renderer: guest memory and the D3D device block are restored from the page/device
// records, and each recorded call's renderer side is re-run with its recorded registers.
// The game itself never runs (see wwe13_app.h, which suppresses the module launch).
#include "replay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/window.h>

#include "core/memory_helpers.h"
#include "recorder.h"
#include "svr_hook.h"

#include "gpu/device.h"
#include "gpu/gpu_census.h"
#include "svr_resources.h"

#if __has_include(<miniz.h>)
#include <miniz.h>
#define W13R_INFLATE 1
#endif

REXCVAR_DEFINE_STRING(wwe13_replay_dir, "", "WWE13",
                      "Folder holding recording.w13r: replay it instead of running the game")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(wwe13_replay_png, "", "WWE13",
                      "Replay: comma list of frame indices to dump as PNG (e.g. 0,5,10)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(wwe13_replay_loops, 1, "WWE13",
                     "Replay: run the recording this many times")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(wwe13_replay_skip, "", "WWE13",
                      "Replay experiment: file of D3DDevice_Resolve ordinals (0-based, one per line, "
                      "per pass) whose calls are skipped; tests an offline copy-liveness list")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(wwe13_replay_ab, "", "WWE13",
                      "Replay in-run A/B: name of a bool option set false on even loops and true on "
                      "odd loops; with the GPU census, each loop logs its own labelled totals")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bd::gpu {
extern bool g_draw_profile;
extern std::atomic<uint64_t> g_draw_prof_ns[7];
extern std::atomic<uint64_t> g_flush_prof_ns[5];
}  // namespace bd::gpu

namespace svr::replay {

bool Enabled() {
  return !REXCVAR_GET(wwe13_replay_dir).empty();
}

namespace {

struct ReplayState {
  rex::memory::Memory* memory = nullptr;
  std::unordered_set<uint32_t> png_frames;
  uint32_t front_buffer_va = 0;  // last Swap's front buffer (r4), for PNG capture
  uint32_t frames = 0;
  uint32_t calls = 0;
  std::unordered_set<uint32_t> committed;  // guest heap pages already allocated
  uint32_t alloc_failures = 0;
  uint32_t target_failures = 0;
  std::unordered_set<uint32_t> skip_resolves;  // wwe13_replay_skip
  uint32_t resolve_ordinal = 0;                // per pass
  uint32_t skipped = 0;
  // CPU time per hook (renderer side) and per record kind, for the renderer CPU profile.
  std::unordered_map<std::string, std::pair<double, uint64_t>> hook_ms;  // name -> (ms, calls)
  double pages_ms = 0, mem_ms = 0, device_ms = 0, other_ms = 0;
};

// Reads <dir>/recording.w13r into one flat byte stream: each chunk is uncompressed (miniz
// mz_uncompress) when stored_bytes < raw_bytes, and concatenated. The result is a stream of
// Records (RecordHeader + body).
bool ReadRecording(const std::string& dir, std::vector<uint8_t>& out) {
  const std::string path = dir + "/recording.w13r";
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  svr::recorder::FileHeader fh{};
  const bool header_ok = std::fread(&fh, sizeof(fh), 1, f) == 1 &&
                         std::memcmp(fh.magic, svr::recorder::kMagic, sizeof(fh.magic)) == 0;
  if (!header_ok) {
    std::fclose(f);
    return false;
  }
  out.clear();
  svr::recorder::ChunkHeader ch{};
  while (std::fread(&ch, sizeof(ch), 1, f) == 1) {
    if (!ch.stored_bytes && ch.raw_bytes) {  // empty chunk (writer never emits these): stop safe
      std::fclose(f);
      return false;
    }
    std::vector<uint8_t> packed(ch.stored_bytes);
    if (ch.stored_bytes && std::fread(packed.data(), 1, ch.stored_bytes, f) != ch.stored_bytes) {
      std::fclose(f);
      return false;
    }
    if (ch.stored_bytes == ch.raw_bytes) {
      out.insert(out.end(), packed.begin(), packed.end());
    } else {
#if defined(W13R_INFLATE)
      std::vector<uint8_t> raw(ch.raw_bytes);
      mz_ulong dest = mz_ulong(raw.size());
      if (mz_uncompress(raw.data(), &dest, packed.data(), mz_ulong(packed.size())) != MZ_OK) {
        std::fclose(f);
        return false;
      }
      raw.resize(dest);
      out.insert(out.end(), raw.begin(), raw.end());
#else
      // This build has no miniz (Vulkan-only): only uncompressed recordings replay.
      std::fclose(f);
      return false;
#endif
    }
  }
  std::fclose(f);
  return !out.empty();
}

// Restores guest memory from a kBasePage/kFramePage body {space, offset, 4096 bytes}.
void ApplyPage(ReplayState& s, const uint8_t* body, uint32_t bytes) {
  if (bytes < 8 + svr::recorder::kPageBytes) return;
  uint32_t space = 0, offset = 0;
  std::memcpy(&space, body, 4);
  std::memcpy(&offset, body + 4, 4);
  if (space == 0) {
    // The game never ran, so its virtual heaps hold nothing: allocate each recorded page through
    // the guest heap (host commit + the heap's page table, which try_at validation consults).
    // Physical memory is host-committed at startup (xmemory.cpp) and needs nothing.
    auto* heap = s.memory->LookupHeap(offset);
    if (!heap) return;
    const uint32_t ps = heap->page_size();
    const uint32_t page = offset & ~(ps - 1);
    if (s.committed.insert(page).second) {
      rex::memory::HeapAllocationInfo info{};
      const bool known = heap->QueryRegionInfo(page, &info);
      const uint32_t rw = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
      if (known && (info.state & rex::memory::kMemoryAllocationCommit) &&
          (info.protect & rw) != rw) {
        // Already committed but not writable (the loaded game image's code and read-only data):
        // make it writable so the recorded bytes can be restored. The game never executes here.
        heap->Protect(page, ps, rw);
      }
      if (!known || !(info.state & rex::memory::kMemoryAllocationCommit)) {
        if (!heap->AllocFixed(page, ps, ps,
                              rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rw) &&
            !heap->AllocFixed(page, ps, ps, rex::memory::kMemoryAllocationCommit, rw)) {
          if (++s.alloc_failures <= 8)
            REXLOG_WARN("[replay] cannot commit guest page 0x{:08X} (heap page {} bytes)", page, ps);
          return;
        }
      }
    }
  }
  uint8_t* base = space == 0 ? s.memory->virtual_membase() : s.memory->physical_membase();
  std::memcpy(base + offset, body + 8, svr::recorder::kPageBytes);
}

// Restores memory the game wrote mid-frame (kMem {space, address, length, bytes}).
void ApplyMem(ReplayState& s, const uint8_t* body, uint32_t bytes) {
  if (bytes < 12) return;
  uint32_t space = 0, address = 0, length = 0;
  std::memcpy(&space, body, 4);
  std::memcpy(&address, body + 4, 4);
  std::memcpy(&length, body + 8, 4);
  if (12ull + length > bytes) return;
  if (space == 1) {
    if (uint64_t(address) + length > 0x20000000ull) return;
    std::memcpy(s.memory->physical_membase() + address, body + 12, length);
    return;
  }
  // A virtual address: every page it spans must be committed (ApplyPage does that for page records).
  auto* heap = s.memory->LookupHeap(address);
  if (!heap) return;
  for (uint32_t a = address & ~(svr::recorder::kPageBytes - 1); a < address + length; a += svr::recorder::kPageBytes) {
    uint32_t protect = 0;
    if (!heap->QueryProtect(a, &protect) || !(protect & rex::memory::kMemoryProtectWrite)) return;
  }
  std::memcpy(s.memory->TranslateVirtual<uint8_t*>(address), body + 12, length);
}

// Restores a GPU-written texture's start-of-recording content (kTarget).
void ApplyTarget(ReplayState& s, const uint8_t* body, uint32_t bytes) {
  uint32_t head[7] = {};
  if (bytes < sizeof(head)) return;
  std::memcpy(head, body, sizeof(head));
  const uint32_t va = head[0], quarters = head[1], face = head[2], level = head[3], w = head[4], h = head[5],
                 row_pitch = head[6];
  if (sizeof(head) + uint64_t(row_pitch) * h > bytes) return;
  bd::gpu::GuestTexture* tex = bd::gpu::SvrResolveTarget(va, float(quarters) / 4.0f);
  if (!tex || std::max(1u, tex->width >> level) != w || std::max(1u, tex->height >> level) != h) {
    if (++s.target_failures <= 8) REXLOG_WARN("[replay] target 0x{:08X} face {} level {}: cannot restore", va, face, level);
    return;
  }
  bd::gpu::Video::UploadTextureLevel(tex, level, body + sizeof(head), row_pitch, face);
}

// Restores changed bytes of the D3D device block {device, {off,len,bytes} runs}.
void ApplyDevice(ReplayState& s, const uint8_t* body, uint32_t bytes) {
  if (bytes < 4) return;
  uint32_t device = 0;
  std::memcpy(&device, body, 4);
  uint8_t* dev = s.memory->TranslateVirtual<uint8_t*>(device);
  uint32_t o = 4;
  while (o + 4 <= bytes) {
    uint16_t off = 0, len = 0;
    std::memcpy(&off, body + o, 2);
    std::memcpy(&len, body + o + 2, 2);
    o += 4;
    if (o + len > bytes) break;
    std::memcpy(dev + off, body + o, len);
    o += len;
  }
}

// Rebuilds the argument registers of one call and runs its renderer side. The recorded
// registers are r1-r13 then f1-f8 (see EventRecord / recorder.cpp RecordHook); only r3-r10,
// r1 (stack args) and f1-f8 are read by HostToGuestFunction, the rest is filled for the
// RESULT observers that peek at raw args.
void ApplyCall(ReplayState& s, const uint8_t* body, uint32_t bytes) {
  if (bytes < sizeof(svr::recorder::EventRecord)) return;
  svr::recorder::EventRecord ev{};
  std::memcpy(&ev, body, sizeof(ev));
  const svr::recorder::HookEntry* hook = svr::recorder::FindHook(ev.hook_id);
  if (!hook) return;  // a call this build does not hook (compiled out): skip
  // Create calls were registered from their kShader / kDecl copies; re-running them would read the
  // game's (possibly reused) buffer.
  if (std::strncmp(hook->name, "D3DDevice_Create", 16) == 0) return;

  PPCContext ctx{};
  const uint64_t* r = ev.registers;
  ctx.r1.u64 = r[0];
  ctx.r2.u64 = r[1];
  ctx.r3.u64 = r[2];
  ctx.r4.u64 = r[3];
  ctx.r5.u64 = r[4];
  ctx.r6.u64 = r[5];
  ctx.r7.u64 = r[6];
  ctx.r8.u64 = r[7];
  ctx.r9.u64 = r[8];
  ctx.r10.u64 = r[9];
  ctx.r11.u64 = r[10];
  ctx.r12.u64 = r[11];
  ctx.r13.u64 = r[12];
  ctx.f1.u64 = r[13];
  ctx.f2.u64 = r[14];
  ctx.f3.u64 = r[15];
  ctx.f4.u64 = r[16];
  ctx.f5.u64 = r[17];
  ctx.f6.u64 = r[18];
  ctx.f7.u64 = r[19];
  ctx.f8.u64 = r[20];

  if (std::strcmp(hook->name, "D3DDevice_Swap") == 0)
    s.front_buffer_va = ctx.r4.u32;  // Swap's second argument is the front buffer
  if (!s.skip_resolves.empty() && std::strcmp(hook->name, "D3DDevice_Resolve") == 0 &&
      s.skip_resolves.count(s.resolve_ordinal++)) {
    ++s.skipped;
    return;
  }

  const auto h0 = std::chrono::steady_clock::now();
  hook->fn(ctx, uint32_t(ev.return_value), s.memory->virtual_membase());
  auto& hm = s.hook_ms[hook->name];
  hm.first += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - h0).count();
  ++hm.second;
  ++s.calls;
}

}  // namespace

// ---- PNG (optional, cvar wwe13_replay_png; the recorder's wwe13_record_png) ----

// Writes the 4-byte-per-texel target at guest address target_va as an RGBA PNG at path. Best-effort:
// the byte order follows the target's format (RGBA8 here, so a BGRA8 target would come out
// red/blue-swapped). Needs miniz (the D3D12 target has it) for the zlib IDAT.
bool WriteTargetPng(uint32_t target_va, const std::string& path) {
  if (!target_va) return false;
  bd::gpu::GuestTexture* tex = bd::gpu::SvrResolveTarget(target_va);
  if (!tex) return false;
  // A deferred resolve (wwe13_resolve_alias) is still in its source surface: read what the
  // present pass shows (SelectPresentSource substitutes the same way), not the stale texture.
  if (tex->sourceSurface && tex->sourceSurface != tex && tex->sourceSurface->texture &&
      tex->sourceSurface->width == tex->width && tex->sourceSurface->height == tex->height &&
      tex->sourceSurface->format == tex->format)
    tex = tex->sourceSurface;
  std::vector<uint8_t> pixels;
  uint32_t row_pitch = 0;
  if (!bd::gpu::Video::ReadbackTextureLevel(tex, 0, 0, pixels, row_pitch)) {
    REXLOG_WARN("[png] {}: readback failed", path);
    return false;
  }
  if (!tex->width || !tex->height || pixels.size() < size_t(row_pitch) * tex->height) return false;
#if defined(W13R_INFLATE)
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const auto put32 = [](std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
  };
  const auto chunk = [&](const char type[4], const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hdr;
    put32(hdr, uint32_t(data.size()));
    hdr.insert(hdr.end(), type, type + 4);
    std::fwrite(hdr.data(), 1, hdr.size(), f);
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    mz_ulong crc = mz_crc32(0, reinterpret_cast<const unsigned char*>(type), 4);
    if (!data.empty()) crc = mz_crc32(crc, data.data(), data.size());
    std::vector<uint8_t> crcb;
    put32(crcb, uint32_t(crc));
    std::fwrite(crcb.data(), 1, 4, f);
  };

  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  std::fwrite(sig, 1, 8, f);

  std::vector<uint8_t> ihdr;
  put32(ihdr, tex->width);
  put32(ihdr, tex->height);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // color type: RGBA
  ihdr.push_back(0);  // compression
  ihdr.push_back(0);  // filter
  ihdr.push_back(0);  // interlace
  chunk("IHDR", ihdr);

  // One scanline per row, each prefixed with filter byte 0 (None).
  std::vector<uint8_t> raw;
  raw.reserve(size_t(row_pitch + 1) * tex->height);
  for (uint32_t y = 0; y < tex->height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), pixels.data() + size_t(y) * row_pitch,
               pixels.data() + size_t(y) * row_pitch + size_t(tex->width) * 4);
  }
  std::vector<uint8_t> idat(mz_compressBound(mz_ulong(raw.size())));
  mz_ulong idat_len = idat.size();
  if (mz_compress(idat.data(), &idat_len, raw.data(), mz_ulong(raw.size())) == MZ_OK) {
    idat.resize(idat_len);
    chunk("IDAT", idat);
    chunk("IEND", {});
  }
  std::fclose(f);
  return true;
#else
  REXLOG_WARN("[png] {}: this build has no miniz, cannot write", path);
  return false;
#endif
}

namespace {

// Reads the presented front buffer back and writes it as <dir>/frame_<n>.png.
void DumpPng(ReplayState& s, uint32_t index) {
  WriteTargetPng(s.front_buffer_va,
                 REXCVAR_GET(wwe13_replay_dir) + "/frame_" + std::to_string(index) + ".png");
}

// Runs one pass over the record stream.
void RunPass(ReplayState& s, const std::vector<uint8_t>& stream) {
  size_t p = 0;
  while (p < stream.size()) {
    if (p + sizeof(svr::recorder::RecordHeader) > stream.size()) break;
    svr::recorder::RecordHeader h{};
    std::memcpy(&h, stream.data() + p, sizeof(h));
    p += sizeof(h);
    if (p + h.bytes > stream.size()) break;
    const uint8_t* body = stream.data() + p;
    const auto r0 = std::chrono::steady_clock::now();
    double* bucket = &s.other_ms;
    switch (h.type) {
      case svr::recorder::kBasePage:
      case svr::recorder::kFramePage:
        ApplyPage(s, body, h.bytes);
        bucket = &s.pages_ms;
        break;
      case svr::recorder::kShader:
        if (h.bytes > 8) {
          uint32_t obj = 0, pixel = 0;
          std::memcpy(&obj, body, 4);
          std::memcpy(&pixel, body + 4, 4);
          bd::gpu::SvrRegisterShaderContainer(obj, body + 8, pixel != 0);
        }
        break;
      case svr::recorder::kDecl:
        if (h.bytes > 8) {
          uint32_t obj = 0;
          std::memcpy(&obj, body, 4);
          bd::gpu::SvrRegisterVertexDeclaration(obj, reinterpret_cast<const bd::gpu::GuestVertexElement*>(body + 8));
        }
        break;
      case svr::recorder::kDevice:
        ApplyDevice(s, body, h.bytes);
        bucket = &s.device_ms;
        break;
      case svr::recorder::kMem:
        ApplyMem(s, body, h.bytes);
        bucket = &s.mem_ms;
        break;
      case svr::recorder::kTarget:
        ApplyTarget(s, body, h.bytes);
        break;
      case svr::recorder::kCall:
        ApplyCall(s, body, h.bytes);
        bucket = nullptr;  // timed per hook inside
        break;
      case svr::recorder::kFrameEnd: {
        uint32_t index = 0;
        if (h.bytes >= 4) std::memcpy(&index, body, 4);
        ++s.frames;
        if (s.png_frames.count(index)) DumpPng(s, index);
        break;
      }
      default:
        return;  // unknown record type: stop this pass
    }
    if (bucket)
      *bucket += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
    p += h.bytes;
  }
}

void ParsePngList(const std::string& text, std::unordered_set<uint32_t>& out) {
  size_t i = 0;
  while (i < text.size()) {
    size_t j = text.find(',', i);
    if (j == std::string::npos) j = text.size();
    const std::string tok = text.substr(i, j - i);
    i = j + 1;
    if (tok.empty()) continue;
    char* end = nullptr;
    const long v = std::strtol(tok.c_str(), &end, 10);
    if (end != tok.c_str() && *end == '\0' && v >= 0) out.insert(uint32_t(v));
  }
}

}  // namespace

bool Run(rex::ui::Window* window) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) {
    REXLOG_ERROR("[replay] guest memory is not up");
    return false;
  }

  // The renderer is normally initialised by Wwe13App::StartNativeRenderer (the game's
  // D3D device-creation path) in OnPreLaunchModule. In replay mode the game never runs, so
  // create the host device here: Video::CreateHostDevice needs only the window, not a guest
  // D3D device.
  if (!bd::gpu::Video::HostDevice() && !bd::gpu::Video::CreateHostDevice(window)) {
    REXLOG_ERROR("[replay] could not create the host renderer device");
    return false;
  }

  std::vector<uint8_t> stream;
  if (!ReadRecording(REXCVAR_GET(wwe13_replay_dir), stream)) {
    REXLOG_ERROR("[replay] cannot read {}/recording.w13r", REXCVAR_GET(wwe13_replay_dir));
    return false;
  }

  ReplayState s;
  s.memory = memory;
  // Physical memory is restored from the recording, not allocated by the game: let the checked
  // accessors (texture headers live at 0xA0000000+) read it.
  bd::mem::set_replay_physical_views(true);
  bd::gpu::g_draw_profile = true;
  ParsePngList(REXCVAR_GET(wwe13_replay_png), s.png_frames);
  if (const std::string skip = REXCVAR_GET(wwe13_replay_skip); !skip.empty()) {
    if (std::FILE* f = std::fopen(skip.c_str(), "r")) {
      unsigned v = 0;
      while (std::fscanf(f, "%u", &v) == 1) s.skip_resolves.insert(v);
      std::fclose(f);
    }
    REXLOG_INFO("[replay] skipping {} resolves listed in {}", s.skip_resolves.size(), skip);
  }

  uint32_t hook_count = 0;
  svr::recorder::ForEachHook([&](const svr::recorder::HookEntry& e) {
    (void)e;
    ++hook_count;
  });
  REXLOG_INFO("[replay] {} hooks registered, {} PNG frames requested", hook_count,
              s.png_frames.size());

  const int loops = std::max(1, REXCVAR_GET(wwe13_replay_loops));
  uint64_t total_frames = 0, total_calls = 0, profile_frames_skip = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const std::string ab = REXCVAR_GET(wwe13_replay_ab);
  for (int i = 0; i < loops; ++i) {
    s.frames = 0;
    s.calls = 0;
    s.resolve_ordinal = 0;
    // A comma list switches several bool options together (a bundle).
    for (size_t b = 0; !ab.empty() && b <= ab.size();) {
      const size_t e = std::min(ab.find(',', b), ab.size());
      if (e > b && !rex::cvar::SetFlagByName(ab.substr(b, e - b), (i & 1) ? "true" : "false"))
        REXLOG_ERROR("[replay] A/B: cannot set {}", ab.substr(b, e - b));
      b = e + 1;
    }
    // Renderer CPU of this loop: every hook but Swap (which holds the vsync wait), and the draws.
    const auto cpu_now = [&s](double& draws, double& textures) {
      double all = 0;
      draws = textures = 0;
      for (const auto& [name, v] : s.hook_ms) {
        if (name != "D3DDevice_Swap") all += v.first;
        if (name.find("Vertices") != std::string::npos) draws += v.first;
        if (name == "D3DDevice_SetTexture") textures += v.first;
      }
      return all;
    };
    double draws0 = 0, draws1 = 0, tex0 = 0, tex1 = 0;
    const double cpu0 = cpu_now(draws0, tex0);
    const uint64_t psc0 = bd::gpu::g_flush_prof_ns[2].load();
    const auto l0 = std::chrono::steady_clock::now();
    RunPass(s, stream);
    if (!ab.empty()) {
      const double lms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - l0).count() / std::max(1u, s.frames);
      const std::string label = "loop " + std::to_string(i) + " " + ab + "=" + ((i & 1) ? "on" : "off") + ": ";
      const double cpu1 = cpu_now(draws1, tex1);
      const double nfr = double(std::max(1u, s.frames));
      REXLOG_INFO("[replay] {}{:.1f} ms/frame | renderer CPU without Swap {:.3f} ms/frame, draws {:.3f}, "
                  "SetTexture {:.3f}, PS constants {:.3f}", label, lms, (cpu1 - cpu0) / nfr,
                  (draws1 - draws0) / nfr, (tex1 - tex0) / nfr,
                  double(bd::gpu::g_flush_prof_ns[2].load() - psc0) / 1e6 / nfr);
      bd::gpu::census::Flush(label.c_str());
    }
    if (i == 0 && loops > 1) {
      // Loop 0 builds pipelines and uploads textures: profile the warm loops only.
      s.hook_ms.clear();
      s.pages_ms = s.mem_ms = s.device_ms = s.other_ms = 0;
      for (auto& v : bd::gpu::g_draw_prof_ns) v.store(0);
      for (auto& v : bd::gpu::g_flush_prof_ns) v.store(0);
      profile_frames_skip += s.frames;
    }
    total_frames += s.frames;
    total_calls += s.calls;
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double ms = total_frames ? secs * 1000.0 / double(total_frames) : 0.0;
  {
    // Renderer CPU profile: hook (renderer side) time vs the replay's own memory restore.
    const double nf = double(std::max<uint64_t>(1, total_frames - profile_frames_skip));
    double hooks = 0;
    std::vector<std::pair<double, std::string>> order;
    for (const auto& [name, v] : s.hook_ms) {
      hooks += v.first;
      order.push_back({v.first, name});
    }
    std::sort(order.rbegin(), order.rend());
    {
      const double draws = double(std::max<uint64_t>(1, bd::gpu::g_draw_prof_ns[5].load()));
      const auto us = [&](int i) { return double(bd::gpu::g_draw_prof_ns[i].load()) / 1000.0 / draws; };
      REXLOG_INFO("[replay-cpu] per draw (us): geometry {:.2f} | pre {:.2f} | framebuffer {:.2f} | "
                  "flush state {:.2f} | record {:.2f} ({:.0f} draws/frame, {:.0f} reach the command list)",
                  us(0), us(1), us(2), us(3), us(4), draws / nf, double(bd::gpu::g_draw_prof_ns[6].load()) / nf);
      const auto fus = [&](int i) { return double(bd::gpu::g_flush_prof_ns[i].load()) / 1000.0 / draws; };
      REXLOG_INFO("[replay-cpu] flush state per draw (us): state+pipeline {:.2f} | VS consts {:.2f} | "
                  "PS consts {:.2f} | shared consts {:.2f} | streams/rest {:.2f}",
                  fus(0), fus(1), fus(2), fus(3), fus(4));
    }
    REXLOG_INFO("[replay-cpu] per frame: hooks {:.2f} ms | pages {:.2f} | kMem {:.2f} | device {:.2f} | other {:.2f}",
                hooks / nf, s.pages_ms / nf, s.mem_ms / nf, s.device_ms / nf, s.other_ms / nf);
    for (const auto& [ms, name] : order)
      REXLOG_INFO("[replay-cpu]   {:34s} {:7.3f} ms/frame  {:7.1f} calls/frame  {:6.2f} us/call", name,
                  ms / nf, double(s.hook_ms[name].second) / nf, ms * 1000.0 / double(std::max<uint64_t>(1, s.hook_ms[name].second)));
  }
  REXLOG_INFO("[replay] {} frames, avg {:.1f} ms/frame ({} calls, {} loop{}, {} resolves skipped, "
              "{} per pass)", total_frames, ms, total_calls, loops, loops == 1 ? "" : "s", s.skipped,
              s.resolve_ordinal);

  window->RequestClose();
  return true;
}

}  // namespace svr::replay
