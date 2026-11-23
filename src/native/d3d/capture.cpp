// WP9 (decision 0006 stage 2): game-thread capture of the D3D device state at hooked emitters. See capture.h for the
// record format and capture_core.h for the delta/overlay core. Written for this project (method: plan WP9).
#include "capture.h"
#include "../nr/nr_pc_sampler.h"

#include <atomic>
#include <exception>
#include <typeinfo>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <thread>

#include <rex/logging.h>
#ifdef _WIN32
#include <windows.h>
#include <csignal>
#include <stdlib.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <rex/exception_handler.h>
#endif
#include <rex/ppc/context.h>
#include <rex/graphics/draw_observer.h>

#include "../nr/nr_record_queue.h"
#include "capture_core.h"
#include "device_state.h"
#include "draw.h"
#include "gate.h"
#include "hle_marker.h"
#include "present.h"
#include "../nr/nr_backend.h"
#include "hooks.h"
#include "sidefile.h"
#include "tag_report.h"  // F2 step 1
#include "shader_identity.h"
#include "worker.h"
#include "stat_timing.h"  // C1-T3: statistics stopwatches sampled 1 frame in N

namespace wwe2k14::d3d {
namespace {

constexpr uint32_t kSlotData = 96 * 1024;  // W1: 96 KB payload per slot (was 192 KB); the game's per-swap burst is ~1100 records
constexpr uint32_t kDefaultSlots = 1024;
constexpr uint32_t kMaxSlots = 8192;  // cap the opt-in reservation at 768 MiB of payload storage
constexpr uint32_t kUpMax = 64 * 1024;
constexpr uint32_t kPhysMask = 0x1FFFFFFF;
constexpr uint32_t kMaxPackets = 8;

struct Slot {
  Slot() {}  // user-provided and empty: slot storage is never zero-filled (pages are touched only when used)
  EvHeader h;
  alignas(16) uint8_t data[kSlotData];
};

using QueueExec = std::function<void(Slot&)>;
struct Queue {
  virtual ~Queue() = default;
  virtual Slot* Acquire() = 0;
  virtual void Submit() = 0;
  virtual void Stop() = 0;
  virtual bool ProducerBlocked() const = 0;
  virtual uint64_t FullWaitNs() = 0;
};

template <uint32_t N>
struct QueueWithCapacity final : Queue {
  explicit QueueWithCapacity(const QueueExec& exec) : queue_(exec) {}
  Slot* Acquire() override { return queue_.Acquire(); }
  void Submit() override { queue_.Submit(); }
  void Stop() override { queue_.Stop(); }
  bool ProducerBlocked() const override { return queue_.ProducerBlocked(); }
  uint64_t FullWaitNs() override { return queue_.stats().full_wait_ns; }

 private:
  nr::RecordQueue<Slot, N> queue_;
};

template <uint32_t N>
Queue* MakeQueue(uint32_t slots, const QueueExec& exec) {
  if (slots == N) return new QueueWithCapacity<N>(exec);
  if constexpr (N < kMaxSlots) return MakeQueue<N * 2>(slots, exec);
  return nullptr;
}

enum class Role : uint8_t { kNone, kDraw, kBegin, kEnd, kResolve, kPreOnly, kPostOnly, kSync, kSwap };
struct HookInfo {
  Role role = Role::kNone;
  EvKind kind = EvKind::kDraw;
  uint8_t id = 0;
};

HookInfo Lookup(uint32_t a) {
  switch (a) {
    case 0x82CB52D0: return {Role::kDraw, EvKind::kDraw, EmDrawIndexedVertices};
    case 0x82CB5ED0: return {Role::kDraw, EvKind::kDraw, EmEmit5ED0};
    case 0x82CB49A8: return {Role::kDraw, EvKind::kDraw, EmEmit49A8};
    case 0x82CB0810: return {Role::kDraw, EvKind::kDraw, EmEmit0810};
    case 0x82CB0968: return {Role::kDraw, EvKind::kDraw, EmEmit0968};
    case 0x82CB7930: return {Role::kDraw, EvKind::kDraw, EmEmit7930};
    case 0x82CC50E0: return {Role::kDraw, EvKind::kDraw, EmEmit50E0};
    case 0x82CCC2E0: return {Role::kDraw, EvKind::kDraw, EmEmitC2E0};
    case 0x82CB4E10: return {Role::kBegin, EvKind::kDrawUP, EmBeginVertices};
    case 0x82CB4E00: return {Role::kEnd, EvKind::kDrawUP, EmEndVertices};
    case 0x82CB2320: return {Role::kResolve, EvKind::kResolve, EmResolveWorker};
    case 0x82CB3478: return {Role::kResolve, EvKind::kResolve, EmResolvePublic};
    case 0x82CB8BA8: return {Role::kPreOnly, EvKind::kClear, EmClear};
    case 0x82CBB1D0: return {Role::kPreOnly, EvKind::kBeginTiling, EmBeginTiling};
    case 0x82CBB540: return {Role::kPreOnly, EvKind::kEndTiling, EmEndTiling};
    case 0x82CAF5E8: return {Role::kPostOnly, EvKind::kSetRT, EmSetRenderTarget};
    case 0x82CAF978: return {Role::kPostOnly, EvKind::kSetDepth, EmSetDepthStencil};
    case 0x82CAFC50: return {Role::kPostOnly, EvKind::kSetRT, EmApplyRTDepth};
    case 0x82CB20C0: return {Role::kSync, EvKind::kFullSnapshot, EmFlushDispatcher};
    case 0x82CBD610: return {Role::kSync, EvKind::kFullSnapshot, EmRunCommandBuffer};
    case 0x82CBD1F0: return {Role::kSync, EvKind::kFullSnapshot, EmEndCommandBuffer};
    case 0x82CB70B0: return {Role::kSwap, EvKind::kSwap, EmSwap};
    default: return {};
  }
}

inline uint32_t Be32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
inline uint64_t Be64(const uint8_t* base, uint32_t a) { return (uint64_t(Be32(base, a)) << 32) | Be32(base, a + 4); }
inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
bool EnvInt(const char* n, int* out) {
  const char* v = std::getenv(n);
  if (!v || !*v) return false;
  *out = std::atoi(v);
  return true;
}

const char* RecordQueueStateName(uint32_t state) {
  switch (state) {
    case 1: return "executing";
    case 2: return "wait_work";
    case 3: return "notify_waiter";
    default: return "idle_spin";
  }
}
const char* RecordPhaseName(uint32_t phase) {
  switch (phase) {
    case 1: return "slot";
    case 2: return "release";
    case 3: return "async_job";
    case 4: return "draw";
    default: return "idle";
  }
}
const char* RecordWaitName(uint32_t wait) {
  switch (wait) {
    case 1: return "plume_queue_submit";
    case 2: return "command_fence";
    case 3: return "record_thread_mutex";
    default: return "none";
  }
}

uint32_t CaptureSlotsFromEnv() {
  const char* raw = std::getenv("WWE2K14_D3D_CAPTURE_SLOTS");
  if (!raw || !*raw) return kDefaultSlots;
  char* end = nullptr;
  const unsigned long slots = std::strtoul(raw, &end, 10);
  if (end == raw || *end != '\0' || slots == 0 || slots > kMaxSlots || (slots & (slots - 1)) != 0) {
    REXLOG_WARN("[d3d-shadow] invalid WWE2K14_D3D_CAPTURE_SLOTS='{}' (expected a power of two from 1 through {}); using {}",
                raw, kMaxSlots, kDefaultSlots);
    return kDefaultSlots;
  }
  return uint32_t(slots);
}

struct Counters {
  std::atomic<uint64_t> records{0}, bytes{0}, by_kind[16] = {}, cap_ns{0}, mirror_dw{0}, overlay_dw{0}, qdepth_max{0};
  std::atomic<uint64_t> nested_dropped{0}, foreign_thread{0}, bad_device{0}, overflow{0}, up_unfinished{0}, up_trunc{0};
  std::atomic<uint64_t> wrapped{0}, no_packet{0}, walk_fallback{0}, load_blocks{0}, load_unmapped{0}, inline_regs{0};
  std::atomic<uint64_t> rb_sampled{0}, rb_unsampled{0};  // card M2b2-C: resolves whose RB_COPY block was read at the register-emit helper / not
  std::atomic<uint64_t> early_closed{0}, dirty_miss_records{0}, dirty_miss_dw{0}, resync{0}, packets_registered{0}, skipped_sync{0};
  // Card S3-58 (G2 step 0): shader literal loads seen at the game's loader call (ShaderConstLoad) vs in the ring window.
  std::atomic<uint64_t> lit_hook{0}, lit_ring{0}, lit_lost_records{0}, lit_ring_extra{0}, lit_outside{0}, lit_resolve{0};
  std::atomic<uint64_t> lit_segsw_records{0}, lit_reject{0};
  // Card S3-63: segment-aware ring walk (RingMakeSpace switch pairs inside records, residual misparses).
  std::atomic<uint64_t> seg_pairs{0}, seg_records{0}, seg_bad{0}, seg_discard{0}, seg_overflow{0}, walk_misparse{0};
  std::atomic<uint64_t> walk_long{0}, walk_trunc{0};
  // P5a CHECK: counts ring-derived outputs that reach the live map/state consumers (diagnostic counters are excluded).
  std::atomic<uint64_t> walk_check_windows{0}, walk_check_map_packets{0}, walk_check_overlay_dw{0};
  // Card DE-2M: indexed draws whose DRAW became a marker / that kept the original packet, by reason.
  std::atomic<uint64_t> mk_written{0}, mk_recording{0}, mk_predicated{0}, mk_split{0}, mk_record{0}, mk_header{0};
  std::atomic<uint64_t> mk_overwritten{0}, mk_up{0}, mk_emit{0};
  std::atomic<uint64_t> ring_load_skipped{0};  // card S3-70 7a: ring LOAD_ALU t1-4 blocks seen but not applied (RING_LOADS=0)
};

struct Frame {
  bool ignored = false;
  uint32_t args[10] = {};
  uint32_t dev = 0;
};

struct Pending {
  bool active = false;
  Role role = Role::kNone;
  EvKind kind = EvKind::kDraw;
  uint8_t id = 0;
  uint32_t seq = 0, dev = 0, cursor_before = 0, cursor_after = 0, frame = 0;
  uint32_t args[10] = {};
  uint16_t flags = 0;
  uint64_t dirty[5] = {};
  cap::Builder mirror, overlay, snap;
  bool has_snap = false;
  bool rb_sampled = false;  // RB_COPY_* already taken (at the register-emit helper) for this resolve record
  uint32_t kind_bytes = 0;
  alignas(8) uint8_t kind_data[sizeof(UpExt) + kUpMax + 64];
  uint32_t pkt[kMaxPackets] = {};
  uint32_t npk = 0;
  uint32_t key_valid = 0;
  uint64_t vs_fnv = 0, ps_fnv = 0;
  uint32_t vs_obj = 0, vs_code = 0, vs_bytes = 0, ps_obj = 0, ps_code = 0, ps_bytes = 0;
  uint32_t up_count_x_stride = 0;
  uint32_t frame_idx = 0;  // index of the hook frame that opened this record
  uint16_t lit_hook_n = 0, lit_ring_n = 0;  // card S3-58: literal loads of this record at the loader call / in the ring walk
  uint8_t segsw = 0, walk_fb = 0;           // RingMakeSpace ran inside the record / ParseWindow took the per-dword fallback
  // Card S3-63: per outermost RingMakeSpace inside the record, the last dword written before it ([dev+48] at entry)
  // and the start of the segment it left the cursor in ([dev+15332] at exit); sw_bad = pairs not usable (discard
  // mode, other device, more than kMaxSw switches).
  static constexpr uint32_t kMaxSw = 4;
  uint8_t nsw = 0, sw_bad = 0;
  uint32_t sw_end[kMaxSw] = {}, sw_start[kMaxSw] = {};
  bool marker = false;       // card DE-2M: the record's DRAW was replaced by an ordering marker
  uint32_t marker_addr = 0;  // physical address of the marker header
};

struct Cap {
  bool inited = false, enabled = false;
  int mode = 0;
  const uint8_t* base = nullptr;
  std::atomic<uint32_t> producer_tag{0}, rebinds{0}, tag_counter{0};
  std::mutex report_mu;
  std::atomic<uint64_t> last_report_ns{0};
  uint32_t device = 0;
  bool need_full = true;
  bool delta_mode = true;      // WWE2K14_D3D_SHADOW_DELTA=0 -> full state every record
  bool inline_mode = false;    // WWE2K14_D3D_SHADOW_INLINE=1
  bool load_temp = false;      // WWE2K14_D3D_SHADOW_LOADMODE=temp
  bool no_worker = false;      // WWE2K14_D3D_SHADOW_NOWORKER=1: events are consumed and discarded
  bool lit_from_hook = true;   // card S3-58: literals from the loader call (default); WWE2K14_D3D_LIT_SOURCE=ring = ring 0x2F parse
  bool seg_walk = true;        // card S3-63: walk the record's ring window per segment, in full; WWE2K14_D3D_SEG_WALK=0 = old walk
  bool ring_loads = true;      // card S3-70 7a: apply ring LOAD_ALU t1-4 (WWE2K14_D3D_RING_LOADS=0: count only)
  int ring_walk_mode = 1;      // WWE2K14_D3D_RING_WALK: unset/1 = current, 0 = no walk/map, check = output census
  bool rms_pre_valid = false;  // card S3-63: outermost RingMakeSpace entry seen inside the open record (producer thread)
  uint32_t rms_pre_cursor = 0, rms_pre_start = 0, rms_dev = 0, seg_logged = 0, nb_logged = 0, segwin_logged = 0;
  uint32_t fullsnap_n = 0, vbcheck_n = 0, report_every = 60, texhash_n = 0;
  uint64_t texhash_ctr = 0;
  uint32_t seq = 0, frame = 0, queue_slots = kDefaultSlots;
  uint32_t frame_atomic() const { return frame; }
  uint64_t t_scan = 0, t_snap = 0, t_parse = 0, t_keys = 0, t_publish = 0, t_ext = 0, n_snap = 0;  // game thread, ns
  uint64_t nested_by[64][64] = {};  // [dropped hook id][outer open record id]
  uint64_t last_t[8] = {}, last_w[8] = {}, last_kind_ns[kEvKindCount] = {}, last_kind_n[kEvKindCount] = {};
  uint64_t draw_records = 0, up_records = 0;
  cap::Shadow shadow;
  std::vector<std::pair<uint32_t, uint32_t>> forced;  // ranges to rescan at the next record (LOADMODE=temp)
  // Card DE-2M: markers written since the last Swap (verified there).
  static constexpr uint32_t kMkVerify = 8192;
  struct MkRef { uint32_t addr, seq; };
  MkRef mk_frame[kMkVerify] = {};
  uint32_t mk_nframe = 0;
  // Card DE-3T: tag-guided scan (WWE2K14_D3D_TAG_SCAN=1 | check). Producer thread only, except tg_foreign.
  int tag_mode = 0;                // 0 off, 1 tag-guided, 2 CHECK (full scan + count what mode 1 would miss)
  bool tg_clean = false;           // no untrusted hook started since the last scan
  int32_t tg_open = 0;             // untrusted hooks still open on the producer thread
  bool tg_ovl_overflow = false;
  std::atomic<bool> tg_foreign{false};  // an untrusted hook ran on another thread
  std::pair<uint16_t, uint16_t> tg_ovl[32];  // overlaid always-section runs since the last scan (rescanned)
  uint32_t tg_novl = 0, tg_nhooks = 0, tg_distinct = 0;
  uint32_t tg_hooks[8] = {};       // CHECK: hook addresses since the last scan (for the miss log)
  // N2 diagnostic (fully native, 11-17): why a tag-mode scan was full. Reasons: 0 need_full, 1 untrusted hook (by address,
  // last one seen), 2 open untrusted hook, 3 foreign thread, 4 overlay overflow. Counted per scan, logged with tag-scan.
  uint64_t tg_why[5] = {}, tg_why_last[5] = {};
  uint32_t tg_last_untrusted = 0;
  bool tg_rt_trust = false;  // N2-RT step 3: SetRenderTarget trust decided at Pre, reused at Post
  uint32_t tg_grp_now = 0;   // N2-GT: group-trust bits decided at Pre, reused at Post (bit = GroupBit(addr))
  std::pair<uint32_t, uint64_t> tg_why_hook[16] = {};
  uint64_t tg_trusted = 0, tg_full = 0, tg_dw = 0, tg_miss_scans = 0, tg_miss_dw = 0, tg_checked = 0;
  uint64_t last_tg[4] = {};
  uint32_t tg_miss_by_dw[cap::kImageDwords] = {};
  uint8_t tg_mark[cap::kImageDwords] = {};
  Frame frames[32];
  uint32_t depth = 0;
  std::unique_ptr<Pending> pend;
  Queue* q = nullptr;
  std::atomic<uint64_t> published{0}, consumed{0};
  std::atomic<uint32_t> last_pub_seq{0};  // card S3-17: seq of the newest published event (dropped events leave gaps)
  std::atomic<uint64_t> last_swap_ns{0};
  std::atomic<uint32_t> wd_hook{0}, wd_depth{0}, wd_in_hook{0}, wd_seq{0};
  std::atomic<bool> wd_stop{false};
  std::thread wd;
  Counters c;
  // report bookkeeping
  uint64_t last_records = 0, last_bytes = 0, last_cap_ns = 0, last_full_wait_ns = 0, last_busy = 0, last_cpu = 0,
           last_lock = 0, last_kind[16] = {}, last_gate_wait = 0, last_swap_wait = 0;
  uint64_t qdepth_win_max = 0;
  uint32_t last_report_frame = 0;
};

Cap& C();
uint32_t ThreadTag() {
  thread_local uint32_t t = C().tag_counter.fetch_add(1) + 1;
  return t;
}

Cap& C() {
  static Cap* c = new Cap;
  return *c;
}

void Report(Cap& c, const char* why);
void InstallCrashHandler();

void Shutdown() {
  Cap& c = C();
  if (!c.q) return;
  c.wd_stop.store(true);
  if (c.wd.joinable()) c.wd.join();
  Report(c, "final");
  WorkerSetDrain(true);
  c.q->Stop();
}

void InstallCrashHandler();

void Init(Cap& c) {
  c.inited = true;
  int v = 0;
  const char* m = std::getenv("WWE2K14_D3D_SHADOW");
  c.mode = (m && *m) ? std::atoi(m) : 0;
  if (c.mode < 0 || c.mode > 2) c.mode = 0;
  c.enabled = c.mode != 0 && ObserveEnabled();
  if (ObserveEnabled()) InstallCrashHandler();  // D3: also for OBSERVE-only / OBSERVE+CHECK runs (shadow off)
  if (!c.enabled) return;
  if (EnvInt("WWE2K14_D3D_SHADOW_FULLSNAP", &v) && v > 0) c.fullsnap_n = uint32_t(v);
  if (EnvInt("WWE2K14_D3D_SHADOW_VBCHECK", &v) && v > 0) c.vbcheck_n = uint32_t(v);
  if (EnvInt("WWE2K14_D3D_SHADOW_TEXHASH", &v) && v > 0) c.texhash_n = uint32_t(v);
  if (EnvInt("WWE2K14_D3D_SHADOW_EVERY", &v) && v > 0) c.report_every = uint32_t(v);
  if (EnvInt("WWE2K14_D3D_SHADOW_DELTA", &v)) c.delta_mode = v != 0;
  if (EnvInt("WWE2K14_D3D_SHADOW_INLINE", &v)) c.inline_mode = v != 0;
  if (EnvInt("WWE2K14_D3D_SHADOW_NOWORKER", &v)) c.no_worker = v != 0;
  if (const char* lm = std::getenv("WWE2K14_D3D_SHADOW_LOADMODE")) c.load_temp = std::strcmp(lm, "temp") == 0;
  if (const char* ls = std::getenv("WWE2K14_D3D_LIT_SOURCE")) c.lit_from_hook = std::strcmp(ls, "ring") != 0;
  if (const char* sw = std::getenv("WWE2K14_D3D_SEG_WALK")) c.seg_walk = std::strcmp(sw, "0") != 0;
  if (const char* rl = std::getenv("WWE2K14_D3D_RING_LOADS")) c.ring_loads = std::strcmp(rl, "0") != 0;
  c.ring_walk_mode = ShadowRingWalkMode();
  if (c.ring_walk_mode == 0) {  // card DE-5A review: these selectors read ring-walk outputs that mode 0 no longer produces
    const bool late_tex = std::getenv("WWE2K14_D3D_LATE_TEX") && std::strcmp(std::getenv("WWE2K14_D3D_LATE_TEX"), "0") != 0;
    if (c.ring_loads || !c.lit_from_hook || c.inline_mode || c.load_temp || late_tex)
      REXLOG_WARN("[d3d-shadow] WWE2K14_D3D_RING_WALK=0: RING_LOADS / LIT_SOURCE=ring / SHADOW_INLINE / SHADOW_LOADMODE=temp / "
                  "LATE_TEX get no ring-walk input (set WWE2K14_D3D_RING_WALK=1 for them)");
  }
  if (const char* ts = std::getenv("WWE2K14_D3D_TAG_SCAN"))
    c.tag_mode = std::strcmp(ts, "1") == 0 ? 1 : std::strcmp(ts, "check") == 0 ? 2 : 0;
  if (c.tag_mode) REXLOG_INFO("[d3d-shadow] tag-guided scan {} (card DE-3T)", c.tag_mode == 1 ? "ON" : "CHECK");
  c.pend = std::make_unique<Pending>();
  GateConfigure(true);
  c.queue_slots = CaptureSlotsFromEnv();
  Cap* cp = &c;
  const QueueExec exec = [cp](Slot& s) {
    const uint64_t done = cp->consumed.load(std::memory_order_relaxed);
    const bool more = cp->published.load(std::memory_order_acquire) > done + 1;
    if (cp->no_worker) GateAdvance(s.h.seq);
    else WorkerConsume(s.h, s.data, more);
    cp->consumed.store(done + 1, std::memory_order_release);
    if (!more) WorkerIdle();
  };
  c.q = MakeQueue<1>(c.queue_slots, exec);
  std::atexit(Shutdown);
  c.last_swap_ns.store(NowNs());
  c.wd = std::thread([cp] {  // watchdog: no Swap for 10 s -> log the shadow state every 10 s
    uint64_t last_log = 0;
    while (!cp->wd_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      const uint64_t now = NowNs();
      if (now - cp->last_report_ns.load() > 10'000'000'000ull && cp->published.load() > 0)
        Report(*cp, "timer");  // summary does not depend on a captured Swap
      if (now - cp->last_swap_ns.load() < 10'000'000'000ull || now - last_log < 10'000'000'000ull) continue;
      last_log = now;
      WorkerStats& W = WorkerStatsRef();
      const GateStats g = GateSnapshot();
      nr::backend::RecordBatchWatchdogState rq{};
      const bool has_record_state = nr::backend::GetRecordBatchWatchdogState(&rq);
      REXLOG_WARN(
          "[d3d-shadow] WATCHDOG no Swap for {:.0f} s | queue published={} consumed={} (head-done={}) seq={} done_seq={} | worker "
          "in_dispatch={} cur_seq={} cur_kind={} cur_emitter={} lock_held={} waiting_for_lock={} events={} | game thread last_hook=0x{:08X} "
          "in_hook={} depth={} frame={} | gate reg={} waits={} timeouts={} unpaired={} swap_waits={} | backend_lock_held_by_worker={} "
          "| cp_where={} backend_wait={} | nr_record active={} head={} tail={} committed={} consumer_state={} slot={} "
          "phase={} op={} item={} ordinal={} job_hash=0x{:016X} consumer_wait={} wait_on={} backend_wait_site={} "
          "wait_serial={} spurious_recoveries={} producer_full={} producer_sync={}",
          double(now - cp->last_swap_ns.load()) / 1e9, cp->published.load(), cp->consumed.load(),
          cp->published.load() - cp->consumed.load(), cp->wd_seq.load(), GateDoneSeq(), W.in_dispatch.load(), W.cur_seq.load(),
          W.cur_kind.load(), W.cur_emitter.load(), W.lock_held.load(), W.in_lock_wait.load(), W.events.load(),
          cp->wd_hook.load(), cp->wd_in_hook.load(), cp->wd_depth.load(), cp->frame_atomic(), g.registered, g.waited, g.timeouts,
          g.unpaired, g.swap_waited, W.lock_held.load(), CpWhere(), nr::backend::BackendWaitCode(), has_record_state,
          rq.head, rq.tail, rq.committed, RecordQueueStateName(rq.queue_state), rq.executing_slot,
          RecordPhaseName(rq.phase), rq.operation ? rq.operation : "none", rq.current_item, rq.current_ordinal,
          rq.current_job_hash, RecordWaitName(rq.consumer_wait), rq.wait_name ? rq.wait_name : "none",
          rq.backend_wait_site, rq.wait_serial, rq.spurious_recoveries, rq.producer_blocked, rq.producer_sync_waiting);
    }
  });
  REXLOG_INFO(
      "[d3d-shadow] capture ON mode={} ({}) fullsnap={} vbcheck={} delta={} inline={} loadmode={} gate={} noworker={} "
      "slots={}x{}KB literals={} seg_walk={} ring_walk={}",
      c.mode, c.mode == 1 ? "translate-only" : "shadow-draw", c.fullsnap_n, c.vbcheck_n, c.delta_mode, c.inline_mode,
      c.load_temp ? "temp" : "persist", GateEnabled(), c.no_worker, c.queue_slots, kSlotData / 1024,
      c.lit_from_hook ? "loader-call" : "ring", c.seg_walk ? 1 : 0,
      c.ring_walk_mode == 0 ? "off" : c.ring_walk_mode == 2 ? "check" : "on");
}

// ---------------------------------------------------------------------------------------------------------------
// state scan

// Card DE-3T: hooks whose guest bodies leave every device change they make to an always-section dword either tagged in
// dev+16/24/32 or inside cap::kTagAlways: the draw entries (their dispatch writes = kTagAlways), the D3D setters, the
// command interpreter, ring space and the register-emit helper. Any other hook makes the next scan a full one.
// N2-RT step 3 (docs/tasks/N2-RT-native.md): every device dword SetRenderTarget 82CAF5E8 changes inside an always-section
// is either in kTagAlways (rt_ds_vb, rs2001, rs2010) or tagged by the tree itself (dev+16 bits 57..52 / 37 / 26..21, dev+32
// bit 36); its other writes lie outside the always-sections. WWE2K14_D3D_TRUST_RT: 1 trusted, 0 (default) untrusted;
// n2rt-trust-check: 21.3 M scans 0 miss; n2rt-trust-speed alone: all threads +0.28 ms/f (10/25 pairs, no clear difference)
// because the other untrusted hooks in the same gaps still force full scans -> off until the group is trusted together.
// WWE2K14_D3D_TRUST_RT_AB=N: in-run A/B (trusted in odd blocks of N frames). Verified by WWE2K14_D3D_TAG_SCAN=check.
bool RtTrustFor(uint32_t frame) {
  static const int mode = [] {
    const char* e = std::getenv("WWE2K14_D3D_TRUST_RT");
    const int m = e && *e == '1' ? 1 : 0;
    if (m) REXLOG_INFO("[d3d-shadow] SetRenderTarget tag trust mode={}", m);
    return m;
  }();
  static const uint32_t ab = [] {
    const char* e = std::getenv("WWE2K14_D3D_TRUST_RT_AB");
    const long n = e && *e ? std::strtol(e, nullptr, 10) : 0;
    if (n > 0) REXLOG_INFO("[d3d-shadow] SetRenderTarget trust in-run A/B blocks of {} frames", n);
    return n > 0 && n <= 1000000 ? uint32_t(n) : 0u;
  }();
  if (!mode) return false;
  return !ab || ((frame / ab) & 1u) != 0;
}
// N2-GT (night 11-18, docs/tasks/NIGHT-1118-PLAN.md T1): trust the remaining untrusted hooks of the full-scan gaps as a
// group. WWE2K14_D3D_TRUST_GROUP=<bit mask, hex ok> (default 0 = none): bit 0 SetRenderTarget 82CAF5E8, 1 SetDepthStencil
// 82CAF978 (+ overlay of dev+10368), 2 Clear 82CB8BA8, 3 ResolvePublic 82CB3478, 4 ResolveWorker 82CB2320, 5 Emit7930
// 82CB7930, 6 SetShaderGPRAllocation 82CB4848. _AB=N: trusted only in odd blocks of N frames. Verify with TAG_SCAN=check
// (the miss log names the hooks since the last scan).
int GroupBit(uint32_t addr) {
  switch (addr) {
    case 0x82CAF5E8: return 0;
    case 0x82CAF978: return 1;
    case 0x82CB8BA8: return 2;
    case 0x82CB3478: return 3;
    case 0x82CB2320: return 4;
    case 0x82CB7930: return 5;
    case 0x82CB4848: return 6;
    default: return -1;
  }
}
uint32_t GroupTrustFor(uint32_t frame) {
  static const uint32_t mask = [] {
    const char* e = std::getenv("WWE2K14_D3D_TRUST_GROUP");
    const unsigned long m = e && *e ? std::strtoul(e, nullptr, 0) : 0;
    if (m) REXLOG_INFO("[d3d-shadow] hook group trust mask=0x{:02X}", unsigned(m & 0x7F));
    return uint32_t(m & 0x7F);
  }();
  static const uint32_t ab = [] {
    const char* e = std::getenv("WWE2K14_D3D_TRUST_GROUP_AB");
    const long n = e && *e ? std::strtol(e, nullptr, 10) : 0;
    if (n > 0) REXLOG_INFO("[d3d-shadow] hook group trust in-run A/B blocks of {} frames", n);
    return n > 0 && n <= 1000000 ? uint32_t(n) : 0u;
  }();
  if (!mask) return 0;
  return (!ab || ((frame / ab) & 1u) != 0) ? mask : 0u;
}
bool GroupTrustedNow(const Cap& c, uint32_t addr) {
  const int b = GroupBit(addr);
  return b >= 0 && ((c.tg_grp_now >> b) & 1u) != 0;
}
bool TagTrusted(uint32_t addr) {
  switch (addr) {
    case 0x82CB4E10: case 0x82CB4E00: case 0x82CB52D0:
    case 0x82CAC430: case 0x82CB3F48: case 0x82CB4020: case 0x82CB44A8: case 0x82CB42A0:
    case 0x82CB46C0: case 0x82CAECB8: case 0x82CAEE60: case 0x82CB4DB0:
    case 0x82BB8C00: case 0x82CBA798: case 0x82CB9670: case 0x82CC0D18:
      return true;
    default:
      return false;
  }
}
// The same list minus the draw entries: what may run on another thread without invalidating the producer's tags.
bool TagSetterOnly(uint32_t addr) {
  return TagTrusted(addr) && addr != 0x82CB4E10 && addr != 0x82CB4E00 && addr != 0x82CB52D0;
}

void TagNoteOverlay(Cap& c, uint32_t off, uint32_t n);
void TagHookPre(Cap& c, uint32_t addr) {
  if (ThreadTag() != c.producer_tag.load(std::memory_order_acquire)) {
    // Review de23 BLOCKER 2: a foreign SetTexture(NULL) writes fetch word 0 without a tag, and its slot re-read below is
    // producer-only. Any hook on another thread (Pre here, Post below) makes the next scan a full one.
    c.tg_foreign.store(true, std::memory_order_relaxed);
    return;
  }
  if (c.tag_mode == 2 && c.tg_nhooks < 8) c.tg_hooks[c.tg_nhooks] = addr;
  ++c.tg_nhooks;
  if (addr == 0x82CAF5E8u) c.tg_rt_trust = RtTrustFor(c.frame);
  const int gb = GroupBit(addr);
  if (gb >= 0) {
    const uint32_t m = 1u << gb;
    c.tg_grp_now = (GroupTrustFor(c.frame) & m) ? (c.tg_grp_now | m) : (c.tg_grp_now & ~m);
  }
  if (!(TagTrusted(addr) || (addr == 0x82CAF5E8u && c.tg_rt_trust) || GroupTrustedNow(c, addr))) {
    c.tg_clean = false;
    ++c.tg_open;
    c.tg_last_untrusted = addr;
  }
}
// SetTexture 82CAC430 (r4 = stage, r5 = texture): a NULL bind clears the type bits of fetch word 0 WITHOUT a tag (and a
// non-NULL bind ORs the caller's r6 into dev+24), CHECK run de3t-check1: 6985 of 7036 missed dwords. Re-read the slot.
// F2 step 1 (tag_report.h): writer-reported always dwords, marked by the hook wrappers from any thread.
alignas(64) std::atomic<uint64_t> g_rep_lo{0};
alignas(64) std::atomic<uint64_t> g_rep_hi{0};
}  // namespace
bool TagReportOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_TAG_REPORT");
    const bool v = e && std::strcmp(e, "1") == 0;
    if (v) REXLOG_INFO("[d3d-shadow] tag-scan writer-reported always dwords ON (F2 step 1)");
    return v;
  }();
  return on;
}
// WWE2K14_D3D_TAG_REPORT_AB=N: in-run A/B, the reported scan is used only in odd blocks of N frames (marks are taken and
// dropped in even blocks, which read all 76 always dwords as before; the marking cost is paid in both arms).
bool TagReportActive(uint32_t frame) {
  static const uint32_t ab = [] {
    const char* e = std::getenv("WWE2K14_D3D_TAG_REPORT_AB");
    const long n = e && *e ? std::strtol(e, nullptr, 10) : 0;
    if (n > 0) REXLOG_INFO("[d3d-shadow] writer-reported scan in-run A/B blocks of {} frames", n);
    return n > 0 && n <= 1000000 ? uint32_t(n) : 0u;
  }();
  return !ab || ((frame / ab) & 1u) != 0;
}
void TagReportMark(uint64_t lo, uint64_t hi) {
  if (lo) g_rep_lo.fetch_or(lo, std::memory_order_release);
  if (hi) g_rep_hi.fetch_or(hi, std::memory_order_release);
}
namespace {
void TagNoteSetTexture(Cap& c, const PPCContext& ctx) {
  if (ctx.r4.u32 >= kFetchSlots || ThreadTag() != c.producer_tag.load(std::memory_order_acquire)) return;
  TagNoteOverlay(c, cap::kAlways[0].off + 6 * ctx.r4.u32, 6);
}
void TagHookPost(Cap& c, uint32_t addr) {
  if (ThreadTag() != c.producer_tag.load(std::memory_order_acquire)) {
    c.tg_foreign.store(true, std::memory_order_relaxed);  // e.g. an old producer's hook ending after a rebind
    return;
  }
  if (TagTrusted(addr) || (addr == 0x82CAF5E8u && c.tg_rt_trust)) return;
  if (GroupTrustedNow(c, addr)) {
    // SetDepthStencil's tiled branch rewrites dev+10368 (rs 0x2000) with no dirty bit (docs/tasks/N2-RT-native.md):
    // the next scan must visit it.
    if (addr == 0x82CAF978u) TagNoteOverlay(c, 10368 / 4, 1);
    return;
  }
  if (c.tg_open > 0) --c.tg_open;
  c.tg_clean = false;  // its body ran after any scan it made
}
// Overlays end at the next scan of their dwords; a tag-guided scan must therefore visit overlaid always-section runs.
void TagNoteOverlay(Cap& c, uint32_t off, uint32_t n) {
  if (off >= cap::kAluVs.off && off + n <= cap::kAluPs.off + cap::kAluPs.n) return;  // ALU: dirty-mask driven
  if (c.tg_novl >= 32) {
    c.tg_ovl_overflow = true;
    return;
  }
  c.tg_ovl[c.tg_novl++] = {uint16_t(off), uint16_t(n)};
}

// CHECK (mode 2): count every always-section dword the full scan is about to send that the tag-guided scan would not
// have visited. Read-only; the full scan that follows is the production path.
void TagCheck(Cap& c, const Pending& p, const uint8_t* dev, const uint64_t t[3], bool rep, uint64_t rlo, uint64_t rhi) {
  std::memset(c.tg_mark, 0, sizeof c.tg_mark);
  auto mark = [&](uint32_t off, uint32_t n) {
    for (uint32_t i = off; i < off + n && i < cap::kImageDwords; ++i) c.tg_mark[i] = 1;
  };
  if (rep) {
    for (uint32_t i = 0; i < cap::kNumTagResidual; ++i) mark(cap::kTagResidual[i].off, cap::kTagResidual[i].n);
    cap::ForEachReported(rlo, rhi, mark);
  } else {
    for (uint32_t i = 0; i < cap::kNumTagAlways; ++i) mark(cap::kTagAlways[i].off, cap::kTagAlways[i].n);
  }
  cap::ForEachTagRun(t, mark);
  for (uint32_t i = 0; i < c.tg_novl; ++i) mark(c.tg_ovl[i].first, c.tg_ovl[i].second);
  uint32_t miss = 0, first = 0, visited = 0;
  for (uint32_t s = 0; s < cap::kNumAlways; ++s) {
    for (uint32_t i = cap::kAlways[s].off; i < cap::kAlways[s].off + cap::kAlways[s].n; ++i) {
      if (c.tg_mark[i]) {
        ++visited;
        continue;
      }
      if (!c.shadow.WouldSend(dev, i)) continue;
      if (!miss) first = i;
      ++miss;
      if (c.tg_miss_by_dw[i]++ == 0 && ++c.tg_distinct <= 64) {
        std::string hooks;
        for (uint32_t k = 0; k < c.tg_nhooks && k < 8; ++k) {
          char h[16];
          std::snprintf(h, sizeof h, " %08X", c.tg_hooks[k]);
          hooks += h;
        }
        REXLOG_WARN("[d3d-shadow] tag-scan CHECK new miss dword dev+{} ({}) seq={} frame={} emitter={} live=0x{:08X} | "
                    "hooks since last scan ({}):{}", 4 * i, cap::SectionName(i), p.seq, c.frame, EmitterName(p.id),
                    Be32(dev, 4 * i), c.tg_nhooks, hooks);
      }
    }
  }
  ++c.tg_checked;
  c.tg_dw += visited;
  if (!miss) return;
  c.tg_miss_dw += miss;
  if (++c.tg_miss_scans <= 24) {
    std::string hooks;
    for (uint32_t i = 0; i < c.tg_nhooks && i < 8; ++i) {
      char h[16];
      std::snprintf(h, sizeof h, " %08X", c.tg_hooks[i]);
      hooks += h;
    }
    REXLOG_WARN("[d3d-shadow] tag-scan CHECK miss seq={} frame={} emitter={} dwords={} first=dev+{} ({}) live=0x{:08X} "
                "tags=0x{:016X}/0x{:016X}/0x{:016X} | hooks since last scan ({}):{}",
                p.seq, c.frame, EmitterName(p.id), miss, 4 * first, cap::SectionName(first),
                Be32(dev, 4 * first), t[0], t[1], t[2], c.tg_nhooks, hooks);
  }
}

void VerifyMarkers(Cap& c, const uint8_t* base);  // card DE-2M

void ScanState(Cap& c, Pending& p, const uint8_t* dev, bool is_draw_kind) {
  const bool tm = stat::On(c.frame);  // C1-T3: latch once for this scope
  const uint64_t ts0 = tm ? NowNs() : 0;
  uint64_t snap_ns = 0;
  cap::Builder& b = p.mirror;
  const bool full = c.need_full || !c.delta_mode;
  if (full) {
    c.shadow.Reset();
    c.forced.clear();
    if (c.need_full) {
      p.flags |= kExtResync;
      c.c.resync.fetch_add(1, std::memory_order_relaxed);
    } else {
      p.flags |= kExtResync;  // delta mode off: every record carries the complete state
    }
    c.need_full = false;
  }
  // Card DE-3T: a trusted scan reads only the always list + the tagged ranges; anything else reads every always-section.
  bool trusted = false;
  // F2 step 1: take the marks at every scan; a scan that is not trusted reads every always dword anyway.
  const bool rep_on = c.tag_mode && TagReportOn();
  uint64_t rlo = 0, rhi = 0;
  if (rep_on) {
    rlo = g_rep_lo.exchange(0, std::memory_order_acq_rel);
    rhi = g_rep_hi.exchange(0, std::memory_order_acq_rel);
  }
  const bool rep = rep_on && TagReportActive(c.frame);
  if (c.tag_mode) {
    const bool foreign = c.tg_foreign.exchange(false, std::memory_order_relaxed);
    trusted = !full && c.tg_clean && c.tg_open == 0 && !foreign && !c.tg_ovl_overflow;
  }
  if (trusted) {
    const uint64_t t[3] = {Be64(dev, 16), Be64(dev, 24), Be64(dev, 32)};
    if (c.tag_mode == 2) TagCheck(c, p, dev, t, rep, rlo, rhi);
    ++c.tg_trusted;
  } else if (c.tag_mode) {
    ++c.tg_full;
    // N2 diagnostic: first matching reason (full/need_full, then the order of the trust test).
    const int why = full ? 0 : !c.tg_clean ? 1 : c.tg_open ? 2 : c.tg_ovl_overflow ? 4 : 3;  // 3: only foreign left
    ++c.tg_why[why];
    if (why == 1) {
      const uint32_t a = c.tg_last_untrusted;
      int slot = -1;
      for (int i = 0; i < 16; ++i)
        if (c.tg_why_hook[i].first == a || c.tg_why_hook[i].first == 0) { slot = i; break; }
      if (slot >= 0) { c.tg_why_hook[slot].first = a; ++c.tg_why_hook[slot].second; }
    }
  }
  if (trusted && c.tag_mode == 1) {
    const uint64_t t[3] = {Be64(dev, 16), Be64(dev, 24), Be64(dev, 32)};
    uint32_t visited = 0;
    if (rep) {
      for (uint32_t i = 0; i < cap::kNumTagResidual; ++i) {
        c.shadow.ScanDiff(dev, cap::kTagResidual[i].off, cap::kTagResidual[i].n, &b);
        visited += cap::kTagResidual[i].n;
      }
      cap::ForEachReported(rlo, rhi, [&](uint32_t off, uint32_t n) {
        c.shadow.ScanDiff(dev, off, n, &b);
        visited += n;
      });
    } else {
      for (uint32_t i = 0; i < cap::kNumTagAlways; ++i) {
        c.shadow.ScanDiff(dev, cap::kTagAlways[i].off, cap::kTagAlways[i].n, &b);
        visited += cap::kTagAlways[i].n;
      }
    }
    cap::ForEachTagRun(t, [&](uint32_t off, uint32_t n) {
      c.shadow.ScanDiff(dev, off, n, &b);
      visited += n;
    });
    for (uint32_t i = 0; i < c.tg_novl; ++i) c.shadow.ScanDiff(dev, c.tg_ovl[i].first, c.tg_ovl[i].second, &b);
    c.tg_dw += visited;
  } else {
    for (uint32_t i = 0; i < cap::kNumAlways; ++i) c.shadow.ScanDiff(dev, cap::kAlways[i].off, cap::kAlways[i].n, &b);
  }
  if (c.tag_mode) {
    c.tg_novl = 0;
    c.tg_ovl_overflow = false;
    c.tg_nhooks = 0;
    c.tg_clean = true;
  }
  if (full) {
    c.shadow.ScanDiff(dev, cap::kAluVs.off, cap::kAluVs.n, &b);
    c.shadow.ScanDiff(dev, cap::kAluPs.off, cap::kAluPs.n, &b);
  } else {
    const uint64_t mv = Be64(dev, 0), mp = Be64(dev, 8);
    for (int i = 0; i < 64; ++i) {
      if ((mv >> (63 - i)) & 1) c.shadow.ScanDiff(dev, cap::kAluVs.off + 16u * i, 16, &b);
      if ((mp >> (63 - i)) & 1) c.shadow.ScanDiff(dev, cap::kAluPs.off + 16u * i, 16, &b);
    }
    for (auto& r : c.forced) c.shadow.ScanDiff(dev, r.first, r.second, &b);
    c.forced.clear();
  }
  for (int i = 0; i < 5; ++i) p.dirty[i] = Be64(dev, 8 * i);
  if (b.overflow) {
    c.need_full = true;
    c.c.overflow.fetch_add(1, std::memory_order_relaxed);
  }
  if (is_draw_kind && c.fullsnap_n && !full) {
    if (++c.draw_records % c.fullsnap_n == 0) {
      uint32_t first = 0;
      const uint64_t tsn = tm ? NowNs() : 0;
      const uint32_t miss = c.shadow.CountDiff(dev, cap::kAluVs.off, cap::kAluVs.n, &first) +
                            c.shadow.CountDiff(dev, cap::kAluPs.off, cap::kAluPs.n, &first);
      if (miss) {
        c.c.dirty_miss_records.fetch_add(1, std::memory_order_relaxed);
        if (c.c.dirty_miss_dw.fetch_add(miss, std::memory_order_relaxed) < 2000 && c.c.dirty_miss_records.load() <= 12)
          REXLOG_WARN("[d3d-shadow] dirty-mask miss: {} ALU dwords changed without a dirty bit (first dw {} {}) emitter={}", miss,
                      first, cap::SectionName(first), EmitterName(p.id));
      }
      p.snap.Clear();
      for (uint32_t i = 0; i < cap::kNumAlways; ++i) c.shadow.ScanForce(dev, cap::kAlways[i].off, cap::kAlways[i].n, &p.snap);
      c.shadow.ScanForce(dev, cap::kAluVs.off, cap::kAluVs.n, &p.snap);
      c.shadow.ScanForce(dev, cap::kAluPs.off, cap::kAluPs.n, &p.snap);
      p.has_snap = !p.snap.overflow;
      if (p.has_snap) p.flags |= kExtSnapshot;
      snap_ns = tm ? NowNs() - tsn : 0;
      ++c.n_snap;
    }
  }
  const uint64_t tot = tm ? NowNs() - ts0 : 0;
  if (tm) {
    const uint64_t sc = stat::Scale(c.frame);
    c.t_snap += snap_ns * sc;
    c.t_scan += (tot - snap_ns) * sc;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// ring window: draw packets and LOAD_ALU_CONSTANT blocks the emitter wrote

void AddLoad(Cap& c, Pending& p, const uint8_t* base, uint32_t reg0, uint32_t src_addr, uint32_t n, bool inline_vals,
             uint32_t a_vals) {
  // map register by register, group contiguous device dwords into overlay runs
  uint32_t vals[1024];
  uint32_t run_off = 0, run_n = 0;
  auto flush = [&] {
    if (run_n) {
      c.shadow.Overlay(run_off, vals, run_n, &p.overlay);
      if (c.tag_mode) TagNoteOverlay(c, run_off, run_n);
    }
    run_n = 0;
  };
  for (uint32_t m = 0; m < n && m < 1024; ++m) {
    uint32_t off;
    if (!cap::RegToDevDw(reg0 + m, &off)) {
      flush();
      c.c.load_unmapped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const uint32_t v = inline_vals ? Be32(base, a_vals + 4 * m) : Be32(base, src_addr + 4 * m);
    if (run_n && off == run_off + run_n) {
      vals[run_n++] = v;
    } else {
      flush();
      run_off = off;
      vals[0] = v;
      run_n = 1;
    }
  }
  flush();
  if (c.load_temp)
    for (uint32_t m = 0; m < n; ++m) {
      uint32_t off;
      if (cap::RegToDevDw(reg0 + m, &off)) c.forced.emplace_back(off, 1);
    }
}

void AddRingLoad(Cap& c, Pending& p, const uint8_t* base, uint32_t reg0, uint32_t src_addr, uint32_t n,
                 bool inline_vals, uint32_t a_vals) {
  const uint32_t before = p.overlay.nd;
  AddLoad(c, p, base, reg0, src_addr, n, inline_vals, a_vals);
  if (c.ring_walk_mode == 2 && p.overlay.nd > before)
    c.c.walk_check_overlay_dw.fetch_add(p.overlay.nd - before, std::memory_order_relaxed);
}

// One contiguous run of written dwords [lo, end] (end = address of the last one). Returns false when a packet overruns
// the end or the walk overshoots it (misparse).
bool WalkRange(Cap& c, Pending& p, const uint8_t* base, uint32_t lo, uint32_t end) {
  uint32_t a = lo;
  while (a <= end) {
    const uint32_t h = Be32(base, a);
    const uint32_t type = h >> 30;
    if (h == 0 || type == 2) { a += 4; continue; }
    if (type == 1) { a += 12; continue; }
    const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
    if (a + 4 * count > end) return false;
    if (type == 3) {
      const uint32_t op = (h >> 8) & 0x7F;
      const bool pred = h & 1;
      if ((op == 0x22 || op == 0x36) && p.npk < kMaxPackets) p.pkt[p.npk++] = a & kPhysMask;
      else if (op == 0x2F && count >= 3 && !pred) {  // LOAD_ALU_CONSTANT
        const uint32_t ad = Be32(base, a + 4) & 0x3FFFFFFF;
        const uint32_t ot = Be32(base, a + 8);
        const uint32_t sz = Be32(base, a + 12) & 0xFFF;
        const uint32_t type_base[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
        const uint32_t t = (ot >> 16) & 0xFF;
        if (t == 0) ++p.lit_ring_n;  // card S3-58: shader literal load (the loader call is the source; this is the cross-check)
        if (t < 5 && (ad & 3) == 0 && ad < 0x1FFF0000u && GuestRangeOk(0xA0000000u + ad, uint64_t(sz) * 4) &&
            !(t == 0 && c.lit_from_hook)) {
          if (t != 0 && !c.ring_loads) {  // card S3-70 7a: the device mirror is the one source; counted only
            c.c.ring_load_skipped.fetch_add(1, std::memory_order_relaxed);
          } else {
            AddRingLoad(c, p, base, type_base[t] + (ot & 0x7FF), 0xA0000000u + ad, sz, false, 0);
            c.c.load_blocks.fetch_add(1, std::memory_order_relaxed);
          }
        }
      } else if (c.inline_mode && !pred && (op == 0x2D || op == 0x55 || op == 0x56)) {
        const uint32_t ot = Be32(base, a + 4);
        int64_t reg0 = -1;
        if (op == 0x2D) {
          const uint32_t type_base[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
          const uint32_t t = (ot >> 16) & 0xFF;
          if (t < 5) reg0 = type_base[t] + (ot & 0x7FF);
        } else {
          reg0 = ot & 0xFFFF;
        }
        if (reg0 >= 0 && count >= 2) {
          AddRingLoad(c, p, base, uint32_t(reg0), 0, count - 1, true, a + 8);
          c.c.inline_regs.fetch_add(count - 1, std::memory_order_relaxed);
        }
      }
    } else if (type == 0 && c.inline_mode) {
      const uint32_t reg = h & 0x7FFF;
      const bool one = (h >> 15) & 1;
      if (!one) {
        AddRingLoad(c, p, base, reg, 0, count, true, a + 4);
        c.c.inline_regs.fetch_add(count, std::memory_order_relaxed);
      } else {
        for (uint32_t m = 0; m < count; ++m) AddRingLoad(c, p, base, reg, 0, 1, true, a + 4 + 4 * m);
      }
    }
    a += 4 + 4 * count;
  }
  return a == end + 4;
}

void ParseWindowImpl(Cap& c, Pending& p, const uint8_t* base, uint32_t before, uint32_t after, bool want_draw) {
  p.cursor_before = before;
  p.cursor_after = after;
  p.npk = 0;
  if (c.ring_walk_mode == 0) return;
  if (c.ring_walk_mode == 2) c.c.walk_check_windows.fetch_add(1, std::memory_order_relaxed);
  if (p.nsw || p.sw_bad) c.c.seg_records.fetch_add(1, std::memory_order_relaxed);
  if (c.seg_walk && !p.sw_bad && (p.nsw || after > before)) {
    // Card S3-63: the emitter's dwords are [before+4, sw_end[0]], [sw_start[0], sw_end[1]], ..., [sw_start[n-1], after]
    // (RingMakeSpace, sub_82CBA410 / sub_82CB9670, starts each new segment at [dev+15332]; what lies between is
    // unwritten this lap or RingMakeSpace's own). Each run is walked in full; the fallback never scans a gap.
    constexpr uint32_t kSegMax = 1u << 20;
    uint32_t lo_[Pending::kMaxSw + 1], hi_[Pending::kMaxSw + 1];
    const uint32_t ns = p.nsw + 1u;
    bool ok = !(before & 3) && !(after & 3) && after >= 0x1000;
    uint32_t total = 0;
    for (uint32_t i = 0; i < ns && ok; ++i) {
      lo_[i] = i == 0 ? before + 4 : p.sw_start[i - 1];
      hi_[i] = i == p.nsw ? after : p.sw_end[i];
      ok = !(lo_[i] & 3) && !(hi_[i] & 3) && hi_[i] + 4 >= lo_[i] && hi_[i] + 4 - lo_[i] <= kSegMax &&
           (hi_[i] + 4 == lo_[i] || GuestRangeOk(lo_[i], uint64_t(hi_[i]) + 4 - lo_[i]));
      if (ok) total += hi_[i] + 4 - lo_[i];
    }
    if (ok) {
      bool clean = true;
      for (uint32_t i = 0; i < ns; ++i) clean = WalkRange(c, p, base, lo_[i], hi_[i]) && clean;
      if (!clean) c.c.walk_misparse.fetch_add(1, std::memory_order_relaxed);
      if (total > 4u * 4096) c.c.walk_long.fetch_add(1, std::memory_order_relaxed);
      if (want_draw && p.npk == 0) {
        c.c.walk_fallback.fetch_add(1, std::memory_order_relaxed);
        p.walk_fb = 1;
        for (uint32_t i = 0; i < ns; ++i)
          for (uint32_t a = lo_[i]; a <= hi_[i] && p.npk < kMaxPackets; a += 4) {
            const uint32_t h = Be32(base, a);
            if ((h >> 30) != 3) continue;
            const uint32_t op = (h >> 8) & 0x7F;
            if (op != 0x22 && op != 0x36) continue;
            const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
            if (a + 4 * count > hi_[i]) continue;
            p.pkt[p.npk++] = a & kPhysMask;
          }
      }
      if (p.nsw && c.segwin_logged < 20) {
        ++c.segwin_logged;
        REXLOG_INFO("[d3d-shadow] seg-window seq={} frame={} emitter={} before=0x{:08X} after=0x{:08X} nsw={} "
                    "run0=0x{:08X}..0x{:08X} run1=0x{:08X}..0x{:08X} total={} npk={} lit_ring={} lit_hook={} clean={} fallback={}",
                    p.seq, p.frame, EmitterName(p.id), before, after, p.nsw, lo_[0], hi_[0], lo_[1], hi_[1], total, p.npk,
                    p.lit_ring_n, p.lit_hook_n, clean ? 1 : 0, p.walk_fb);
      }
      return;
    }
    if (p.nsw) c.c.seg_bad.fetch_add(1, std::memory_order_relaxed);
    else c.c.walk_trunc.fetch_add(1, std::memory_order_relaxed);  // one run > 1 MB: old last-1-KB walk below
  } else if (p.sw_bad) {
    c.c.seg_bad.fetch_add(1, std::memory_order_relaxed);
  }
  if (after == before || (after & 3) || after < 0x1000) return;
  if (!GuestRangeOk(before, 4) || !GuestRangeOk(after, 4)) { c.c.no_packet.fetch_add(0); return; }
  uint32_t lo;
  bool wrapped = false;
  if (after > before && (after - before) <= 4u * 4096) lo = before + 4;
  else if (after > before) {
    lo = after - 4u * 256;
  } else { lo = after - 4u * 64; wrapped = true; }
  if (wrapped) {
    p.flags |= kExtWrapped;
    c.c.wrapped.fetch_add(1, std::memory_order_relaxed);
  } else {
    WalkRange(c, p, base, lo, after);
  }
  if (want_draw && p.npk == 0) {  // fallback: per-dword header search
    c.c.walk_fallback.fetch_add(1, std::memory_order_relaxed);
    p.walk_fb = 1;
    const uint32_t from = wrapped ? lo : lo;
    for (uint32_t a = from; a <= after && p.npk < kMaxPackets; a += 4) {
      const uint32_t h = Be32(base, a);
      if ((h >> 30) != 3) continue;
      const uint32_t op = (h >> 8) & 0x7F;
      if (op != 0x22 && op != 0x36) continue;
      const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
      if (a + 4 * count > after) continue;
      p.pkt[p.npk++] = a & kPhysMask;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// publish

inline uint32_t Align8(uint32_t v) { return (v + 7u) & ~7u; }

void Publish(Cap& c, Pending& p) {
  const bool tm = stat::On(c.frame);  // C1-T3: latch once for this scope
  const uint64_t pt0 = tm ? NowNs() : 0;
  {
    const uint64_t need = uint64_t(p.mirror.nb + p.overlay.nb) * 4 + uint64_t(p.mirror.nd + p.overlay.nd) * 4 + 8 +
                          sizeof(EvExt) + 8 + p.kind_bytes + 8 +
                          (p.has_snap ? uint64_t(p.snap.nb) * 4 + uint64_t(p.snap.nd) * 4 : 0);
    if (need > kSlotData) {  // cannot happen with the builder capacities; never overrun a slot
      p.active = false;
      c.need_full = true;
      c.c.overflow.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  Slot* s = c.q->Acquire();
  uint8_t* d = s->data;
  const uint32_t nb = p.mirror.nb + p.overlay.nb;
  uint32_t o = 0;
  std::memcpy(d + o, p.mirror.blocks, p.mirror.nb * sizeof(DeltaBlock));
  o += p.mirror.nb * uint32_t(sizeof(DeltaBlock));
  std::memcpy(d + o, p.overlay.blocks, p.overlay.nb * sizeof(DeltaBlock));
  o += p.overlay.nb * uint32_t(sizeof(DeltaBlock));
  std::memcpy(d + o, p.mirror.data, p.mirror.nd * 4);
  o += p.mirror.nd * 4;
  std::memcpy(d + o, p.overlay.data, p.overlay.nd * 4);
  o += p.overlay.nd * 4;
  o = Align8(o);
  EvExt x;
  std::memset(&x, 0, sizeof x);
  x.magic = kExtMagic;
  x.n_mirror_blocks = uint16_t(p.mirror.nb);
  x.flags = p.flags;
  std::memcpy(x.dirty, p.dirty, sizeof x.dirty);
  x.device = p.dev;
  x.kind_bytes = p.kind_bytes;
  if (p.has_snap) {
    x.snap_blocks = p.snap.nb;
    x.snap_dwords = p.snap.nd;
  }
  x.cursor_before = p.cursor_before;
  x.cursor_after = p.cursor_after;
  x.key_valid = p.key_valid;
  x.vs_fnv = p.vs_fnv;
  x.ps_fnv = p.ps_fnv;
  x.vs_object = p.vs_obj; x.vs_code_addr = p.vs_code; x.vs_bytes = p.vs_bytes;
  x.ps_object = p.ps_obj; x.ps_code_addr = p.ps_code; x.ps_bytes = p.ps_bytes;
  std::memcpy(d + o, &x, sizeof x);
  o += sizeof x;
  std::memcpy(d + o, p.kind_data, p.kind_bytes);
  o += Align8(p.kind_bytes);
  if (p.has_snap) {
    std::memcpy(d + o, p.snap.blocks, p.snap.nb * sizeof(DeltaBlock));
    o += p.snap.nb * uint32_t(sizeof(DeltaBlock));
    std::memcpy(d + o, p.snap.data, p.snap.nd * 4);
    o += p.snap.nd * 4;
  }
  EvHeader& h = s->h;
  std::memset(&h, 0, sizeof h);
  h.seq = p.seq;
  h.frame = p.frame;
  h.kind = p.kind;
  h.emitter = p.id;
  h.n_blocks = uint16_t(nb);
  h.payload_bytes = o;
  h.packet_addr = p.npk ? p.pkt[0] : 0xFFFFFFFFu;
  std::memcpy(h.args, p.args, sizeof h.args);
  if (sidefile::RecordEnabled()) sidefile::Record(h, d);
  const uint64_t pub = c.published.fetch_add(1, std::memory_order_acq_rel) + 1;
  c.last_pub_seq.store(h.seq, std::memory_order_release);
  c.q->Submit();
  const uint64_t depth = pub - c.consumed.load(std::memory_order_acquire);
  uint64_t m = c.c.qdepth_max.load(std::memory_order_relaxed);
  if (depth > m) c.c.qdepth_max.store(depth, std::memory_order_relaxed);
  if (depth > c.qdepth_win_max) c.qdepth_win_max = depth;
  c.c.records.fetch_add(1, std::memory_order_relaxed);
  c.c.bytes.fetch_add(o + sizeof(EvHeader), std::memory_order_relaxed);
  c.c.by_kind[uint8_t(p.kind) & 15].fetch_add(1, std::memory_order_relaxed);
  c.c.mirror_dw.fetch_add(p.mirror.nd, std::memory_order_relaxed);
  c.c.overlay_dw.fetch_add(p.overlay.nd, std::memory_order_relaxed);
  p.active = false;
  c.t_publish += tm ? (NowNs() - pt0) * stat::Scale(c.frame) : 0;
}

std::atomic<uint32_t> g_latest_seq{0};  // newest event seq the producer allocated (any thread may read)

void BeginPending(Cap& c, Pending& p, const HookInfo& hi, PPCContext& ctx, uint32_t nested) {
  p.active = true;
  p.role = hi.role;
  p.kind = hi.kind;
  p.id = uint8_t(hi.id | (nested ? EmNestedBit : 0));
  p.dev = ctx.r3.u32;
  p.seq = ++c.seq;
  g_latest_seq.store(p.seq, std::memory_order_release);  // card DE-2M torn-marker fallback
  LateLearnNoteCall(p.seq);  // card S3-67 (no-op unless WWE2K14_D3D_LATE_LEARN=d3d|census)
  p.frame = c.frame;
  p.flags = 0;
  p.has_snap = false;
  p.rb_sampled = false;
  p.kind_bytes = 0;
  p.npk = 0;
  p.key_valid = 0;
  p.lit_hook_n = p.lit_ring_n = 0;
  p.segsw = p.walk_fb = 0;
  p.nsw = p.sw_bad = 0;
  p.marker = false;
  p.mirror.Clear();
  p.overlay.Clear();
  p.cursor_before = p.cursor_after = 0;
  p.args[0] = ctx.r4.u32; p.args[1] = ctx.r5.u32; p.args[2] = ctx.r6.u32; p.args[3] = ctx.r7.u32;
  p.args[4] = ctx.r8.u32; p.args[5] = ctx.r9.u32; p.args[6] = ctx.r10.u32;
  const float f = float(ctx.f1.f64);
  std::memcpy(&p.args[7], &f, 4);
  p.args[8] = p.args[9] = 0;
}

void ReadShaderKeys(Pending& p) {
  ShaderKey k;
  p.key_valid = 0;
  if (CurrentShaderKey(0, &k)) {
    p.key_valid |= 1; p.vs_fnv = k.fnv; p.vs_obj = k.object; p.vs_code = k.code_addr; p.vs_bytes = k.bytes;
  }
  if (CurrentShaderKey(1, &k)) {
    p.key_valid |= 2; p.ps_fnv = k.fnv; p.ps_obj = k.object; p.ps_code = k.code_addr; p.ps_bytes = k.bytes;
  }
}

// Card S3-58 (G2 step 0): compare the record's literal loads at the loader call with the ones its ring walk found.
void CheckLit(Cap& c, Pending& p) {
  if (c.ring_walk_mode == 0) return;
  c.c.lit_ring.fetch_add(p.lit_ring_n, std::memory_order_relaxed);
  if (p.segsw) c.c.lit_segsw_records.fetch_add(1, std::memory_order_relaxed);
  if (p.lit_hook_n == p.lit_ring_n) return;
  const bool lost = p.lit_hook_n > p.lit_ring_n;
  const uint64_t n = (lost ? c.c.lit_lost_records : c.c.lit_ring_extra).fetch_add(1, std::memory_order_relaxed);
  if (n < (lost ? 40u : 20u))
    REXLOG_WARN("[d3d-shadow] lit-{} seq={} frame={} emitter={} span={} wrapped={} npk={} fallback={} segsw={} hook_n={} ring_n={}",
                lost ? "lost" : "extra", p.seq, p.frame, EmitterName(p.id), int64_t(p.cursor_after) - int64_t(p.cursor_before),
                (p.flags & kExtWrapped) ? 1 : 0, p.npk, p.walk_fb, p.segsw, p.lit_hook_n, p.lit_ring_n);
}

void RegisterPackets(Cap& c, Pending& p) {
  if (c.ring_walk_mode == 2 && GateEnabled() && p.npk)
    c.c.walk_check_map_packets.fetch_add(p.npk, std::memory_order_relaxed);
  for (uint32_t i = 0; i < p.npk; ++i) {
    GateRegister(p.pkt[i], p.seq);
    c.c.packets_registered.fetch_add(1, std::memory_order_relaxed);
  }
  if (!p.npk && (p.kind == EvKind::kDraw || p.kind == EvKind::kDrawUP)) c.c.no_packet.fetch_add(1, std::memory_order_relaxed);
  DrawExt de;
  std::memset(&de, 0, sizeof de);
  de.n_packets = p.npk;
  for (uint32_t i = 0; i < p.npk; ++i) de.packet_addr[i] = p.pkt[i];
  std::memcpy(p.kind_data, &de, sizeof de);
  p.kind_bytes = sizeof de;
}

uint64_t Fnv(const uint8_t* b, uint32_t n) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

void FinishUp(Cap& c, Pending& p, const uint8_t* base, bool unfinished) {
  UpExt up;
  std::memset(&up, 0, sizeof up);
  const uint32_t total = p.up_count_x_stride;
  up.total_bytes = total;
  if (!unfinished) {
    up.guest_addr = Be32(base, p.dev + 13848);
    uint32_t n = total;
    if (n > kUpMax) { n = kUpMax; p.flags |= kExtTruncated; c.c.up_trunc.fetch_add(1, std::memory_order_relaxed); }
    if (GuestRangeOk(up.guest_addr, n)) {
      up.copied_bytes = n;
      std::memcpy(p.kind_data + sizeof up, base + up.guest_addr, n);
      if (c.vbcheck_n && (++c.up_records % c.vbcheck_n == 0)) {
        up.hash = Fnv(p.kind_data + sizeof up, n);
        p.flags |= kExtVbCheck;
      }
    }
  } else {
    c.c.up_unfinished.fetch_add(1, std::memory_order_relaxed);
  }
  std::memcpy(p.kind_data, &up, sizeof up);
  p.kind_bytes = uint32_t(sizeof up) + up.copied_bytes;
  Publish(c, p);
}

void ReportIfDue(Cap& c);

}  // namespace

// ---------------------------------------------------------------------------------------------------------------


bool GuestRangeOk(uint32_t addr, uint64_t len) {
  if (addr < 0x1000) return false;
  if (len == 0) return true;
  if (uint64_t(addr) + len > 0x100000000ull) return false;
  if (addr >= 0xA0000000u) return uint64_t(addr & 0x1FFFFFFFu) + len <= 0x20000000ull;
  return true;
}

namespace {
thread_local int t_role = 0;  // 0 other, 2 worker, 3 CP; the producer is recognised through its capture tag
std::atomic<bool> g_window_closing{false};  // NoteWindowClosing (crash filter: teardown races are not crashes)

void CrashReport(const char* what, uint64_t code, uint64_t fault_addr, uint64_t ip, void* const* frames, int nframes,
                 uint64_t module_base) {
  Cap& c = C();
  WorkerStats& W = WorkerStatsRef();
  const uint32_t tag = ThreadTag();
  const char* role = t_role == 2 ? "worker" : t_role == 3 ? "CP" : (c.producer_tag.load() == tag ? "producer(game/draw)" : "other");
  char line[1400];
  std::snprintf(line, sizeof line,
                "[d3d-shadow] CRASH %s code=%llu fault_addr=0x%llx ip=0x%llx (module+0x%llx) thread_tag=%u role=%s | worker in_dispatch=%u "
                "phase=%u seq=%u kind=%u emitter=%s packet=0x%08X lock_held=%u | game last_hook=0x%08X in_hook=%u depth=%u seq=%u frame=%u "
                "published=%llu consumed=%llu module_base=0x%llx",
                what, (unsigned long long)code, (unsigned long long)fault_addr, (unsigned long long)ip,
                (unsigned long long)(ip - module_base), tag, role, W.in_dispatch.load(), W.phase.load(), W.cur_seq.load(),
                W.cur_kind.load(), EmitterName(uint8_t(W.cur_emitter.load())), W.cur_packet.load(), W.lock_held.load(),
                c.wd_hook.load(), c.wd_in_hook.load(), c.wd_depth.load(), c.wd_seq.load(), c.frame,
                (unsigned long long)c.published.load(), (unsigned long long)c.consumed.load(), (unsigned long long)module_base);
  std::fprintf(stderr, "%s\n", line);
  std::string bt = "[d3d-shadow] CRASH backtrace (module-relative):";
  char b[48];
  for (int i = 0; i < nframes; ++i) {
    std::snprintf(b, sizeof b, " +0x%llx", (unsigned long long)(uint64_t(reinterpret_cast<uintptr_t>(frames[i])) - module_base));
    bt += b;
  }
  std::fprintf(stderr, "%s\n", bt.c_str());
  std::fflush(stderr);
  REXLOG_ERROR("{}", line);
  REXLOG_ERROR("{}", bt);
}

#ifndef _WIN32
struct sigaction g_old[32];
uint64_t g_module_base = 0;
int g_crash_fd = -1;  // $WWE2K14_NR_OUT/d3d-crash.txt (else ./d3d-crash.txt), O_APPEND, opened at install time

// D3 async-signal-safe output: write(2) only, no stdio/fmt/logger/malloc.
void SafeWrite(const char* s, size_t n) {
  if (write(2, s, n) < 0) {}
  if (g_crash_fd >= 0 && write(g_crash_fd, s, n) < 0) {}
}
void SafeStr(const char* s) { SafeWrite(s, std::strlen(s)); }
void SafeNum(const char* label, uint64_t v, bool hex) {
  char b[40];
  int i = int(sizeof b);
  do {
    const unsigned d = unsigned(v % (hex ? 16 : 10));
    b[--i] = char(d < 10 ? '0' + d : 'a' + d - 10);
    v /= hex ? 16 : 10;
  } while (v && i > 2);
  if (hex) { b[--i] = 'x'; b[--i] = '0'; }
  SafeStr(label);
  SafeWrite(b + i, sizeof b - size_t(i));
}

void SafeBacktrace() {
  void* frames[48];
  const int n = backtrace(frames, 48);
#if defined(__ANDROID__)
  // Android port: bionic has no backtrace_symbols_fd - raw frame addresses only.
  for (int i = 0; i < n; ++i) SafeNum("  frame ", uint64_t(reinterpret_cast<uintptr_t>(frames[i])), true), SafeStr("\n");
#else
  backtrace_symbols_fd(frames, n, 2);
  if (g_crash_fd >= 0) backtrace_symbols_fd(frames, n, g_crash_fd);
#endif
}

// One `[d3d-crash]` header line (signal, si_addr, RIP, tid, module base) + backtrace, to fd 2 and the crash file.
void SafeReport(const char* name, int sig, uint64_t addr, uint64_t ip) {
  SafeStr("[d3d-crash] ");
  SafeStr(name);
  SafeNum(" sig=", uint64_t(sig), false);
  SafeNum(" si_addr=", addr, true);
  SafeNum(" rip=", ip, true);
  SafeNum(" (module+", ip - g_module_base, true);
  SafeNum(") tid=", uint64_t(syscall(SYS_gettid)), false);
  SafeNum(" module_base=", g_module_base, true);
  SafeStr("\n");
  SafeBacktrace();
  SafeStr("[d3d-crash] end\n");
}

// SIGSEGV/SIGILL are owned by the runtime (rex::arch::ExceptionHandler): write-watch faults are NORMAL and its handler
// list resolves them. Handlers run in install order and the first that returns true wins; only when all return false does
// ChainUnhandledSignal run (SIG_DFL, raise, _exit - never returns, which is why a handler stacked on top of it printed
// nothing). The MMIO/write-watch handler is installed at memory init, long before the first game hook, so a handler
// installed here sits LAST: it runs only for faults nobody resolved. It prints and returns false (behaviour unchanged).
// Capped so a pathological chain cannot spam.
bool UnhandledFaultCb(rex::arch::Exception* ex, void*) {
  static std::atomic<int> n{0};
  if (n.fetch_add(1) >= 8) return false;
  const bool segv = ex->code() == rex::arch::Exception::Code::kAccessViolation;
  SafeReport(segv ? "SIGSEGV(unhandled)" : "SIGILL(unhandled)", segv ? SIGSEGV : SIGILL, ex->fault_address(),
#if defined(__aarch64__)
             ex->thread_context() ? ex->thread_context()->pc : 0);  // Android port
#else
             ex->thread_context() ? ex->thread_context()->rip : 0);
#endif
  return false;
}

// SIGBUS/SIGFPE/SIGABRT are not touched by the runtime: print first, then the old behaviour.
void CrashHandler(int sig, siginfo_t* si, void* uc) {
  const ucontext_t* u = static_cast<const ucontext_t*>(uc);
#if defined(__x86_64__)
  const uint64_t ip = u ? uint64_t(u->uc_mcontext.gregs[REG_RIP]) : 0;
#else
  const uint64_t ip = 0;
  (void)u;
#endif
  const uint64_t addr = uint64_t(reinterpret_cast<uintptr_t>(si ? si->si_addr : nullptr));
  const char* name = sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" : "SIGABRT";
  SafeReport(name, sig, addr, ip);
  const struct sigaction& old = g_old[sig];
  const bool chained = (old.sa_flags & SA_SIGINFO) ? old.sa_sigaction != nullptr
                                                    : (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN);
  if (chained && sig == SIGBUS) {
    if (old.sa_flags & SA_SIGINFO) old.sa_sigaction(sig, si, uc);
    else old.sa_handler(sig);
    return;
  }
  void* frames[48];
  const int n = backtrace(frames, 48);
  CrashReport(name, uint64_t(si ? si->si_code : 0), addr, ip, frames, n, g_module_base);
  signal(sig, SIG_DFL);
  if (sig == SIGBUS || sig == SIGILL || sig == SIGFPE) return;  // fault re-executes under the default action
  raise(sig);
}

void TerminateHandler() {
  SafeStr("[d3d-crash] terminate: ");
  if (std::exception_ptr ep = std::current_exception()) {
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      SafeStr(typeid(e).name());
      SafeStr(": ");
      SafeStr(e.what());
    } catch (...) {
      SafeStr("non-std exception");
    }
  } else {
    SafeStr("no active exception");
  }
  SafeStr("\n");
  SafeBacktrace();
  std::abort();
}

int ModuleCb(struct dl_phdr_info* info, size_t, void*) {
  g_module_base = uint64_t(info->dlpi_addr);
  return 1;  // first object = the main executable
}

void InstallCrashHandler() {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;
  dl_iterate_phdr(ModuleCb, nullptr);
  {
    Dl_info di;
    if (dladdr(reinterpret_cast<void*>(&InstallCrashHandler), &di) && di.dli_fbase)
      g_module_base = uint64_t(reinterpret_cast<uintptr_t>(di.dli_fbase));  // main-module base (PIE-safe)
  }
  const char* out = std::getenv("WWE2K14_NR_OUT");
  std::string path = (out && *out) ? std::string(out) + "/d3d-crash.txt" : std::string("./d3d-crash.txt");
  g_crash_fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  void* warm[4];
  backtrace(warm, 4);  // loads libgcc now; backtrace() is not async-signal-safe on first use
  std::set_terminate(&TerminateHandler);
  rex::arch::ExceptionHandler::Install(&UnhandledFaultCb, nullptr);
  static char altstack[1 << 16];
  stack_t ss;
  ss.ss_sp = altstack;
  ss.ss_size = sizeof altstack;
  ss.ss_flags = 0;
  sigaltstack(&ss, nullptr);
  const int sigs[] = {SIGBUS, SIGFPE, SIGABRT};
  for (int s : sigs) {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = &CrashHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(s, &sa, &g_old[s]);
  }
}
#else
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
  if (g_window_closing.load(std::memory_order_relaxed)) {
    std::fprintf(stderr, "[d3d-shadow] exception 0x%08lX during shutdown after window close (ignored, exit 0)\n",
                 static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode));
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 0);
  }
  void* frames[48];
  const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
  const uint64_t base = uint64_t(reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)));
  const EXCEPTION_RECORD* r = ep->ExceptionRecord;
  const uint64_t fa = (r->NumberParameters >= 2) ? uint64_t(r->ExceptionInformation[1]) : 0;
  // 0xE06D7363 = an uncaught C++ exception (MSVC); set_terminate is per thread on Windows, so name it here.
  CrashReport(r->ExceptionCode == 0xE06D7363u ? "UNHANDLED_CPP_EXCEPTION" : "UNHANDLED_EXCEPTION", r->ExceptionCode, fa,
              uint64_t(reinterpret_cast<uintptr_t>(r->ExceptionAddress)), frames, n, base);
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}
// GitHub #174 #177 #179 (11-22): crashes that end with 0xC0000409 and no log line. On Windows an uncaught C++ exception,
// abort(), a CRT invalid-parameter stop or a pure virtual call all end in __fastfail (0xC0000409), which skips the
// exception filter above, and the old terminate handler only wrote to stderr (no console in a player's game). Write the
// reason and a backtrace to the game log first, flush it, then stop as before. One report per process.
std::atomic<bool> g_fatal_reported{false};
void FatalReport(const char* what) {
  if (g_fatal_reported.exchange(true)) return;
  void* frames[48];
  const USHORT n = CaptureStackBackTrace(1, 48, frames, nullptr);
  const uint64_t base = uint64_t(reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)));
  CrashReport(what, 0xC0000409ull, 0, 0, frames, n, base);
  rex::FlushLogging();
}
void TerminateHandler() {
  std::string what = "TERMINATE ";
  if (std::exception_ptr ep = std::current_exception()) {
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      what += typeid(e).name();
      what += ": ";
      what += e.what();
    } catch (...) {
      what += "non-std exception";
    }
  } else {
    what += "(no active exception)";
  }
  std::fprintf(stderr, "[d3d-crash] %s\n", what.c_str());
  FatalReport(what.c_str());
  std::abort();
}
void AbortSignal(int) {
  FatalReport("ABORT (abort() or a failed check)");
  signal(SIGABRT, SIG_DFL);
}
void InvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
  FatalReport("CRT_INVALID_PARAMETER");
  std::abort();
}
void PureCall() {
  FatalReport("PURE_VIRTUAL_CALL");
  std::abort();
}
void InstallCrashHandler() {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;
  std::set_terminate(&TerminateHandler);
  SetUnhandledExceptionFilter(&CrashFilter);
  signal(SIGABRT, &AbortSignal);
  _set_invalid_parameter_handler(&InvalidParameter);
  _set_purecall_handler(&PureCall);
}
#endif
}  // namespace

void NoteThreadRole(int role) { t_role = role; }
void NoteWindowClosing() { g_window_closing.store(true, std::memory_order_relaxed); }

bool ShadowEnabled() {
  static std::once_flag once;
  Cap& c = C();
  std::call_once(once, [&] { Init(c); });
  return c.enabled;
}
int ShadowMode() { return ShadowEnabled() ? C().mode : 0; }
const uint8_t* ShadowGuestBase() { return C().base; }
void SetShadowGuestBase(const uint8_t* base) { C().base = base; }

uint32_t LastPublishedSeq() { return C().last_pub_seq.load(std::memory_order_acquire); }

bool CaptureProducerBlocked() {  // card S3-44
  Cap& c = C();
  return c.q && c.q->ProducerBlocked();
}

void PublishRecorded(const EvHeader& h, const uint8_t* payload) {
  Cap& c = C();
  if (!ShadowEnabled() || !payload) return;
  if (h.payload_bytes > kSlotData) {  // oversize replayed record: counted, never overruns a slot
    c.c.overflow.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Slot* s = c.q->Acquire();
  std::memcpy(s->data, payload, h.payload_bytes);
  s->h = h;
  s->h.seq = ++c.seq;  // fresh gap-free sequence for this run
  if (h.packet_addr != 0xFFFFFFFFu) GateRegister(h.packet_addr, s->h.seq);
  if (h.kind == EvKind::kSwap) GateRegisterSwap(h.frame, s->h.seq);
  c.published.fetch_add(1, std::memory_order_acq_rel);
  c.last_pub_seq.store(s->h.seq, std::memory_order_release);
  c.q->Submit();
  c.c.records.fetch_add(1, std::memory_order_relaxed);
}

const char* EmitterName(uint8_t e) {
  switch (e & 0x7F) {
    case EmBeginVertices: return "BeginVertices";
    case EmEndVertices: return "EndVertices";
    case EmDrawIndexedVertices: return "DrawIndexedVertices";
    case EmEmit5ED0: return "Emit_5ED0";
    case EmEmit49A8: return "Emit_49A8";
    case EmEmit0810: return "Emit_0810";
    case EmEmit0968: return "Emit_0968";
    case EmEmit7930: return "Emit_7930";
    case EmEmit50E0: return "Emit_50E0";
    case EmEmitC2E0: return "Emit_C2E0";
    case EmResolveWorker: return "ResolveWorker";
    case EmResolvePublic: return "ResolvePublic";
    case EmClear: return "Clear";
    case EmRunCommandBuffer: return "RunCommandBuffer";
    case EmEndCommandBuffer: return "EndCommandBuffer";
    case EmBeginTiling: return "BeginTiling";
    case EmEndTiling: return "EndTiling";
    case EmSetRenderTarget: return "SetRenderTarget";
    case EmSetDepthStencil: return "SetDepthStencil";
    case EmApplyRTDepth: return "ApplyRTDepth";
    case EmFlushDispatcher: return "FlushDispatcher";
    case EmSwap: return "Swap";
    default: return "?";
  }
}

// Card M2b2-C. The game's resolve worker (sub_82CB2320) stores RB_COPY_CONTROL (dev+10776), DEST_BASE (+10780),
// DEST_PITCH (+10784), DEST_INFO (+10788) and the function/ref/mask/slice words (+10808..+10820), emits them to the ring
// through sub_82CC0D18(dev, dirty mask, r5 = 8960 (0x2300), r6 = dev + 10680), and at its end stores 0 back into
// +10776 and +10788 (generated/wwe2k14_recomp.85.cpp: stores :8896/:8875, emit :9331-9337 (and the same call in the
// flush dispatcher :8266-8270), reset :10780-10782 with r19 = 0). The values are therefore only observable between the
// stores and the reset: sample the device block (guest D3D device fields, not the GPU register file) at that emit call.
constexpr uint32_t kRegEmitHelper = 0x82CC0D18;
void SampleRbCopy(PPCContext& ctx, uint8_t* base) {
  Cap& c = C();
  if (ctx.r5.u32 != 8960 || ctx.r6.u32 != ctx.r3.u32 + cap::kOffRs2300Bytes) return;
  Pending& p = *c.pend;
  if (!p.active || p.role != Role::kResolve || p.rb_sampled || p.dev != ctx.r3.u32 || c.depth == 0) return;
  if (ThreadTag() != c.producer_tag.load(std::memory_order_acquire)) return;
  c.shadow.ScanDiff(base + p.dev, cap::kRbCopyOff, cap::kRbCopyN, &p.mirror);
  if (p.mirror.overflow) c.need_full = true;
  p.rb_sampled = true;
}

// Card S3-58: the game's shader literal loader sub_82CC1C58 (r3 = device, r4 = shader record: PS obj + 40 / VS obj + 872,
// r5 = data base). Its walk (generated 85:46527-46620): T = r4 + [r4+20] (0 = nothing); entries from T+20 to
// T+20+[T+16], 8 bytes each: u16 idx, u16 cnt (0 ends), u32 off; each emits LOAD_ALU_CONSTANT 0xC0022F00, addr =
// fixup(off + r5), idx*4, cnt. Called before the original (it reads only r4/r5 memory and writes the ring), every load
// goes into the open record's overlay at call time: one source, pinned to the actual load, independent of ring bytes.
void ShadowConstLoad(PPCContext& ctx, uint8_t* base) {
  Cap& c = C();
  if (!c.enabled || ThreadTag() != c.producer_tag.load(std::memory_order_acquire)) return;
  const bool tm = stat::On(c.frame);  // C1-T3: latch once for this scope
  const uint64_t t0 = tm ? NowNs() : 0;
  Pending& p = *c.pend;
  const bool open = p.active && c.depth > 0 && p.dev == ctx.r3.u32;
  const uint32_t rec = ctx.r4.u32, data = ctx.r5.u32;
  if (!GuestRangeOk(rec, 24)) { c.c.lit_reject.fetch_add(1, std::memory_order_relaxed); return; }
  const uint32_t off = Be32(base, rec + 20);
  if (!off) return;
  const uint32_t tbl = rec + off;
  if (!GuestRangeOk(tbl, 20)) { c.c.lit_reject.fetch_add(1, std::memory_order_relaxed); return; }
  const uint32_t bytes = Be32(base, tbl + 16);
  if (bytes > 8192 || !GuestRangeOk(tbl + 20, (uint64_t(bytes) + 7) & ~7ull)) {
    c.c.lit_reject.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  for (uint32_t q = tbl + 20; q < tbl + 20 + bytes; q += 8) {
    const uint32_t idx = (Be32(base, q) >> 16) & 0xFFFF, cnt = Be32(base, q) & 0xFFFF;
    if (cnt == 0) break;
    const uint32_t a = Be32(base, q + 4) + data;
    const uint32_t ad = ((a & 0x1FFFFFFFu) + ((((a >> 20) & 0xFFFu) + 512u) & 0x1000u)) & 0x3FFFFFFFu;
    if (idx * 4 + cnt > 0x800 || cnt > 1024 || (ad & 3) || ad >= 0x1FFF0000u ||
        !GuestRangeOk(0xA0000000u + ad, uint64_t(cnt) * 4)) {
      c.c.lit_reject.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (!open) {
      if (c.c.lit_outside.fetch_add(1, std::memory_order_relaxed) < 20)
        REXLOG_WARN("[d3d-shadow] lit-outside frame={} last_hook=0x{:08X} rec=0x{:08X} reg=0x{:04X} cnt={} (no open record)",
                    c.frame, c.wd_hook.load(std::memory_order_relaxed), rec, 0x4000 + idx * 4, cnt);
      continue;
    }
    ++p.lit_hook_n;
    c.c.lit_hook.fetch_add(1, std::memory_order_relaxed);
    if (c.lit_from_hook) {
      AddLoad(c, p, base, 0x4000 + idx * 4, 0xA0000000u + ad, cnt, false, 0);
      c.c.load_blocks.fetch_add(1, std::memory_order_relaxed);
    }
  }
  c.c.cap_ns.fetch_add(tm ? (NowNs() - t0) * stat::Scale(c.frame) : 0, std::memory_order_relaxed);
}

void ShadowNoteRingSwitch() {
  Cap& c = C();
  if (!c.enabled || c.ring_walk_mode == 0 || !c.pend->active || c.depth == 0) return;
  if (ThreadTag() == c.producer_tag.load(std::memory_order_acquire)) c.pend->segsw = 1;
}

// Card S3-63: outermost RingMakeSpace (sub_82CBA798) or RingNewBuffer (sub_82CB9670, called directly by the ring reserve
// sub_82CBA9D0 when RingMakeSpace's segment is still too small) entry / exit inside an open record on the producer thread. Entry:
// [dev+48] = last dword the record wrote in the old segment, [dev+15332] = old segment start. Exit: [dev+15332] = start of
// the segment the cursor is in now (sub_82CBA410 loc_82CBA580 align32 restart, or sub_82CB9670 loc_82CB976C new buffer);
// [dev+11069] & 0x20 = discard area (sub_82CB9670 fallback, 15332 not updated) -> pairs unusable.
void ShadowRingSwitchPre(const uint8_t* base, uint32_t dev) {
  Cap& c = C();
  c.rms_pre_valid = false;
  if (!c.enabled || c.ring_walk_mode == 0 || !c.seg_walk || !c.pend->active || c.depth == 0 || dev < 0x1000 || (dev & 3)) return;
  if (ThreadTag() != c.producer_tag.load(std::memory_order_acquire)) return;
  c.rms_pre_cursor = Be32(base, dev + 48);
  c.rms_pre_start = Be32(base, dev + 15332);
  c.rms_dev = dev;
  c.rms_pre_valid = true;
}

void ShadowRingSwitchPost(const uint8_t* base, uint32_t dev, bool new_buffer) {
  Cap& c = C();
  if (!c.rms_pre_valid) return;
  c.rms_pre_valid = false;
  if (!c.enabled || c.ring_walk_mode == 0 || !c.pend->active || c.depth == 0) return;
  if (ThreadTag() != c.producer_tag.load(std::memory_order_acquire)) return;
  Pending& p = *c.pend;
  const uint32_t post_start = Be32(base, dev + 15332), post_cursor = Be32(base, dev + 48);
  const uint8_t f11068 = base[dev + 11068], f11069 = base[dev + 11069];
  const bool discard = (f11069 & 0x20) != 0;
  uint32_t& logged = new_buffer ? c.nb_logged : c.seg_logged;
  if (logged < 20) {
    ++logged;
    REXLOG_INFO("[d3d-shadow] seg-switch via={} seq={} frame={} emitter={} rec_dev=0x{:08X} dev=0x{:08X} before=0x{:08X} "
                "entry_cursor=0x{:08X} entry_start=0x{:08X} exit_start=0x{:08X} exit_cursor=0x{:08X} gap={} "
                "f11068=0x{:02X} f11069=0x{:02X} n13600={}",
                new_buffer ? "RingNewBuffer" : "RingMakeSpace", p.seq, p.frame, EmitterName(p.id), p.dev, dev,
                p.cursor_before, c.rms_pre_cursor, c.rms_pre_start,
                post_start, post_cursor, int64_t(post_start) - int64_t(c.rms_pre_cursor) - 4, f11068, f11069,
                Be32(base, dev + 13600) ? 1 : 0);
  }
  if (dev != c.rms_dev || dev != p.dev) { p.sw_bad = 1; return; }  // counted as seg bad at the walk
  if (discard) { p.sw_bad = 1; c.c.seg_discard.fetch_add(1, std::memory_order_relaxed); return; }
  if (post_start == c.rms_pre_cursor + 4) return;  // contiguous: nothing skipped
  if (p.nsw >= Pending::kMaxSw) { p.sw_bad = 1; c.c.seg_overflow.fetch_add(1, std::memory_order_relaxed); return; }
  p.sw_end[p.nsw] = c.rms_pre_cursor;
  p.sw_start[p.nsw] = post_start;
  ++p.nsw;
  c.c.seg_pairs.fetch_add(1, std::memory_order_relaxed);
}

void CaptureNoteSetTexture(const PPCContext& ctx) {
  Cap& c = C();
  if (c.enabled && c.tag_mode) TagNoteSetTexture(c, ctx);
}

bool CaptureOnProducerThread() {
  Cap& c = C();
  return c.enabled && ThreadTag() == c.producer_tag.load(std::memory_order_acquire);
}

void ShadowPre(uint32_t addr, PPCContext& ctx, uint8_t* base) {
  Cap& c = C();
  if (!ShadowEnabled()) return;
  if (c.tag_mode) {
    TagHookPre(c, addr);
    if (addr == 0x82CAC430) TagNoteSetTexture(c, ctx);
  }
  if (addr == kRegEmitHelper) {
    SampleRbCopy(ctx, base);
    return;
  }
  const HookInfo hi = Lookup(addr);
  if (hi.role == Role::kNone) return;
  // Card S3-32 diagnostic (WWE2K14_NR_PC_SAMPLER=1 + WWE2K14_NR_PC_SAMPLER_GAME=1): sample the game's render thread
  // (kind 3) instead of the D3D worker - is its CPU time game work or a spin-wait on the CP? Remove when settled.
  {
    static thread_local bool sampled = false;
    static const bool on = [] {
      const char* a = std::getenv("WWE2K14_NR_PC_SAMPLER");
      const char* b = std::getenv("WWE2K14_NR_PC_SAMPLER_GAME");
      return a && *a == '1' && b && *b == '1';
    }();
    if (on && !sampled && hi.role == Role::kSwap) {
      sampled = true;
      nr::sampler::AddCurrentThread(3);
    }
  }
  if (!c.base) c.base = base;
  const uint32_t me = ThreadTag();
  Pending& p = *c.pend;
  uint32_t prod = c.producer_tag.load(std::memory_order_acquire);
  if (prod == 0) {
    c.producer_tag.store(me, std::memory_order_release);
    prod = me;
  }
  if (me != prod) {
    // The queue has one producer. The thread that issues Swap / draw emitters is the drawing thread: it takes over
    // (boot threads may have touched hooks first); a few rebinds are allowed, logged, anything else is counted + dropped.
    const bool driver = hi.role == Role::kSwap || hi.role == Role::kDraw || hi.role == Role::kBegin;
    if (driver && c.rebinds.load() < 4) {
      c.rebinds.fetch_add(1);
      REXLOG_INFO("[d3d-shadow] capture producer thread changed (tag {} -> {}) at hook 0x{:08X}, seq={}", prod, me, addr, c.seq);
      c.producer_tag.store(me, std::memory_order_release);
      c.depth = 0;
      p.active = false;
      c.need_full = true;
      c.tg_open = 0;
      c.tg_clean = false;
    } else {
      c.c.foreign_thread.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  c.wd_hook.store(addr, std::memory_order_relaxed);
  c.wd_in_hook.store(1, std::memory_order_relaxed);
  c.wd_depth.store(c.depth, std::memory_order_relaxed);
  c.wd_seq.store(c.seq, std::memory_order_relaxed);
  const bool tm = stat::On(c.frame);  // C1-T3: latch once for this scope
  const uint64_t t0 = tm ? NowNs() : 0;
  const uint32_t dev = ctx.r3.u32;
  Frame& fr = c.frames[c.depth < 31 ? c.depth : 31];
  fr.ignored = false;
  fr.dev = dev;
  const bool dev_ok = hi.role == Role::kEnd || (dev >= 0x1000 && !(dev & 3));
  if (!dev_ok) {
    c.c.bad_device.fetch_add(1, std::memory_order_relaxed);
    fr.ignored = true;
    ++c.depth;
    return;
  }
  if (hi.role != Role::kEnd && hi.role != Role::kSwap && c.device && dev != c.device) c.need_full = true;
  if (hi.role != Role::kEnd) c.device = dev;
  const uint32_t nested = c.depth;
  // An open record: BeginVertices waiting for EndVertices is closed first if something else shows up; any other open
  // record is an emitter call in progress, so whatever it calls is a sub-step of it.
  if (p.active) {
    if (p.role == Role::kBegin && hi.role != Role::kEnd) {
      FinishUp(c, p, base, true);
    } else if (p.role != Role::kBegin && (hi.role == Role::kDraw || hi.role == Role::kBegin)) {
      // A draw emitter inside an open emitter call: do not lose the draw. Close the outer record now (what its Post would
      // add is taken from the current state), so the record order stays the call order.
      if (p.role == Role::kDraw) {
        ParseWindowImpl(c, p, base, p.cursor_before, Be32(base, p.dev + 48), true);
        ReadShaderKeys(p);
        RegisterPackets(c, p);
        CheckLit(c, p);
      } else if (p.role == Role::kResolve) {
        if (p.lit_hook_n) c.c.lit_resolve.fetch_add(p.lit_hook_n, std::memory_order_relaxed);
        if (!p.rb_sampled) c.shadow.ScanDiff(base + p.dev, cap::kRbCopyOff, cap::kRbCopyN, &p.mirror);
      }
      c.nested_by[hi.id & 63][p.id & 63]++;  // counted (as "early closed"), nothing dropped
      c.c.early_closed.fetch_add(1, std::memory_order_relaxed);
      c.frames[p.frame_idx < 31 ? p.frame_idx : 31].ignored = true;
      Publish(c, p);
    } else if (p.role != Role::kBegin) {
      c.c.nested_dropped.fetch_add(1, std::memory_order_relaxed);
      c.nested_by[hi.id & 63][p.id & 63]++;
      fr.ignored = true;
      ++c.depth;
      return;
    }
  }
  ++c.depth;
  switch (hi.role) {
    case Role::kEnd:
      fr.ignored = true;
      if (p.active && p.role == Role::kBegin) FinishUp(c, p, base, false);
      break;
    case Role::kDraw:
    case Role::kResolve: {
      BeginPending(c, p, hi, ctx, nested);
      p.frame_idx = c.depth - 1;
      if (hi.role == Role::kResolve) {
        if (GuestRangeOk(ctx.r1.u32 + 92, 12)) {
          p.args[8] = Be32(base, ctx.r1.u32 + 92);
          p.args[9] = Be32(base, ctx.r1.u32 + 100);
        }
      }
      p.cursor_before = Be32(base, dev + 48);
      ScanState(c, p, base + dev, hi.role == Role::kDraw);
      if (hi.role == Role::kResolve && hi.id == EmResolvePublic) {
        ResolveExt x;
        std::memset(&x, 0, sizeof x);
        if (GuestRangeOk(p.args[1], 16)) {
          x.present |= 1;
          for (int i = 0; i < 4; ++i) x.rect[i] = int32_t(Be32(base, p.args[1] + 4 * i));
        }
        if (GuestRangeOk(p.args[3], 8)) {
          x.present |= 2;
          for (int i = 0; i < 2; ++i) x.point[i] = int32_t(Be32(base, p.args[3] + 4 * i));
        }
        if (GuestRangeOk(p.args[6], 16)) {
          x.present |= 4;
          for (int i = 0; i < 4; ++i) {
            const uint32_t w = Be32(base, p.args[6] + 4 * i);
            std::memcpy(&x.clear_rgba[i], &w, 4);
          }
        }
        std::memcpy(p.kind_data, &x, sizeof x);
        p.kind_bytes = sizeof x;
      }
      break;
    }
    case Role::kBegin:
      BeginPending(c, p, hi, ctx, nested);
      p.frame_idx = c.depth - 1;
      p.up_count_x_stride = p.args[1] * p.args[2];
      p.cursor_before = Be32(base, dev + 48);
      ScanState(c, p, base + dev, true);
      break;
    case Role::kPreOnly: {
      BeginPending(c, p, hi, ctx, nested);
      ScanState(c, p, base + dev, false);
      if (hi.kind == EvKind::kClear) {
        ClearExt ce;
        uint32_t n = p.args[0] > 64 ? 64 : p.args[0];
        if (!GuestRangeOk(p.args[1], 16ull * n)) n = 0;
        ce.n_rects = n;
        std::memcpy(p.kind_data, &ce, sizeof ce);
        std::memcpy(p.kind_data + sizeof ce, base + p.args[1], 16 * size_t(n));
        p.kind_bytes = uint32_t(sizeof ce) + 16 * n;
      } else if (hi.kind == EvKind::kBeginTiling) {
        TilingExt te;
        std::memset(&te, 0, sizeof te);
        uint32_t n = p.args[1] > 64 ? 64 : p.args[1];
        if (!GuestRangeOk(p.args[2], 16ull * n)) n = 0;
        te.n_tiles = n;
        if (GuestRangeOk(p.args[3], 16)) {
          te.has_color = 1;
          std::memcpy(te.color_be, base + p.args[3], 16);
        }
        std::memcpy(p.kind_data, &te, sizeof te);
        std::memcpy(p.kind_data + sizeof te, base + p.args[2], 16 * size_t(n));
        p.kind_bytes = uint32_t(sizeof te) + 16 * n;
      }
      p.cursor_before = p.cursor_after = Be32(base, dev + 48);
      Publish(c, p);
      break;
    }
    case Role::kPostOnly:  // args now, scan + publish at Post (the event means "after the call")
      fr.args[0] = ctx.r4.u32; fr.args[1] = ctx.r5.u32; fr.args[2] = ctx.r6.u32; fr.args[3] = ctx.r7.u32;
      fr.args[4] = ctx.r8.u32; fr.args[5] = ctx.r9.u32; fr.args[6] = ctx.r10.u32;
      fr.args[7] = fr.args[8] = fr.args[9] = 0;
      break;
    case Role::kSync: {
      BeginPending(c, p, hi, ctx, nested);
      ScanState(c, p, base + dev, false);
      if (p.mirror.Empty() && p.overlay.Empty() && !(p.flags & kExtResync)) {
        --c.seq;
        p.active = false;
        c.c.skipped_sync.fetch_add(1, std::memory_order_relaxed);
      } else {
        Publish(c, p);
      }
      break;
    }
    case Role::kSwap: {
      if (c.mk_nframe) VerifyMarkers(c, base);
      BeginPending(c, p, hi, ctx, nested);
      // Card S3-19: the D3D device word dev+14012 whose bits 17..29 Swap patches into the front-buffer fetch constant
      // (num_format + swizzle; generated 85.cpp:20153-20202). Read at Swap time; 0 = not readable.
      p.args[9] = GuestRangeOk(dev + 14012, 4) ? Be32(base, dev + 14012) : 0;
      ScanState(c, p, base + dev, false);
      Publish(c, p);
      GateRegisterSwap(c.frame, p.seq);
      ++c.frame;
      // Every recorded frame starts with the complete state (self-contained replay); RR2: only frames inside the window.
      if (sidefile::RecordEnabled() && sidefile::InWindow(c.frame)) c.need_full = true;
      break;
    }
    default:
      break;
  }
  c.c.cap_ns.fetch_add(tm ? (NowNs() - t0) * stat::Scale(c.frame) : 0, std::memory_order_relaxed);
  if (hi.role == Role::kSwap) {
    c.last_swap_ns.store(NowNs(), std::memory_order_relaxed);  // watchdog: every frame, unchanged
    ReportIfDue(c);
  }
}

// Card DE-2M (piece 2, draw-packet removal; WWE2K14_D3D_HLE_INDEXED=1, draw-off only). Called by the DrawIndexedVertices
// hook right after the original body returned, before ShadowPost: replaces the DRAW it just wrote with the same-size
// ordering marker (hle_marker.h). The original has no guest call between its DRAW stores and the cursor commit/return, so
// no ring kick can have exposed the DRAW yet. Kept as the original packet (counted): command-buffer recording
// (dev+13920 != 0, Begin/EndCommandBuffer), D3D predicated tiling (dev+11068 bit 0: other suffix), index count > 65535
// (split loop), no matching open record, or an unexpected header at dev+48 - 16.
bool HleIndexedOn() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_HLE_INDEXED");
    if (!e || std::strcmp(e, "1") != 0) return false;
    Cap& c = C();
    const bool ok = c.enabled && NrDrawOff() && GateEnabled() && c.lit_from_hook && !c.inline_mode;
    if (ok) REXLOG_INFO("[d3d-shadow] indexed draws -> ordering markers (card DE-2M)");
    else REXLOG_WARN("[d3d-shadow] WWE2K14_D3D_HLE_INDEXED=1 ignored: needs draw-off, gate on, literals from the loader call, no inline mode");
    return ok;
  }();
  return on;
}

uint32_t ShadowLatestSeq() { return g_latest_seq.load(std::memory_order_acquire); }

void ShadowIndexedMarker(uint8_t* base) {
  Cap& c = C();
  if (!c.enabled || !HleIndexedOn()) return;
  if (ThreadTag() != c.producer_tag.load(std::memory_order_acquire) || c.depth == 0) return;
  Pending& p = *c.pend;
  const Frame& fr = c.frames[(c.depth - 1) < 31 ? c.depth - 1 : 31];
  if (fr.ignored || !p.active || p.role != Role::kDraw || p.id != EmDrawIndexedVertices || p.dev != fr.dev ||
      p.frame_idx != c.depth - 1) {
    c.c.mk_record.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t dev = p.dev;
  if (Be32(base, dev + 13920) != 0) { c.c.mk_recording.fetch_add(1, std::memory_order_relaxed); return; }
  if (p.args[3] > 65535) { c.c.mk_split.fetch_add(1, std::memory_order_relaxed); return; }
  // Non-predicated suffix (7 dwords): VGT offset c+4/c+8, DRAW c+12..c+28 = dev+48. D3D predicated tiling (dev+11068 bit 0,
  // G85:15965-16010): VGT offset, SET_BIN_MASK_LO c+12/c+16 (fixup list entry -> c+8), DRAW c+20..c+36, SET_BIN_MASK_LO
  // c+40/c+44, EVENT_WRITE_EXT (screen extent) c+48..c+56, EVENT_WRITE c+60/c+64 = dev+48. Only the DRAW is replaced; the
  // bin masks, extent query and event stay. Every neighbouring header is checked before writing.
  const uint32_t end = Be32(base, dev + 48);
  const bool pred = (base[dev + 11068] & 1) != 0;
  const uint32_t hdr = end - (pred ? 44u : 16u);
  bool ok = !(end & 3) && GuestRangeOk(end - 60, 64) && Be32(base, hdr) == marker::kDrawIndxHeader &&
            Be32(base, hdr - 8) == (pred ? 0xC0006000u : 0x00002102u);
  if (ok && pred)
    ok = Be32(base, end - 60) == 0x00002102u && Be32(base, end - 24) == 0xC0006000u && Be32(base, end - 16) == 0xC0015A01u &&
         Be32(base, end - 4) == 0xC0004601u;
  if (!ok) {
    c.c.mk_header.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  marker::Encode(base + hdr, p.seq);
  p.marker = true;
  p.marker_addr = hdr & kPhysMask;
  c.c.mk_written.fetch_add(1, std::memory_order_relaxed);
  if (pred) c.c.mk_predicated.fetch_add(1, std::memory_order_relaxed);
  if (c.mk_nframe < Cap::kMkVerify) c.mk_frame[c.mk_nframe++] = {hdr, p.seq};
}

// Card DE-2M stage 2 (piece 4: no packet-address key): BeginVertices' UP DRAW becomes a marker too. Begin writes its
// suffix but commits the ring cursor only in EndVertices (dev+48 = dev+13844, generated/wwe2k14_recomp.85.cpp 82CB4E00), so
// no kick can expose the DRAW before this runs. Non-predicated (G85 82CB4E10 tail): DRAW_INDX header at dev+13844 - 8.
// D3D predicated tiling: SET_BIN_MASK_LO, DRAW (end - 36), SET_BIN_MASK_LO (end - 24), EVENT_WRITE_EXT (end - 16), EVENT_WRITE
// (end - 4). Same exclusions as indexed draws (recording; unexpected layout).
void ShadowUpMarker(uint8_t* base) {
  Cap& c = C();
  if (!c.enabled || !HleIndexedOn()) return;
  if (ThreadTag() != c.producer_tag.load(std::memory_order_acquire) || c.depth == 0) return;
  Pending& p = *c.pend;
  const Frame& fr = c.frames[(c.depth - 1) < 31 ? c.depth - 1 : 31];
  if (fr.ignored || !p.active || p.role != Role::kBegin || p.id != EmBeginVertices || p.dev != fr.dev ||
      p.frame_idx != c.depth - 1) {
    c.c.mk_record.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t dev = p.dev;
  if (Be32(base, dev + 13920) != 0) { c.c.mk_recording.fetch_add(1, std::memory_order_relaxed); return; }
  const uint32_t end = Be32(base, dev + 13844);
  const bool pred = (base[dev + 11068] & 1) != 0;
  const uint32_t hdr = end - (pred ? 36u : 8u);
  bool ok = !(end & 3) && GuestRangeOk(end - 48, 52) && Be32(base, hdr) == marker::kDrawIndxUpHeader;
  if (ok && pred)
    ok = Be32(base, end - 44) == 0xC0006000u && Be32(base, end - 24) == 0xC0006000u && Be32(base, end - 16) == 0xC0015A01u &&
         Be32(base, end - 4) == 0xC0004601u;
  if (!ok) {
    c.c.mk_header.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  marker::EncodeUp(base + hdr, p.seq);
  p.marker = true;
  p.marker_addr = hdr & kPhysMask;
  c.c.mk_up.fetch_add(1, std::memory_order_relaxed);
}

// Card DE-2M stage 3 (piece 4): the clear helper 82CB7930's DRAW_INDX_2 (G85 82CB7930 tail: type-0 0x2208 header at end - 12,
// DRAW_INDX_2 0xC0003601 at end - 4, initiator = end; cursor committed at its end) becomes a 2-dword marker. Same
// commit-then-patch window as indexed draws (a torn marker fails closed on the CP side).
void ShadowEmitMarker(uint8_t* base) {
  Cap& c = C();
  if (!c.enabled || !HleIndexedOn()) return;
  if (ThreadTag() != c.producer_tag.load(std::memory_order_acquire) || c.depth == 0) return;
  Pending& p = *c.pend;
  const Frame& fr = c.frames[(c.depth - 1) < 31 ? c.depth - 1 : 31];
  if (fr.ignored || !p.active || p.role != Role::kDraw || (p.id & ~EmNestedBit) != EmEmit7930 || p.dev != fr.dev ||
      p.frame_idx != c.depth - 1) {
    c.c.mk_record.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t dev = p.dev;
  if (Be32(base, dev + 13920) != 0) { c.c.mk_recording.fetch_add(1, std::memory_order_relaxed); return; }
  const uint32_t end = Be32(base, dev + 48);
  const uint32_t hdr = end - 4;
  if ((end & 3) || !GuestRangeOk(end - 12, 16) || Be32(base, hdr) != marker::kDrawIndx2Header1 ||
      Be32(base, end - 12) != 0x00002208u) {
    c.c.mk_header.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  marker::EncodeOne(base + hdr, p.seq);
  p.marker = true;
  p.marker_addr = hdr & kPhysMask;
  c.c.mk_emit.fetch_add(1, std::memory_order_relaxed);
}

namespace {
// Card DE-2M integrity: at Swap, every marker written this frame must still be our marker (nothing in the guest D3D
// rewrote the replaced DRAW slot, e.g. a tiling fixup). Counted; the first few are logged.
void VerifyMarkers(Cap& c, const uint8_t* base) {
  for (uint32_t i = 0; i < c.mk_nframe; ++i) {
    uint32_t seq = 0;
    const bool intact = marker::LoadBe(base + c.mk_frame[i].addr) == marker::kNopHeader &&
                        marker::Decode(base + c.mk_frame[i].addr + 4, &seq) && seq == c.mk_frame[i].seq;
    if (intact) continue;
    if (c.c.mk_overwritten.fetch_add(1, std::memory_order_relaxed) < 8)
      REXLOG_WARN("[d3d-shadow] marker overwritten before Swap: addr=0x{:08X} seq={} now 0x{:08X} 0x{:08X}",
                  c.mk_frame[i].addr, c.mk_frame[i].seq, Be32(base, c.mk_frame[i].addr), Be32(base, c.mk_frame[i].addr + 4));
  }
  c.mk_nframe = 0;
}
}  // namespace

void ShadowPost(uint32_t addr, PPCContext& ctx, uint8_t* base) {
  Cap& c = C();
  if (!c.enabled) return;
  if (c.tag_mode) TagHookPost(c, addr);
  if (addr == kRegEmitHelper) return;  // sampling hook only: no frame, no record
  const HookInfo hi = Lookup(addr);
  if (hi.role == Role::kNone) return;
  c.wd_in_hook.store(0, std::memory_order_relaxed);  // back from the original function
  if (ThreadTag() != c.producer_tag.load(std::memory_order_acquire)) return;
  if (c.depth == 0) return;
  --c.depth;
  Frame& fr = c.frames[c.depth < 31 ? c.depth : 31];
  if (fr.ignored) return;
  const bool tm = stat::On(c.frame);  // C1-T3: latch once for this scope
  const uint64_t t0 = tm ? NowNs() : 0;
  Pending& p = *c.pend;
  const uint32_t dev = fr.dev;
  switch (hi.role) {
    case Role::kDraw:
    case Role::kResolve:
      if (!p.active || p.seq == 0) break;
      if (hi.role == Role::kDraw && p.marker) {
        // Card DE-2M: the window holds no DRAW (the marker replaced it) and the gate key is the seq in the marker:
        // no window walk, no packet registration. Literal loads come from the loader call (lit_from_hook).
        const uint64_t a1 = tm ? NowNs() : 0;
        p.cursor_after = Be32(base, dev + 48);
        ReadShaderKeys(p);
        const uint64_t a2 = tm ? NowNs() : 0;
        NoteDrawKick(p.seq);
        p.npk = 1;
        p.pkt[0] = p.marker_addr;
        DrawExt de;
        std::memset(&de, 0, sizeof de);
        de.n_packets = 1;
        de.packet_addr[0] = p.marker_addr;
        std::memcpy(p.kind_data, &de, sizeof de);
        p.kind_bytes = sizeof de;
        c.t_keys += (a2 - a1) * stat::Scale(c.frame);
        c.t_ext += tm ? (NowNs() - a2) * stat::Scale(c.frame) : 0;
      } else if (hi.role == Role::kDraw) {
        {
          const uint64_t a0 = tm ? NowNs() : 0;
          ParseWindowImpl(c, p, base, p.cursor_before, Be32(base, dev + 48), true);
          const uint64_t a1 = tm ? NowNs() : 0;
          ReadShaderKeys(p);
          const uint64_t a2 = tm ? NowNs() : 0;
          RegisterPackets(c, p);
          CheckLit(c, p);
          if (c.texhash_n && (++c.texhash_ctr % c.texhash_n) == 0) {
            // Card M2c9: the first 64 KB at the base page of every bound texture fetch constant (type 2; dword1 bits
            // 12-31 = base page), distinct, at most 16 - the window the CPU rewrites when it updates a video texture.
            VbHashExt th;
            std::memset(&th, 0, sizeof th);
            for (uint32_t slot = 0; slot < kFetchSlots && th.n < 16; ++slot) {
              const uint32_t off = dev + kOffFetch + 4u * (slot * 6);
              const uint32_t d0 = Be32(base, off), d1 = Be32(base, off + 4);
              if ((d0 & 3u) != 2u) continue;
              const uint32_t phys = (d1 & 0xFFFFF000u) & 0x1FFFFFFFu;
              if (!phys) continue;
              bool dup = false;
              for (uint32_t j = 0; j < th.n; ++j) dup = dup || th.w[j].phys == phys;
              if (dup) continue;
              const uint32_t len = 65536u;
              if (!GuestRangeOk(0xA0000000u + phys, len)) continue;
              th.w[th.n++] = {phys, len, WindowHash(base + 0xA0000000u + phys, len)};
            }
            if (p.kind_bytes + sizeof th <= sizeof(p.kind_data)) {
              std::memcpy(p.kind_data + p.kind_bytes, &th, sizeof th);
              p.kind_bytes += uint32_t(sizeof th);
              p.flags |= kExtTexHash;
            }
          }
          c.t_parse += (a1 - a0) * stat::Scale(c.frame);
          c.t_keys += (a2 - a1) * stat::Scale(c.frame);
          c.t_ext += tm ? (NowNs() - a2) * stat::Scale(c.frame) : 0;
        }
      } else {
        if (p.lit_hook_n) c.c.lit_resolve.fetch_add(p.lit_hook_n, std::memory_order_relaxed);  // card S3-58 path D
        // RB_COPY_* control/info are zeroed again by the worker's tail, so the values were taken at the emit helper
        // (SampleRbCopy). Only a resolve that never reached it falls back to the (post-reset) device words.
        if (p.rb_sampled) {
          c.c.rb_sampled.fetch_add(1, std::memory_order_relaxed);
        } else {
          c.c.rb_unsampled.fetch_add(1, std::memory_order_relaxed);
          c.shadow.ScanDiff(base + dev, cap::kRbCopyOff, cap::kRbCopyN, &p.mirror);
          if (p.mirror.overflow) c.need_full = true;
        }
      }
      Publish(c, p);
      break;
    case Role::kBegin:
      if (!p.active) break;
      if (p.marker) {  // card DE-2M stage 2: no window walk / packet registration (the marker carries the seq)
        const uint64_t a1 = tm ? NowNs() : 0;
        p.cursor_after = Be32(base, dev + 13844);
        ReadShaderKeys(p);
        c.t_keys += tm ? (NowNs() - a1) * stat::Scale(c.frame) : 0;
        NoteDrawKick(p.seq);
        p.npk = 1;
        p.pkt[0] = p.marker_addr;
      } else {
        const uint64_t a0 = tm ? NowNs() : 0;
        ParseWindowImpl(c, p, base, p.cursor_before, Be32(base, dev + 13844), true);
        const uint64_t a1 = tm ? NowNs() : 0;
        ReadShaderKeys(p);
        c.t_parse += (a1 - a0) * stat::Scale(c.frame);
        c.t_keys += tm ? (NowNs() - a1) * stat::Scale(c.frame) : 0;
        RegisterPackets(c, p);
        CheckLit(c, p);
      }
      p.active = true;  // stays open until EndVertices
      p.role = Role::kBegin;
      p.kind_bytes = 0;  // kind data of a kDrawUP is the UpExt written at EndVertices
      break;
    case Role::kPostOnly: {
      BeginPending(c, p, hi, ctx, c.depth);
      p.dev = dev;
      std::memcpy(p.args, fr.args, sizeof p.args);
      ScanState(c, p, base + dev, false);
      p.cursor_before = p.cursor_after = Be32(base, dev + 48);
      Publish(c, p);
      break;
    }
    default:
      break;
  }
  c.c.cap_ns.fetch_add(tm ? (NowNs() - t0) * stat::Scale(c.frame) : 0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------
// report

namespace {

void Report(Cap& c, const char* why) {
  std::lock_guard<std::mutex> lk(c.report_mu);
  c.last_report_ns.store(NowNs());
  const uint32_t frames = c.frame - c.last_report_frame;
  const double f = frames ? double(frames) : 1.0;
  Counters& k = c.c;
  const uint64_t records = k.records.load(), bytes = k.bytes.load(), cap_ns = k.cap_ns.load();
  const uint64_t full_wait = c.q ? c.q->FullWaitNs() : 0;
  WorkerStats& W = WorkerStatsRef();
  const uint64_t busy = W.busy_ns.load(), cpu = W.cpu_ns.load(), lock = W.lock_wait_ns.load();
  const GateStats g = GateSnapshot();
  const uint64_t gwait = g.wait_ns, swwait = g.swap_wait_ns;
  std::string extra;
  if (!c.ring_loads)  // card S3-70 7a (WWE2K14_D3D_RING_LOADS=0)
    extra = " | ring_loads=off skipped_t1-4=" + std::to_string(k.ring_load_skipped.load());
  if (HleIndexedOn())  // card DE-2M
    extra += " | markers written=" + std::to_string(k.mk_written.load()) + " up=" + std::to_string(k.mk_up.load()) + " emit7930=" + std::to_string(k.mk_emit.load()) + " reached=" + std::to_string(g.markers) +
             " (tiled " + std::to_string(k.mk_predicated.load()) + ") overwritten=" +
             std::to_string(k.mk_overwritten.load()) + " kept: recording=" + std::to_string(k.mk_recording.load()) + " split=" + std::to_string(k.mk_split.load()) + " record=" +
             std::to_string(k.mk_record.load()) + " header=" + std::to_string(k.mk_header.load());
  char kinds[192];
  std::snprintf(kinds, sizeof kinds, "draw=%.1f up=%.1f clear=%.1f resolve=%.1f setrt=%.1f setds=%.1f tile=%.1f swap=%.1f sync=%.1f",
                double(k.by_kind[uint8_t(EvKind::kDraw)].load() - c.last_kind[0]) / f,
                double(k.by_kind[uint8_t(EvKind::kDrawUP)].load() - c.last_kind[1]) / f,
                double(k.by_kind[uint8_t(EvKind::kClear)].load() - c.last_kind[2]) / f,
                double(k.by_kind[uint8_t(EvKind::kResolve)].load() - c.last_kind[3]) / f,
                double(k.by_kind[uint8_t(EvKind::kSetRT)].load() - c.last_kind[4]) / f,
                double(k.by_kind[uint8_t(EvKind::kSetDepth)].load() - c.last_kind[5]) / f,
                double((k.by_kind[uint8_t(EvKind::kBeginTiling)].load() - c.last_kind[6]) +
                       (k.by_kind[uint8_t(EvKind::kEndTiling)].load() - c.last_kind[7])) / f,
                double(k.by_kind[uint8_t(EvKind::kSwap)].load() - c.last_kind[8]) / f,
                double(k.by_kind[uint8_t(EvKind::kFullSnapshot)].load() - c.last_kind[9]) / f);
  c.last_kind[0] = k.by_kind[uint8_t(EvKind::kDraw)].load();
  c.last_kind[1] = k.by_kind[uint8_t(EvKind::kDrawUP)].load();
  c.last_kind[2] = k.by_kind[uint8_t(EvKind::kClear)].load();
  c.last_kind[3] = k.by_kind[uint8_t(EvKind::kResolve)].load();
  c.last_kind[4] = k.by_kind[uint8_t(EvKind::kSetRT)].load();
  c.last_kind[5] = k.by_kind[uint8_t(EvKind::kSetDepth)].load();
  c.last_kind[6] = k.by_kind[uint8_t(EvKind::kBeginTiling)].load();
  c.last_kind[7] = k.by_kind[uint8_t(EvKind::kEndTiling)].load();
  c.last_kind[8] = k.by_kind[uint8_t(EvKind::kSwap)].load();
  c.last_kind[9] = k.by_kind[uint8_t(EvKind::kFullSnapshot)].load();
  REXLOG_INFO(
      "[d3d-shadow] {} mode={} frame={} win={} | capture: rec/f={:.1f} ({}) KB/f={:.1f} q_depth_max={} (win {}) cap_ms/f={:.3f} "
      "full_wait_ms/f={:.3f} mirror_dw/rec(total)={} overlay_dw={} loads={} | worker: busy_ms/f={:.3f} cpu_ms/f={:.3f} "
      "lock_wait_ms/f={:.3f} handoffs={} ok={} skipped={} submitted={} submit_fail={} tex_fail_draws={} texhash={}/{} | gate: reg={} pass={} waits={} timeouts={} unpaired={} "
      "wait_ms/f={:.3f} max_wait_ms={:.2f} swap_waits={} swap_timeouts={} swap_unpaired={} swap_wait_ms/f={:.3f}{}",
      why, c.mode, c.frame, frames, double(records - c.last_records) / f, kinds, double(bytes - c.last_bytes) / 1024.0 / f,
      k.qdepth_max.load(), c.qdepth_win_max, double(cap_ns - c.last_cap_ns) / 1e6 / f,
      double(full_wait - c.last_full_wait_ns) / 1e6 / f, k.mirror_dw.load(), k.overlay_dw.load(), k.load_blocks.load(),
       double(busy - c.last_busy) / 1e6 / f, double(cpu - c.last_cpu) / 1e6 / f, double(lock - c.last_lock) / 1e6 / f,
      W.handoffs.load(), W.draws_translated.load(), W.draws_skipped.load(), W.draws_submitted.load(), W.submit_fail.load(), W.tex_fail_draws.load(), W.texh_changed.load(), W.texh_checked.load(), g.registered,
      g.passed, g.waited, g.timeouts, g.unpaired, double(gwait - c.last_gate_wait) / 1e6 / f, double(g.max_wait_ns) / 1e6,
       g.swap_waited, g.swap_timeouts, g.swap_unpaired, double(swwait - c.last_swap_wait) / 1e6 / f, extra);
  REXLOG_INFO(
      "[d3d-shadow] {} checks: fullsnap checked={} bad_records={} bad_dwords={} dirty_miss_records={} dirty_miss_dw={} | "
      "vbcheck checked={} bad={} | capture anomalies: resync={} overflow={} nested_dropped={} foreign_thread={} bad_device={} "
      "up_unfinished={} up_trunc={} wrapped={} no_packet={} walk_fallback={} load_unmapped={} inline_regs={} skipped_sync={} early_closed={} "
      "apply_fail={} packets={} rb_sampled={} rb_unsampled={} | literals({}): hook={} ring={} lost_records={} ring_extra={} "
      "outside={} in_resolve={} segsw_records={} reject={} | seg({}): pairs={} records={} bad={} discard={} overflow={} "
      "misparse={} long={} trunc={}",
      why, W.snap_checked.load(), W.snap_bad_records.load(), W.snap_bad_dwords.load(), k.dirty_miss_records.load(),
      k.dirty_miss_dw.load(), W.vb_checked.load(), W.vb_bad.load(), k.resync.load(), k.overflow.load(),
      k.nested_dropped.load(), k.foreign_thread.load(), k.bad_device.load(), k.up_unfinished.load(), k.up_trunc.load(),
      k.wrapped.load(), k.no_packet.load(), k.walk_fallback.load(), k.load_unmapped.load(), k.inline_regs.load(),
      k.skipped_sync.load(), k.early_closed.load(), W.apply_fail.load(), k.packets_registered.load(), k.rb_sampled.load(),
      k.rb_unsampled.load(), c.lit_from_hook ? "loader-call" : "ring", k.lit_hook.load(), k.lit_ring.load(),
      k.lit_lost_records.load(), k.lit_ring_extra.load(), k.lit_outside.load(), k.lit_resolve.load(),
      k.lit_segsw_records.load(), k.lit_reject.load(), c.seg_walk ? "on" : "off", k.seg_pairs.load(), k.seg_records.load(),
      k.seg_bad.load(), k.seg_discard.load(), k.seg_overflow.load(), k.walk_misparse.load(), k.walk_long.load(),
      k.walk_trunc.load());
  {
    const double ms = 1e6 * f;
    REXLOG_INFO("[d3d-shadow] {} capture split ms/f: scan={:.3f} snapshot={:.3f} (n/f={:.1f}) parse={:.3f} shader_keys={:.3f} packets+ext={:.3f} publish(incl full-queue wait)={:.3f} | worker split ms/f: apply={:.3f} rv_apply={:.3f} snapcmp={:.3f} translate={:.3f} other_events={:.3f}",
                why, double(c.t_scan - c.last_t[0]) / ms, double(c.t_snap - c.last_t[1]) / ms, double(c.n_snap - c.last_kind[10 & 15]) / f,
                double(c.t_parse - c.last_t[2]) / ms, double(c.t_keys - c.last_t[3]) / ms, double(c.t_ext - c.last_t[4]) / ms,
                double(c.t_publish - c.last_t[5]) / ms, double(W.ns_apply.load() - c.last_w[0]) / ms,
                double(W.ns_rv.load() - c.last_w[1]) / ms, double(W.ns_snap.load() - c.last_w[2]) / ms,
                double(W.ns_translate.load() - c.last_w[3]) / ms, double(W.ns_other.load() - c.last_w[4]) / ms);
    c.last_t[0] = c.t_scan; c.last_t[1] = c.t_snap; c.last_t[2] = c.t_parse; c.last_t[3] = c.t_keys; c.last_t[4] = c.t_ext; c.last_t[5] = c.t_publish;
    c.last_kind[10 & 15] = c.n_snap;
    c.last_w[0] = W.ns_apply.load(); c.last_w[1] = W.ns_rv.load(); c.last_w[2] = W.ns_snap.load();
     c.last_w[3] = W.ns_translate.load(); c.last_w[4] = W.ns_other.load();
    {  // C1-T2: other_events by event kind (worker), same window
      std::string kl;
      for (uint32_t ki = 0; ki < kEvKindCount; ++ki) {
        const uint64_t ns_now = W.ns_kind[ki].load(std::memory_order_relaxed), n_now = W.n_kind[ki].load(std::memory_order_relaxed);
        if (n_now != c.last_kind_n[ki])
          kl += fmt::format(" {}={:.3f}(n/f={:.1f})", kEvKindNames[ki], double(ns_now - c.last_kind_ns[ki]) / ms,
                            double(n_now - c.last_kind_n[ki]) / f);
        c.last_kind_ns[ki] = ns_now;
        c.last_kind_n[ki] = n_now;
      }
      REXLOG_INFO("[d3d-shadow] {} other_events by kind ms/f:{}", why, kl);
    }
    std::string nl;
    for (int i = 0; i < 64; ++i)
      for (int o = 0; o < 64; ++o)
        if (c.nested_by[i][o]) nl += " " + std::string(EmitterName(uint8_t(i))) + "<-" + EmitterName(uint8_t(o)) + "=" + std::to_string(c.nested_by[i][o]);
    if (!nl.empty()) REXLOG_INFO("[d3d-shadow] {} nested dropped (inner<-outer):{}", why, nl);
    if (c.tag_mode) {
      // Card DE-3T: share of tag-guided scans, always-section dwords they read, CHECK misses (+ worst dwords so far).
      const uint64_t tr = c.tg_trusted - c.last_tg[0], fu = c.tg_full - c.last_tg[1], dw = c.tg_dw - c.last_tg[2];
      std::string top;
      if (c.tag_mode == 2 && c.tg_miss_dw) {
        uint32_t idx[8] = {}, cnt[8] = {};
        for (uint32_t i = 0; i < cap::kImageDwords; ++i) {
          const uint32_t v = c.tg_miss_by_dw[i];
          for (int k = 0; k < 8; ++k) {
            if (v <= cnt[k]) continue;
            for (int m = 7; m > k; --m) { cnt[m] = cnt[m - 1]; idx[m] = idx[m - 1]; }
            cnt[k] = v; idx[k] = i;
            break;
          }
        }
        for (int k = 0; k < 8 && cnt[k]; ++k)
          top += " dev+" + std::to_string(4 * idx[k]) + "(" + cap::SectionName(idx[k]) + ")=" + std::to_string(cnt[k]);
      }
      REXLOG_INFO("[d3d-shadow] {} tag-scan {}: trusted/f={:.1f} full/f={:.1f} share={:.1f}% always_dw/trusted={:.1f} | "
                  "check: scans={} miss_scans={} miss_dw={}{}",
                  why, c.tag_mode == 1 ? "ON" : "CHECK", double(tr) / f, double(fu) / f,
                  tr + fu ? 100.0 * double(tr) / double(tr + fu) : 0.0, tr ? double(dw) / double(tr) : 0.0,
                  c.tg_checked, c.tg_miss_scans, c.tg_miss_dw, top);
      c.last_tg[0] = c.tg_trusted; c.last_tg[1] = c.tg_full; c.last_tg[2] = c.tg_dw;
      std::string hooks;
      for (const auto& h : c.tg_why_hook)
        if (h.first) hooks += fmt::format(" {:08X}={}", h.first, h.second);
      REXLOG_INFO("[d3d-shadow] {} tag-scan full-why/f: need_full={:.1f} untrusted_hook={:.1f} open_hook={:.1f} foreign={:.1f} "
                  "overlay_overflow={:.1f} | untrusted (last hook, total):{}",
                  why, double(c.tg_why[0] - c.tg_why_last[0]) / f, double(c.tg_why[1] - c.tg_why_last[1]) / f,
                  double(c.tg_why[2] - c.tg_why_last[2]) / f, double(c.tg_why[3] - c.tg_why_last[3]) / f,
                  double(c.tg_why[4] - c.tg_why_last[4]) / f, hooks);
      for (int i = 0; i < 5; ++i) c.tg_why_last[i] = c.tg_why[i];
    }
  }
  if (c.ring_walk_mode != 1) {
    const uint64_t map_packets = k.walk_check_map_packets.load(std::memory_order_relaxed);
    const uint64_t overlay_dw = k.walk_check_overlay_dw.load(std::memory_order_relaxed);
    REXLOG_INFO("[d3d-shadow] {} ring-walk {}: windows={} reachable_map_packets={} reachable_overlay_dw={} consumed_total={}",
                why, c.ring_walk_mode == 0 ? "off" : "check", k.walk_check_windows.load(std::memory_order_relaxed),
                map_packets, overlay_dw, map_packets + overlay_dw);
  }
  c.last_records = records;
  c.last_bytes = bytes;
  c.last_cap_ns = cap_ns;
  c.last_full_wait_ns = full_wait;
  c.last_busy = busy;
  c.last_cpu = cpu;
  c.last_lock = lock;
  c.last_gate_wait = gwait;
  c.last_swap_wait = swwait;
  c.last_report_frame = c.frame;
  c.qdepth_win_max = 0;
}

void ReportIfDue(Cap& c) {
  if (c.frame - c.last_report_frame >= c.report_every) Report(c, "");
}

}  // namespace

int ShadowRingWalkMode() {
  static const int mode = [] {
    const char* value = std::getenv("WWE2K14_D3D_RING_WALK");
    if (!value || !*value || std::strcmp(value, "1") == 0) return 1;
    if (std::strcmp(value, "0") == 0) return 0;
    if (std::strcmp(value, "check") == 0) return 2;
    REXLOG_WARN("[d3d-shadow] invalid WWE2K14_D3D_RING_WALK='{}' (expected 0, 1 or check); retaining current path", value);
    return 1;
  }();
  return mode;
}

}  // namespace wwe2k14::d3d
