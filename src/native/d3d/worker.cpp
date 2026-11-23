// WP9 (decision 0006 stage 2): worker side of the capture -> worker pipeline. See capture.h for the record layout.
// Runs on the consumer thread of the nr::RecordQueue owned by capture.cpp. Written for this project.
#include "worker.h"

#include "stat_timing.h"  // C1-T3: sample the statistics-only stopwatches 1 frame in N
#include <chrono>
#include <cstdlib>
#include <thread>
#include "../nr/nr_record_queue.h"  // nr::CpuRelax
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include <rex/logging.h>

#include "../nr/nr_backend.h"
#include "../nr/nr_pc_sampler.h"
#include "capture.h"
#include "capture_core.h"
#include "device_state.h"
#include "draw.h"
#include "gate.h"
#include "rr_mem.h"
#include "present.h"
#include "regview.h"
#include "resolves.h"
#include "shader_identity.h"
#include "targets.h"
#include "textures.h"
#include "texture_quality.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace wwe2k14::d3d {
namespace runtime { void BootBackendFromWorker(); }  // runtime.cpp
namespace {

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
uint64_t ThreadCpuNs() {
#ifdef _WIN32
  FILETIME c, e, k, u;
  if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
  auto to100 = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return (to100(k) + to100(u)) * 100;
#else
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
}

struct EventSlot {  // the event currently handed to the worker
  EvHeader h;
  const uint8_t* payload = nullptr;
  bool pending = false;
};
EventSlot g_cur;

struct WorkerState {
  bool backend_seen_ready = false;  // S3-2
  cap::Image img;
  DeviceState st;
  RegView rv;
  std::unique_ptr<nr::backend::Draw> local_draw;
  bool lock_held = false;
  uint32_t batch = 0;
  bool drain = false;
  uint64_t last_cpu = 0;
  bool init = false;
  uint32_t snap_logged = 0, fail_logged = 0;
  std::mutex why_mu;
  std::map<std::string, uint64_t> why;
};
WorkerState& W() {
  static WorkerState* w = new WorkerState;
  return *w;
}

// V17d step 1: opt-in census of TranslateDraw time for the known crowd pixel shader. This is narrower than the
// aggregate worker translate bucket, which also includes DoDraw setup and backend submission; disabled by default.
constexpr uint64_t kCrowdTranslatePsFnv = 0xF5C9EF1CFA228C6Eull;
struct CrowdTranslateCensus {
  bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_CROWD_TRANSLATE_CENSUS");
    return e && std::strcmp(e, "1") == 0;
  }();
  bool have_frame = false;
  uint32_t last_frame = 0;
  uint32_t draw_frames = 0;
  uint64_t all_calls = 0, crowd_calls = 0;
  uint64_t all_ns = 0, crowd_ns = 0;
};
CrowdTranslateCensus& CrowdCensus() {
  static CrowdTranslateCensus c;
  return c;
}
void CrowdCensusFrame(uint32_t frame) {
  CrowdTranslateCensus& c = CrowdCensus();
  if (!c.have_frame) {
    c.have_frame = true;
    c.last_frame = frame;
    c.draw_frames = 1;
    return;
  }
  if (frame == c.last_frame) return;
  if (c.draw_frames == 300) {
    const double all_ms = double(c.all_ns) / 1.0e6;
    const double crowd_ms = double(c.crowd_ns) / 1.0e6;
    REXLOG_INFO("[d3d-crowd-xlate] ps={:016X} scope=TranslateDraw draw_frames={} translate_calls={} crowd_calls={} "
                "translate_ms={:.3f} crowd_translate_ms={:.3f} crowd_translate_ms/f={:.4f} share_pct={:.2f}",
                kCrowdTranslatePsFnv, c.draw_frames, c.all_calls, c.crowd_calls, all_ms, crowd_ms,
                crowd_ms / double(c.draw_frames), all_ms > 0.0 ? crowd_ms * 100.0 / all_ms : 0.0);
    c.draw_frames = 0;
    c.all_calls = c.crowd_calls = c.all_ns = c.crowd_ns = 0;
  }
  c.last_frame = frame;
  ++c.draw_frames;
}
void CrowdCensusRecord(bool is_crowd, uint64_t elapsed_ns) {
  CrowdTranslateCensus& c = CrowdCensus();
  ++c.all_calls;
  c.all_ns += elapsed_ns;
  if (is_crowd) {
    ++c.crowd_calls;
    c.crowd_ns += elapsed_ns;
  }
}

bool GuestRead(uint32_t addr, void* dst, uint32_t bytes) {
  const uint8_t* b = ShadowGuestBase();
  if (!b || !GuestRangeOk(addr, bytes)) return false;
  rr::NoteRead(addr, bytes);
  std::memcpy(dst, b + addr, bytes);
  return true;
}

void CountWhy(WorkerState& w, const std::string& why) {
  std::lock_guard<std::mutex> lk(w.why_mu);
  if (w.why.size() < 64 || w.why.count(why)) {
    const uint64_t n = ++w.why[why];
    // Bounded log: each reason at its 1st, 10th, 100th, ... occurrence (late-session draw losses, s398/s402).
    if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 || n == 100000 || n == 1000000)
      REXLOG_WARN("[d3d-worker] draw dropped x{}: {}", n, why);
  }
}

void Init(WorkerState& w) {
  w.init = true;
  w.local_draw = std::make_unique<nr::backend::Draw>();
  SetGuestBase(ShadowGuestBase());
  targets::SetGuestReader(&GuestRead);
}

void DoDraw(WorkerState& w, const EvHeader& h, const EvView& v, bool mode2) {
  WorkerStats& S = WorkerStatsRef();
  pd::WholeScope pd_whole;
  const uint64_t pd_pre = pd::Now();
  w.rv.ApplyDrawArgs(h);
  {
    ShaderKey vs, ps;
    const EvExt& x = *v.ext;
    if (x.key_valid & 1) { vs.fnv = x.vs_fnv; vs.object = x.vs_object; vs.code_addr = x.vs_code_addr; vs.bytes = x.vs_bytes; vs.stage = 0; }
    if (x.key_valid & 2) { ps.fnv = x.ps_fnv; ps.object = x.ps_object; ps.code_addr = x.ps_code_addr; ps.bytes = x.ps_bytes; ps.stage = 1; }
    w.rv.SetShaderKeys(vs, ps);
  }
  pd::Add(pd::kDoPre, pd_pre);
  const uint64_t pd_acquire = pd::Now();
  nr::backend::Draw* d;
  if (mode2) d = nr::backend::AcquireDraw();
  else { d = w.local_draw.get(); d->Reset(); }
  pd::Add(pd::kAcquire, pd_acquire);
  DrawSide side;
  side.packet_addr = h.packet_addr;
  side.seq = h.seq;
  std::string why;
  const uint64_t pd_translate = pd::Now();
  CrowdTranslateCensus& crowd_census = CrowdCensus();
  if (crowd_census.on) CrowdCensusFrame(h.frame);
  const bool census_translate = crowd_census.on && d;
  const uint64_t census_start = census_translate ? NowNs() : 0;
  const bool ok = d && TranslateDraw(h, w.rv, w.st, d, &side, &why);
  if (census_translate)
    CrowdCensusRecord(v.ext->ps_fnv == kCrowdTranslatePsFnv, NowNs() - census_start);
  pd::Add(pd::kTranslate, pd_translate);
  if (!ok) {
    S.draws_skipped.fetch_add(1, std::memory_order_relaxed);
    if (!side.thinned) CountWhy(w, why.empty() ? "translate failed" : why);
    if (mode2) nr::backend::AbandonDraw();
    return;
  }
  S.draws_translated.fetch_add(1, std::memory_order_relaxed);
  d->ordinal = h.seq;
  if (mode2 && side.texture_fail) {
    S.tex_fail_draws.fetch_add(1, std::memory_order_relaxed);  // not submitted: the pooled slot is reused (AcquireDraw)
    nr::backend::AbandonDraw();
  } else if (mode2) {
    std::string err;
    const uint64_t pd_sub = pd::Now();
    const bool sub_ok = nr::backend::SubmitDraw(d, &err);
    pd::Add(pd::kSubmit, pd_sub);
    if (sub_ok) {
      S.draws_submitted.fetch_add(1, std::memory_order_relaxed);
      const uint64_t pd_ds = pd::Now();
      DrawSubmitted(side);
      pd::Add(pd::kDrawSub, pd_ds);
    }
    else {
      S.submit_fail.fetch_add(1, std::memory_order_relaxed);
      CountWhy(w, "submit: " + err);
    }
  }
}

void DoResolve(WorkerState& w, const EvHeader& h, const EvView& v) {
  ResolveDesc r{};
  r.flags = h.args[0];
  r.dest_texture = h.args[2];
  r.level = h.args[4];
  r.slice = h.args[5];
  r.src_object = (r.flags & 7) == 4 ? w.st.depth : w.st.color_rt[0];  // candidate for flags != 4 (WP6 uses the RB regs)
  std::memcpy(r.rb_copy, w.st.rs2300 + (0x2318 - 0x2300), sizeof r.rb_copy);
  if (h.emitter == EmResolvePublic && v.ext->kind_bytes >= sizeof(ResolveExt)) {
    ResolveExt x;
    std::memcpy(&x, v.kind, sizeof x);
    if (x.present & 1) std::memcpy(r.src_rect, x.rect, sizeof r.src_rect);
    if (x.present & 2) {
      std::memcpy(r.dst_point, x.point, sizeof r.dst_point);
      r.has_dst_point = true;
    }
    if (x.present & 4) std::memcpy(r.clear_rgba, x.clear_rgba, sizeof r.clear_rgba);
  }
  float z;
  std::memcpy(&z, &h.args[7], 4);
  r.clear_z = z;
  r.clear_stencil = h.args[8];
  resolves::OnResolve(h, w.st, r);
}

void Dispatch(WorkerState& w, const EvHeader& h, const uint8_t* payload) {
  WorkerStats& S = WorkerStatsRef();
  const EvView v = ParseEvent(h, payload);
  if (!v.ok) {
    S.apply_fail.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const int mode = ShadowMode();
  const uint32_t nm = v.ext->n_mirror_blocks;
  if (v.ext->flags & kExtResync) {
    if (targets::TileEpochOn()) targets::CloseTilingTail();  // card S3-66 review F5
    w.img.Reset();
    w.st = DeviceState();
  }
  // C1-T3: worker frame = h.frame of the event being processed; latch once (the scope may end on a
  // later frame). ns_* accumulate only on timing frames, scaled by that frame's N at accumulation
  // time (the worker stats report is not in this file; per-frame scaling is exact even in A/B mode).
  const bool tm = stat::On(h.frame);
  S.phase.store(1, std::memory_order_relaxed);
  const uint64_t w0 = tm ? NowNs() : 0;
  if (!w.img.Apply(payload, h.n_blocks, nm, &w.st)) S.apply_fail.fetch_add(1, std::memory_order_relaxed);
  w.st.device = v.ext->device;
  std::memcpy(w.st.dirty, v.ext->dirty, sizeof w.st.dirty);
  S.phase.store(2, std::memory_order_relaxed);
  const uint64_t w1 = tm ? NowNs() : 0;
  w.rv.Apply(h, payload);
  const uint64_t w2 = tm ? NowNs() : 0;
  if (tm) {
    const uint32_t sc = stat::Scale(h.frame);
    S.ns_apply.fetch_add((w1 - w0) * sc, std::memory_order_relaxed);
    S.ns_rv.fetch_add((w2 - w1) * sc, std::memory_order_relaxed);
  }

  S.phase.store(3, std::memory_order_relaxed);
  const uint64_t w3 = tm ? NowNs() : 0;
  if (v.ext->flags & kExtSnapshot) {
    S.snap_checked.fetch_add(1, std::memory_order_relaxed);
    cap::Image::Mismatch rep[8];
    uint32_t nrep = 0;
    const uint32_t bad = w.img.Compare(v.snap, v.ext->snap_blocks, &w.st, true, rep, 8, &nrep);
    if (bad) {
      S.snap_bad_records.fetch_add(1, std::memory_order_relaxed);
      S.snap_bad_dwords.fetch_add(bad, std::memory_order_relaxed);
      if (w.snap_logged < 24) {
        ++w.snap_logged;
        std::string line;
        char b[160];
        for (uint32_t i = 0; i < nrep; ++i) {
          std::snprintf(b, sizeof b, " [dw %u (+%u) %s: delta=0x%08X live=0x%08X]", rep[i].off, rep[i].off * 4,
                        cap::SectionName(rep[i].off), rep[i].ours, rep[i].live);
          line += b;
        }
        REXLOG_WARN("[d3d-shadow] FULLSNAP mismatch seq={} emitter={} kind={} dwords={}:{}", h.seq, EmitterName(h.emitter),
                    int(h.kind), bad, line);
      }
      w.rv.SyncFromState(w.st);
    }
  }

  S.phase.store(4, std::memory_order_relaxed);
  const uint64_t w4 = tm ? NowNs() : 0;
  if (tm) S.ns_snap.fetch_add((w4 - w3) * stat::Scale(h.frame), std::memory_order_relaxed);
  // Before the first resolve/upload too, so historical exclusions are complete.
  if (texq::Mode()) textures::EnableSamplingQualityMetadata();
  switch (h.kind) {
    case EvKind::kDraw:
    case EvKind::kDrawUP:
      DoDraw(w, h, v, mode == 2);
      if (tm) S.ns_translate.fetch_add((NowNs() - w4) * stat::Scale(h.frame), std::memory_order_relaxed);
      goto after_switch;
    case EvKind::kClear:
      S.phase.store(6, std::memory_order_relaxed);
      if (mode == 2) {
        ClearExt c;
        std::memcpy(&c, v.kind, sizeof c);
        targets::OnClearData(h, w.st, c.n_rects ? v.kind + sizeof(ClearExt) : nullptr);
      }
      break;
    case EvKind::kBeginTiling:
      if (mode == 2) {
        TilingExt t;
        std::memcpy(&t, v.kind, sizeof t);
        targets::OnBeginTilingData(h, w.st, t.n_tiles ? v.kind + sizeof(TilingExt) : nullptr,
                                   t.has_color ? t.color_be : nullptr);
      }
      break;
    case EvKind::kEndTiling:
      if (mode == 2) targets::OnEndTiling(h);
      break;
    case EvKind::kSetRT:
    case EvKind::kSetDepth:
      if (mode == 2) targets::OnSetRT(h, w.st);
      break;
    case EvKind::kResolve:
      if (mode == 2) DoResolve(w, h, v);
      break;
    case EvKind::kSwap: {
      if (mode == 2) {
        if (targets::TileEpochOn()) targets::CloseTilingTail();  // card S3-66 review F5
        OnSwap(h);
        targets::OnFrameEnd();  // card V1
        textures::OnWorkerFrameEnd();
        resolves::CensusFrameEnd(h.frame);
        stat::NoteBlock(h.frame);  // C1-T3: log each A/B block switch once
        LoadLockFrameEnd();  // card N4e (WWE2K14_LOCK_30=4)
        PairCensusFrameEnd();  // card S3-31 diagnostic (off unless WWE2K14_D3D_PAIR_CENSUS)
        MvCensusFrameEnd();  // U0b diagnostic (off unless WWE2K14_D3D_MV_CENSUS)
        if (mode == 2) SceneScaleFrameEnd();  // card S3-39
      }
      texq::FrameEnd();  // every worker swap, including an empty/translate-only frame
      const uint64_t cpu = ThreadCpuNs();
      if (w.last_cpu) S.cpu_ns.fetch_add(cpu - w.last_cpu, std::memory_order_relaxed);
      w.last_cpu = cpu;
      break;
    }
    default:
      break;  // kFullSnapshot (state sync) and unproduced kinds: state already applied
  }
  {
    // C1-T2 (Qwen draft, reviewed): the same delta also per event kind, so other_events can be ranked by kind.
    // C1-T3: sampled 1 frame in N (latched at Dispatch start); n_kind is gated too so ns_kind/n_kind stays a per-event average.
    const uint64_t other_ns = tm ? NowNs() - w4 : 0;
    if (tm) {
      const uint64_t sc = uint64_t(stat::Scale(h.frame));
      S.ns_other.fetch_add(other_ns * sc, std::memory_order_relaxed);
      const uint32_t ki = uint32_t(h.kind);
      if (ki < kEvKindCount) {
        S.ns_kind[ki].fetch_add(other_ns * sc, std::memory_order_relaxed);
        S.n_kind[ki].fetch_add(sc, std::memory_order_relaxed);  // C1-T3: counted on timing frames only, so scale like ns
      }
    }
  }
after_switch:

  if (h.kind == EvKind::kDraw && (v.ext->flags & kExtTexHash) && ShadowGuestBase()) {  // card M2c9: textures
    const size_t at = sizeof(DrawExt) + ((v.ext->flags & kExtVbHash) ? sizeof(VbHashExt) : 0);
    if (v.ext->kind_bytes >= at + sizeof(VbHashExt)) {
      VbHashExt th;
      std::memcpy(&th, v.kind + at, sizeof th);
      for (uint32_t i = 0; i < th.n && i < 16; ++i) {
        const auto& wd = th.w[i];
        if (!GuestRangeOk(0xA0000000u + wd.phys, wd.len)) continue;
        S.texh_checked.fetch_add(1, std::memory_order_relaxed);
        if (WindowHash(ShadowGuestBase() + 0xA0000000u + wd.phys, wd.len) != wd.hash) {
          const uint64_t n = S.texh_changed.fetch_add(1, std::memory_order_relaxed);
          if (n < 60)
            REXLOG_INFO("[d3d-shadow] TEXHASH changed since the draw call: frame={} seq={} pkt=0x{:08X} window {} "
                        "phys=0x{:08X}", h.frame, h.seq, h.packet_addr, i, wd.phys);
        }
      }
    }
  }
  if ((h.kind == EvKind::kDrawUP) && (v.ext->flags & kExtVbCheck) && ShadowGuestBase()) {  // dynamic-data check
    UpExt up;
    std::memcpy(&up, v.kind, sizeof up);
    uint64_t hsh = 1469598103934665603ull;
    if (!GuestRangeOk(up.guest_addr, up.copied_bytes)) return;
    const uint8_t* g = ShadowGuestBase() + up.guest_addr;
    for (uint32_t i = 0; i < up.copied_bytes; ++i) hsh = (hsh ^ g[i]) * 1099511628211ull;
    S.vb_checked.fetch_add(1, std::memory_order_relaxed);
    if (hsh != up.hash) {
      if (S.vb_bad.fetch_add(1, std::memory_order_relaxed) < 10)
        REXLOG_WARN("[d3d-shadow] VBCHECK: guest vertex memory changed before the worker read it seq={} addr=0x{:08X} bytes={}",
                    h.seq, up.guest_addr, up.copied_bytes);
    }
  }
}

}  // namespace

WorkerStats& WorkerStatsRef() {
  static WorkerStats s;
  return s;
}

namespace {
bool FairLockOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_FAIR_LOCK");
    return e && *e == '1';
  }();
  return on;
}
}  // namespace
thread_local bool t_is_d3d_worker = false;
bool LockHandoffOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_LOCK_HANDOFF");
    return e && *e == '1';
  }();
  return on;
}
void BackendMutex::lock() {
  if (!FairLockOn()) {
    if (!LockHandoffOn() || t_is_d3d_worker) return m_.lock();
    other_waiters_.fetch_add(1, std::memory_order_relaxed);
    m_.lock();
    other_waiters_.fetch_sub(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t t = next_.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0; serving_.load(std::memory_order_acquire) != t; ++i) {
    if (i < 256) nr::CpuRelax();
    else if (i < 2048) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
}
void BackendMutex::unlock() {
  if (!FairLockOn()) return m_.unlock();
  serving_.fetch_add(1, std::memory_order_release);
}
bool BackendMutex::try_lock() {
  if (!FairLockOn()) return m_.try_lock();
  uint32_t s = serving_.load(std::memory_order_acquire), n = s;
  return next_.compare_exchange_strong(n, s + 1, std::memory_order_acquire);
}
BackendMutex& BackendLock() {
  static BackendMutex m;
  return m;
}

bool EventReader::Next(EvHeader* h, const uint8_t** payload) {
  if (!g_cur.pending) return false;
  g_cur.pending = false;
  *h = g_cur.h;
  *payload = g_cur.payload;
  return true;
}
void EventReader::Done(uint32_t seq) { GateAdvance(seq); }

void WorkerSetDrain(bool drain) { W().drain = drain; }

void WorkerIdle() {
  WorkerState& w = W();
  if (w.lock_held) {
    BackendLock().unlock();
    WorkerStatsRef().lock_held.store(0, std::memory_order_relaxed);
    w.lock_held = false;
    w.batch = 0;
  }
}

void WorkerConsume(const EvHeader& h, const uint8_t* payload, bool more_pending) {
  WorkerState& w = W();
  WorkerStats& S = WorkerStatsRef();
  if (!w.init) {
    Init(w);
    NoteThreadRole(2);
    t_is_d3d_worker = true;
    // S3-5: sample this thread too when the PC sampler is on (kind 2; tools/nr/pc-sampler-report.sh).
    if (const char* e = std::getenv("WWE2K14_NR_PC_SAMPLER"); e && std::strcmp(e, "1") == 0) {
      const char* g = std::getenv("WWE2K14_NR_PC_SAMPLER_GAME");  // card S3-32: the game thread takes the extra slot
      if (!(g && *g == '1')) nr::sampler::AddCurrentThread(2);
    }
    // Card S3-15: lazy colour snapshots also in shadow-compare mode (validation: both renderers then use them).
    if (const char* e = std::getenv("WWE2K14_D3D_LAZY_SNAP"); e && *e == '1') nr::backend::SetLazySnapshots(true);
  }
  const bool mode2 = ShadowMode() == 2;
  if (!w.drain) {
    // S3-2: the nr backend is created by the nr frontend (first draw, or at once with WWE2K14_NR_DRAW_OFF). Mode 2
    // must not call it before that: wait here, outside BackendLock (bounded: 10 s, then counted and carry on).
    if (mode2 && !w.backend_seen_ready) {
      static const bool native_boot = [] {
        const char* e = std::getenv("WWE2K14_D3D_NATIVE_BOOT");
        return e && *e == '1';
      }();
      if (native_boot) runtime::BootBackendFromWorker();  // F4 row 19; the wait below then passes at once or times out as before
      for (int i = 0; i < 10000 && !nr::backend::Ready(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      w.backend_seen_ready = true;
      if (!nr::backend::Ready()) REXLOG_WARN("[d3d-shadow] worker: nr backend still not ready after 10 s");
    }
    if (mode2 && !w.lock_held) {
      const uint64_t t = NowNs();
      S.in_lock_wait.store(1, std::memory_order_relaxed);
      BackendLock().lock();
      S.in_lock_wait.store(0, std::memory_order_relaxed);
      S.lock_held.store(1, std::memory_order_relaxed);
      w.lock_held = true;
      S.lock_wait_ns.fetch_add(NowNs() - t, std::memory_order_relaxed);
      S.batches.fetch_add(1, std::memory_order_relaxed);
    }
    // C1-T3: latch at batch start with the first event's frame (a batch may span frames).
    const bool tm = stat::On(h.frame);
    const uint64_t t0 = tm ? NowNs() : 0;
    g_cur.h = h;
    g_cur.payload = payload;
    g_cur.pending = true;
    EventReader r;
    EvHeader eh;
    const uint8_t* ep;
    while (r.Next(&eh, &ep)) {
      S.cur_seq.store(eh.seq, std::memory_order_relaxed);
      S.cur_kind.store(uint32_t(eh.kind), std::memory_order_relaxed);
      S.cur_emitter.store(eh.emitter, std::memory_order_relaxed);
      S.cur_packet.store(eh.packet_addr, std::memory_order_relaxed);
      S.in_dispatch.store(1, std::memory_order_relaxed);
      rr::SetCurrent(eh.seq, eh.frame);  // RR1: memory records carry this event's seq
      Dispatch(w, eh, ep);
      S.in_dispatch.store(0, std::memory_order_relaxed);
      r.Done(eh.seq);
    }
    if (tm) S.busy_ns.fetch_add((NowNs() - t0) * stat::Scale(h.frame), std::memory_order_relaxed);
  } else {
    GateAdvance(h.seq);
  }
  S.events.fetch_add(1, std::memory_order_relaxed);
  if (mode2 && w.lock_held && (!more_pending || ++w.batch >= 32)) WorkerIdle();
  // WWE2K14_D3D_LOCK_HANDOFF=1: another thread waits for the backend lock (the CP's present at the game's swap, or a
  // guest-memory-write callback on the game thread): release it now instead of after up to 32 more events, and give
  // the waiter a moment to take it (std::mutex is not fair; bounded so a stuck waiter cannot stall the worker).
  if (mode2 && w.lock_held && LockHandoffOn() && BackendLock().other_waiters_.load(std::memory_order_relaxed)) {
    WorkerIdle();
    for (int i = 0; i < 2000 && BackendLock().other_waiters_.load(std::memory_order_relaxed); ++i) {
      if (i < 64) nr::CpuRelax();
      else std::this_thread::yield();
    }
    S.handoffs.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace wwe2k14::d3d
