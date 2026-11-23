// See crowd_callers.h (crowd decompile step C1, test-only).
#include "crowd_callers.h"

#ifdef WWE2K14_NO_TEST_TOOLS
namespace wwe2k14::d3d::crowd_callers {
bool On() { return false; }
void Pre(const PPCContext&, const uint8_t*) {}
void Post() {}
}  // namespace wwe2k14::d3d::crowd_callers
#else

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

#include "shader_identity.h"
#include "shadow_types.h"

namespace wwe2k14::d3d::crowd_callers {
namespace {

constexpr uint64_t kCrowdVs = 0xEB1F2E0E8FA90E1Full;  // perspective crowd (crowd_thin.h)
constexpr int kDepth = 6;
using Chain = std::array<uint32_t, kDepth>;

struct ChainHash {
  size_t operator()(const Chain& c) const {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t v : c) h = (h ^ v) * 1099511628211ull;
    return size_t(h);
  }
};
struct Entry {
  uint64_t n = 0;
  uint64_t by_class[4] = {};  // r6 < 300, < 1000, < 2000, other
  uint32_t r4 = 0, r5 = 0, r6 = 0, r7 = 0;  // first sample's arguments
};

std::mutex g_mu;
std::unordered_map<Chain, Entry, ChainHash> g_chains;
uint64_t g_draws = 0, g_crowd = 0, g_unavail = 0;
std::chrono::steady_clock::time_point g_last = std::chrono::steady_clock::now();

thread_local Chain t_chain{};
thread_local uint32_t t_args[4] = {};
thread_local bool t_valid = false;

inline uint32_t Be32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

void LogLocked() {
  std::vector<std::pair<const Chain*, const Entry*>> v;
  v.reserve(g_chains.size());
  for (const auto& kv : g_chains) v.push_back({&kv.first, &kv.second});
  std::sort(v.begin(), v.end(), [](auto a, auto b) { return a.second->n > b.second->n; });
  REXLOG_INFO("[crowd-callers] draws={} crowd_eb1f={} key_unavailable={} distinct_chains={}", g_draws, g_crowd, g_unavail,
              g_chains.size());
  for (size_t i = 0; i < v.size() && i < 8; ++i) {
    const Chain& c = *v[i].first;
    const Entry& e = *v[i].second;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%08X<%08X<%08X<%08X<%08X<%08X", c[0], c[1], c[2], c[3], c[4], c[5]);
    REXLOG_INFO("[crowd-callers] #{} n={} r6<300={} <1000={} <2000={} other={} chain={} args r4={:#x} r5={:#x} r6={:#x} r7={:#x}",
                i + 1, e.n, e.by_class[0], e.by_class[1], e.by_class[2], e.by_class[3], buf, e.r4, e.r5, e.r6, e.r7);
  }
}

}  // namespace

bool On() {
  static const bool on = [] {
    const char* e = std::getenv("WWE2K14_D3D_CROWD_CALLERS");
    const bool v = e && std::strcmp(e, "1") == 0;
    if (v) REXLOG_INFO("[crowd-callers] ON (test-only crowd call-chain census, VS {:016X})", kCrowdVs);
    return v;
  }();
  return on;
}

void Pre(const PPCContext& ctx, const uint8_t* base) {
  Chain c{};
  c[0] = uint32_t(ctx.lr);
  uint32_t sp = ctx.r1.u32;
  for (int k = 1; k < kDepth; ++k) {
    if (sp < 0x10000 || (sp & 3)) break;
    const uint32_t back = Be32(base, sp);
    if (back <= sp || back - sp > 0x100000) break;  // back chain must climb the stack
    c[k] = Be32(base, back - 8);                     // the caller saved its LR at entry_sp - 8 (mflr r12; stw r12,-8(r1))
    sp = back;
  }
  t_chain = c;
  t_args[0] = ctx.r4.u32;
  t_args[1] = ctx.r5.u32;
  t_args[2] = ctx.r6.u32;
  t_args[3] = ctx.r7.u32;
  t_valid = true;
}

void Post() {
  if (!t_valid) return;
  t_valid = false;
  ShaderKey k;
  const bool have = CurrentShaderKey(0, &k);
  std::lock_guard<std::mutex> lk(g_mu);
  ++g_draws;
  if (!have) {
    ++g_unavail;
  } else if (k.fnv == kCrowdVs) {
    ++g_crowd;
    Entry& e = g_chains[t_chain];
    if (e.n++ == 0) std::memcpy(&e.r4, t_args, sizeof t_args);
    const uint32_t r6 = t_args[2];
    ++e.by_class[r6 < 300 ? 0 : r6 < 1000 ? 1 : r6 < 2000 ? 2 : 3];
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last > std::chrono::seconds(20)) {
    g_last = now;
    LogLocked();
  }
}

}  // namespace wwe2k14::d3d::crowd_callers
#endif
