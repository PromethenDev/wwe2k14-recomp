// RR2: offline replay (rr_play.h).
#include "rr_play.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

#include "../nr/nr_backend.h"
#include "capture.h"
#include "present.h"
#include "rr_mem.h"
#include "rr_mem_core.h"
#include "rr_png.h"
#include "shadow_types.h"
#include "shader_identity.h"
#include "sidefile.h"
#include "textures.h"
#include "worker.h"

namespace wwe2k14::d3d {
namespace runtime {
void BootBackendFromWorker();  // runtime.cpp
}
}  // namespace wwe2k14::d3d

namespace wwe2k14::d3d::rr {

namespace {

namespace nb = nr::backend;

void SetEnv(const char* k, const char* v) {
#ifdef _WIN32
  _putenv_s(k, v ? v : "");
#else
  if (v) setenv(k, v, 1);
  else unsetenv(k);
#endif
}

uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// Pages the replay applied, per space (the self-check of rr_mem.h BeginReplayCheck).
std::vector<uint8_t> g_cov[2];
bool g_started = false;  // past the start state (snapshot + registry): later page changes act as guest writes
bool Covered(uint32_t space, uint32_t page) {
  const uint32_t i = page >> 12;
  return space < 2 && i < g_cov[space].size() && g_cov[space][i];
}

// Same channel mapping as the live presenter (runtime.cpp PresentD3dOnly).
void SwizzleOf(int32_t swz, bool swap_rb, uint8_t sw[4]) {
  for (int c = 0; c < 4; ++c) {
    uint8_t ch = uint8_t((uint32_t(swz) >> (3 * c)) & 7);
    if (swap_rb && (ch == 0 || ch == 2)) ch = uint8_t(2 - ch);
    sw[c] = ch;
  }
}

// Reads snapshot `snap` back and writes <dir>/frame-g<game_frame>.png; *desc = one result line. Backend lock held.
bool WriteFramePng(const std::filesystem::path& dir, uint32_t game_frame, uint32_t snap, uint32_t w, uint32_t h,
                   const uint8_t sw[4], std::string* desc) {
  char name[64], line[256];
  std::snprintf(name, sizeof name, "frame-g%06u.png", game_frame);
  std::vector<uint8_t> px;
  std::string err;
  if (!nb::ReadSnapshot(snap, w, h, px, &err)) {
    std::snprintf(line, sizeof line, "frame %u: readback FAILED: %s", game_frame, err.c_str());
    *desc = line;
    return false;
  }
  std::vector<uint8_t> rgb(size_t(w) * h * 3);
  for (size_t p = 0; p < size_t(w) * h; ++p)
    for (int c = 0; c < 3; ++c) rgb[p * 3 + c] = sw[c] <= 3 ? px[p * 4 + sw[c]] : (sw[c] == 5 ? 255 : 0);
  const bool ok = png::WriteRgb((dir / name).string(), rgb.data(), w, h);
  std::snprintf(line, sizeof line, "frame %u: %s %ux%u hash=%016llX%s", game_frame, name, w, h,
                (unsigned long long)Fnv(rgb.data(), rgb.size()), ok ? "" : " WRITE FAILED");
  *desc = line;
  return ok;
}

[[noreturn]] void Finish(int code, FILE* summary, const char* why) {
  REXLOG_INFO("[rr-play] exit {}: {}", code, why);
  std::fprintf(stderr, "[rr-play] exit %d: %s\n", code, why);
  if (summary) {
    std::fprintf(summary, "exit %d: %s\n", code, why);
    std::fclose(summary);
  }
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(code);  // no runtime / backend teardown (nothing of the game was started)
}

}  // namespace

void RunReplayIfRequested() {
  const char* file = std::getenv("WWE2K14_RR_PLAY");
  if (!file || !*file) return;
  const std::filesystem::path out = std::getenv("WWE2K14_RR_OUT") && *std::getenv("WWE2K14_RR_OUT")
                                        ? std::getenv("WWE2K14_RR_OUT")
                                        : "rr-out";
  const char* swaps_env = std::getenv("WWE2K14_RR_SWAPS");
  const uint64_t max_swaps = swaps_env && *swaps_env ? std::strtoull(swaps_env, nullptr, 10) : UINT64_MAX;
  std::error_code ec;
  std::filesystem::create_directories(out, ec);
  FILE* summary = std::fopen((out / "summary.txt").string().c_str(), "w");

  // Offline switches (rr_play.h). Nothing has read them yet: every reader latches on first use.
  SetEnv("WWE2K14_D3D_OBSERVE", "1");
  SetEnv("WWE2K14_D3D_SHADOW", "2");
  SetEnv("WWE2K14_NR_PRESENT_D3D", "1");
  SetEnv("WWE2K14_NR_DRAW_OFF", "1");
  SetEnv("WWE2K14_D3D_NATIVE_BOOT", "1");
  SetEnv("WWE2K14_NR_DIRECT_PRESENT", "0");
  SetEnv("WWE2K14_D3D_LATE_TEX", nullptr);
  SetEnv("WWE2K14_D3D_SIDEFILE_OUT", nullptr);
  SetEnv("WWE2K14_D3D_SIDEFILE", nullptr);

  sidefile::Reader reader;
  std::string why;
  if (!reader.Load(file, &why)) Finish(2, summary, ("load " + std::string(file) + ": " + why).c_str());
  const std::string sum = reader.Summary();
  REXLOG_INFO("[rr-play] {}: {}{}", file, sum, reader.truncated() ? " (truncated tail)" : "");
  if (summary) std::fprintf(summary, "file %s\nversion %u\n%s%s\n", file, reader.version(), sum.c_str(),
                            reader.truncated() ? " (truncated tail)" : "");
  if (!reader.size()) Finish(2, summary, "no events");

  // Standalone guest memory (SDK Memory without the kernel: reserves the 4 GB + physical views, commits physical).
  static auto* memory = new rex::memory::Memory();  // leaked: worker caches keep pointers until _Exit
  if (!memory->Initialize()) Finish(3, summary, "Memory::Initialize failed");
  uint8_t* const vbase = memory->virtual_membase();
  uint8_t* const pbase = memory->TranslatePhysical<uint8_t*>(0);
  if (const char* st = std::getenv("WWE2K14_RR_SNAPSHOT_TEST"); st && *st == '1') {
    std::string rep;
    const bool ok = SnapshotSelfTest(memory, &rep);
    REXLOG_INFO("[rr-play] {}", rep);
    if (summary) std::fprintf(summary, "%s\n", rep.c_str());
    if (!ok) Finish(7, summary, "snapshot self-test failed");
  }
  textures::SetMemory(memory);
  SetShadowGuestBase(vbase);
  if (const char* it = std::getenv("WWE2K14_RR_IMAGE_TEST"); it && *it == '1') {
    // RR5 desk check: CreateSnapshotRaw -> ReadSnapshotRaw returns the same bytes (odd sizes: row repitch), for the
    // formats resolves use; a depth format must be refused. plume RenderFormat: 20 R8G8B8A8_UNORM,
    // 11 R16G16B16A16_UNORM, 35 R32_FLOAT, 32 D32_FLOAT.
    runtime::BootBackendFromWorker();
    BackendLock().lock();
    bool ok = nb::Ready();
    std::string rep = nb::Ready() ? "" : "backend not ready; ";
    uint32_t x = 99;
    for (const auto& [fmt, bpp] : {std::pair<uint32_t, uint32_t>{20, 4}, {11, 8}, {35, 4}}) {
      const uint32_t w = 37, h = 19;
      std::vector<uint8_t> in(size_t(w) * h * bpp);
      for (auto& b : in) b = uint8_t((x = x * 1664525u + 1013904223u) >> 24);
      if (fmt == 35) for (size_t i = 0; i < in.size(); i += 4) in[i + 3] = 0x3F;  // finite floats
      std::string err;
      const uint32_t id = nb::CreateSnapshotRaw(fmt, w, h, in, &err);
      uint32_t f2 = 0, w2 = 0, h2 = 0;
      std::vector<uint8_t> out;
      const bool good = id != nb::kNone && nb::ReadSnapshotRaw(id, &f2, &w2, &h2, out, &err) && f2 == fmt && w2 == w &&
                        h2 == h && out == in;
      if (id != nb::kNone) nb::ReleaseSnapshot(id);
      rep += "fmt" + std::to_string(fmt) + (good ? " ok; " : " BAD (" + err + "); ");
      ok &= good;
    }
    std::string err;
    const uint32_t depth = nb::CreateSnapshotRaw(32, 8, 8, std::vector<uint8_t>(256), &err);
    rep += depth == nb::kNone ? "depth refused ok" : "depth NOT refused";
    ok &= depth == nb::kNone;
    if (depth != nb::kNone) nb::ReleaseSnapshot(depth);
    BackendLock().unlock();
    rep = "image round-trip self-test: " + rep + (ok ? " -> PASS" : " -> FAIL");
    REXLOG_INFO("[rr-play] {}", rep);
    if (summary) std::fprintf(summary, "%s\n", rep.c_str());
    if (!ok) Finish(8, summary, "image self-test failed");
  }
  g_cov[kVirtual].assign(size_t(1) << 20, 0);
  g_cov[kPhysical].assign(size_t(0x20000000u) >> 12, 0);
  if (reader.phys_snapshot()) std::fill(g_cov[kPhysical].begin(), g_cov[kPhysical].begin() + (kPhysSnapshotEnd >> 12), 1);
  BeginReplayCheck(pbase, &Covered);
  REXLOG_INFO("[rr-play] guest memory: virtual base {} physical base {}", static_cast<void*>(vbase),
              static_cast<void*>(pbase));

  const auto write_page = [&](uint32_t space, uint32_t page, const uint8_t* bytes) {
    uint8_t* dst = nullptr;
    if (space == kPhysical) {
      dst = pbase + (page & 0x1FFFF000u);
    } else {
      dst = vbase + page;
#ifdef _WIN32
      // Windows: guest views are reserved, not committed (physical is committed by Initialize).
      if (!g_cov[kVirtual][page >> 12])
        rex::memory::AllocFixed(dst, kPageBytes, rex::memory::AllocationType::kCommit, rex::memory::PageAccess::kReadWrite);
#endif
    }
    // RR5 (drift found 11-18 night): live, a CPU write to a watched page reaches the caches through the write watch.
    // Replay writes the host page directly, so after the start state a changed physical page marks its watch
    // generation like the live callback would (else the static vertex cache serves old crowd poses: 95 % match).
    if (g_started && space == kPhysical && std::memcmp(dst, bytes, kPageBytes) != 0)
      textures::ReplayMarkWritten(page & 0x1FFFF000u, kPageBytes);
    std::memcpy(dst, bytes, kPageBytes);
    g_cov[space == kPhysical ? kPhysical : kVirtual][(space == kPhysical ? (page & 0x1FFFF000u) : page) >> 12] = 1;
  };
  const auto invalidate = [](uint32_t start, uint32_t length) { textures::ReplayMarkWritten(start, length); };

  // RR4: the recorded shader table first (live, the game thread captured it before / during the window).
  for (const ShdRecord& sr : reader.shd()) ShaderReplayIntern(sr.stage, sr.raw, sr.bytes);
  REXLOG_INFO("[rr-play] {} shaders interned", reader.shd().size());

  Applier applier(&reader.mem(), &reader.inv());
  uint64_t swaps = 0, written = 0, no_frame = 0, read_fail = 0;
  // RR5: the live resolve registry at window open (GPU-only images made before the window). Memory first (registration
  // samples the destination bytes), then the backend (started as the worker would), then each image re-registered as
  // one plain resolve covering its whole saved image.
  uint64_t rim_ok = 0, rim_fail = 0;
  if (!reader.rim().empty()) {
    applier.Before(reader.at(0).h.seq, write_page, invalidate);
    runtime::BootBackendFromWorker();
    BackendLock().lock();
    std::string first_error;
    for (const RimRecord& ri : reader.rim()) {
      std::string err;
      const std::vector<uint8_t> px(ri.px, ri.px + ri.px_bytes);
      const uint32_t id = nb::Ready() ? nb::CreateSnapshotRaw(ri.format, ri.w, ri.h, px, &err) : nb::kNone;
      if (id == nb::kNone || ri.desc_bytes != sizeof(ResolveDesc) || ri.key_bytes != sizeof(nb::ResolveCensusKey)) {
        if (id != nb::kNone) nb::ReleaseSnapshot(id);
        if (!rim_fail++) first_error = err.empty() ? "record layout differs from this build" : err;
        continue;
      }
      ResolveDesc d;
      std::memcpy(&d, ri.desc, sizeof d);
      d.src_rect[0] = d.src_rect[1] = 0;
      d.src_rect[2] = int32_t(ri.w);
      d.src_rect[3] = int32_t(ri.h);
      nb::ResolveCensusKey key;
      std::memcpy(&key, ri.key, sizeof key);
      textures::RegisterResolveImage(ri.start, ri.length, d, id, key);
      ++rim_ok;
    }
    BackendLock().unlock();
    REXLOG_INFO("[rr-play] resolve registry restored: {} images, {} failed{}{}", rim_ok, rim_fail, rim_fail ? ": " : "",
                first_error);
    if (summary) std::fprintf(summary, "resolve registry restored: %llu images, %llu failed %s\n",
                              (unsigned long long)rim_ok, (unsigned long long)rim_fail, first_error.c_str());
  }
  applier.Before(reader.at(0).h.seq, write_page, invalidate);  // start state (window-open snapshot) without marks
  g_started = true;
  const auto t0 = std::chrono::steady_clock::now();
  size_t i = 0;
  for (; i < reader.size() && swaps < max_swaps; ++i) {
    const sidefile::Event& ev = reader.at(i);
    applier.Before(ev.h.seq, write_page, invalidate);
    WorkerConsume(ev.h, ev.payload, i + 1 < reader.size());
    if (ev.h.kind != EvKind::kSwap) continue;
    WorkerIdle();  // releases the backend lock the worker holds across a batch
    const uint64_t ord = swaps++;
    BackendLock().lock();
    uint32_t w = 0, h = 0;
    bool swap_rb = false;
    int32_t swz = -1;
    std::string info;
    uint32_t gframe = 0;
    const uint32_t snap = TakePresentFrame(ord, &w, &h, &swap_rb, &swz, &info, &gframe);
    char line[256];
    if (snap == nb::kNone || swz < 0 || !w || !h) {
      ++no_frame;
      std::snprintf(line, sizeof line, "swap %llu frame %u: not presentable (snap=%u swizzle=%d %ux%u)",
                    (unsigned long long)ord, ev.h.frame, snap, swz, w, h);
      if (snap != nb::kNone) nb::ReleaseSnapshot(snap);
    } else {
      uint8_t sw[4];
      SwizzleOf(swz, swap_rb, sw);
      std::string desc;
      written += WriteFramePng(out, gframe, snap, w, h, sw, &desc);
      read_fail += desc.find("FAILED") != std::string::npos;
      std::snprintf(line, sizeof line, "swap %llu %s", (unsigned long long)ord, desc.c_str());
      nb::ReleaseSnapshot(snap);
    }
    nb::OfflineFrameEnd();
    BackendLock().unlock();
    REXLOG_INFO("[rr-play] {}", line);
    if (summary) std::fprintf(summary, "%s\n", line);
  }
  WorkerIdle();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const ReplayCheckStats cs = GetReplayCheckStats();
  const WorkerStats& ws = WorkerStatsRef();
  char tail[512];
  std::snprintf(tail, sizeof tail,
                "events %zu/%zu swaps %llu pngs %llu not_presentable %llu readback_fail %llu | pages applied %zu/%zu "
                "invalidations %zu/%zu | reads checked %llu uncovered_reads %llu uncovered_pages %llu | worker events "
                "%llu apply_fail %llu | %.1f s",
                i, reader.size(), (unsigned long long)swaps, (unsigned long long)written, (unsigned long long)no_frame,
                (unsigned long long)read_fail, applier.mem_applied(), reader.mem().size(), applier.inv_applied(),
                reader.inv().size(), (unsigned long long)cs.reads, (unsigned long long)cs.uncovered_reads,
                (unsigned long long)cs.uncovered_pages, (unsigned long long)ws.events.load(),
                (unsigned long long)ws.apply_fail.load(), secs);
  REXLOG_INFO("[rr-play] {}", tail);
  if (summary) std::fprintf(summary, "%s\n", tail);
  {  // texture census (live prints the same counters every 300 frames as [d3d-tex]): compare per frame
    const textures::Counters& tc = textures::GetCounters();
    char tx[384];
    std::snprintf(tx, sizeof tx,
                  "textures: hits %llu misses %llu resolve_binds %llu refusals %llu (resolve %llu) resolves_registered %llu "
                  "unusable %llu | per swap: resolve_binds %.1f resolves %.1f misses %.1f",
                  (unsigned long long)tc.hits, (unsigned long long)tc.misses, (unsigned long long)tc.resolve_binds,
                  (unsigned long long)tc.refusals, (unsigned long long)tc.refused_resolve,
                  (unsigned long long)tc.resolves_registered, (unsigned long long)tc.resolves_unusable,
                  swaps ? double(tc.resolve_binds) / swaps : 0.0, swaps ? double(tc.resolves_registered) / swaps : 0.0,
                  swaps ? double(tc.misses) / swaps : 0.0);
    REXLOG_INFO("[rr-play] {}", tx);
    if (summary) std::fprintf(summary, "%s\n", tx);
  }
  if (!nb::Ready()) Finish(4, summary, "renderer not ready");
  if (!written) Finish(5, summary, "no frame presented");
  if (cs.uncovered_reads) Finish(6, summary, "worker read pages no recorded page covered");
  Finish(0, summary, "ok");
}

void MaybeLiveShot(uint32_t snap, uint32_t w, uint32_t h, const uint8_t swizzle[4], uint32_t game_frame) {
  static const std::filesystem::path dir = [] {
    const char* e = std::getenv("WWE2K14_RR_LIVE_OUT");
    std::filesystem::path p = e && *e ? e : "";
    std::error_code ec;
    if (!p.empty()) std::filesystem::create_directories(p, ec);
    return p;
  }();
  if (dir.empty() || snap == nb::kNone || !w || !h || !sidefile::InWindow(game_frame)) return;
  std::string desc;
  WriteFramePng(dir, game_frame, snap, w, h, swizzle, &desc);
  REXLOG_INFO("[rr-live] {}", desc);
}

}  // namespace wwe2k14::d3d::rr
