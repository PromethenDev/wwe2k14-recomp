// Card S3-68: see dirty_translate.h. Worker thread only. Written for this project.
#include "dirty_translate.h"

#include <cstdio>
#include <cstdlib>

#include <rex/logging.h>

#include "regview.h"
#include "dirty_translate_ab.h"
#include "v17c_ab.h"

namespace wwe2k14::d3d::dtr {
namespace {

const char* const kGroupName[kGCount] = {"shaders", "noop", "variant", "state", "layout", "slots", "sampler"};

struct RuntimeConfig {
  int mode = 0;
  uint32_t ab_frames = 0;
};

const RuntimeConfig& Config() {
  static const RuntimeConfig config = [] {
    RuntimeConfig out;
    const char* mode = std::getenv("WWE2K14_D3D_DIRTY_TRANSLATE");
    // ALL-1104 TEST build: dirty-translate on by default (M1); WWE2K14_D3D_DIRTY_TRANSLATE=0 turns it off.
    out.mode = 1;
    if (mode && *mode == '0') out.mode = 0;
    if (mode && *mode == '2') out.mode = 2;
    const char* schedule_text = std::getenv("WWE2K14_D3D_DIRTY_TRANSLATE_AB");
    bool invalid = false;
    out.ab_frames = ab::ParseBlockFrames(schedule_text, &invalid);
    if (invalid)
      REXLOG_WARN("[d3d-dtr] invalid WWE2K14_D3D_DIRTY_TRANSLATE_AB='{}' (expected 1..1000000); disabling A/B schedule",
                  schedule_text ? schedule_text : "");
    if (out.ab_frames && out.mode != 1) {
      REXLOG_WARN("[d3d-dtr] WWE2K14_D3D_DIRTY_TRANSLATE_AB requires WWE2K14_D3D_DIRTY_TRANSLATE=1; disabling A/B schedule");
      out.ab_frames = 0;
    }
    return out;
  }();
  return config;
}

struct Stats {
  uint64_t hit[kGCount] = {}, miss[kGCount] = {}, bad[kGCount] = {};
  uint64_t rs_epochs = 0, slot_epochs = 0, draws = 0;
  uint64_t bad_total = 0, bad_logged = 0;
};
Stats g_st;
uint64_t g_rs_epoch = 1;
uint64_t g_slot_epoch[32] = {};

}  // namespace

int Mode() {
  return Config().mode;
}

int ModeForFrame(uint32_t frame) {
  const int combined_mode = v17c_ab::ModeForFrame(frame);
  if (combined_mode >= 0) return combined_mode;
  const RuntimeConfig& config = Config();
  if (!config.ab_frames) return config.mode;
  const uint32_t block = frame / config.ab_frames;
  const int mode = ab::ModeForFrame(config.mode, config.ab_frames, frame);
  static bool have_last_block = false;
  static uint32_t last_block = 0;
  if (!have_last_block || block != last_block) {
    const uint64_t start = uint64_t(block) * config.ab_frames;
    const uint64_t end = start + config.ab_frames;
    REXLOG_INFO("[d3d-dtr-ab] block={} frames=[{}, {}) mode={} ({})", block, start, end, mode,
                mode ? "reuse" : "legacy");
    have_last_block = true;
    last_block = block;
  }
  return mode;
}

void Count(Group g, bool hit) {
  if (hit) ++g_st.hit[g];
  else ++g_st.miss[g];
}

void Mismatch(Group g, uint32_t seq, const std::string& what) {
  ++g_st.bad[g];
  ++g_st.bad_total;
  if (g_st.bad_logged < 20) {
    ++g_st.bad_logged;
    REXLOG_WARN("[d3d-dtr] CHECK mismatch group={} seq={} {}", kGroupName[g], seq, what);
  }
}

void BeginDraw(const RegView& rv) {
  ++g_st.draws;
  const uint64_t m = rv.TakeDirty();
  if (!m) return;
  if (m & RegView::kDirtyRs) {
    ++g_rs_epoch;
    ++g_st.rs_epochs;
  }
  const uint32_t slots = uint32_t(m >> 32);
  for (uint32_t s = 0; s < 32; ++s) {
    if (slots & (1u << s)) {
      ++g_slot_epoch[s];
      ++g_st.slot_epochs;
    }
  }
}

uint64_t RsEpoch() { return g_rs_epoch; }
uint64_t SlotEpoch(uint32_t slot) { return g_slot_epoch[slot & 31]; }

void LogLine(uint32_t frames, uint32_t frame) {
  if (!frames) return;
  const RuntimeConfig& config = Config();
  const int mode = ModeForFrame(frame);
  if (!mode && !config.ab_frames && !v17c_ab::Enabled()) return;
  std::string l;
  char b[96];
  for (uint32_t g = 0; g < kGCount; ++g) {
    const uint64_t n = g_st.hit[g] + g_st.miss[g];
    std::snprintf(b, sizeof b, " %s=%.1f%%(%llu/f)", kGroupName[g], n ? 100.0 * double(g_st.hit[g]) / double(n) : 0.0,
                  (unsigned long long)(n / frames));
    l += b;
  }
  std::string bad;
  for (uint32_t g = 0; g < kGCount; ++g) {
    if (g_st.bad[g]) {
      std::snprintf(b, sizeof b, " %s=%llu", kGroupName[g], (unsigned long long)g_st.bad[g]);
      bad += b;
    }
  }
  REXLOG_INFO("[d3d-dtr] mode={} hit rate (lookups/f):{} | draws/f={:.0f} rs_epochs/f={:.1f} slot_epochs/f={:.1f} | "
              "check mismatches window={}{} total={}", mode, l, double(g_st.draws) / frames,
              double(g_st.rs_epochs) / frames, double(g_st.slot_epochs) / frames,
              g_st.bad[0] + g_st.bad[1] + g_st.bad[2] + g_st.bad[3] + g_st.bad[4] + g_st.bad[5] + g_st.bad[6], bad,
              g_st.bad_total);
  const uint64_t keep_total = g_st.bad_total, keep_logged = g_st.bad_logged;
  g_st = Stats{};
  g_st.bad_total = keep_total;
  g_st.bad_logged = keep_logged;
}

#ifdef WWE2K14_HAS_NR
namespace nb = nr::backend;

void StateOut::From(const nb::Draw& d, uint32_t spec_in, uint32_t param_gen_in) {
  spec = spec_in;
  param_gen = param_gen_in;
  std::memcpy(alpha, d.shared + 528, sizeof alpha);
  std::memcpy(ndc, d.shared + 544, sizeof ndc);
  std::memcpy(scales, d.shared + 816, sizeof scales);
  std::memcpy(viewport, d.viewport, sizeof viewport);
  std::memcpy(scissor, d.scissor, sizeof scissor);
  depth_test = d.depth_test;
  depth_write = d.depth_write;
  depth_compare = d.depth_compare;
  stencil = d.stencil;
  stencil_ref = d.stencil_ref;
  stencil_read = d.stencil_read;
  stencil_write = d.stencil_write;
  stencil_front = d.stencil_front;
  stencil_back = d.stencil_back;
  blend = d.blend;
}

void StateOut::ApplyTo(nb::Draw& d) const {
  std::memcpy(d.shared + 528, alpha, sizeof alpha);
  std::memcpy(d.shared + 544, ndc, sizeof ndc);
  std::memcpy(d.shared + 816, scales, sizeof scales);
  std::memcpy(d.viewport, viewport, sizeof viewport);
  std::memcpy(d.scissor, scissor, sizeof scissor);
  d.depth_test = depth_test;
  d.depth_write = depth_write;
  d.depth_compare = depth_compare;
  d.stencil = stencil;
  d.stencil_ref = stencil_ref;
  d.stencil_read = stencil_read;
  d.stencil_write = stencil_write;
  d.stencil_front = stencil_front;
  d.stencil_back = stencil_back;
  d.blend = blend;
}

namespace {
bool SameFace(const nb::StencilFace& a, const nb::StencilFace& b) {
  return a.fail == b.fail && a.depth_fail == b.depth_fail && a.pass == b.pass && a.compare == b.compare;
}
bool SameBlend(const nb::BlendTarget& a, const nb::BlendTarget& b) {
  return a.enabled == b.enabled && a.src == b.src && a.dst == b.dst && a.op == b.op && a.src_a == b.src_a &&
         a.dst_a == b.dst_a && a.op_a == b.op_a && a.write_mask == b.write_mask;
}
}  // namespace

std::string StateOut::Diff(const StateOut& o) const {
  if (spec != o.spec) return "spec";
  if (param_gen != o.param_gen) return "param_gen";
  if (std::memcmp(alpha, o.alpha, sizeof alpha)) return "alpha";
  if (std::memcmp(ndc, o.ndc, sizeof ndc)) return "ndc/vte";
  if (std::memcmp(scales, o.scales, sizeof scales)) return "colour scales";
  if (std::memcmp(viewport, o.viewport, sizeof viewport)) return "viewport";
  if (std::memcmp(scissor, o.scissor, sizeof scissor)) return "scissor";
  if (depth_test != o.depth_test || depth_write != o.depth_write || depth_compare != o.depth_compare) return "depth";
  if (stencil != o.stencil || stencil_ref != o.stencil_ref || stencil_read != o.stencil_read ||
      stencil_write != o.stencil_write || !SameFace(stencil_front, o.stencil_front) || !SameFace(stencil_back, o.stencil_back))
    return "stencil";
  if (!SameBlend(blend, o.blend)) return "blend";
  return {};
}

namespace {
constexpr uint32_t kShaderSlots = 1024, kLayoutSlots = 512, kSlotsSlots = 512;
ShaderMemo g_shader[kShaderSlots];
LayoutMemo g_layout[kLayoutSlots];
SlotsMemo g_slots[kSlotsSlots];
SamplerMemo g_sampler[32];
inline uint64_t Mix(uint64_t x) {
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDull;
  x ^= x >> 33;
  x *= 0xC4CEB9FE1A85EC53ull;
  x ^= x >> 33;
  return x;
}
}  // namespace

ShaderMemo& ShaderSlot(uint64_t vfnv, uint64_t pfnv, bool has_ps) {
  return g_shader[Mix(vfnv * 31 + pfnv * 7 + (has_ps ? 1 : 0)) & (kShaderSlots - 1)];
}
void ResetShaderMemo(ShaderMemo& m, uint64_t vfnv, uint64_t pfnv, bool has_ps) {
  m = ShaderMemo();
  m.vfnv = vfnv;
  m.pfnv = pfnv;
  m.has_ps = has_ps;
}

LayoutMemo& LayoutSlot(const nr::shared::ShaderEntry* vs) {
  return g_layout[Mix(uint64_t(reinterpret_cast<uintptr_t>(vs))) & (kLayoutSlots - 1)];
}
bool SameAttributes(const std::vector<nb::VertexAttribute>& a, const std::vector<nb::VertexAttribute>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].location != b[i].location || a[i].format != b[i].format || a[i].binding != b[i].binding ||
        a[i].offset_bytes != b[i].offset_bytes)
      return false;
  }
  return true;
}

SlotsMemo& SlotsSlot(const nr::shared::ShaderEntry* vs, const nr::shared::ShaderEntry* ps) {
  return g_slots[Mix(uint64_t(reinterpret_cast<uintptr_t>(vs)) * 3 + uint64_t(reinterpret_cast<uintptr_t>(ps))) &
                 (kSlotsSlots - 1)];
}

SamplerMemo& SamplerSlot(uint32_t slot) { return g_sampler[slot & 31]; }
bool SameSampler(const nb::SamplerDesc& a, const nb::SamplerDesc& b) {
  return a.mag_linear == b.mag_linear && a.min_linear == b.min_linear && a.mip_linear == b.mip_linear &&
         a.address_u == b.address_u && a.address_v == b.address_v && a.address_w == b.address_w && a.border == b.border &&
         a.max_anisotropy == b.max_anisotropy && std::memcmp(&a.min_lod, &b.min_lod, 4) == 0 &&
         std::memcmp(&a.max_lod, &b.max_lod, 4) == 0 && std::memcmp(&a.lod_bias, &b.lod_bias, 4) == 0;
}
#endif

}  // namespace wwe2k14::d3d::dtr
