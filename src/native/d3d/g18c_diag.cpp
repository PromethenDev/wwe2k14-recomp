// Diagnostic for card G18 / GitHub #161 #150 #105 #167 #181 #184 #186 (attire composite tinted, noisy or black).
// WWE2K14_D3D_G18C=1: full test trace (below). Default (unset): LITE player trace (2026-11-22) - the compositor's pass
// binds (sub_82BD7F30) are kept in memory, de-duplicated, and written once per finished composite readback together with
// its colour statistics, so a normal player bug report from a machine that shows the fault every time carries the
// answer (~5 short blocks per match). WWE2K14_D3D_G18C=0 turns everything off. Remove once G18 is settled (AGENTS.md).
//
// Question: in bad runs the attire recolour pass (PS C8AED5A6A1A46145) samples the composite itself as its mask. A NULL
// SetTexture keeps the slot's old address and only clears the type bits (capture.cpp TagNoteSetTexture), which is that
// state. The known NULL-bind caller is sub_82BB7B20 (generated 76:4419): SetTexture(stage = obj+20, tex = obj+16), return
// address 0x82BB7B60. Log, for that caller only: every NULL bind (obj, its words, who called the binder) and the first
// non-NULL bind of each obj (texture object and its base address), so one run shows whether the mask pointer is filled
// later (load timing) or never.
#include "g18c_diag.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

namespace wwe2k14::d3d {
namespace {
constexpr uint32_t kBinderReturn = 0x82BB7B60;  // sub_82BB7B20's call of SetTexture sub_82CAC430
constexpr uint32_t kCompositorBindReturn = 0x82BD7FA4;  // sub_82BD7F30's call of SetTexture (attire compositor, g18c-3)
std::atomic<uint32_t> g_null_lines{0}, g_first_lines{0};
std::atomic<uint64_t> g_null_binds{0}, g_binds{0};
std::atomic<uint32_t> g_frame{0}, g_rec_null_lines{0}, g_rec_lr_lines{0};
std::mutex g_mu;
std::unordered_set<uint32_t> g_seen;
std::unordered_map<uint32_t, uint64_t> g_rec_null_site, g_rec_site;

// LITE trace: compositor pass binds since the last readback, consecutive duplicates folded into a count.
struct SelEntry {
  uint32_t first_frame, last_frame, caller, obj, stage, count;
  int32_t sel;
  uint32_t tex[3], tex_base[3];
};
constexpr size_t kSelKeep = 512;
std::mutex g_sel_mu;
std::deque<SelEntry> g_sel;
uint64_t g_sel_dropped = 0;
uint32_t g_readbacks = 0;

int G18cMode() {  // 0 off, 1 lite (default), 2 full; only the first character counts ('1...' full, '0...' off, else lite)
  static const int mode = [] {
    const char* e = std::getenv("WWE2K14_D3D_G18C");
    const int m = !e || !e[0] ? 1 : e[0] == '1' ? 2 : e[0] == '0' ? 0 : 1;
    if (m == 2) REXLOG_INFO("[g18c] diagnostic on: binder 0x{:08X} NULL binds + first bind per object; readback stats", kBinderReturn);
    if (m == 1) REXLOG_INFO("[g18c] attire trace (lite): compositor binds + colour stats per composite readback");
    return m;
  }();
  return mode;
}
bool G18cLite() { return G18cMode() >= 1; }

void PushSel(uint32_t frame, uint32_t caller, uint32_t obj, uint32_t stage, int32_t sel, const uint32_t tex[3],
             const uint32_t tex_base[3]) {
  std::lock_guard<std::mutex> lk(g_sel_mu);
  // Fold repeats of the same bind in the same frame (Fable 11-22: match frames alternate stage 0/1, so comparing only
  // the last entry let ~30 large binds per frame flush the buffer).
  const size_t look = std::min<size_t>(g_sel.size(), 16);
  for (size_t i = 0; i < look; ++i) {
    SelEntry& b = g_sel[g_sel.size() - 1 - i];
    if (b.last_frame != frame) break;
    if (b.caller == caller && b.obj == obj && b.stage == stage && b.sel == sel &&
        std::equal(tex, tex + 3, b.tex) && std::equal(tex_base, tex_base + 3, b.tex_base)) {
      ++b.count;
      return;
    }
  }
  SelEntry e{frame, frame, caller, obj, stage, 1, sel, {tex[0], tex[1], tex[2]}, {tex_base[0], tex_base[1], tex_base[2]}};
  g_sel.push_back(e);
  if (g_sel.size() > kSelKeep) {
    g_sel.pop_front();
    ++g_sel_dropped;
  }
}
}  // namespace

bool G18cEnabled() { return G18cMode() == 2; }

void G18cSetTexture(PPCContext& ctx, uint8_t* base, uint32_t frame) {
  if (!G18cLite()) return;
  g_frame.store(frame, std::memory_order_relaxed);
  if (uint32_t(ctx.lr) == kCompositorBindReturn) {
    // sub_82BD7F30 is the game's general pass binder (g18e-lite-2: UI binds too). The attire compositor's source,
    // composite and masks are all 2048x1024 (tex+36 = 007FE7FF in g18c-3): keep binds of textures >= 1M texels, and
    // NULL binds (the type-0 self-mask state) within 2 frames of such a bind. sel = -100 marks these in the dump.
    static std::atomic<uint32_t> last_large_frame{0};
    const uint32_t tex = ctx.r5.u32;
    bool keep = false;
    uint32_t dims = 0;
    if (tex) {
      dims = PPC_LOAD_U32(tex + 36);
      const uint64_t w = (dims & 0x1FFFu) + 1, h = ((dims >> 13) & 0x1FFFu) + 1;
      keep = w * h >= 1024u * 1024u;
      if (keep) last_large_frame.store(frame, std::memory_order_relaxed);
    } else {
      const uint32_t last = last_large_frame.load(std::memory_order_relaxed);
      keep = last != 0 && frame >= last && frame - last <= 2;
    }
    if (keep) {
      const uint32_t t[3] = {tex, tex ? PPC_LOAD_U32(tex + 28) : 0u, dims};
      const uint32_t b[3] = {tex ? PPC_LOAD_U32(tex + 32) & 0x1FFFF000u : 0u, 0u, 0u};
      PushSel(frame, uint32_t(ctx.lr), 0, ctx.r4.u32, -100, t, b);
    }
  }
  if (!G18cEnabled()) return;
  if (ctx.lr != kBinderReturn) {
    // Direct binds (the attire compositor does not use the command recorder, run g18c-2): any texture whose base lies in the
    // mask/composite pages (card G18: mask 1CA85000, composite 1D285000, mips down to 1CFE5000), and NULL binds of
    // stage 0 per call site (first 3, then every 4096th).
    const uint32_t lr = uint32_t(ctx.lr), stage = ctx.r4.u32, tex = ctx.r5.u32;
    if (tex) {
      const uint32_t pb = PPC_LOAD_U32(tex + 32) & 0x1FFFF000u;
      if (pb >= 0x1C800000u && pb < 0x1D400000u && g_first_lines.fetch_add(1, std::memory_order_relaxed) < 3000)
        REXLOG_INFO("[g18c-dbind] f={} site={:08X} stage={} tex={:08X} base {:08X} tex+28 {:08X} tex+36 {:08X}", frame, lr,
                    stage, tex, pb, PPC_LOAD_U32(tex + 28), PPC_LOAD_U32(tex + 36));
    } else if (stage == 0) {
      uint64_t k;
      {
        std::lock_guard<std::mutex> lk(g_mu);
        k = ++g_rec_null_site[lr ^ 0x80000000u];  // separate key space from the recorder sites
      }
      if ((k <= 3 || (k & 4095) == 0) && g_null_lines.fetch_add(1, std::memory_order_relaxed) < 600)
        REXLOG_INFO("[g18c-dnull] f={} site={:08X} #{} slot0 now {:08X} {:08X}", frame, lr, k,
                    PPC_LOAD_U32(ctx.r3.u32 + 48 * 24), PPC_LOAD_U32(ctx.r3.u32 + 48 * 24 + 4));
    }
    return;
  }
  const uint32_t obj = ctx.r31.u32, stage = ctx.r4.u32, tex = ctx.r5.u32;
  const uint32_t caller = PPC_LOAD_U32(ctx.r1.u32 + 88);  // sub_82BB7B20 saved its LR at r1(old)-8 = r1+96-8
  g_binds.fetch_add(1, std::memory_order_relaxed);
  if (tex == 0) {
    const uint64_t n = g_null_binds.fetch_add(1, std::memory_order_relaxed) + 1;
    if (false)  // run g18c-1: NULL binds via the command player are routine slot clears
      REXLOG_INFO("[g18c-null] f={} n={} stage={} obj={:08X} caller={:08X} obj+0..28 {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
                  "{:08X} {:08X}",
                  frame, n, stage, obj, caller, PPC_LOAD_U32(obj + 0), PPC_LOAD_U32(obj + 4), PPC_LOAD_U32(obj + 8),
                  PPC_LOAD_U32(obj + 12), PPC_LOAD_U32(obj + 16), PPC_LOAD_U32(obj + 20), PPC_LOAD_U32(obj + 24),
                  PPC_LOAD_U32(obj + 28));
    return;
  }
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_seen.insert(obj).second) return;
  }
  if (g_first_lines.fetch_add(1, std::memory_order_relaxed) < 600)
    REXLOG_INFO("[g18c-bind] f={} stage={} obj={:08X} caller={:08X} tex={:08X} tex+28 {:08X} base {:08X} tex+36 {:08X} "
                "tex+40 {:08X} binds={} null={}",
                frame, stage, obj, caller, tex, PPC_LOAD_U32(tex + 28), PPC_LOAD_U32(tex + 32), PPC_LOAD_U32(tex + 36),
                PPC_LOAD_U32(tex + 40), g_binds.load(std::memory_order_relaxed),
                g_null_binds.load(std::memory_order_relaxed));
}

void G18cReadback(uint32_t start, const uint8_t* bytes, size_t n) {
  // Readbacks below 64 KB (small mips) are skipped; blocks are capped at 600 per session (Fable 11-22: shipped builds
  // must not log without bound, e.g. if Creation Suite reads back per edit).
  if (!G18cLite() || n < 0x10000) return;
  static std::atomic<uint32_t> blocks{0};
  if (blocks.fetch_add(1, std::memory_order_relaxed) >= 600) return;
  const size_t words = n / 4;
  uint64_t zero = 0, white = 0, sum[4] = {0, 0, 0, 0};
  uint8_t lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
  for (size_t i = 0; i < words; ++i) {
    const uint8_t* p = bytes + i * 4;
    const uint32_t w = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    zero += w == 0;
    white += w == 0xFFFFFFFFu;
    for (int k = 0; k < 4; ++k) {
      sum[k] += p[k];
      lo[k] = std::min(lo[k], p[k]);
      hi[k] = std::max(hi[k], p[k]);
    }
  }
  const uint32_t own_base = (start - 0x1000u) & 0x1FFFF000u;
  std::deque<SelEntry> sel;
  uint64_t dropped = 0;
  uint32_t index = 0;
  {
    // Take the buffer only for this composite's own readback (its base was bound, or the 2048x1024 size), so an
    // unrelated readback between a composite's passes and its readback does not erase them (Fable 11-22).
    std::lock_guard<std::mutex> lk(g_sel_mu);
    const bool own = n == 0x800000 || std::any_of(g_sel.begin(), g_sel.end(), [&](const SelEntry& e) {
      return e.sel == -100 && e.tex[0] && e.tex_base[0] == own_base;
    });
    if (own) {
      sel.swap(g_sel);
      dropped = g_sel_dropped;
      g_sel_dropped = 0;
    }
    index = ++g_readbacks;
  }
  REXLOG_INFO("[g18c-readback] #{} {:08X}+{:X} texels {} zero {:.1f}% white {:.1f}% mean bytes {} {} {} {} range "
              "{}-{} {}-{} {}-{} {}-{} binds {} (+{} older)",
              index, start, n, words, 100.0 * double(zero) / double(words), 100.0 * double(white) / double(words),
              sum[0] / words, sum[1] / words, sum[2] / words, sum[3] / words, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
              lo[3], hi[3], sel.size(), dropped);
  // (pass lines below: only frames that bound this composite, see own_base)
  // Keep the frames where this composite itself was bound (base = readback start - 0x1000, g18e-lite-3: 1D284000 for
  // 1D285000) +-5 frames (the source bind came 3 frames earlier), so other large textures bound in a match do not bury the passes; else the newest 16 entries.
  std::vector<uint32_t> own_frames;
  for (const SelEntry& e : sel)
    if (e.sel == -100 && e.tex[0] && e.tex_base[0] == own_base) own_frames.push_back(e.first_frame);
  const auto near_own = [&](const SelEntry& e) {
    for (const uint32_t f : own_frames)
      if (e.last_frame + 5 >= f && e.first_frame <= f + 5) return true;
    return false;
  };
  // Choose up to 64 lines: every bind of the composite itself and every ODD/NULL entry first, then the other entries
  // of its frames newest-first; print them in time order.
  std::vector<bool> chosen(sel.size(), false);
  size_t picked = 0;
  for (size_t i = 0; i < sel.size() && picked < 64; ++i) {
    const SelEntry& e = sel[i];
    const bool key = (e.sel == -100 && e.tex[0] && e.tex_base[0] == own_base) || e.sel != -100 || e.tex[0] == 0;
    if (key && (own_frames.empty() || near_own(e))) chosen[i] = true, ++picked;
  }
  for (size_t i = sel.size(); i-- > 0 && picked < 64;) {
    if (chosen[i]) continue;
    if (own_frames.empty() ? i + 16 < sel.size() : !near_own(sel[i])) continue;
    chosen[i] = true;
    ++picked;
  }
  for (size_t i = 0; i < sel.size(); ++i) {
    if (!chosen[i]) continue;
    const SelEntry& e = sel[i];
    if (e.sel == -100) {
      REXLOG_INFO("[g18c-pass]{} #{} f={}-{} x{} bind stage={} tex={:08X} base {:08X} tex+28 {:08X} tex+36 {:08X}",
                  e.tex[0] ? "" : " NULL", index, e.first_frame, e.last_frame, e.count, e.stage, e.tex[0],
                  e.tex_base[0], e.tex[1], e.tex[2]);
      continue;
    }
    REXLOG_INFO("[g18c-pass] ODD #{} f={}-{} x{} select caller={:08X} obj={:08X} stage={} sel={} tex {:08X} {:08X} "
                "{:08X} base {:08X} {:08X} {:08X}",
                index, e.first_frame, e.last_frame, e.count, e.caller, e.obj, e.stage, e.sel, e.tex[0], e.tex[1],
                e.tex[2], e.tex_base[0], e.tex_base[1], e.tex_base[2]);
  }
}

}  // namespace wwe2k14::d3d

// The game's "record a set-texture command" helper sub_82BB4F80 (generated 75:76188): rec+0 = 44, rec+16 = texture (r4),
// rec+20 = stage (r5); the command player sub_82BB8C00 case 44 later binds it via sub_82BB7B20. 55 call sites. Log each
// NULL record with its return address (= the recording call site), and the first record per call site with its texture.
extern "C" PPC_FUNC(__imp__sub_82BB4F80);
extern "C" PPC_FUNC(sub_82BB4F80) {
  using namespace wwe2k14::d3d;
  if (G18cEnabled()) {
    const uint32_t lr = uint32_t(ctx.lr), tex = ctx.r4.u32, stage = ctx.r5.u32;
    const uint32_t f = g_frame.load(std::memory_order_relaxed);
    // Per recording site: count every record; log the first 3 of each kind (NULL / texture), then every 4096th.
    const uint32_t tbase = tex ? PPC_LOAD_U32(tex + 32) : 0;
    uint64_t k;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      k = ++(tex ? g_rec_site : g_rec_null_site)[lr];
    }
    if ((k <= 3 || (k & 4095) == 0) && g_rec_lr_lines.fetch_add(1, std::memory_order_relaxed) < 3000)
      REXLOG_INFO("[g18c-rec] f={} site={:08X} {} #{} stage={} rec={:08X} tex={:08X} base {:08X} tex+28 {:08X}", f, lr,
                  tex ? "tex" : "NULL", k, stage, ctx.r3.u32, tex, tbase, tex ? PPC_LOAD_U32(tex + 28) : 0u);
    // Any record of the attire mask / composite pages (card G18: mask 1CA85000, composite 1D285000 + mips): always log.
    const uint32_t pb = tbase & 0x1FFFF000u;
    if (false && tex && pb >= 0x1C000000u && pb < 0x1F000000u && g_rec_null_lines.fetch_add(1, std::memory_order_relaxed) < 2000)
      REXLOG_INFO("[g18c-recatt] f={} site={:08X} stage={} tex={:08X} base {:08X} tex+28 {:08X}", f, lr, stage, tex, tbase,
                  PPC_LOAD_U32(tex + 28));
  }
  __imp__sub_82BB4F80(ctx, base);
}

// The attire compositor's pass-texture binder sub_82BD7F30(obj, stage) (generated 77:13246, called indirectly): obj+52
// selects obj+80 / +84 / +88 (0/1/2); another selector binds nothing, a NULL pointer binds NULL. Log every call.
extern "C" PPC_FUNC(__imp__sub_82BD7F30);
extern "C" PPC_FUNC(sub_82BD7F30) {
  using namespace wwe2k14::d3d;
  if (G18cLite()) {
    const uint32_t obj = ctx.r3.u32;
    const int32_t sel = int32_t(PPC_LOAD_U32(obj + 52));
    // Normal selections already show up as compositor SetTexture binds; keep only the ones that bind nothing or NULL
    // (selector outside 0-2 or a NULL selected texture) - the predicted bad-composite signature. Only the selected
    // slot is read on the common path; the three pointers are logged (never dereferenced) for an odd one (Fable 11-22).
    const bool in_range = sel >= 0 && sel <= 2;
    if (!in_range || PPC_LOAD_U32(obj + 80 + 4 * uint32_t(sel)) == 0) {
      const uint32_t tex[3] = {PPC_LOAD_U32(obj + 80), PPC_LOAD_U32(obj + 84), PPC_LOAD_U32(obj + 88)};
      const uint32_t tex_base[3] = {0u, 0u, 0u};
      PushSel(g_frame.load(std::memory_order_relaxed), uint32_t(ctx.lr), obj, ctx.r4.u32, sel, tex, tex_base);
    }
  }
  if (G18cEnabled()) {
    static std::atomic<uint32_t> lines{0};
    const uint32_t obj = ctx.r3.u32;
    const int32_t sel = int32_t(PPC_LOAD_U32(obj + 52));
    const uint32_t chosen = sel >= 0 && sel <= 2 ? PPC_LOAD_U32(obj + 80 + 4 * uint32_t(sel)) : 0u;
    const bool odd = sel < 0 || sel > 2 || chosen == 0;  // binds nothing, or binds NULL
    static std::atomic<uint32_t> odd_lines{0};
    if (odd ? odd_lines.fetch_add(1, std::memory_order_relaxed) < 2000 : lines.fetch_add(1, std::memory_order_relaxed) < 200) {
      const uint32_t t0 = PPC_LOAD_U32(obj + 80), t1 = PPC_LOAD_U32(obj + 84), t2 = PPC_LOAD_U32(obj + 88);
      REXLOG_INFO("[g18c-sel]{} f={} caller={:08X} obj={:08X} stage={} sel={} tex {:08X} {:08X} {:08X} base {:08X} {:08X} "
                  "{:08X}",
                  odd ? " ODD" : "", g_frame.load(std::memory_order_relaxed), uint32_t(ctx.lr), obj, ctx.r4.u32, sel, t0, t1, t2, t0 ? PPC_LOAD_U32(t0 + 32) & 0x1FFFF000u : 0u,
                  t1 ? PPC_LOAD_U32(t1 + 32) & 0x1FFFF000u : 0u, t2 ? PPC_LOAD_U32(t2 + 32) & 0x1FFFF000u : 0u);
    }
  }
  __imp__sub_82BD7F30(ctx, base);
}

// Match-setup attire calls (to learn the WM 7 attire number from a 30 Years match): sub_822C6208(M, slot, id, attire, 1)
// and sub_822C7A18(M, slot, value). Log every call with its caller.
extern "C" PPC_FUNC(__imp__sub_822C6208);
extern "C" PPC_FUNC(sub_822C6208) {
  static std::atomic<uint32_t> lines6208{0};
  if (wwe2k14::d3d::G18cLite() && lines6208.fetch_add(1, std::memory_order_relaxed) < 2000)
    REXLOG_INFO("[g18c-attire] 6208 caller={:08X} slot={} id={} attire={} r7={}", uint32_t(ctx.lr), ctx.r4.s32, ctx.r5.s32,
                ctx.r6.s32, ctx.r7.s32);
  __imp__sub_822C6208(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_822C7A18);
extern "C" PPC_FUNC(sub_822C7A18) {
  static std::atomic<uint32_t> lines7a18{0};
  if (wwe2k14::d3d::G18cLite() && lines7a18.fetch_add(1, std::memory_order_relaxed) < 2000)
    REXLOG_INFO("[g18c-attire] 7A18 caller={:08X} slot={} value={}", uint32_t(ctx.lr), ctx.r4.s32, ctx.r5.s32);
  __imp__sub_822C7A18(ctx, base);
}
