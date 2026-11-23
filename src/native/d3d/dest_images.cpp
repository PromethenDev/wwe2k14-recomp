// Card S3-69 (SVR plan SV-6): see dest_images.h.
#include "dest_images.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <set>
#include <tuple>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/logging.h>

#include "../nr/nr_backend.h"

namespace wwe2k14::d3d::dest {
namespace {

namespace nb = nr::backend;

bool Env1(const char* name) {
  const char* e = std::getenv(name);
  return e && std::strcmp(e, "1") == 0;
}

struct Hist {
  std::deque<RegRec> regs;
  std::deque<std::string> drops;
  uint64_t first_fp = 0, first_frame = 0;
  bool have_first = false;
};
constexpr size_t kRegsPerDest = 8, kDropsPerDest = 6, kMaxDests = 8192, kMaxLines = 400;
std::unordered_map<uint32_t, Hist> g_hist;
std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, std::string>> g_seen;
Counters g_c;

}  // namespace

bool On() {
  static const bool on = Env1("WWE2K14_D3D_DEST_IMAGES");
  return on;
}
bool CanvasOn() {
  static const bool on = On() && Env1("WWE2K14_D3D_DEST_CANVAS");
  return on;
}
bool CanvasReuseOn() {
  static const bool on = [] {
    if (!(CanvasOn() && Env1("WWE2K14_D3D_DEST_CANVAS_REUSE"))) return false;
    if (nb::IsD3D12()) {
      REXLOG_WARN("[d3d-dest] CANVAS_REUSE=1 disabled: D3D12 queued snapshot calls are not covered by this cache");
      return false;
    }
    return true;
  }();
  return on;
}
bool MipsOn() {
  // Card S3-69 (5b): on the D3D12 backend the per-level CreateTexture and copy calls are queued to the record thread
  // (they can no longer be issued inside the worker thread), so the mip image cannot be rebuilt synchronously; the
  // M2c5 snapshot-derived path is used instead.
  static const bool on = [] {
    if (!(On() && Env1("WWE2K14_D3D_DEST_MIPS"))) return false;
    if (nb::IsD3D12()) {
      REXLOG_WARN("[d3d-dest] MIPS=1 disabled: D3D12 backend queues texture creation (5b needs sync rebuild)");
      return false;
    }
    return true;
  }();
  return on;
}
bool CubeOn() {
  // Card S3-69 (5c): same D3D12 limitation as 5b.
  static const bool on = [] {
    if (!(On() && Env1("WWE2K14_D3D_DEST_CUBE"))) return false;
    if (nb::IsD3D12()) {
      REXLOG_WARN("[d3d-dest] CUBE=1 disabled: D3D12 backend queues texture creation (5c needs sync rebuild)");
      return false;
    }
    return true;
  }();
  return on;
}
bool ObjBypassOn() {
  static const bool on = On() && Env1("WWE2K14_D3D_DEST_OBJ_BYPASS");
  return on;
}
bool TraceOn() {
  static const bool on = Env1("WWE2K14_D3D_DEST_TRACE");
  return on;
}
bool CountOn() {
  static const bool on = On() || TraceOn();
  return on;
}
Counters& C() { return g_c; }

std::string Section() {
  if (!CountOn()) return {};
  const Counters& c = g_c;
  return fmt::format(" | dest: canvas_new={} (forced {} seeded {} too_big {}) inplace={} cache_reuse={} cache_miss/evict={}/{} seed_cpu_detile_ms={}({}) seed_gpu_submit_ms={}({}) seed_upload_ms={}({}) seed_upload_source_MB={} seed_upload_pad_MB={} seed_upload_avg_B={} seed_source_transform_ms={}({}) seed_trace_cpu_ms={}({}) seed_cache_MB={} uncovered_binds={} obj_bypass={} obj eq/ne/null={}/{}/{} (fetch0 {} dest0 {}) part eq/ne={}/{} "
                     "mip new/inplace/levels={}/{}/{} cube new/inplace/faces={}/{}/{} entries={} canvases={} dest_mb={:.1f} dest_high_mb={:.1f}",
                      c.canvas_new, c.canvas_forced, c.canvas_seeded, c.canvas_too_big, c.canvas_inplace,
                      c.canvas_cache_reuse, c.canvas_cache_miss, c.canvas_cache_evict,
                      double(c.seed_detile_ns) / 1.0e6, c.seed_detile_calls,
                      double(c.seed_gpu_submit_ns) / 1.0e6, c.seed_gpu_calls, double(c.seed_upload_ns) / 1.0e6,
                      c.seed_upload_calls, double(c.seed_upload_bytes) / 1048576.0,
                      double(c.seed_upload_padded_bytes) / 1048576.0,
                      c.seed_upload_calls ? c.seed_upload_bytes / c.seed_upload_calls : 0,
                      double(c.seed_source_transform_ns) / 1.0e6, c.seed_source_transform_calls,
                      double(c.seed_trace_cpu_ns) / 1.0e6, c.seed_trace_cpu_calls,
                      double(c.seed_cache_bytes) / 1048576.0, c.uncovered_binds, c.obj_bypass,
                      c.obj_eq, c.obj_ne, c.obj_null, c.obj_fetch_null, c.obj_dest_null, c.obj_part_eq, c.obj_part_ne, c.mip_new, c.mip_inplace, c.mip_levels_copied, c.cube_new, c.cube_inplace, c.cube_faces_copied, c.dest_entries, c.dest_canvases, double(c.dest_bytes) / 1048576.0,
                      double(c.dest_bytes_high) / 1048576.0);
}

void NoteRegistration(uint32_t dest_base, const RegRec& r) {
  if (!TraceOn()) return;
  if (g_hist.size() >= kMaxDests && !g_hist.count(dest_base)) g_hist.clear();  // bounded (crude: restart)
  Hist& h = g_hist[dest_base];
  if (!h.have_first) {
    h.have_first = true;
    h.first_fp = r.dest_fp;
    h.first_frame = r.frame;
  }
  h.regs.push_back(r);
  if (h.regs.size() > kRegsPerDest) h.regs.pop_front();
}

void NoteDrop(uint32_t dest_base, uint64_t seq, uint64_t frame, const char* reason) {
  if (!TraceOn()) return;
  auto it = g_hist.find(dest_base);
  if (it == g_hist.end()) return;
  it->second.drops.push_back(fmt::format("seq={}@f{}:{}", seq, frame, reason));
  if (it->second.drops.size() > kDropsPerDest) it->second.drops.pop_front();
}

std::string History(uint32_t dest_base) {
  auto it = g_hist.find(dest_base);
  if (it == g_hist.end()) return {};
  std::string s;
  for (const RegRec& r : it->second.regs) {
    s += fmt::format(" {{f{} seq={} rect={},{}-{},{} pitch={} dest_h={} info={:08X} kind={} canvas={} at={:08X}+{:X} obj={:08X}{} "
                     "dfp={:016x} dzero={}}}",
                     r.frame, r.seq, r.rect[0], r.rect[1], r.rect[2], r.rect[3], r.pitch, r.dest_h, r.info, r.kind, r.canvas,
                     r.start, r.length, r.dest_texture,
                     r.has_point ? fmt::format(" point={},{}", r.point[0], r.point[1]) : std::string(), r.dest_fp,
                     r.dest_zero);
  }
  if (!it->second.drops.empty()) {
    s += " drops:";
    for (const std::string& d : it->second.drops) s += " " + d;
  }
  return s;
}

bool FirstFingerprint(uint32_t dest_base, uint64_t* fp, uint64_t* frame) {
  auto it = g_hist.find(dest_base);
  if (it == g_hist.end() || !it->second.have_first) return false;
  *fp = it->second.first_fp;
  *frame = it->second.first_frame;
  return true;
}

bool TraceRefusalOnce(uint32_t base, uint32_t fmt_, uint32_t w, uint32_t h, const std::string& cls) {
  if (!TraceOn() || g_seen.size() >= kMaxLines) return false;
  return g_seen.insert({base, fmt_, w, h, cls}).second;
}

}  // namespace wwe2k14::d3d::dest
