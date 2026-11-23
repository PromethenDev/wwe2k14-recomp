// Opt-in guest-memory probe for the R7 Steel-Cage entrance freeze
// (docs/BUGHINT-2K14.md R7; evidence out/r7/R7-HANG-HUNT.md).
//
// The hang watchdog (src/thread_profiler.cpp, WWE13_HANG_DUMP) proves the guest
// vdswap/main thread spins at ~100 % CPU inside sub_82BB9418, the "job/command
// list" pump (0x82BB9418 -> sub_82BB8C00 state machine -> repeat). This file
// adds a strong PPC_FUNC override of that pump which, when the watchdog reports
// a freeze, dumps the queue/command state from the guest thread (where the PPC
// `base` pointer and the guest addresses are valid, so no unmapped read).
//
// Off by default: enabled only with WWE2K14_CAGE_PROBE=1; the dump is appended
// to "$WWE13_HANG_DUMP.probe". Same strong-PPC_FUNC mechanism as
// src/cloth_handshake.cpp and src/present_interval.cpp.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "pump_drain_fixed.h"  // G46 opt-in drain transcription (header-only, host-tested)
#include "d5_reset_probe.h"  // D5 reset-race probe drain flag (src/d5_reset_probe.cpp)
#include "native/d3d/wide_aspect.h"  // W3c game-image aspect patch before the pump drains render commands

namespace {

bool ProbeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("WWE2K14_CAGE_PROBE");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

std::string ProbePath() {
  static const std::string path = [] {
    const char* value = std::getenv("WWE13_HANG_DUMP");
    return std::string(value && *value ? value : "hang-probe.dump") + ".probe";
  }();
  return path;
}

// Diagnostic for docs/cards/D5-pump-delay-test.md (default off): busy-wait around
// the pump to test whether extra vdswap-thread time alone reproduces the pump spin.
// WWE2K14_DIAG_PUMP_DELAY_US=N (0/unset = off), WWE2K14_DIAG_PUMP_DELAY_AT=before|after.
struct PumpDelayConfig {
  uint32_t delay_us = 0;
  bool after = false;
};

const PumpDelayConfig& PumpDelay() {
  static const PumpDelayConfig config = [] {
    PumpDelayConfig c;
    const char* us = std::getenv("WWE2K14_DIAG_PUMP_DELAY_US");
    if (us != nullptr && *us) {
      c.delay_us = static_cast<uint32_t>(std::strtoul(us, nullptr, 10));
    }
    const char* at = std::getenv("WWE2K14_DIAG_PUMP_DELAY_AT");
    c.after = at != nullptr && std::strcmp(at, "after") == 0;
    return c;
  }();
  return config;
}

// Only reached when the delay is on. Spins (no sleep: too coarse) and logs a
// summary about every 10 s of wall time.
void PumpDelaySpin(const PumpDelayConfig& config) {
  using Clock = std::chrono::steady_clock;
  static uint64_t calls = 0;  // pump runs on one guest thread
  static Clock::time_point last_log;
  if (calls == 0) {
    last_log = Clock::now();
    REXLOG_INFO("[diag-pump] delay {} us {} sub_82BB9418 (card D5)", config.delay_us,
                config.after ? "after" : "before");
  }
  ++calls;
  const auto start = Clock::now();
  const auto end = start + std::chrono::microseconds(config.delay_us);
  while (Clock::now() < end) {
  }
  if (start - last_log >= std::chrono::seconds(10)) {
    last_log = start;
    REXLOG_INFO("[diag-pump] calls={} total_delay_ms={}", calls,
                calls * config.delay_us / 1000);
  }
}

std::atomic<uint32_t> g_freeze_generation{0};
uint32_t g_dumped_generation = 0;

// Dumps manager fields / entry list / current command node of the pump. Runs on
// the guest thread inside the overridden pump, so `base` and all guest reads are
// valid. `obj` is the manager pointer the pump was called with (ctx.r3).
void DumpPumpState(uint8_t* base, uint32_t obj) {
  FILE* file = std::fopen(ProbePath().c_str(), "ab");
  if (!file) {
    return;
  }
  std::fprintf(file, "\n==== cage-probe pump state ====\nobj=0x%08X\n", obj);
  if (obj == 0) {
    std::fclose(file);
    return;
  }
  for (uint32_t off = 0; off <= 0x8C; off += 4) {
    const uint32_t value = PPC_LOAD_U32(obj + off);
    std::fprintf(file, "  obj[%02X]=0x%08X (%d)\n", off, value, int32_t(value));
  }
  const uint32_t bank = PPC_LOAD_U32(obj + 0x24);
  const uint32_t current = PPC_LOAD_U32(obj + 0x2C);
  const uint32_t list = PPC_LOAD_U32(obj + 0x6C);
  const uint32_t count = PPC_LOAD_U32(obj + 0x84);
  std::fprintf(file, "bank=%u current=%u list=0x%08X count=%u\n", bank, current, list, count);
  const uint32_t entries = count > 64 ? 64 : count;
  for (uint32_t i = 0; i < entries && list != 0; ++i) {
    const uint32_t entry = list + i * 0x44;
    std::fprintf(file, "  entry[%u]=0x%08X end=%u cur=%u\n", i, entry, PPC_LOAD_U32(entry + 0xC),
                 PPC_LOAD_U32(entry + 0x10));
  }
  if (bank < 0x1000) {
    const uint32_t bank_ptr = PPC_LOAD_U32(obj + 0x30 + 4 * bank);
    std::fprintf(file, "bank_ptr=0x%08X\n", bank_ptr);
    if (bank_ptr != 0) {
      const uint32_t node = bank_ptr + current * 16;
      std::fprintf(file, "node=0x%08X\n", node);
      for (uint32_t j = 0; j < 16; ++j) {
        const uint32_t value = PPC_LOAD_U32(node + j * 4);
        std::fprintf(file, "  node[%02X]=0x%08X (%d)\n", j * 4, value, int32_t(value));
      }
      for (int32_t k = -2; k <= 2; ++k) {
        const uint32_t q = bank_ptr + uint32_t(int32_t(current) + k) * 16;
        std::fprintf(file, "  near[%d]=0x%08X state=%d size=%d\n", k, q, int32_t(PPC_LOAD_U32(q)),
                     int32_t(PPC_LOAD_U32(q + 8)));
      }
    }
  }
  std::fflush(file);
  std::fclose(file);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// D8 (docs/cards/D8-pump-freeze-recorder.md): read-only freeze recorder for the 0x82BB command pump.
// Armed by WWE2K14_DIAG_PUMP_RING=1 (0 = off); unset = on only when WWE2K14_D3D_OBSERVE=1. Needs no D3D hook for the
// per-drain snapshots; the per-node ring is fed by the RenderCmdInterp hook (sub_82BB8C00) in src/native/d3d/hooks.cpp,
// so it exists only in hook runs. Offsets (generated/wwe2k14_recomp.76.cpp; docs/reports/20261014-d2-pump-analysis.md):
//   pump+0x1C capacity, +0x20/+0x24 producer/consumer bank, +0x28 frame, +0x2C cursor (8209 drain: lwz 44(r31)),
//   +0x30+4*bank bank base (drain: (bank+12)*4), +0x3C+4*d saved bank, +0x44+4*d saved cursor, +0x4C depth
//   (sub_82BB78A0/7900: 76(r3)), +0x58 per-bank flags, +0x6C active entry array (108), +0x84 entry count (132);
//   entry stride 0x44, +0x0C end, +0x10 start. Node: word0 opcode, word2 size, drain advance = size+1; op 81 target = node+16.
namespace pumpring {

inline uint32_t L32(const uint8_t* base, uint32_t a) {
  return __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base + a));
}

bool Armed() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_DIAG_PUMP_RING");
    if (v && *v) return std::strcmp(v, "0") != 0;
    const char* o = std::getenv("WWE2K14_D3D_OBSERVE");
    return o && std::strcmp(o, "1") == 0;
  }();
  return on;
}

struct NodeRec {
  uint32_t seq, cursor, node, op, size, depth, bank_ptr, target;  // target = [node+16] for op 81, else 0
};
struct DrainRec {
  uint32_t seq;
  uint64_t t_enter_ns, t_exit_ns;
  uint32_t enter[16];  // pump+0x1C .. +0x58 step 4
  uint32_t count, entries_max_end, nodes;
  uint32_t exit_depth, exit_cursor, exit_bank0, exit_bank1;
};
constexpr uint32_t kNodes = 64, kDrains = 16;
NodeRec g_nodes[kNodes];
DrainRec g_drains[kDrains];
std::atomic<uint32_t> g_node_head{0}, g_drain_head{0}, g_triggers{0};
std::atomic<uint64_t> g_last_trigger_ns{0};
std::atomic<uint32_t> g_pump{0};
std::atomic<const uint8_t*> g_base{nullptr};
thread_local bool t_in_drain = false;
thread_local uint32_t t_zero_run = 0, t_max_end = 0, t_nodes = 0;
thread_local DrainRec* t_cur = nullptr;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Fmt(std::string& out, const char* f, ...) __attribute__((format(printf, 2, 3)));
void Fmt(std::string& out, const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  out += buf;
}

// Full text: pump fields, drain snapshots, node ring (oldest first), node window clamped to the node's 4 KB page.
std::string Format(const uint8_t* base, uint32_t pump, const char* reason, uint32_t focus_node) {
  std::string o;
  Fmt(o, "==== pump ring (D8) reason=%s pump=0x%08X ====\n", reason, pump);
  if (base && pump >= 0x10000) {
    Fmt(o, "pump+0x1C cap=%u +0x20 prod_bank=%u +0x24 cons_bank=%u +0x28 frame=%u +0x2C cursor=%u +0x4C depth=%u +0x84 entries=%u\n",
        L32(base, pump + 0x1C), L32(base, pump + 0x20), L32(base, pump + 0x24), L32(base, pump + 0x28), L32(base, pump + 0x2C),
        L32(base, pump + 0x4C), L32(base, pump + 0x84));
    o += "pump+0x30..0x58:";
    for (uint32_t off = 0x30; off <= 0x58; off += 4) Fmt(o, " [%02X]=%08X", off, L32(base, pump + off));
    o += "\n";
  }
  const uint32_t dh = g_drain_head.load(std::memory_order_relaxed);
  const uint32_t dn = dh < kDrains ? dh : kDrains;
  for (uint32_t i = 0; i < dn; ++i) {
    const DrainRec& d = g_drains[(dh - dn + i) % kDrains];
    Fmt(o, "drain#%u t=%llu..%llu us nodes=%u entries=%u max_end=%u enter{cap=%u pb=%u cb=%u frame=%u cur=%u bank0=%08X bank1=%08X depth=%u} exit{depth=%u cur=%u bank0=%08X bank1=%08X}\n",
        d.seq, (unsigned long long)(d.t_enter_ns / 1000), (unsigned long long)(d.t_exit_ns / 1000), d.nodes, d.count, d.entries_max_end,
        d.enter[0], d.enter[1], d.enter[2], d.enter[3], d.enter[4], d.enter[5], d.enter[6], d.enter[(0x4C - 0x1C) / 4], d.exit_depth,
        d.exit_cursor, d.exit_bank0, d.exit_bank1);
  }
  const uint32_t nh = g_node_head.load(std::memory_order_relaxed);
  const uint32_t nn = nh < kNodes ? nh : kNodes;
  Fmt(o, "node ring (%u of last %u, oldest first): seq cursor node op size depth bank_ptr target\n", nn, kNodes);
  for (uint32_t i = 0; i < nn; ++i) {
    const NodeRec& r = g_nodes[(nh - nn + i) % kNodes];
    Fmt(o, "  #%u cur=%u node=%08X op=%u size=%08X depth=%u bank_ptr=%08X target=%08X\n", r.seq, r.cursor, r.node, r.op, r.size, r.depth,
        r.bank_ptr, r.target);
  }
  if (base && focus_node >= 0x10000) {
    uint32_t lo = focus_node - 8 * 16, hi = focus_node + 9 * 16;  // 8 nodes before, 8 after (inclusive window end)
    const uint32_t page = focus_node & ~0xFFFu;
    if (lo < page) lo = page;
    if (hi > page + 0x1000) hi = page + 0x1000;
    Fmt(o, "node window 0x%08X..0x%08X (clamped to the page of the focus node 0x%08X)\n", lo, hi, focus_node);
    for (uint32_t a = lo; a + 16 <= hi; a += 16)
      Fmt(o, "  %08X: %08X %08X %08X %08X%s\n", a, L32(base, a), L32(base, a + 4), L32(base, a + 8), L32(base, a + 12),
          a == focus_node ? "  <== focus" : "");
  }
  return o;
}

void Trigger(const uint8_t* base, uint32_t pump, const char* reason, uint32_t focus_node) {
  const uint64_t now = NowNs();
  const uint32_t n = g_triggers.load(std::memory_order_relaxed);
  if (n >= 12) return;
  if (n >= 3 && now - g_last_trigger_ns.load(std::memory_order_relaxed) < 5000000000ull) return;
  g_triggers.store(n + 1, std::memory_order_relaxed);
  g_last_trigger_ns.store(now, std::memory_order_relaxed);
  const std::string text = Format(base, pump, reason, focus_node);
  const char* dir = std::getenv("WWE2K14_NR_OUT");
  const std::string path = (dir && *dir ? std::string(dir) + "/" : std::string()) + "d3d-pump-ring.txt";
  if (FILE* f = std::fopen(path.c_str(), "ab")) {
    std::fputs(text.c_str(), f);
    std::fclose(f);
  }
  // console: header + the last lines (the full text is in the file)
  size_t cut = text.size() > 3000 ? text.size() - 3000 : 0;
  REXLOG_WARN("[pump-ring] trigger #{} reason={} pump=0x{:08X} (full dump: {})\n{}", n + 1, reason, pump, path, text.c_str() + cut);
}

void DrainEnter(uint8_t* base, uint32_t pump) {
  g_pump.store(pump, std::memory_order_relaxed);
  g_base.store(base, std::memory_order_relaxed);
  const uint32_t seq = g_drain_head.load(std::memory_order_relaxed);
  DrainRec& d = g_drains[seq % kDrains];
  d = DrainRec{};
  d.seq = seq;
  d.t_enter_ns = NowNs();
  for (uint32_t i = 0; i < 16; ++i) d.enter[i] = L32(base, pump + 0x1C + 4 * i);
  d.count = L32(base, pump + 0x84);
  const uint32_t active = L32(base, pump + 0x6C);
  uint32_t max_end = 0;
  if (active >= 0x10000 && d.count && d.count <= 16)
    for (uint32_t i = 0; i < d.count; ++i) {
      const uint32_t e = L32(base, active + i * 0x44 + 0x0C);
      if (e > max_end) max_end = e;
    }
  d.entries_max_end = max_end;
  t_max_end = max_end;
  t_zero_run = 0;
  t_nodes = 0;
  t_in_drain = true;
  t_cur = &d;
  g_drain_head.store(seq + 1, std::memory_order_release);
  if (d.enter[(0x4C - 0x1C) / 4] != 0) Trigger(base, pump, "depth_nonzero_at_drain_entry", 0);
}

void DrainExit(uint8_t* base, uint32_t pump) {
  t_in_drain = false;
  DrainRec* d = t_cur;
  t_cur = nullptr;
  if (!d) return;
  d->t_exit_ns = NowNs();
  d->nodes = t_nodes;
  d->exit_depth = L32(base, pump + 0x4C);
  d->exit_cursor = L32(base, pump + 0x2C);
  d->exit_bank0 = L32(base, pump + 0x30);
  d->exit_bank1 = L32(base, pump + 0x34);
  if (d->exit_depth != 0) Trigger(base, pump, "depth_nonzero_at_drain_exit", 0);
}

}  // namespace pumpring

// Per-node record, called from the sub_82BB8C00 hook (r3 = pump, r4 = address of the stack slot that holds the node pointer,
// 76:8209 drain: addi r4,r1,80). Only while the drain override is active on this thread.
extern "C" void wwe2k14_pump_ring_node(uint8_t* base, uint32_t pump, uint32_t slot) {
  using namespace pumpring;
  if (!t_in_drain) return;
  const uint32_t node = L32(base, slot);
  if (node < 0x10000) return;
  NodeRec& r = g_nodes[g_node_head.load(std::memory_order_relaxed) % kNodes];
  r.seq = g_node_head.load(std::memory_order_relaxed);
  r.node = node;
  r.op = L32(base, node);  // the drain has already read this word and word2 (76:8272, 8301)
  r.size = L32(base, node + 8);
  r.cursor = L32(base, pump + 0x2C);
  r.depth = L32(base, pump + 0x4C);
  const uint32_t bank = L32(base, pump + 0x24);
  r.bank_ptr = bank < 4 ? L32(base, pump + 0x30 + 4 * bank) : 0;
  r.target = r.op == 81 ? L32(base, node + 16) : 0;
  g_node_head.store(r.seq + 1, std::memory_order_release);
  ++t_nodes;
  t_zero_run = r.op == 0 ? t_zero_run + 1 : 0;
  const char* why = nullptr;
  if (r.op > 105) why = "opcode_gt_105";
  else if (t_zero_run == 16) why = "16_consecutive_zero_opcodes";
  else if (r.depth == 0 && t_max_end && r.cursor > t_max_end) why = "cursor_beyond_entry_end";
  else if (node >= 0x70000000u && node < 0x7F000000u) why = "node_in_stack_range";
  else if (r.size == 0xFFFFFFFFu) why = "node_size_ffffffff";
  if (why) Trigger(base, pump, why, node);
}

// S3-56 (docs/cards/S3-56-pump-spin-guard.md), opt-in WWE2K14_PUMP_GUARD=1: called from the sub_82BB8C00 hook before
// the original. Inside a CALL sub-list (depth pump+0x4C > 0) a run of kGuardRun consecutive no-op-class nodes (op 0 or
// op > 105) means the drain is walking recycled memory past the list's missing RETURN (card D5 pump spin). Then do
// what the game's RETURN (opcode 82, 76:7242) does: sub_82BB7900 pop on this pump (depth--, bank slot := saved bank,
// cursor := saved cursor) and reload the node pointer in the drain's stack slot from the restored bank + cursor*16.
// The caller then dispatches the reloaded node. Returns true when it popped.
namespace pumpguard {
constexpr uint32_t kGuardRun = 4096;
thread_local uint32_t t_noop_run = 0;
std::atomic<uint32_t> g_fired{0};
inline void S32(uint8_t* base, uint32_t a, uint32_t v) {
  *reinterpret_cast<volatile uint32_t*>(base + a) = __builtin_bswap32(v);
}
bool On() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_PUMP_GUARD");
    return v && std::strcmp(v, "1") == 0;
  }();
  return on;
}
}  // namespace pumpguard

extern "C" bool wwe2k14_pump_guard(uint8_t* base, uint32_t pump, uint32_t slot) {
  using namespace pumpguard;
  using pumpring::L32;
  if (!On()) return false;
  const uint32_t depth = L32(base, pump + 0x4C);
  const uint32_t node = L32(base, slot);
  const uint32_t op = node >= 0x10000 ? L32(base, node) : 0;
  if (depth == 0 || depth > 2 || (op != 0 && op <= 105)) {
    t_noop_run = 0;
    return false;
  }
  if (++t_noop_run < kGuardRun) return false;
  t_noop_run = 0;
  const uint32_t bank = L32(base, pump + 0x24);
  if (bank >= 4) return false;
  const uint32_t cursor_before = L32(base, pump + 0x2C);
  const uint32_t list = L32(base, pump + 0x30 + 4 * bank);
  // sub_82BB7900 (pop), same stores in the same order.
  const uint32_t d = depth - 1;
  S32(base, pump + 0x4C, d);
  S32(base, pump + (bank + 12) * 4, L32(base, pump + (d + 15) * 4));
  S32(base, pump + 0x2C, L32(base, pump + (d + 17) * 4));
  // RETURN handler tail (76:7242-7262): node = bank slot + cursor * 16, stored into the drain's slot.
  const uint32_t new_node = L32(base, pump + (bank + 12) * 4) + (L32(base, pump + 0x2C) << 4);
  S32(base, slot, new_node);
  const uint32_t n = g_fired.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 50 || (n % 100) == 0)
    REXLOG_WARN("[pump-guard] #{} forced RETURN: pump={:08X} depth={} list={:08X} cursor={} node={:08X} op={} -> "
                "cursor={} node={:08X} op={}",
                n, pump, depth, list, cursor_before, node, op, L32(base, pump + 0x2C), new_node,
                new_node >= 0x10000 ? L32(base, new_node) : 0);
  return true;
}

// Freeze-report extension (called from src/freeze_guest_state.cpp DumpGuestState on the watchdog thread).
extern "C" void wwe2k14_pump_ring_dump(FILE* file) {
  using namespace pumpring;
  if (!Armed() || !file) return;
  const uint8_t* base = g_base.load(std::memory_order_relaxed);
  const uint32_t pump = g_pump.load(std::memory_order_relaxed);
  const uint32_t nh = g_node_head.load(std::memory_order_relaxed);
  const uint32_t focus = nh ? g_nodes[(nh - 1) % kNodes].node : 0;
  const std::string text = Format(base, pump, "freeze_report", focus);
  std::fputs(text.c_str(), file);
}


// Called by the hang watchdog every 5 s while frozen (weakly linked); request a
// dump on the next pump call. Ignores the FILE* (the probe writes its own file).
extern "C" void wwe2k14_hang_extra_dump(FILE*) {
  if (ProbeEnabled()) {
    g_freeze_generation.fetch_add(1, std::memory_order_relaxed);
  }
}

extern "C" PPC_FUNC(__imp__sub_82BB9418);

// ---------------------------------------------------------------------------------------------------------------------
// G46 (docs/cards/G46-victims-back-crash.md, GitHub #46). Render-command op 43 (generated/wwe2k14_recomp.76.cpp:
// 7307-7328) calls [[obj]+28](obj, [node+24], 0) on a raw object pointer recorded one frame earlier (recorder
// sub_82BB4ED0). Leaving the 30 Years Victims page destroys that object before the drain runs the node: its vptr is
// then the abstract base vtable 0x820699D8 (set by the base dtor sub_82BACE50), whose slot 7 is the CRT _purecall
// sub_82C7D760 (R6025 + abort; abort/_exit return here, leaving abort's frame on the stack -> crash).
// Fix: when _purecall is entered from exactly the op-43 call site (LR 0x82BB8EBC, 76.cpp:7327) return at once, so the
// bind on the destroyed object is skipped and the handler continues at loc_82BB9400 as after a normal bind.
// Any other pure-virtual call runs the game's _purecall unchanged. Reads ctx registers only (no guest memory).
// WWE2K14_PUMP_PURECALL_SKIP: unset/1 = on (default), 0 = off.
namespace g46 {
bool PurecallSkipOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_PUMP_PURECALL_SKIP");
    const bool r = !(v && std::strcmp(v, "0") == 0);
    REXLOG_INFO("[pump-fix] G46 op-43 pure-virtual skip {} (WWE2K14_PUMP_PURECALL_SKIP)", r ? "ON" : "OFF");
    return r;
  }();
  return on;
}
constexpr uint32_t kOp43ReturnSite = 0x82BB8EBC;  // 76.cpp:7327 `ctx.lr = 0x82BB8EBC` before the op-43 bctrl
constexpr bool PurecallSkip(bool on, uint32_t lr) { return on && lr == kOp43ReturnSite; }
static_assert(PurecallSkip(true, 0x82BB8EBC));
static_assert(!PurecallSkip(false, 0x82BB8EBC));
static_assert(!PurecallSkip(true, 0x82BB8EC0));
std::atomic<uint32_t> g_purecall_skipped{0}, g_purecall_other{0};

// Opt-in (WWE2K14_PUMP_DRAIN_FIX=1, default OFF): run the host transcription of the drain (src/pump_drain_fixed.h),
// which restores r1/r14..r31 after a callee that breaks the ABI and replaces a null reloaded node by the drain's
// invariant node. Diagnostic/safety net only; it did not fire with the skip on (w46c2 runs).
bool DrainFixOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_PUMP_DRAIN_FIX");
    return v && std::strcmp(v, "1") == 0;
  }();
  return on;
}
bool DrainCheckOn() {
  static const bool on = [] {
    const char* v = std::getenv("WWE2K14_PUMP_DRAIN_CHECK");
    return v && std::strcmp(v, "1") == 0;
  }();
  return on;
}
std::atomic<uint32_t> g_drift{0}, g_null{0}, g_mismatch{0};
inline bool Rate(uint32_t n) { return n <= 20 || (n % 1000) == 0; }
}  // namespace g46

extern "C" PPC_FUNC(sub_82BB8C00);  // the strong RenderCmdInterp hook (src/native/d3d/hooks.cpp)
extern "C" PPC_FUNC(__savegprlr_29);
extern "C" PPC_FUNC(__restgprlr_29);
namespace g46 {
struct ProdHooks {
  void SaveGpr(PPCContext& ctx, uint8_t* base) { __savegprlr_29(ctx, base); }
  void RestGpr(PPCContext& ctx, uint8_t* base) { __restgprlr_29(ctx, base); }
  void Interp(PPCContext& ctx, uint8_t* base) { sub_82BB8C00(ctx, base); }
  void Drift(PPCContext& ctx, uint8_t*, uint32_t, uint32_t, uint64_t frame_r1, const NonVol&, uint32_t nv) {
    const uint32_t n = g_drift.fetch_add(1, std::memory_order_relaxed) + 1;
    if (Rate(n))
      REXLOG_WARN("[pump-fix] #{} ABI drift after RenderCmdInterp: r1 {:08X} -> {:08X} nonvol_mask={:05X} lr={:08X} -> restored",
                  n, uint32_t(frame_r1), ctx.r1.u32, nv, uint32_t(ctx.lr));
  }
  void Null(PPCContext&, uint8_t*, uint32_t, uint32_t slot, uint32_t reload, uint32_t inv, bool ok) {
    const uint32_t n = g_null.fetch_add(1, std::memory_order_relaxed) + 1;
    if (Rate(n))
      REXLOG_WARN("[pump-fix] #{} null node after RenderCmdInterp: slot={:08X} [slot]={:08X} invariant={:08X} -> {}", n,
                  slot, reload, inv, ok ? "use invariant node" : "end drain");
  }
  bool Check() { return DrainCheckOn(); }
  void Mismatch(uint8_t*, uint32_t, uint32_t value, uint32_t inv) {
    const uint32_t n = g_mismatch.fetch_add(1, std::memory_order_relaxed) + 1;
    if (Rate(n)) REXLOG_WARN("[pump-fix] CHECK #{} slot/invariant mismatch: slot={:08X} invariant={:08X}", n, value, inv);
  }
};
}  // namespace g46

extern "C" PPC_FUNC(__imp__sub_82C7D760);
extern "C" PPC_FUNC(sub_82C7D760) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (g46::PurecallSkip(g46::PurecallSkipOn(), lr)) {
    const uint32_t n = g46::g_purecall_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g46::Rate(n))
      REXLOG_WARN("[pump-fix] #{} op-43 pure virtual call skipped: lr={:08X} obj={:08X} (card G46)", n, lr, ctx.r3.u32);
    return;  // like the bound method returning: the op-43 handler continues at loc_82BB9400
  }
  const uint32_t n = g46::g_purecall_other.fetch_add(1, std::memory_order_relaxed) + 1;
  if (g46::Rate(n))
    REXLOG_WARN("[pump-fix] #{} pure virtual call (not op 43): lr={:08X} obj={:08X} -> game _purecall", n, lr, ctx.r3.u32);
  __imp__sub_82C7D760(ctx, base);
}

// Always-on pump capture for the freeze logger (src/freeze_guest_state.cpp).
// This is the single sub_82BB9418 override; the store is two relaxed atomics.
extern "C" void wwe2k14_freeze_record_pump(uint8_t* base, uint32_t object);

extern "C" PPC_FUNC(sub_82BB9418) {
  wwe2k14::d3d::wide_aspect::ApplyGuestImagePatch(base);
  wwe2k14_freeze_record_pump(base, ctx.r3.u32);
  const bool ring = pumpring::Armed();
  if (ring) pumpring::DrainEnter(base, ctx.r3.u32);
  if (ProbeEnabled()) {
    const uint32_t generation = g_freeze_generation.load(std::memory_order_relaxed);
    if (generation != 0 && generation != g_dumped_generation) {
      g_dumped_generation = generation;
      DumpPumpState(base, ctx.r3.u32);
    }
  }
  const PumpDelayConfig& delay = PumpDelay();
  if (delay.delay_us != 0 && !delay.after) PumpDelaySpin(delay);
  // D5 reset-race probe (src/d5_reset_probe.cpp): mark the drain window for the reset-path
  // overrides. Gate read once; off = no stores. `prev` preserves nesting (restores 0 normally).
  const bool d5 = d5probe::Enabled();
  int d5_prev_active = 0;
  if (d5) {
    d5_prev_active = g_d5_drain_active.load(std::memory_order_relaxed);
    g_d5_drain_tid.store(d5probe::HostThreadId(), std::memory_order_relaxed);
    g_d5_drain_active.store(1, std::memory_order_release);
  }
  const uint32_t pump_obj = ctx.r3.u32;
  if (g46::DrainFixOn()) {  // G46 opt-in, default off
    g46::ProdHooks hooks;
    g46::DrainFixed(ctx, base, hooks);
  } else {
    __imp__sub_82BB9418(ctx, base);
  }
  if (d5) {
    g_d5_drain_active.store(d5_prev_active, std::memory_order_release);
    g_d5_drain_count.fetch_add(1, std::memory_order_relaxed);
    d5probe::MaybeLogSummary();
  }
  if (ring) pumpring::DrainExit(base, pump_obj);
  if (delay.delay_us != 0 && delay.after) PumpDelaySpin(delay);
}
