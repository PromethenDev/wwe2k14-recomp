// F2 step 1 host test: the residual + writer-reported always dwords cover cap::kTagAlways exactly (no gap, no overlap),
// ForEachReported walks exactly the set bits, and each mapped hook marks the dwords documented in tag_report.h.
#include <cstdio>
#include <random>
#include <set>
#include <vector>

#include "native/d3d/tag_report.h"

namespace wwe2k14::d3d {
bool TagReportOn() { return true; }
void TagReportMark(uint64_t, uint64_t) {}
}  // namespace wwe2k14::d3d
using namespace wwe2k14::d3d;

static int fails = 0;
#define EXPECT(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::set<uint32_t> Walk(uint64_t lo, uint64_t hi) {
  std::set<uint32_t> s;
  cap::ForEachReported(lo, hi, [&](uint32_t off, uint32_t n) { for (uint32_t i = 0; i < n; ++i) EXPECT(s.insert(off + i).second, "dup %u", off + i); });
  return s;
}
static uint32_t BitDw(uint32_t b) {
  for (const auto& r : cap::kRepRuns) if (b >= r.bit && b < r.bit + r.n) return r.dw + (b - r.bit);
  return ~0u;
}
static std::set<uint32_t> HookDw(uint32_t addr, uint32_t r4) {
  PPCContext ctx{};
  ctx.r4.u64 = r4;
  const TagReportBits b = TagReportFor(addr, ctx);
  return Walk(b.lo, b.hi);
}
static std::set<uint32_t> Bytes(std::initializer_list<std::pair<uint32_t, uint32_t>> runs) {  // (byte off, dword count)
  std::set<uint32_t> s;
  for (auto [o, n] : runs) for (uint32_t i = 0; i < n; ++i) s.insert(o / 4 + i);
  return s;
}

int main() {
  // 1. coverage
  std::set<uint32_t> always, residual, reported;
  for (const auto& s : cap::kTagAlways) for (uint32_t i = 0; i < s.n; ++i) always.insert(s.off + i);
  for (const auto& s : cap::kTagResidual) for (uint32_t i = 0; i < s.n; ++i) EXPECT(residual.insert(s.off + i).second, "residual dup");
  uint32_t bits = 0;
  for (const auto& r : cap::kRepRuns) { EXPECT(r.bit == bits, "run bit %u expected %u", r.bit, bits); bits += r.n; }
  EXPECT(bits == cap::kRepBits, "kRepBits %u vs %u", cap::kRepBits, bits);
  for (uint32_t b = 0; b < cap::kRepBits; ++b) EXPECT(reported.insert(BitDw(b)).second, "reported dup bit %u", b);
  for (uint32_t d : residual) EXPECT(!reported.count(d), "overlap dw %u", d);
  std::set<uint32_t> uni = residual;
  uni.insert(reported.begin(), reported.end());
  EXPECT(uni == always, "union %zu != always %zu", uni.size(), always.size());
  EXPECT(always.size() == 76 && residual.size() == 7 && reported.size() == 69, "sizes %zu %zu %zu", always.size(), residual.size(), reported.size());
  // 2. ForEachReported == set bits, random masks
  std::mt19937_64 rng(1119);
  for (int t = 0; t < 200000; ++t) {
    uint64_t lo = rng() & rng(), hi = (rng() & rng()) & ((1ull << (cap::kRepBits - 64)) - 1);
    if (t % 3 == 0) lo = rng(), hi = rng() & 31;
    std::set<uint32_t> want;
    for (uint32_t b = 0; b < cap::kRepBits; ++b) if (b < 64 ? (lo >> b) & 1 : (hi >> (b - 64)) & 1) want.insert(BitDw(b));
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    cap::ForEachReported(lo, hi, [&](uint32_t o, uint32_t n) { runs.push_back({o, n}); });
    std::set<uint32_t> got;
    for (auto [o, n] : runs) for (uint32_t i = 0; i < n; ++i) got.insert(o + i);
    EXPECT(got == want, "walk mismatch t=%d", t);
    if (fails > 20) break;
  }
  // 3. hook map vs documented device byte offsets
  for (uint32_t s = 0; s < 26; ++s) EXPECT(HookDw(0x82CAC430, s) == Bytes({{12920 + 4 * s, 1}}), "SetTexture stage %u", s);
  EXPECT(HookDw(0x82CAC430, 40) == Bytes({{12920, 26}}), "SetTexture bad stage");
  for (uint32_t s = 0; s < 16; ++s)
    EXPECT(HookDw(0x82CAECB8, s) == Bytes({{12836 + 4 * s, 1}, {12904 + (s & ~3u), 1}}), "SetStreamSource %u", s);
  EXPECT(HookDw(0x82CAEE60, 0) == Bytes({{12812, 1}}), "SetIndices");
  EXPECT(HookDw(0x82CB42A0, 0) == Bytes({{13068, 1}}), "SetPixelShader");
  EXPECT(HookDw(0x82CB44A8, 0) == Bytes({{13072, 1}}), "SetVertexShader");
  EXPECT(HookDw(0x82CB4E10, 0) == Bytes({{12904, 4}}), "BeginVertices");
  EXPECT(HookDw(0x82CC1E48, 0) == Bytes({{10528, 3}}), "ShaderFlush");
  for (uint32_t a : {0x82CAF5E8u, 0x82CAF978u, 0x82CAFC50u, 0x82CB8BA8u}) EXPECT(HookDw(a, 0) == Bytes({{12816, 5}}), "RT/DS %08X", a);
  for (uint32_t a : {0x82CB2320u, 0x82CB3478u}) EXPECT(HookDw(a, 0) == Bytes({{10776, 12}, {12816, 5}}), "resolve %08X", a);
  EXPECT(HookDw(0x82CB52D0, 0).empty() && HookDw(0x82BB8C00, 0).empty(), "unmapped hooks mark nothing");
  std::printf("%s: %d failures\n", fails ? "FAIL" : "PASS", fails);
  return fails ? 1 : 0;
}
