// WWE '13 native-renderer recorder (recorder.h). Recording only: every cost below is paid only while
// wwe13_record_frames > 0, and the game simply runs slower then (frames are still produced in order).
#include "recorder.h"
namespace w13r { void NoteGuestSwap(); }  // w13r_2k14_glue.cpp (FPS counter)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if __has_include(<miniz.h>)
#include <miniz.h>
#define W13R_DEFLATE 1
#endif

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "core/memory_helpers.h"
#include "replay.h"
#include "svr_geometry.h"
#include "gpu/d3d.h"
#include "gpu/resources.h"
#include "gpu/device.h"
#include "svr_resources.h"

REXCVAR_DEFINE_INT32(wwe13_record_frames, 0, "WWE13",
                     "Record this many native-renderer frames for offline replay (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(wwe13_record_after, 0, "WWE13",
                     "Start recording after this many frames (Swaps), e.g. to skip boot and menus")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(wwe13_record_dir, "recording", "WWE13", "Output folder for a native-renderer recording")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(wwe13_record_png, "", "WWE13",
                      "Recorder: comma list of recorded frame indices to save as live_<n>.png (live-vs-replay check)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace svr::recorder {

uint32_t HookId(const char* name) {
  uint32_t h = 2166136261u;
  for (; *name; ++name) h = (h ^ static_cast<uint8_t>(*name)) * 16777619u;
  return h;
}

namespace {
// Replay hook table: populated by the SVR_HOOK_* macros at static-init time, read
// by the replay. A Meyers singleton so registration order across TUs never matters.
struct HookRegistry {
  std::mutex mutex;
  std::vector<HookEntry> entries;
};
HookRegistry& R() {
  static HookRegistry r;
  return r;
}
}  // namespace

const HookEntry* RegisterHook(const char* name, ReplayHookFn fn, HookKind kind) {
  if (!name || !fn) return nullptr;
  HookRegistry& r = R();
  std::lock_guard lock(r.mutex);
  for (const HookEntry& e : r.entries)
    if (std::strcmp(e.name, name) == 0) return &e;
  r.entries.push_back(HookEntry{HookId(name), name, fn, kind});
  return &r.entries.back();
}

const HookEntry* FindHook(uint32_t id) {
  HookRegistry& r = R();
  std::lock_guard lock(r.mutex);
  for (const HookEntry& e : r.entries)
    if (e.id == id) return &e;
  return nullptr;
}

void ForEachHook(const std::function<void(const HookEntry&)>& visitor) {
  if (!visitor) return;
  HookRegistry& r = R();
  std::lock_guard lock(r.mutex);
  for (const HookEntry& e : r.entries) visitor(e);
}

namespace {
constexpr size_t kChunkBytes = 32u << 20;

// Background writer: compresses and writes full chunks so the hooks only append to a buffer.
class Writer {
 public:
  bool Open(const std::string& path) {
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return false;
    FileHeader h{};
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    h.version = kVersion;
    h.page_bytes = kPageBytes;
    std::fwrite(&h, sizeof(h), 1, file_);
    thread_ = std::thread([this] { Run(); });
    return true;
  }
  void Submit(std::vector<uint8_t>&& chunk) {
    if (chunk.empty()) return;
    std::unique_lock lock(mutex_);
    // Bound memory: wait while 8 chunks (256 MB) are queued.
    space_.wait(lock, [this] { return queue_.size() < 8; });
    queue_.push_back(std::move(chunk));
    ready_.notify_one();
  }
  uint64_t Close() {
    {
      std::lock_guard lock(mutex_);
      closing_ = true;
      ready_.notify_one();
    }
    if (thread_.joinable()) thread_.join();
    uint64_t bytes = 0;
    if (file_) {
      bytes = uint64_t(std::ftell(file_));
      std::fclose(file_);
      file_ = nullptr;
    }
    return bytes;
  }

 private:
  void Run() {
    for (;;) {
      std::vector<uint8_t> raw;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return closing_ || !queue_.empty(); });
        if (queue_.empty()) return;
        raw = std::move(queue_.front());
        queue_.pop_front();
        space_.notify_one();
      }
      ChunkHeader ch{uint32_t(raw.size()), uint32_t(raw.size())};
      const uint8_t* data = raw.data();
      std::vector<uint8_t> packed;
#if defined(W13R_DEFLATE)
      mz_ulong packed_size = mz_compressBound(mz_ulong(raw.size()));
      packed.resize(packed_size);
      if (mz_compress2(packed.data(), &packed_size, raw.data(), mz_ulong(raw.size()), 1) == MZ_OK &&
          packed_size < raw.size()) {
        ch.stored_bytes = uint32_t(packed_size);
        data = packed.data();
      }
#endif
      std::fwrite(&ch, sizeof(ch), 1, file_);
      std::fwrite(data, 1, ch.stored_bytes, file_);
    }
  }
  std::FILE* file_ = nullptr;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable ready_, space_;
  std::deque<std::vector<uint8_t>> queue_;
  bool closing_ = false;
};

struct State {
  std::mutex mutex;
  Writer writer;
  std::vector<uint8_t> chunk;
  bool active = false, done = false, scan_pending = false, base_taken = false;
  std::unordered_set<uint32_t> png_frames;  // wwe13_record_png
  uint32_t png_index = 0, png_front = 0;    // a frame to save at the next hook (its Swap has run by then)
  bool png_pending = false;
  uint32_t bv_va = 0, bv_size = 0;  // the open BeginVertices block (its data is recorded at EndVertices)
  uint64_t mem_records = 0, mem_bytes = 0;
  int requested = 0, frames = 0, skipped = 0;
  uint32_t device = 0;
  std::vector<uint8_t> device_copy;
  std::unordered_map<uint32_t, std::vector<uint8_t>> shaders, decls;  // object -> kShader / kDecl body
  std::unordered_map<uint64_t, uint64_t> page_hash;  // (space << 32 | offset) -> XXH3
  uint64_t base_pages = 0, frame_pages = 0, device_bytes = 0, calls = 0;
  double scan_ms = 0;
  std::chrono::steady_clock::time_point start;
};
State& S() {
  static State s;
  return s;
}

void Put(State& s, const void* p, size_t n) {
  const auto* b = static_cast<const uint8_t*>(p);
  s.chunk.insert(s.chunk.end(), b, b + n);
}
void PutRecord(State& s, uint32_t type, const void* body, uint32_t bytes) {
  RecordHeader h{type, bytes};
  Put(s, &h, sizeof(h));
  Put(s, body, bytes);
  if (s.chunk.size() >= kChunkBytes) {
    s.writer.Submit(std::move(s.chunk));
    s.chunk.clear();
    s.chunk.reserve(kChunkBytes + (1u << 20));
  }
}

// Every committed, readable host page of one guest space: [base, base + size).
template <typename F>
void ForEachReadablePage(const uint8_t* base, uint64_t size, F&& f) {
#if defined(_WIN32)
  uint64_t off = 0;
  while (off < size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + off, &mbi, sizeof(mbi))) break;
    const uint64_t region_end =
        std::min<uint64_t>(size, uint64_t(static_cast<const uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize);
    const DWORD prot = mbi.Protect & 0xFF;
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                          (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                           prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                           prot == PAGE_EXECUTE_WRITECOPY);
    if (readable)
      for (uint64_t p = off & ~uint64_t(kPageBytes - 1); p + kPageBytes <= region_end; p += kPageBytes)
        f(uint32_t(p), base + p);
    off = region_end > off ? region_end : off + kPageBytes;
  }
#else
  (void)base;
  (void)size;
  (void)f;
#endif
}

// Base snapshot (base == true) or the pages changed since the last scan.
void Scan(State& s, bool base) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  const auto t0 = std::chrono::steady_clock::now();
  struct Space {
    uint32_t id;
    const uint8_t* base;
    uint64_t size;
  };
  // Virtual 0x00000000-0x9FFFFFFF; 0xA0000000+ are views of the 512 MB physical memory, taken once.
  const Space spaces[] = {{0, memory->virtual_membase(), 0xA0000000ull},
                          {1, memory->physical_membase(), 0x20000000ull}};
  for (const Space& sp : spaces) {
    ForEachReadablePage(sp.base, sp.size, [&](uint32_t offset, const uint8_t* page) {
      const uint64_t key = (uint64_t(sp.id) << 32) | offset;
      const uint64_t h = XXH3_64bits(page, kPageBytes);
      auto [it, inserted] = s.page_hash.try_emplace(key, h);
      if (!inserted) {
        if (it->second == h) return;
        it->second = h;
      }
      // A page first seen after the base snapshot that is all zero needs no record (replay memory starts zeroed).
      static const uint64_t kZeroHash = [] {
        static const uint8_t zero[kPageBytes] = {};
        return XXH3_64bits(zero, kPageBytes);
      }();
      if (!base && inserted && h == kZeroHash) return;
      uint8_t body[8 + kPageBytes];
      std::memcpy(body, &sp.id, 4);
      std::memcpy(body + 4, &offset, 4);
      std::memcpy(body + 8, page, kPageBytes);
      PutRecord(s, base ? kBasePage : kFramePage, body, sizeof(body));
      (base ? s.base_pages : s.frame_pages)++;
    });
  }
  s.scan_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Changed bytes of the D3D device block since the last hook, as {offset, length, bytes} runs.
void DeviceDelta(State& s) {
  if (!s.device) return;
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  const auto* dev = memory->TranslateVirtual<const uint8_t*>(s.device);
  if (s.device_copy.size() != kDeviceBytes) s.device_copy.assign(kDeviceBytes, 0xCD);  // forces a full first record
  std::vector<uint8_t> body(4);
  std::memcpy(body.data(), &s.device, 4);
  constexpr uint32_t kBlock = 16;
  for (uint32_t o = 0; o < kDeviceBytes;) {
    if (std::memcmp(dev + o, s.device_copy.data() + o, std::min(kBlock, kDeviceBytes - o)) == 0) {
      o += kBlock;
      continue;
    }
    uint32_t e = o;
    while (e < kDeviceBytes && e - o < 0xF000 &&
           std::memcmp(dev + e, s.device_copy.data() + e, std::min(kBlock, kDeviceBytes - e)) != 0)
      e += kBlock;
    e = std::min(e, kDeviceBytes);
    const uint16_t off = uint16_t(o), len = uint16_t(e - o);
    body.insert(body.end(), reinterpret_cast<const uint8_t*>(&off), reinterpret_cast<const uint8_t*>(&off) + 2);
    body.insert(body.end(), reinterpret_cast<const uint8_t*>(&len), reinterpret_cast<const uint8_t*>(&len) + 2);
    body.insert(body.end(), dev + o, dev + e);
    std::memcpy(s.device_copy.data() + o, dev + o, len);
    o = e;
  }
  if (body.size() > 4) {
    PutRecord(s, kDevice, body.data(), uint32_t(body.size()));
    s.device_bytes += body.size();
  }
}

// Keeps a copy of a created shader / vertex declaration (from boot, while recording is enabled).
const std::vector<uint8_t>* NoteCreate(State& s, const char* name, const PPCContext& args, uint32_t object) {
  const bool vs = std::strcmp(name, "D3DDevice_CreateVertexShader") == 0;
  const bool ps = std::strcmp(name, "D3DDevice_CreatePixelShader") == 0;
  const bool decl = std::strcmp(name, "D3DDevice_CreateVertexDeclaration") == 0;
  if ((!vs && !ps && !decl) || !object) return nullptr;
  std::vector<uint8_t> body(8);
  std::memcpy(body.data(), &object, 4);
  if (decl) {
    const auto* e = bd::mem::at<const bd::gpu::GuestVertexElement>(args.r3.u32);
    if (!e) return nullptr;
    uint32_t n = 0;
    while (n < 64 && uint16_t(e[n].stream) != 0xFF) ++n;
    ++n;  // the terminator
    std::memcpy(body.data() + 4, &n, 4);
    body.insert(body.end(), reinterpret_cast<const uint8_t*>(e), reinterpret_cast<const uint8_t*>(e + n));
    return &(s.decls[object] = std::move(body));
  }
  const auto* c = bd::mem::at<const bd::gpu::ShaderContainer>(args.r3.u32);
  if (!c) return nullptr;
  const uint32_t length = uint32_t(c->virtualSize) + uint32_t(c->physicalSize);
  if (length < sizeof(bd::gpu::ShaderContainer) || length > (4u << 20)) return nullptr;
  const uint32_t pixel = ps ? 1u : 0u;
  std::memcpy(body.data() + 4, &pixel, 4);
  body.insert(body.end(), reinterpret_cast<const uint8_t*>(c), reinterpret_cast<const uint8_t*>(c) + length);
  return &(s.shaders[object] = std::move(body));
}

// A kMem record of [address, address + length): space 1 = physical offset, else a virtual address.
void PutMem(State& s, uint32_t space, uint32_t address, uint32_t length) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory || !length || length > (16u << 20)) return;
  if (space == 1 && (uint64_t(address) + length > 0x20000000ull)) return;
  const uint8_t* src = space == 1 ? memory->physical_membase() + address
                                  : memory->TranslateVirtual<const uint8_t*>(address);
  std::vector<uint8_t> body(12 + size_t(length));
  std::memcpy(body.data(), &space, 4);
  std::memcpy(body.data() + 4, &address, 4);
  std::memcpy(body.data() + 8, &length, 4);
  std::memcpy(body.data() + 12, src, length);
  PutRecord(s, kMem, body.data(), uint32_t(body.size()));
  ++s.mem_records;
  s.mem_bytes += length;
}

// A small argument block a call reads through a pointer (kMem): physical views as physical offsets.
void PutArg(State& s, uint32_t va, uint32_t length) {
  if (!va || !bd::mem::try_at<const uint8_t>(va)) return;
  if (va >= 0xA0000000u)
    PutMem(s, 1, bd::gpu::SvrGpuAddress(va) & 0x1FFFFFFFu, length);
  else
    PutMem(s, 0, va, length);
}

// Mid-frame writes a recorded call reads (see kMem in recorder.h).
void NoteMidFrameWrites(State& s, const char* name, const PPCContext& args, uint32_t return_value) {
  if (std::strcmp(name, "D3DDevice_BeginVertices") == 0) {
    s.bv_va = return_value;
    s.bv_size = args.r5.u32 * args.r6.u32;
  } else if (std::strcmp(name, "D3DDevice_EndVertices") == 0) {
    if (s.bv_va && s.bv_size) {
      if (s.bv_va >= 0xA0000000u)
        PutMem(s, 1, bd::gpu::SvrGpuAddress(s.bv_va) & 0x1FFFFFFFu, s.bv_size);
      else
        PutMem(s, 0, s.bv_va, s.bv_size);
    }
    s.bv_va = s.bv_size = 0;
  } else if (std::strcmp(name, "D3DDevice_Resolve") == 0) {
    // Arguments the game builds on its stack just before the call: source rect, dest point, clear colour.
    PutArg(s, args.r5.u32, 16);
    PutArg(s, args.r7.u32, 8);
    PutArg(s, args.r10.u32, 16);
  } else if (std::strcmp(name, "D3DDevice_Clear") == 0) {
    if (args.r4.u32 && args.r4.u32 <= 64) PutArg(s, args.r5.u32, args.r4.u32 * 16u);
  } else if (std::strcmp(name, "D3DDevice_SetScissorRect") == 0) {
    PutArg(s, args.r4.u32, 16);
  } else if (std::strcmp(name, "D3DVertexBuffer_Unlock") == 0) {
    if (const auto* vb = bd::mem::try_at<const bd::gpu::D3DVertexBuffer>(args.r3.u32)) {
      const uint32_t address = uint32_t(vb->FetchLo) & ~3u, size = uint32_t(vb->FetchHi) & 0x03FFFFFCu;
      if (address) PutMem(s, 1, bd::gpu::SvrGpuAddress(address) & 0x1FFFFFFFu, size);
    }
  } else if (std::strcmp(name, "D3DIndexBuffer_Unlock") == 0) {
    if (const auto* ib = bd::mem::try_at<const bd::gpu::D3DIndexBuffer>(args.r3.u32)) {
      const uint32_t address = uint32_t(ib->FetchLo), size = uint32_t(ib->FetchHi);
      if (address) PutMem(s, 1, bd::gpu::SvrGpuAddress(address) & 0x1FFFFFFFu, size);
    }
  }
}

// Start-of-recording content of every GPU-written texture (kTarget, see recorder.h).
void SnapshotTargets(State& s) {
  std::vector<bd::gpu::SvrTargetInfo> targets;
  bd::gpu::SvrListTargets(targets);
  uint32_t saved = 0, skipped = 0;
  for (const auto& t : targets) {
    bd::gpu::GuestTexture* tex = t.host;
    if (!tex || !tex->texture || plume::RenderFormatSize(tex->format) != 4 ||
        tex->sampleCount != plume::RenderSampleCount::COUNT_1) {
      ++skipped;
      continue;
    }
    const uint32_t faces = tex->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE ? 6u : 1u;
    for (uint32_t face = 0; face < faces; ++face) {
      for (uint32_t level = 0; level < tex->mipLevels; ++level) {
        std::vector<uint8_t> pixels;
        uint32_t row_pitch = 0;
        if (!bd::gpu::Video::ReadbackTextureLevel(tex, level, face, pixels, row_pitch)) continue;
        const uint32_t w = std::max(1u, tex->width >> level), h = std::max(1u, tex->height >> level);
        const uint32_t head[7] = {t.texture_va, uint32_t(t.scale * 4.0f + 0.5f), face, level, w, h, row_pitch};
        std::vector<uint8_t> body(sizeof(head) + pixels.size());
        std::memcpy(body.data(), head, sizeof(head));
        std::memcpy(body.data() + sizeof(head), pixels.data(), pixels.size());
        PutRecord(s, kTarget, body.data(), uint32_t(body.size()));
      }
    }
    ++saved;
  }
  REXLOG_INFO("[recorder] snapshot of {} GPU-written textures ({} skipped: not 4 bytes per texel)", saved, skipped);
}

void Finish(State& s) {
  if (!s.chunk.empty()) s.writer.Submit(std::move(s.chunk));
  s.chunk.clear();
  const uint64_t bytes = s.writer.Close();
  s.active = false;
  s.done = true;
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
  REXLOG_INFO(
      "[recorder] wrote {} frames, {:.1f} MB: {} calls, {} base pages, {} changed pages, {:.1f} MB device deltas, "
      "{:.0f} ms scanning, {} mid-frame writes {:.1f} MB, {:.1f} s",
      s.frames, bytes / 1048576.0, s.calls, s.base_pages, s.frame_pages, s.device_bytes / 1048576.0, s.scan_ms,
      s.mem_records, s.mem_bytes / 1048576.0, secs);
}

void WriteMeta(const std::string& dir, uint32_t device) {
  std::FILE* f = std::fopen((dir + "/meta.txt").c_str(), "w");
  if (!f) return;
  std::fprintf(f, "format=W13REC2\ndevice=0x%08X\nframes=%d\n", device, REXCVAR_GET(wwe13_record_frames));
  std::fclose(f);
}
}  // namespace

// w13r test pictures (2K14 harness, tools/arc/arc-run.ps1, same switches as the shipped renderer): WWE2K14_NR_SHOT_EVERY=S
// saves the front buffer every S seconds as WWE2K14_NR_OUT/shot-<n>.png, at most WWE2K14_NR_SHOT_MAX (default 120)
// pictures (default off). Written at the hook after the Swap (its renderer side has run by then), as the recorder does.
static void TestShot(const char* name, const PPCContext& args) {
  if (std::strcmp(name, "D3DDevice_Swap") == 0) ::w13r::NoteGuestSwap();
  static const double every = [] {
    const char* v = std::getenv("WWE2K14_NR_SHOT_EVERY");
    return v ? std::strtod(v, nullptr) : 0.0;
  }();
  if (every <= 0) return;
  static const unsigned max_shots = [] {
    const char* v = std::getenv("WWE2K14_NR_SHOT_MAX");
    return v && v[0] ? unsigned(std::strtoul(v, nullptr, 10)) : 120u;
  }();
  using clock = std::chrono::steady_clock;
  static clock::time_point next = clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(every));
  static uint32_t pending_front = 0;
  static unsigned shots = 0;
  if (pending_front) {
    const char* out = std::getenv("WWE2K14_NR_OUT");
    const std::string dir = out && out[0] ? out : "nr-out";
    char file[64];
    std::snprintf(file, sizeof(file), "/shot-%04u.png", shots);
    svr::replay::WriteTargetPng(pending_front, dir + file);
    pending_front = 0;
  }
  if (shots < max_shots && std::strcmp(name, "D3DDevice_Swap") == 0 && clock::now() >= next) {
    pending_front = args.r4.u32;  // Swap's second argument is the front buffer
    ++shots;
    next += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(every));
  }
}

void RecordHook(const char* name, const PPCContext& args, uint32_t return_value, bool has_return) {
  TestShot(name, args);
  static const bool on = REXCVAR_GET(wwe13_record_frames) > 0;
  if (!on) return;
  State& s = S();
  std::lock_guard lock(s.mutex);
  if (s.done) return;
  const std::vector<uint8_t>* created = has_return ? NoteCreate(s, name, args, return_value) : nullptr;
  const bool swap = std::strcmp(name, "D3DDevice_Swap") == 0;
  if (!s.active) {
    // Start at a frame boundary: the first Swap; its device argument is the D3D device.
    if (!swap) return;
    if (s.skipped++ < REXCVAR_GET(wwe13_record_after)) return;
    const std::string dir = REXCVAR_GET(wwe13_record_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!s.writer.Open(dir + "/recording.w13r")) {
      REXLOG_ERROR("[recorder] cannot write to {}", dir);
      s.done = true;
      return;
    }
    s.requested = REXCVAR_GET(wwe13_record_frames);
    {
      const std::string list = REXCVAR_GET(wwe13_record_png);
      size_t i = 0;
      while (i < list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        const std::string tok = list.substr(i, j - i);
        if (!tok.empty()) s.png_frames.insert(uint32_t(std::strtoul(tok.c_str(), nullptr, 10)));
        i = j + 1;
      }
    }
    s.device = args.r3.u32;
    s.chunk.reserve(kChunkBytes + (1u << 20));
    s.active = true;
    s.scan_pending = true;
    s.start = std::chrono::steady_clock::now();
    WriteMeta(dir, s.device);
    for (const auto& [obj, body] : s.shaders) PutRecord(s, kShader, body.data(), uint32_t(body.size()));
    for (const auto& [obj, body] : s.decls) PutRecord(s, kDecl, body.data(), uint32_t(body.size()));
    REXLOG_INFO("[recorder] recording {} frames to {} (device 0x{:08X}, {} shaders, {} declarations)", s.requested,
                dir, s.device, s.shaders.size(), s.decls.size());
    return;
  }
  if (s.png_pending) {
    // The renderer side of the previous Swap has run by now (SVR_HOOK_AFTER records before it).
    s.png_pending = false;
    svr::replay::WriteTargetPng(s.png_front,
                                REXCVAR_GET(wwe13_record_dir) + "/live_" + std::to_string(s.png_index) + ".png");
  }
  if (s.scan_pending) {
    const bool base = !s.base_taken;
    Scan(s, base);
    if (base) SnapshotTargets(s);
    s.base_taken = true;
    s.scan_pending = false;
  }
  if (created)
    PutRecord(s, std::strcmp(name, "D3DDevice_CreateVertexDeclaration") == 0 ? kDecl : kShader, created->data(),
              uint32_t(created->size()));
  NoteMidFrameWrites(s, name, args, return_value);
  DeviceDelta(s);
  EventRecord event{};
  event.hook_id = HookId(name);
  event.flags = has_return ? 1u : 0u;
  event.timestamp_ns = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - s.start).count());
  event.return_value = return_value;
  const uint64_t regs[21] = {args.r1.u64,  args.r2.u64,  args.r3.u64, args.r4.u64, args.r5.u64, args.r6.u64,
                             args.r7.u64,  args.r8.u64,  args.r9.u64, args.r10.u64, args.r11.u64, args.r12.u64,
                             args.r13.u64, args.f1.u64,  args.f2.u64, args.f3.u64, args.f4.u64, args.f5.u64,
                             args.f6.u64,  args.f7.u64,  args.f8.u64};
  std::memcpy(event.registers, regs, sizeof(regs));
  PutRecord(s, kCall, &event, sizeof(event));
  ++s.calls;
  if (swap) {
    const uint32_t index = uint32_t(s.frames);
    PutRecord(s, kFrameEnd, &index, sizeof(index));
    if (s.png_frames.count(index)) {
      s.png_pending = true;
      s.png_index = index;
      s.png_front = args.r4.u32;  // Swap's second argument is the front buffer
    }
    s.scan_pending = true;  // the next frame's data is written after this Swap
    if (++s.frames >= s.requested) Finish(s);
  }
}
}  // namespace svr::recorder
