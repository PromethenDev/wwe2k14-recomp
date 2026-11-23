// T2 sampled, bounded diagnostics for suspicious D3D-fed draws.
#include "glitch_detectors.h"

// WWE2K14_NO_TEST_TOOLS (public release builds, owner 2026-10-26): the stubs below, no detector code.
#if defined(WWE2K14_HAS_NR) && !defined(WWE2K14_NO_TEST_TOOLS)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

#include <rex/logging.h>

#include "glitch_detectors_core.h"

namespace wwe2k14::d3d::glitch {
namespace {

constexpr uint8_t kMissingShader = 1;
constexpr uint8_t kNonFiniteConstant = 2;
constexpr uint8_t kUnwrittenProbe = 3;
constexpr uint8_t kUnwrittenTexture = 4;
constexpr uint8_t kDimensionMismatch = 5;
constexpr uint8_t kFormatMismatch = 6;
constexpr uint8_t kStaleFallback = 7;
constexpr size_t kMaxLoggedKeys = 4096;
constexpr size_t kMaxTextureProbes = 16384;

std::mutex g_logged_keys_mutex;
std::mutex g_texture_probe_keys_mutex;
bool g_logged_keys_cap_reported = false;
bool g_texture_probe_keys_cap_reported = false;

thread_local DrawContext g_draw;
thread_local uint32_t g_last_frame = std::numeric_limits<uint32_t>::max();
thread_local uint32_t g_frame_draws = 0;

core::UniqueKeys& LoggedKeys() {
  static core::UniqueKeys* keys = new core::UniqueKeys(kMaxLoggedKeys);
  return *keys;
}

core::UniqueKeys& TextureProbeKeys() {
  static core::UniqueKeys* keys = new core::UniqueKeys(kMaxTextureProbes);
  return *keys;
}

bool ClaimLog(const DrawContext& draw, const core::UniqueKey& key) {
  if (!draw.sampled) return false;
  bool inserted = false;
  bool report_cap = false;
  {
    std::lock_guard<std::mutex> lock(g_logged_keys_mutex);
    core::UniqueKeys& keys = LoggedKeys();
    inserted = keys.Insert(key);
    if (inserted && keys.size() == kMaxLoggedKeys && !g_logged_keys_cap_reported) {
      g_logged_keys_cap_reported = true;
      report_cap = true;
    }
  }
  if (report_cap) REXLOG_WARN("[glitch] cap reached logged_keys");
  return inserted;
}

core::UniqueKey Key(uint8_t kind, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0) {
  return {kind, a, b, c, d};
}

const char* StageName(uint8_t stage) { return stage == 0 ? "vs" : "ps"; }

const char* FloatName(uint32_t bits) {
  return (bits & 0x007FFFFFu) ? "nan" : ((bits & 0x80000000u) ? "-inf" : "+inf");
}

}  // namespace

uint32_t SampleInterval() {
  static const uint32_t interval = [] {
    const char* value = std::getenv("WWE2K14_GLITCH_CHECK");
    return core::ParseFrameInterval(value);
  }();
  return interval;
}

void BeginDraw(uint32_t frame, uint32_t event_seq) {
  if (frame != g_last_frame) {
    g_last_frame = frame;
    g_frame_draws = 0;
  }
  g_draw = DrawContext{};
  g_draw.frame = frame;
  g_draw.draw = ++g_frame_draws;
  g_draw.event_seq = event_seq;
  g_draw.sampled = core::IsSampleFrame(frame, SampleInterval());
}

void SetShaderKeys(uint64_t vs_key, uint64_t ps_key) {
  g_draw.vs_key = vs_key;
  g_draw.ps_key = ps_key;
}

const DrawContext& CurrentDraw() { return g_draw; }

void MissingShader(const DrawContext& draw, uint8_t stage, uint64_t shader_key, const char* reason,
                   uint64_t ucode_key, uint32_t dwords, const char* dump) {
  if (!draw.sampled) return;
  const uint64_t reason_tag = reason && std::strcmp(reason, "missing_contract") == 0 ? 1 :
                              reason && std::strcmp(reason, "microcode_missing") == 0 ? 2 :
                              reason && std::strcmp(reason, "shader_set_not_loaded") == 0 ? 3 : 4;
  if (!ClaimLog(draw, Key(kMissingShader, stage, shader_key, reason_tag))) return;
  REXLOG_WARN("[glitch] class=missing_shader frame={} draw={} event_seq={} vs={:016X} ps={:016X} scene=unknown \
stage={} shader={:016X} reason={} ucode={:016X} dwords={} dump={}",
              draw.frame, draw.draw, draw.event_seq, draw.vs_key, draw.ps_key, StageName(stage), shader_key,
              reason ? reason : "unknown", ucode_key, dwords, dump ? dump : "none");
}

void CheckConstants(const DrawContext& draw, const uint8_t* vs_constants, const uint8_t* ps_constants,
                    const uint64_t* vs_constant_usage, const uint64_t* ps_constant_usage, size_t bytes_per_stage) {
  if (!draw.sampled) return;
  const uint8_t* banks[2] = {vs_constants, ps_constants};
  const uint64_t* usage[2] = {vs_constant_usage, ps_constant_usage};
  for (uint8_t stage = 0; stage < 2; ++stage) {
    if (!banks[stage] || !usage[stage]) continue;
    core::ForEachNonFiniteUsedFloatConstant(
        banks[stage], bytes_per_stage, usage[stage], [&](uint32_t reg, uint32_t component, uint32_t bits) {
      if (ClaimLog(draw, core::NonFiniteConstantKey(kNonFiniteConstant, draw.vs_key, draw.ps_key, stage, reg))) {
        static constexpr char kComponent[] = "xyzw";
        REXLOG_WARN("[glitch] class=nonfinite_constant frame={} draw={} event_seq={} vs={:016X} ps={:016X} scene=unknown \
stage={} shader={:016X} c{}.{:c} bits=0x{:08X} value={}",
                    draw.frame, draw.draw, draw.event_seq, draw.vs_key, draw.ps_key, StageName(stage),
                    stage == 0 ? draw.vs_key : draw.ps_key,
                    reg, kComponent[component], bits, FloatName(bits));
      }
    });
  }
}

bool ClaimUnwrittenTextureProbe(const DrawContext& draw, uint32_t base, uint32_t mip_base, uint32_t mip_bytes,
                                uint32_t format, uint32_t width, uint32_t height, uint32_t dimension) {
  if (!draw.sampled) return false;
  bool inserted = false;
  bool report_cap = false;
  {
    std::lock_guard<std::mutex> lock(g_texture_probe_keys_mutex);
    core::UniqueKeys& keys = TextureProbeKeys();
    inserted = keys.Insert(
        Key(kUnwrittenProbe, base, (uint64_t(format) << 32) | mip_base, (uint64_t(width) << 32) | height,
            (uint64_t(dimension) << 32) | mip_bytes));
    if (inserted && keys.size() == kMaxTextureProbes && !g_texture_probe_keys_cap_reported) {
      g_texture_probe_keys_cap_reported = true;
      report_cap = true;
    }
  }
  if (report_cap) REXLOG_WARN("[glitch] cap reached texture_probe_keys");
  return inserted;
}

void UnwrittenTextureCandidate(const DrawContext& draw, uint32_t slot, uint32_t base, uint32_t bytes,
                               uint32_t mip_base, uint32_t mip_bytes, uint32_t format, uint32_t width,
                               uint32_t height, uint32_t dimension, uint32_t zero_dwords,
                               uint32_t sampled_dwords) {
  if (!ClaimLog(draw, Key(kUnwrittenTexture, base, (uint64_t(format) << 32) | mip_base,
                          (uint64_t(width) << 32) | height, (uint64_t(dimension) << 32) | mip_bytes)))
    return;
  REXLOG_WARN("[glitch] class=unwritten_texture_candidate frame={} draw={} event_seq={} vs={:016X} ps={:016X} \
scene=unknown slot={} tex={:08X}.fmt{}.{}x{}.dim{} bytes={:X} mip={:08X}+{:X} zero_dwords={}/{} evidence=sampled_only",
              draw.frame, draw.draw, draw.event_seq, draw.vs_key, draw.ps_key, slot, base, format, width, height,
              dimension, bytes, mip_base, mip_bytes, zero_dwords, sampled_dwords);
}

void FetchDimensionMismatch(const DrawContext& draw, uint32_t slot, uint32_t base, uint32_t format,
                            uint32_t fetch_dimension, uint32_t shader_dimension, const char* reason) {
  if (!ClaimLog(draw, Key(kDimensionMismatch, slot, base, (uint64_t(fetch_dimension) << 32) | shader_dimension, format)))
    return;
  REXLOG_WARN("[glitch] class=fetch_bound_dimension_mismatch frame={} draw={} event_seq={} vs={:016X} ps={:016X} \
scene=unknown slot={} tex={:08X}.fmt{} fetch_dim={} shader_dim={} reason={}", draw.frame, draw.draw, draw.event_seq,
              draw.vs_key, draw.ps_key, slot, base, format, fetch_dimension, shader_dimension,
              reason ? reason : "unknown");
}

void FetchFormatMismatch(const DrawContext& draw, uint32_t slot, uint32_t base, uint32_t fetch_format,
                         uint32_t resolve_kind, uint32_t resolve_format, uint32_t width, uint32_t height,
                         const char* reason) {
  if (!ClaimLog(draw, Key(kFormatMismatch, slot, base, (uint64_t(fetch_format) << 32) | resolve_kind,
                          resolve_format)))
    return;
  REXLOG_WARN("[glitch] class=fetch_bound_format_mismatch frame={} draw={} event_seq={} vs={:016X} ps={:016X} \
scene=unknown slot={} tex={:08X}.fmt{}.{}x{} resolve_kind={} resolve_fmt={} reason={}", draw.frame, draw.draw,
              draw.event_seq, draw.vs_key, draw.ps_key, slot, base, fetch_format, width, height, resolve_kind,
              resolve_format, reason ? reason : "unknown");
}

void StaleResolveFallback(const DrawContext& draw, uint32_t slot, uint32_t fetch_base, uint32_t fetch_format,
                          uint32_t width, uint32_t height, uint32_t resolve_base, uint64_t resolve_seq,
                          const char* reason) {
  if (!draw.sampled) return;
  const uint64_t reason_tag = reason && std::strcmp(reason, "guest_write") == 0 ? 1 : 2;
  if (!ClaimLog(draw, Key(kStaleFallback, fetch_base, resolve_base, resolve_seq,
                          (uint64_t(fetch_format) << 32) | reason_tag)))
    return;
  REXLOG_WARN("[glitch] class=stale_resolve_fallback frame={} draw={} event_seq={} vs={:016X} ps={:016X} \
scene=unknown slot={} tex={:08X}.fmt{}.{}x{} resolve={:08X} resolve_seq={} reason={} fallback=guest_memory", draw.frame,
              draw.draw, draw.event_seq, draw.vs_key, draw.ps_key, slot, fetch_base, fetch_format, width, height,
              resolve_base, resolve_seq, reason ? reason : "unknown");
}

}  // namespace wwe2k14::d3d::glitch

#else

namespace wwe2k14::d3d::glitch {
uint32_t SampleInterval() { return 0; }
void BeginDraw(uint32_t, uint32_t) {}
void SetShaderKeys(uint64_t, uint64_t) {}
const DrawContext& CurrentDraw() { static const DrawContext empty{}; return empty; }
void MissingShader(const DrawContext&, uint8_t, uint64_t, const char*, uint64_t, uint32_t, const char*) {}
void CheckConstants(const DrawContext&, const uint8_t*, const uint8_t*, const uint64_t*, const uint64_t*, size_t) {}
bool ClaimUnwrittenTextureProbe(const DrawContext&, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t) { return false; }
void UnwrittenTextureCandidate(const DrawContext&, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                               uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void FetchDimensionMismatch(const DrawContext&, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const char*) {}
void FetchFormatMismatch(const DrawContext&, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                         const char*) {}
void StaleResolveFallback(const DrawContext&, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t,
                          const char*) {}
}  // namespace wwe2k14::d3d::glitch

#endif
