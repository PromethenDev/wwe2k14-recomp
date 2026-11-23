// WP1 (decision 0006 stage 1): observe-only call counters / timing on the WWE 2K14 guest D3D functions.
// Addresses and roles: docs/reports/20261014-2k14-d3d-entry.md (+ -coverage, -layout). Each hook calls the
// original guest body with the guest context untouched, so behaviour is identical on and off.
// Hook mechanism: strong extern "C" PPC_FUNC(sub_X) replaces the weak generated alias; __imp__sub_X is the original
// (same as src/present_interval.cpp). Written here from the project's own override pattern.
#include "hooks.h"
#include "capture.h"     // WP9: ShadowEnabled/ShadowPre/ShadowPost
#include "stat_timing.h"  // C1-T3: statistics stopwatches sampled 1 frame in N
#include "present.h"     // NrDrawOff (card DE-3R)
#include "shader_identity.h"  // WP5-impl: shader flush identity
#include "native_setters.h"  // F1: native D3D setter bodies
#include "g18c_diag.h"
#include "native_rt.h"         // N2-RT: native SetRenderTarget tree (default off)       // TEST-ONLY card G18 diagnostic (default off)
#include "native_resolve.h"
#include "native_clear.h"
#include "native_draw.h"       // N3: native draw entry 82CB52D0 (default on on desktop)
#include "native_shader.h"     // N4: native SetPixelShader / SetVertexShader (default on on desktop)
#include "crowd_callers.h"       // crowd decompile C1 (test-only diagnostic)
#include "tag_report.h"         // F2 step 1: writer-reported always dwords
#include "native_flush.h"      // N4 step 2c: native shader flush 82CC1E48 (default on on desktop)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/diagnostics/observers.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

REXCVAR_DECLARE(bool, WWE2K14_DIAG_HANDLES);

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

// Hook list (append only: capture.h Emitter ordinals follow this order). X(addr, name, argsLog) ; argsLog=1 logs r3..r6 of the first 20 calls (disputed-role evidence).
// Already overridden elsewhere (not hooked here): 82CB6318, 82CB9048, 82CB5840, 82CB9310.
#define D3D_HOOK_LIST(X)                       \
  X(82CB4E10, BeginVertices, 0)                \
  X(82CB4E00, EndVertices, 0)                  \
  X(82CB52D0, DrawIndexedVertices, 0)          \
  X(82CB5ED0, Emit_5ED0_DRAW_INDX, 0)          \
  X(82CB49A8, Emit_49A8_DRAW_UP, 0)            \
  X(82CB0810, Emit_0810_DRAW_INDX_2, 0)        \
  X(82CB0968, Emit_0968_DRAW_INDX_2, 0)        \
  X(82CB7930, Emit_7930_DRAW_INDX_2, 0)        \
  X(82CC50E0, Emit_50E0_DRAW_INDX_2, 0)        \
  X(82CCC2E0, Emit_C2E0_DRAW_INDX_2, 0)        \
  X(82CB2320, ResolveWorker, 0)                \
  X(82CB3478, ResolvePublic, 0)                \
  X(82CB8BA8, Clear, 0)                        \
  X(82CB97E0, IB_97E0, 0)                      \
  X(82CB4848, IB_4848_ShaderGPR, 0)            \
  X(82CBD610, RunCommandBuffer, 0)             \
  X(82CC9D58, IB_9D58, 0)                      \
  X(82CBC088, BeginCommandBuffer, 0)           \
  X(82CBD1F0, EndCommandBuffer, 0)             \
  X(82CBB1D0, BeginTiling, 0)                  \
  X(82CBB540, EndTiling, 0)                    \
  X(82CAF5E8, SetRenderTarget, 0)              \
  X(82CAF978, SetDepthStencil, 0)              \
  X(82CAFC50, ApplyRTDepth, 0)                 \
  X(82CAC430, SetTexture, 0)                   \
  X(82CB3F48, SetVSConstantF, 0)               \
  X(82CB4020, SetPSConstantF, 0)               \
  X(82CB20C0, FlushDispatcher, 0)              \
  X(82CB44A8, SetVertexShader, 0)              \
  X(82CB42A0, SetPixelShader, 0)               \
  X(82CB46C0, SetVertexDecl_46C0, 1)           \
  X(82CAECB8, SetStreamSrc_ECB8, 1)            \
  X(82CAEE60, SetIndices_EE60, 1)              \
  X(82CB4DB0, SetFVF_4DB0, 1)                  \
  X(82BB8C00, RenderCmdInterp, 0)              \
  X(82CBA798, RingMakeSpace, 0)                \
  X(82CB70B0, Swap, 0)                         \
  X(82CC1E48, ShaderFlush, 0)                  \
  X(82CC1B00, ShaderVSUpload, 0)               \
  X(82CC1C58, ShaderConstLoad, 0)               \
  X(82CC0D18, RegEmitHelper, 0)                \
  X(82CB9670, RingNewBuffer, 0)

extern "C" void wwe2k14_pump_ring_node(uint8_t* base, uint32_t pump, uint32_t slot);  // src/cage_hang_probe.cpp (D8)
extern "C" bool wwe2k14_pump_guard(uint8_t* base, uint32_t pump, uint32_t slot);    // src/cage_hang_probe.cpp (S3-56)

namespace {

// Card DE-3R (piece 3, state-upload removal): with native drawing (draw-off) nothing consumes the render state, fetch and
// ALU constants the guest D3D encodes into PM4: the CP skips host draws (nr_skip_host_draws, SDK command_processor.cpp
// ExecutePacketType3Draw) and the native renderer reads the D3D device, never the SDK register file. WWE2K14_D3D_STATE_UPLOAD=0
// skips the three state encoders: 82CC0D18 per register group (except r5 = 8960, the 0x2300 group: RB_SAMPLE_COUNT_ADDR
// 0x2325 drives the CP's occlusion-query memory writes, EVENT_WRITE_ZPD), 82CC0F58 (fetch constants) and 82CC10B0 (ALU
// constants). Their callers keep every other effect (dirty-tag clears, shader flush, coherency 82CC0DD8, draw packets); the
// encoders only write the ring and the cursor dev+48 (and may call the ring make-space helper, skipped with them).
bool StateUploadOff() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_STATE_UPLOAD");
    const bool off = e && std::strcmp(e, "0") == 0 && wwe2k14::d3d::NrDrawOff();
    if (off) REXLOG_INFO("[d3d-obs] state upload removed (card DE-3R): 82CC0D18 groups except 0x2300, 82CC0F58, 82CC10B0");
    return off;
  }();
  return on;
}
std::atomic<uint64_t> g_skip_reg{0}, g_skip_fetch{0}, g_skip_alu{0};
// Review de23 SHOULD-FIX 3: never while the device records a command buffer (dev+13920 != 0, Begin/EndCommandBuffer):
// a recorded buffer is replayed later and must keep the original packets.
inline bool Recording(const uint8_t* base, uint32_t dev) {
  uint32_t v;
  std::memcpy(&v, base + dev + 13920, 4);
  return v != 0;
}
inline bool SkipRegGroupUpload(const uint8_t* base, uint32_t dev, uint32_t reg_base) {
  if (reg_base == 8960 || !StateUploadOff() || Recording(base, dev)) return false;
  g_skip_reg.fetch_add(1, std::memory_order_relaxed);
  return true;
}

enum HookId : uint32_t {
#define X(addr, name, args) H_##name,
  D3D_HOOK_LIST(X)
#undef X
  H_COUNT
};

const char* const kHookNames[H_COUNT] = {
#define X(addr, name, args) #name,
    D3D_HOOK_LIST(X)
#undef X
};
constexpr uint32_t kHookAddrs[H_COUNT] = {
#define X(addr, name, args) 0x##addr,
    D3D_HOOK_LIST(X)
#undef X
};

// WP3: hooks whose bodies write a DRAW_INDX/DRAW_INDX_2 packet into the ring/IB (coverage report; verified for
// 82CB52D0 in generated/wwe2k14_recomp.85.cpp: stores 0xC0032201 then 4 dwords, cursor dev+48 = last dword written).
constexpr bool IsDrawEmitter(uint32_t id) {
  return id == H_BeginVertices || id == H_DrawIndexedVertices || id == H_Emit_5ED0_DRAW_INDX ||
         id == H_Emit_49A8_DRAW_UP || id == H_Emit_0810_DRAW_INDX_2 || id == H_Emit_0968_DRAW_INDX_2 ||
         id == H_Emit_7930_DRAW_INDX_2 || id == H_Emit_50E0_DRAW_INDX_2 || id == H_Emit_C2E0_DRAW_INDX_2 ||
         id == H_ResolveWorker || id == H_Clear;
}

bool EnvFlag(const char* name) {
  const char* v = std::getenv(name);
  return v && std::strcmp(v, "1") == 0;
}

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// CPU time of the calling thread (ns). Windows: GetThreadTimes (coarse, ~ms granularity).
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

struct State {
  std::atomic<uint64_t> calls[H_COUNT]{};
  std::atomic<uint64_t> last_report_calls[H_COUNT]{};
  std::atomic<uint32_t> last_r3[H_COUNT]{};
  std::atomic<uint32_t> arg_logged[H_COUNT]{};
  std::atomic<uint32_t> distinct_r3_changes[H_COUNT]{};
  std::atomic<uint64_t> overhead_ns{0};  // time in hook bookkeeping, excluding the original body
  std::atomic<uint32_t> swap_thread{0};
  std::atomic<uint32_t> draw_thread{0};
  std::atomic<uint32_t> thread_counter{0};
  // Frame bookkeeping (touched by the Swap-calling thread only, except reports).
  uint64_t last_swap_wall_ns = 0;
  uint64_t last_swap_cpu_ns = 0;
  uint64_t win_wall_ns = 0;
  uint64_t win_cpu_ns = 0;
  uint64_t win_max_wall_ns = 0;
  uint64_t win_frames = 0;
  uint64_t frame_total = 0;
  uint64_t last_overhead_ns = 0;
  uint32_t report_every = 60;
  bool full_timing = false;           // WWE2K14_D3D_OBSERVE_TIMING=1: time every hooked call (old behaviour)
  // Wait/work split (swap-thread values, written only by the thread that calls Swap).
  uint64_t win_swap_wall_ns = 0;
  uint64_t win_swap_cpu_ns = 0;
  uint64_t win_ring_out_ns = 0;       // ring make-space time outside Swap
  uint64_t win_ring_in_swap_ns = 0;   // ring make-space time inside Swap (already part of swap)
};

State& S() {
  static State s;
  return s;
}

bool Enabled() {
  static const bool on = [] {
    const bool e = EnvFlag("WWE2K14_D3D_OBSERVE");
    if (e) {
      if (const char* n = std::getenv("WWE2K14_D3D_OBSERVE_EVERY")) {
        const int v = std::atoi(n);
        if (v > 0) S().report_every = uint32_t(v);
      }
      S().full_timing = EnvFlag("WWE2K14_D3D_OBSERVE_TIMING");
      REXLOG_INFO("[d3d-obs] observe-only hooks ON ({} hooks, summary every {} frames, timing={})", uint32_t(H_COUNT),
                  S().report_every, S().full_timing ? "full" : "swap+emitters+ringspace");
    }
    return e;
  }();
  return on;
}

uint32_t ThreadOrdinal() {
  thread_local uint32_t id = S().thread_counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return id;
}

void Report(State& s) {
  const uint64_t frames = s.win_frames ? s.win_frames : 1;
  const uint64_t ovh = s.overhead_ns.load(std::memory_order_relaxed);
  // wait = Swap call (includes the vblank/ring waits inside it) + ring make-space time outside Swap.
  const uint64_t wait = s.win_swap_wall_ns + s.win_ring_out_ns;
  const uint64_t wall_minus_wait = s.win_wall_ns > wait ? s.win_wall_ns - wait : 0;
  std::string line;
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "[d3d-obs] frame=%llu win=%llu wall_ms/f=%.3f max_ms=%.3f cpu_ms/f=%.3f work_ms/f=%.3f "
                "swap_ms/f=%.3f swap_cpu_ms/f=%.3f wait_ms/f=%.3f ring_ms/f=%.3f hook_ovh_ms/f=%.4f "
                "swap_thr=%u draw_thr=%u dev=0x%08X |",
                (unsigned long long)s.frame_total, (unsigned long long)s.win_frames,
                double(s.win_wall_ns) / 1e6 / double(frames), double(s.win_max_wall_ns) / 1e6,
                double(s.win_cpu_ns) / 1e6 / double(frames),
                double(wall_minus_wait) / 1e6 / double(frames), double(s.win_swap_wall_ns) / 1e6 / double(frames),
                double(s.win_swap_cpu_ns) / 1e6 / double(frames), double(wait) / 1e6 / double(frames),
                double(s.win_ring_out_ns) / 1e6 / double(frames), double(ovh - s.last_overhead_ns) / 1e6 / double(frames),
                s.swap_thread.load(), s.draw_thread.load(), s.last_r3[H_DrawIndexedVertices].load());
  line = buf;
  for (uint32_t i = 0; i < H_COUNT; ++i) {
    const uint64_t c = s.calls[i].load(std::memory_order_relaxed);
    const uint64_t d = c - s.last_report_calls[i].load(std::memory_order_relaxed);
    s.last_report_calls[i].store(c, std::memory_order_relaxed);
    if (!d) continue;
    std::snprintf(buf, sizeof buf, " %s=%.1f", kHookNames[i], double(d) / double(frames));
    line += buf;
  }
  if (StateUploadOff()) {
    static uint64_t last[3] = {};
    const uint64_t v[3] = {g_skip_reg.load(), g_skip_fetch.load(), g_skip_alu.load()};
    std::snprintf(buf, sizeof buf, " | state_upload_skipped/f reg_groups=%.1f fetch=%.1f alu=%.1f",
                  double(v[0] - last[0]) / double(frames), double(v[1] - last[1]) / double(frames),
                  double(v[2] - last[2]) / double(frames));
    line += buf;
    for (int i = 0; i < 3; ++i) last[i] = v[i];
  }
  s.last_overhead_ns = ovh;
  s.win_wall_ns = s.win_cpu_ns = s.win_max_wall_ns = 0;
  s.win_swap_wall_ns = s.win_swap_cpu_ns = s.win_ring_out_ns = s.win_ring_in_swap_ns = 0;
  s.win_frames = 0;
  REXLOG_INFO("{}", line);
}

// Frame boundary: called at entry of the Swap hook, on the thread that presents.
// In-game FPS counter (wwe2k14_app.h FpsOverlay): game frames (Swap calls) and the longest frame since the last read.
std::atomic<uint64_t> g_game_swaps{0}, g_game_max_gap_ns{0};

namespace {
std::atomic<uint64_t> g_h1_file_reads{0};

void CountH1FileRead() { g_h1_file_reads.fetch_add(1, std::memory_order_relaxed); }
}  // namespace

// Card S3-73 (endless loading screen after character select, seen only on Windows): the game keeps presenting but
// draws almost nothing in 3D. After a 3D scene (> 100 indexed draws in a frame), 45 s of <= 3 indexed draws per frame
// asks the freeze logger for a report (guest thread waits) by creating its WWE2K14_FREEZE_DUMP_NOW file (set in
// Wwe2k14App::OnPostSetup). At most twice. Indexed draws only: the CAW-match loading screen of GitHub #84 (reproduced
// as s52c-off on the UHD 630) issues ~314 2D BeginVertices draws but 1 indexed draw per frame.
void NoteLoadStall(State& s, uint64_t wall) {
  static uint64_t last_draws = 0, quiet_since_ns = 0;
  static bool seen_busy = false;
  static int reports = 0;
  const uint64_t draws = s.calls[H_DrawIndexedVertices].load(std::memory_order_relaxed);
  const uint64_t d = draws - last_draws;
  last_draws = draws;
  if (d > 100) seen_busy = true;
  if (d > 3 || !seen_busy || reports >= 2) {
    quiet_since_ns = 0;
    return;
  }
  if (!quiet_since_ns) quiet_since_ns = wall;
  if (wall - quiet_since_ns < 45ull * 1000000000ull) return;
  quiet_since_ns = 0;
  seen_busy = false;
  ++reports;
  const char* path = std::getenv("WWE2K14_FREEZE_DUMP_NOW");
  REXLOG_WARN("[load-stall] 45 s of <= 3 indexed draws/frame while presenting (frame {}); freeze report {}", s.frame_total,
              path && *path ? "requested" : "unavailable");
  if (path && *path) {
    if (FILE* f = std::fopen(path, "wb")) std::fclose(f);
  }
}

// H1: a low indexed-draw rate is the existing loading-screen proxy. When the optional handle diagnostic is on,
// report after that proxy stays active for 20 s and the guest XFile::Read observer has seen no read for 20 s.
// The report uses the same marker consumed by freeze_logger's per-thread wait/call-chain capture.
void NoteReadQuietLoadStall(State& s, uint64_t wall) {
  static bool observer_enabled = false;
  static bool initialized = false;
  static bool reported_this_quiet_period = false;
  static bool seen_busy = false;
  static uint64_t last_reads = 0;
  static uint64_t last_draws = 0;
  static uint64_t no_read_since = 0;
  static uint64_t low_draw_since = 0;
  static uint32_t reports = 0;

  const bool enabled = REXCVAR_GET(WWE2K14_DIAG_HANDLES);
  if (enabled != observer_enabled) {
    rex::diagnostics::observers::SetFileReadObserver(enabled ? &CountH1FileRead : nullptr);
    observer_enabled = enabled;
    last_reads = g_h1_file_reads.load(std::memory_order_relaxed);
    no_read_since = enabled ? wall : 0;
    low_draw_since = 0;
    last_draws = s.calls[H_DrawIndexedVertices].load(std::memory_order_relaxed);
    seen_busy = false;
    reported_this_quiet_period = false;
    initialized = enabled;
  }
  if (!enabled) return;

  const uint64_t reads = g_h1_file_reads.load(std::memory_order_relaxed);
  if (!initialized) {
    last_reads = reads;
    no_read_since = wall;
    initialized = true;
  } else if (reads != last_reads) {
    last_reads = reads;
    no_read_since = wall;
    reported_this_quiet_period = false;
  }

  const uint64_t draws = s.calls[H_DrawIndexedVertices].load(std::memory_order_relaxed);
  const uint64_t draws_this_frame = draws - last_draws;
  last_draws = draws;
  if (draws_this_frame > 100) seen_busy = true;
  if (draws_this_frame > 3) {
    low_draw_since = 0;
    reported_this_quiet_period = false;
    return;
  }
  if (!seen_busy) return;
  if (!low_draw_since) low_draw_since = wall;
  if (!no_read_since) no_read_since = wall;
  if (reports >= 8 || reported_this_quiet_period ||
      wall - low_draw_since < 20ull * 1000000000ull ||
      wall - no_read_since < 20ull * 1000000000ull) {
    return;
  }

  reported_this_quiet_period = true;
  ++reports;
  seen_busy = false;
  const char* path = std::getenv("WWE2K14_FREEZE_DUMP_NOW");
  bool requested = false;
  if (path && *path) {
    if (FILE* f = std::fopen(path, "wb")) {
      std::fclose(f);
      requested = true;
    }
  }
  REXLOG_WARN("[load-stall] H1 low-draw present with no guest file read for 20 s (frame {}, read_count={}); "
              "freeze report {}",
              s.frame_total, reads, requested ? "requested" : "unavailable");
}

void OnSwap(State& s) {
  const uint64_t wall = NowNs();
  const uint64_t cpu = ThreadCpuNs();
  s.swap_thread.store(ThreadOrdinal(), std::memory_order_relaxed);
  g_game_swaps.fetch_add(1, std::memory_order_relaxed);
  NoteLoadStall(s, wall);
  NoteReadQuietLoadStall(s, wall);
  if (s.last_swap_wall_ns) {
    const uint64_t dw = wall - s.last_swap_wall_ns;
    if (dw > g_game_max_gap_ns.load(std::memory_order_relaxed)) g_game_max_gap_ns.store(dw, std::memory_order_relaxed);
    s.win_wall_ns += dw;
    s.win_cpu_ns += cpu - s.last_swap_cpu_ns;
    if (dw > s.win_max_wall_ns) s.win_max_wall_ns = dw;
    ++s.win_frames;
    ++s.frame_total;
    if (s.win_frames >= s.report_every) Report(s);
  }
  s.last_swap_wall_ns = NowNs();
  s.last_swap_cpu_ns = ThreadCpuNs();
}

// Pre-call bookkeeping: counts are plain load+store (no locked RMW; a lost update is possible only if two threads
// call the same hook at once, which the match data shows does not happen for the hot ones).
inline void Pre(uint32_t id, PPCContext& ctx) {
  State& s = S();
  s.calls[id].store(s.calls[id].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  s.last_r3[id].store(ctx.r3.u32, std::memory_order_relaxed);
  if (id == H_DrawIndexedVertices) {
    thread_local const uint32_t ord = ThreadOrdinal();
    s.draw_thread.store(ord, std::memory_order_relaxed);
  }
  if (id == H_Swap) OnSwap(s);
}

constexpr bool IsTimedAlways(uint32_t id) {
  return id == H_Swap || id == H_RingMakeSpace || id == H_RingNewBuffer || IsDrawEmitter(id);
}

// Per-thread wait tracking for the ring make-space hook (outermost call only) and Swap.
thread_local bool t_in_swap = false;
inline bool OnSwapThread() {
  thread_local const uint32_t ord = ThreadOrdinal();
  return ord == S().swap_thread.load(std::memory_order_relaxed);
}
thread_local uint32_t t_ring_depth = 0;

inline void LogArgs(uint32_t id, PPCContext& ctx) {
  State& s = S();
  const uint32_t n = s.arg_logged[id].fetch_add(1, std::memory_order_relaxed);
  if (n < 20)
    REXLOG_INFO("[d3d-obs] args {} (0x{:08X}) #{} r3=0x{:08X} r4=0x{:08X} r5=0x{:08X} r6=0x{:08X}", kHookNames[id],
                kHookAddrs[id], n + 1, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

}  // namespace

namespace wwe2k14::d3d {
bool ObserveEnabled() { return Enabled(); }
void GameFrameStats(uint64_t* swaps, uint64_t* max_gap_ns) {
  *swaps = g_game_swaps.load(std::memory_order_relaxed);
  *max_gap_ns = g_game_max_gap_ns.exchange(0, std::memory_order_relaxed);
}
}  // namespace wwe2k14::d3d

// N4 step 2a: ShaderConstLoad body - native (WWE2K14_D3D_NATIVE_CONSTLOAD) or the guest body. Used as the identity hook's
// `original` and by HOOK_BODY, so the shader identity bookkeeping around it is unchanged.
extern "C" PPC_FUNC(__imp__sub_82CC1C58);
static void ConstLoadBody(PPCContext& ctx, uint8_t* base) {
  if (!wwe2k14::d3d::native_shader::BodyConstLoad(ctx, base, __imp__sub_82CC1C58,
                                                   uint32_t(g_game_swaps.load(std::memory_order_relaxed))))
    __imp__sub_82CC1C58(ctx, base);
}

// N4 step 2b: inline VS upload body - native tree (WWE2K14_D3D_NATIVE_VSUPLOAD) or the guest body; the identity hook's
// `original`, so its post-hook parse of the uploaded packet is unchanged.
extern "C" PPC_FUNC(__imp__sub_82CC1B00);
static void VSUploadBody(PPCContext& ctx, uint8_t* base) {
  if (!wwe2k14::d3d::native_shader::BodyVSUpload(ctx, base, __imp__sub_82CC1B00,
                                                  uint32_t(g_game_swaps.load(std::memory_order_relaxed))))
    __imp__sub_82CC1B00(ctx, base);
}

// N4 step 2c: shader flush body - native (WWE2K14_D3D_NATIVE_FLUSH) or the guest body; the identity hook's `original`.
extern "C" PPC_FUNC(__imp__sub_82CC1E48);
static void FlushBody(PPCContext& ctx, uint8_t* base) {
  if (!wwe2k14::d3d::native_flush::Body(ctx, base, __imp__sub_82CC1E48,
                                         uint32_t(g_game_swaps.load(std::memory_order_relaxed))))
    __imp__sub_82CC1E48(ctx, base);
}

// N2-RT: the body call of a hook. SetRenderTarget may run the native tree (WWE2K14_D3D_NATIVE_RT); every other hook, and
// SetRenderTarget with the switch off, runs the guest body. The hook's observe/capture bookkeeping is unchanged.
#define HOOK_BODY(name, addr)                                                                                  \
  do {                                                                                                         \
    if (H_##name == H_SetRenderTarget && wwe2k14::d3d::native_rt::Body(ctx, base, __imp__sub_##addr, \
                                                         uint32_t(g_game_swaps.load(std::memory_order_relaxed)))) break; \
    if (H_##name == H_SetDepthStencil && wwe2k14::d3d::native_rt::BodyDs(ctx, base, __imp__sub_##addr, \
                                                         uint32_t(g_game_swaps.load(std::memory_order_relaxed)))) break; \
    if (H_##name == H_Clear && wwe2k14::d3d::native_clear::Body(ctx, base, __imp__sub_##addr, \
                                                         uint32_t(g_game_swaps.load(std::memory_order_relaxed)))) break; \
    if (H_##name == H_ResolveWorker && wwe2k14::d3d::native_resolve::Body(ctx, base, __imp__sub_##addr, \
                                                         uint32_t(g_game_swaps.load(std::memory_order_relaxed)))) break; \
    if (H_##name == H_DrawIndexedVertices && wwe2k14::d3d::native_draw::Body(ctx, base, __imp__sub_##addr, \
                                                         uint32_t(g_game_swaps.load(std::memory_order_relaxed)))) break; \
    if ((H_##name == H_SetPixelShader || H_##name == H_SetVertexShader) &&                                     \
        wwe2k14::d3d::native_shader::Body(ctx, base, __imp__sub_##addr,                                        \
                                          uint32_t(g_game_swaps.load(std::memory_order_relaxed)),              \
                                          H_##name == H_SetVertexShader)) break;                               \
    if (H_##name == H_ShaderConstLoad) { ConstLoadBody(ctx, base); break; }                                    \
    if (H_##name == H_ShaderVSUpload) { VSUploadBody(ctx, base); break; }                                      \
    if (H_##name == H_ShaderFlush) { FlushBody(ctx, base); break; }                                            \
    if (H_##name == H_RegEmitHelper && wwe2k14::d3d::native_shader::BodyRegEmit(ctx, base, __imp__sub_##addr,  \
                                                         uint32_t(g_game_swaps.load(std::memory_order_relaxed)))) break; \
    __imp__sub_##addr(ctx, base);                                                                              \
  } while (0)

// One strong override per hook: observe, then call the unchanged original.
#define X(addr, name, args)                                                  \
  extern "C" PPC_FUNC(__imp__sub_##addr);                                    \
  extern "C" PPC_FUNC(sub_##addr) {                                          \
    /* F2 step 1: mark the always dwords this hook may write (entry + exit, before any fast path returns) */  \
    wwe2k14::d3d::TagReportScope f2_rep_(0x##addr##u, ctx);                  \
    /* crowd decompile C1 (test-only, WWE2K14_D3D_CROWD_CALLERS=1): who issues the crowd's draws */           \
    wwe2k14::d3d::crowd_callers::Scope c1_cc_(H_##name == H_DrawIndexedVertices, ctx, base);              \
    /* N3 CHECK: record the draw body's direct callees (inert outside a CHECK draw) */                         \
    wwe2k14::d3d::native_draw::RecScope n3_rec_((H_##name == H_ShaderFlush || H_##name == H_RegEmitHelper ||    \
                                                 H_##name == H_RingMakeSpace) ? 0x##addr##u : 0u, ctx, base);  \
    /* N4 step 2c CHECK: record the flush's hooked callees (inert outside a CHECK flush) */                    \
    wwe2k14::d3d::native_flush::RecScope n4c_rec_((H_##name == H_ShaderConstLoad || H_##name == H_ShaderVSUpload || \
                                                  H_##name == H_RingMakeSpace) ? 0x##addr##u : 0u, ctx, base); \
    if (H_##name == H_RenderCmdInterp) wwe2k14_pump_guard(base, ctx.r3.u32, ctx.r4.u32); \
    if (H_##name == H_SetTexture) wwe2k14::d3d::G18cSetTexture(ctx, base, uint32_t(g_game_swaps.load(std::memory_order_relaxed))); /* G18 attire trace */ \
    if (H_##name == H_RenderCmdInterp && /* F1 step 2: no hook layer on ~12,500 calls/f */                  \
        wwe2k14::d3d::native_setters::InterpFast(uint32_t(g_game_swaps.load(std::memory_order_relaxed)))) {  \
      wwe2k14_pump_ring_node(base, ctx.r3.u32, ctx.r4.u32);                  \
      __imp__sub_##addr(ctx, base);                                          \
      return;                                                                \
    }                                                                        \
    if (H_##name == H_RegEmitHelper && SkipRegGroupUpload(base, ctx.r3.u32, ctx.r5.u32)) return;  /* card DE-3R */ \
    if ((H_##name == H_SetVSConstantF || H_##name == H_SetPSConstantF) && /* F1 */                  \
        wwe2k14::d3d::native_setters::TryConst(ctx, base, H_##name == H_SetPSConstantF,            \
                                               uint32_t(g_game_swaps.load(std::memory_order_relaxed)), \
                                               __imp__sub_##addr)) return;                        \
    if ((H_##name == H_SetVertexDecl_46C0 || H_##name == H_SetIndices_EE60 || H_##name == H_SetStreamSrc_ECB8 || H_##name == H_SetTexture) && /* F1 steps 3-4 */ \
        wwe2k14::d3d::native_setters::TryState(ctx, base,                    \
            H_##name == H_SetVertexDecl_46C0 ? wwe2k14::d3d::native_setters::StateSetter::kDecl :   \
            H_##name == H_SetIndices_EE60 ? wwe2k14::d3d::native_setters::StateSetter::kIndices :   \
            H_##name == H_SetTexture ? wwe2k14::d3d::native_setters::StateSetter::kTexture :        \
            wwe2k14::d3d::native_setters::StateSetter::kStream,                                     \
            uint32_t(g_game_swaps.load(std::memory_order_relaxed)), __imp__sub_##addr)) return;     \
    if (!Enabled()) {                                                        \
      HOOK_BODY(name, addr);                                                 \
      return;                                                                \
    }                                                                        \
    const bool timed = IsTimedAlways(H_##name) || S().full_timing;           \
    /* C1-T3: path choice unchanged (ring/swap bookkeeping below is functional); only the clock reads are     \
       sampled 1 frame in N, latched once so a Swap scope that crosses the frame flip stays consistent. */   \
    const uint32_t tfr = uint32_t(g_game_swaps.load(std::memory_order_relaxed)); \
    const bool tm = timed && wwe2k14::d3d::stat::On(tfr);                    \
    const uint64_t tsc = wwe2k14::d3d::stat::Scale(tfr);                     \
    const uint64_t t0 = tm ? NowNs() : 0;                                    \
    Pre(H_##name, ctx);                                                      \
    if (args) LogArgs(H_##name, ctx);                                        \
    if (H_##name == H_RenderCmdInterp) wwe2k14_pump_ring_node(base, ctx.r3.u32, ctx.r4.u32); \
    if (H_##name == H_ShaderFlush) { wwe2k14::d3d::ShaderIdentityHookedFlush(ctx, base, FlushBody); return; } \
    if (H_##name == H_ShaderVSUpload) { wwe2k14::d3d::ShaderIdentityHookedVSUpload(ctx, base, VSUploadBody); return; } \
    if (H_##name == H_ShaderConstLoad) { wwe2k14::d3d::ShaderIdentityHookedConstLoad(ctx, base, ConstLoadBody); return; } \
    const uint32_t wp3_dev = ctx.r3.u32;                                     \
    const bool wp9_on = wwe2k14::d3d::ShadowEnabled();                       \
    if (wp9_on) wwe2k14::d3d::ShadowPre(0x##addr, ctx, base);                \
    if (!timed) {                                                            \
      HOOK_BODY(name, addr);                                                 \
      if (H_##name == H_DrawIndexedVertices && wp9_on) wwe2k14::d3d::ShadowIndexedMarker(base); /* card DE-2M */ \
      if (H_##name == H_BeginVertices && wp9_on) wwe2k14::d3d::ShadowUpMarker(base); \
      if (H_##name == H_Emit_7930_DRAW_INDX_2 && wp9_on) wwe2k14::d3d::ShadowEmitMarker(base); \
      if (wp9_on) wwe2k14::d3d::ShadowPost(0x##addr, ctx, base);             \
      return;                                                                \
    }                                                                        \
    const uint64_t t1 = tm ? NowNs() : 0;                                    \
    /* card S3-63: RingNewBuffer (sub_82CB9670) also starts a segment; the ring reserve sub_82CBA9D0 calls it directly */ \
    const bool ring_fn = (H_##name == H_RingMakeSpace) || (H_##name == H_RingNewBuffer); \
    const bool ring_outer = ring_fn && (t_ring_depth++ == 0);                \
    if (ring_outer && wp9_on) wwe2k14::d3d::ShadowRingSwitchPre(base, wp3_dev); \
    const bool swap_outer = (H_##name == H_Swap);                            \
    uint64_t cpu1 = 0;                                                       \
    if (swap_outer) { t_in_swap = true; cpu1 = tm ? ThreadCpuNs() : 0; }    \
    HOOK_BODY(name, addr);                                                   \
    const uint64_t t2 = tm ? NowNs() : 0;                                    \
    if (H_##name == H_DrawIndexedVertices && wp9_on) wwe2k14::d3d::ShadowIndexedMarker(base); /* card DE-2M */ \
      if (H_##name == H_BeginVertices && wp9_on) wwe2k14::d3d::ShadowUpMarker(base); \
      if (H_##name == H_Emit_7930_DRAW_INDX_2 && wp9_on) wwe2k14::d3d::ShadowEmitMarker(base); \
    if (wp9_on) wwe2k14::d3d::ShadowPost(0x##addr, ctx, base);               \
    if (H_##name == H_RingNewBuffer) {                                       \
      --t_ring_depth;                                                        \
      if (ring_outer && wp9_on) wwe2k14::d3d::ShadowRingSwitchPost(base, wp3_dev, true); \
    }                                                                        \
    if (H_##name == H_RingMakeSpace) {                                       \
      --t_ring_depth;                                                        \
      if (ring_outer && wp9_on) wwe2k14::d3d::ShadowNoteRingSwitch();      \
      if (ring_outer && wp9_on) wwe2k14::d3d::ShadowRingSwitchPost(base, wp3_dev, false); \
      if (tm && ring_outer && OnSwapThread()) (t_in_swap ? S().win_ring_in_swap_ns : S().win_ring_out_ns) += (t2 - t1) * tsc; \
    }                                                                        \
    if (swap_outer) {                                                        \
      t_in_swap = false;                                                     \
      if (tm) {                                                              \
        S().win_swap_wall_ns += (t2 - t1) * tsc;                             \
        S().win_swap_cpu_ns += (ThreadCpuNs() - cpu1) * tsc;                 \
      }                                                                      \
    }                                                                        \
    if (tm) S().overhead_ns.fetch_add(((t1 - t0) + (NowNs() - t2)) * tsc, std::memory_order_relaxed); \
  }
D3D_HOOK_LIST(X)
#undef X

// Card DE-3R: the fetch and ALU constant encoders (not observed hooks: off mode is one branch on a static).
extern "C" PPC_FUNC(__imp__sub_82CC0F58);
extern "C" PPC_FUNC(sub_82CC0F58) {
  wwe2k14::d3d::native_draw::RecScope n3_rec_(0x82CC0F58u, ctx, base);  // N3 CHECK recording
  if (StateUploadOff() && !Recording(base, ctx.r3.u32)) {
    g_skip_fetch.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  __imp__sub_82CC0F58(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82CC10B0);
extern "C" PPC_FUNC(sub_82CC10B0) {
  wwe2k14::d3d::native_draw::RecScope n3_rec_(0x82CC10B0u, ctx, base);  // N3 CHECK recording
  if (StateUploadOff() && !Recording(base, ctx.r3.u32)) {
    g_skip_alu.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  __imp__sub_82CC10B0(ctx, base);
}
