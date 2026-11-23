// P25-VB17: TEST-ONLY, read-only diagnostic for the p25 RUN16 crash (docs/tasks/P25-VB17.md).
// Built only with WWE2K14_TEST_TOOLS and inert unless WWE2K14_TEST_P25_MODEL_ALLOC=1. Every override calls the original
// body exactly once with the same context and changes no result.
//
// RUN16 evidence (out/test-runs/p25-run16/run/nr-out/d3d-crash.txt): SIGSEGV si_addr=0x100000018 in sub_82CB18A8
// (lwz r7,24(r3), r3 = 0) <- sub_82BD8E68. Verified code (generated/wwe2k14_recomp.77.cpp:15616-15805):
//   sub_82BD8E68(r3 = model, r4 = mesh index) computes the index count of mesh r4 (sum of its strip counts + 1 per join),
//   calls sub_82CB0750(bytes = count*2, 8, 1, 0) (index-buffer create: 32-byte header + data, both via the pooled
//   allocator sub_8225FBA0; returns 0 if either fails, .85.cpp:3975-4030), stores the result at model+344[mesh] WITHOUT a
//   check, then locks it with sub_82CB18A8 -> null dereference when the create failed.
// This file logs: each sub_82BD8E68 call (model, mesh, IB request, result), each failed sub_8225FBA0 call made from inside
// an IB create (size, tag, caller), and failed vertex-buffer creates (sub_82CB0678) for comparison.
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "p25_model_alloc_core.h"

extern "C" {
PPC_FUNC(__imp__sub_82BD8E68);
PPC_FUNC(sub_82BD8E68);
PPC_FUNC(__imp__sub_82CB0750);
PPC_FUNC(sub_82CB0750);
PPC_FUNC(__imp__sub_82CB0678);
PPC_FUNC(sub_82CB0678);
PPC_FUNC(__imp__sub_8225FBA0);
PPC_FUNC(sub_8225FBA0);
}

namespace {
using namespace wwe2k14::p25diag;

constexpr uint32_t kCallerIbCreate = 0x82BD8F88u;  // ctx.lr before sub_82CB0750 in sub_82BD8E68 (.77.cpp:15780, sub_82BD8E68 +165)

bool On() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_TEST_P25_MODEL_ALLOC");
    return e && e[0] == '1' && e[1] == '\0';
  }();
  return on;
}

thread_local bool t_in_builder = false;
thread_local uint32_t t_model = 0, t_mesh = 0;
thread_local bool t_in_create = false;
thread_local uint32_t t_ib_bytes = 0, t_ib_result = 0;
thread_local bool t_ib_seen = false;

std::atomic<uint32_t> g_builder_lines{0};
std::atomic<uint32_t> g_fail_lines{0};
std::atomic<uint64_t> g_ib_bytes_ok{0};
std::atomic<uint32_t> g_ib_ok{0};
constexpr uint32_t kBuilderCap = 600;
constexpr uint32_t kFailCap = 64;

}  // namespace

extern "C" PPC_FUNC(sub_82BD8E68) {
  if (!On()) {
    __imp__sub_82BD8E68(ctx, base);
    return;
  }
  const uint32_t model = ctx.r3.u32, mesh = ctx.r4.u32;
  const bool outer = !t_in_builder;
  const uint32_t pm = t_model, pmesh = t_mesh;
  t_in_builder = true;
  t_model = model;
  t_mesh = mesh;
  t_ib_seen = false;
  // A failed create makes the original crash inside this call; log the request before it can, from the create hook.
  __imp__sub_82BD8E68(ctx, base);
  if (t_ib_seen && g_builder_lines.fetch_add(1, std::memory_order_relaxed) < kBuilderCap) {
    REXLOG_INFO("[p25-ib] build model={:08X} mesh={} ib_bytes={} result={:08X} ok_total={} ok_bytes_total={}", model,
                mesh, t_ib_bytes, t_ib_result, g_ib_ok.load(), g_ib_bytes_ok.load());
  }
  t_model = pm;
  t_mesh = pmesh;
  if (outer) t_in_builder = false;
}

extern "C" PPC_FUNC(sub_82CB0750) {
  if (!On()) {
    __imp__sub_82CB0750(ctx, base);
    return;
  }
  const SavedArgs s = SaveArgs(ctx);
  const bool mine = t_in_builder && uint32_t(s.lr) == kCallerIbCreate;
  const bool prev = t_in_create;
  t_in_create = true;
  __imp__sub_82CB0750(ctx, base);
  t_in_create = prev;
  const uint32_t result = ctx.r3.u32;
  if (mine) {
    t_ib_seen = true;
    t_ib_bytes = s.r3;
    t_ib_result = result;
  }
  if (result != 0) {
    g_ib_ok.fetch_add(1, std::memory_order_relaxed);
    g_ib_bytes_ok.fetch_add(s.r3, std::memory_order_relaxed);
  } else if (g_fail_lines.fetch_add(1, std::memory_order_relaxed) < kFailCap) {
    REXLOG_INFO("[p25-ib] IB CREATE FAILED bytes={} r4={:08X} r5={:08X} lr={:08X} from_builder={} model={:08X} mesh={} "
                "ok_before={} ok_bytes_before={}",
                s.r3, s.r4, s.r5, uint32_t(s.lr), mine, t_model, t_mesh, g_ib_ok.load(), g_ib_bytes_ok.load());
    rex::FlushLogging();
  }
}

extern "C" PPC_FUNC(sub_82CB0678) {
  if (!On()) {
    __imp__sub_82CB0678(ctx, base);
    return;
  }
  const SavedArgs s = SaveArgs(ctx);
  const bool prev = t_in_create;
  t_in_create = true;
  __imp__sub_82CB0678(ctx, base);
  t_in_create = prev;
  if (ctx.r3.u32 == 0 && g_fail_lines.fetch_add(1, std::memory_order_relaxed) < kFailCap) {
    REXLOG_INFO("[p25-ib] VB CREATE FAILED bytes={} r4={:08X} r5={:08X} lr={:08X} in_builder={} model={:08X} mesh={}", s.r3,
                s.r4, s.r5, uint32_t(s.lr), t_in_builder, t_model, t_mesh);
    rex::FlushLogging();
  }
}

// Pooled allocator: log only failures made from inside an IB/VB create (header or data), so boot probes are excluded.
extern "C" PPC_FUNC(sub_8225FBA0) {
  if (!On() || !t_in_create) {
    __imp__sub_8225FBA0(ctx, base);
    return;
  }
  const SavedArgs s = SaveArgs(ctx);
  __imp__sub_8225FBA0(ctx, base);
  if (ctx.r3.u32 == 0 && g_fail_lines.fetch_add(1, std::memory_order_relaxed) < kFailCap) {
    REXLOG_INFO("[p25-ib] pool alloc FAILED size={} tag={:08X} pool={} lr={:08X} model={:08X} mesh={}", s.r3, s.r4,
                (s.r4 >> 16) & 0xFFu, uint32_t(s.lr), t_model, t_mesh);
    rex::FlushLogging();
  }
}
