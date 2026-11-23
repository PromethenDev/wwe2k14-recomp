// F1 (docs/PLAN-FULLY-NATIVE.md): native D3D setter bodies. Semantics: native_setters_core.h.
#include "native_setters.h"
#include "native_setters_core.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/memory.h>

#include "capture.h"

namespace wwe2k14::d3d::native_setters {
namespace {

struct Config {
  int mode = 0;
  uint32_t ab = 0;
};
// N1 (fully native, owner 2026-11-17): the F1 native setters are ON by default on desktop PC (combined lane-A CHECK n1-check:
// 146 M constant + 43 M state/texture calls, 0 mismatches, tag-scan 0 misses, picture OK). =0 restores the guest bodies.
// Android and the Xbox app keep the old default until they are checked on their own hardware.
#if defined(__ANDROID__) || defined(REX_UWP)
constexpr int kNativeDefault = 0;
#else
constexpr int kNativeDefault = 1;
#endif
int NativeMode(const char* name) {
  const char* e = std::getenv(name);
  if (!e || !*e) return kNativeDefault;
  return (*e == '1' || *e == '2') ? *e - '0' : 0;
}

const Config& Cfg() {
  static const Config c = [] {
    Config v;
    v.mode = NativeMode("WWE2K14_D3D_NATIVE_CONST");
    if (const char* e = std::getenv("WWE2K14_D3D_NATIVE_CONST_AB"); e && *e) {
      const long n = std::strtol(e, nullptr, 10);
      if (n >= 1 && n <= 1000000 && v.mode == 1) v.ab = uint32_t(n);
      else REXLOG_WARN("[d3d-native-const] WWE2K14_D3D_NATIVE_CONST_AB='{}' needs mode 1 and 1..1000000; disabled", e);
    }
    if (v.mode) REXLOG_INFO("[d3d-native-const] mode={} ab={}", v.mode, v.ab);
    return v;
  }();
  return c;
}

std::atomic<uint64_t> g_calls{0}, g_rows{0}, g_bad_calls{0}, g_bad_rows{0}, g_bad_dirty{0};

void CopyRows(const PPCContext& ctx, uint8_t* base, uint32_t dest, uint32_t count) {
  const uint32_t src = ctx.r5.u32;
  for (uint32_t i = 0; i < count; ++i) std::memcpy(PPC_RAW_ADDR(dest + i * 16), PPC_RAW_ADDR(src + i * 16), 16);
}

void RunCheck(PPCContext& ctx, uint8_t* base, bool ps, uint32_t dest, uint32_t count, GuestBody original) {
  thread_local uint8_t expect[kMaxRows * 16];
  const uint32_t src = ctx.r5.u32;
  const uint32_t dev = ctx.r3.u32;
  for (uint32_t i = 0; i < count; ++i) std::memcpy(expect + i * 16, PPC_RAW_ADDR(src + i * 16), 16);
  uint8_t dirty[8];
  std::memcpy(dirty, PPC_RAW_ADDR(dev + DirtyOffset(ps)), 8);
  OrDirtyBe(dirty, ctx.r7.u64);
  original(ctx, base);
  uint32_t bad_rows = 0;
  for (uint32_t i = 0; i < count; ++i)
    if (std::memcmp(expect + i * 16, PPC_RAW_ADDR(dest + i * 16), 16) != 0) ++bad_rows;
  const bool bad_dirty = std::memcmp(dirty, PPC_RAW_ADDR(dev + DirtyOffset(ps)), 8) != 0;
  const uint64_t calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  g_rows.fetch_add(count, std::memory_order_relaxed);
  if (bad_rows || bad_dirty) {
    g_bad_calls.fetch_add(1, std::memory_order_relaxed);
    g_bad_rows.fetch_add(bad_rows, std::memory_order_relaxed);
    if (bad_dirty) g_bad_dirty.fetch_add(1, std::memory_order_relaxed);
    if (g_bad_calls.load(std::memory_order_relaxed) <= 16)
      REXLOG_WARN("[d3d-native-const] CHECK mismatch ps={} dest={:08X} count={} bad_rows={} bad_dirty={}", ps, dest, count,
                  bad_rows, bad_dirty);
  }
  if ((calls & 0x3FFFFu) == 0)
    REXLOG_INFO("[d3d-native-const] CHECK calls={} rows={} mismatch_calls={} bad_rows={} bad_dirty={}", calls,
                g_rows.load(std::memory_order_relaxed), g_bad_calls.load(std::memory_order_relaxed),
                g_bad_rows.load(std::memory_order_relaxed), g_bad_dirty.load(std::memory_order_relaxed));
}

}  // namespace

bool TryConst(PPCContext& ctx, uint8_t* base, bool ps, uint32_t frame, GuestBody original) {
  const Config& cfg = Cfg();
  if (cfg.mode == 0) return false;
  if (cfg.mode == 1 && !AbNewArm(frame, cfg.ab)) return false;
  // Another thread's call stays on the old path: its hook forces the next full capture scan (capture.cpp TagHookPre).
  if (!CaptureOnProducerThread()) return false;
  const uint32_t dev = ctx.r3.u32, first = ctx.r4.u32, count = ctx.r6.u32;
  if (!ConstArgsOk(dev, first, count) || !GuestRangeOk(ctx.r5.u32, uint64_t(count) * 16)) return false;
  const uint32_t dest = ConstDest(dev, first, ps);
  if (cfg.mode == 2) {
    RunCheck(ctx, base, ps, dest, count, original);
    return true;
  }
  CopyRows(ctx, base, dest, count);
  OrDirtyBe(PPC_RAW_ADDR(dev + DirtyOffset(ps)), ctx.r7.u64);
  return true;
}

namespace {

struct StateConfig {
  int mode = 0;
  uint32_t ab = 0;
};
StateConfig ReadStateConfig(const char* name, const char* ab_name, const char* tag) {
  StateConfig v;
  v.mode = NativeMode(name);
  if (const char* e = std::getenv(ab_name); e && *e && v.mode == 1) {
    const long n = std::strtol(e, nullptr, 10);
    if (n >= 1 && n <= 1000000) v.ab = uint32_t(n);
  }
  if (v.mode) REXLOG_INFO("[{}] mode={} ab={}", tag, v.mode, v.ab);
  return v;
}
const StateConfig& StateCfg(StateSetter which) {
  static const StateConfig state = ReadStateConfig("WWE2K14_D3D_NATIVE_STATE", "WWE2K14_D3D_NATIVE_STATE_AB", "d3d-native-state");
  static const StateConfig tex = ReadStateConfig("WWE2K14_D3D_NATIVE_TEX", "WWE2K14_D3D_NATIVE_TEX_AB", "d3d-native-tex");
  return which == StateSetter::kTexture ? tex : state;
}

std::atomic<uint64_t> g_st_calls{0}, g_st_fallback{0}, g_st_bad{0};

// The stores of one setter call, in guest order. Mode 1 applies them at once; CHECK replays them on a copy of the device.
struct Stores {
  struct S { uint32_t addr; uint32_t size; uint64_t value; };
  S s[12];  // SetTexture at most: 6 fetch words, dev+24, slot, old texture fence = 9
  uint32_t n = 0;
  void Add(uint32_t addr, uint32_t size, uint64_t value) { s[n++] = {addr, size, value}; }
};

inline void ApplyStore(uint8_t* base, const Stores::S& x) {
  if (x.size == 1) PPC_STORE_U8(x.addr, uint8_t(x.value));
  else if (x.size == 4) PPC_STORE_U32(x.addr, uint32_t(x.value));
  else PPC_STORE_U64(x.addr, x.value);
}

// Shared head of SetIndices / SetStreamSource for the buffer being replaced. False = the record path, original must run.
inline bool ReleaseOld(uint8_t* base, uint32_t dev, uint32_t old, Stores& out) {
  if (!old) return true;
  const uint32_t now = PPC_LOAD_U32(dev + kOffFenceNow);
  if (now) {
    out.Add(old + 8, 4, now);
    return true;
  }
  return (PPC_LOAD_U32(dev + kOffFenceMask) & PPC_LOAD_U32(old + 0)) == 0;
}

bool PredictState(PPCContext& ctx, uint8_t* base, StateSetter which, Stores& out) {
  const uint32_t dev = ctx.r3.u32;
  switch (which) {
    case StateSetter::kDecl:
      out.Add(dev + kOffDecl, 4, ctx.r4.u32);
      out.Add(dev + 16, 8, PPC_LOAD_U64(dev + 16) | kDirtyDecl);
      return true;
    case StateSetter::kIndices:
      if (!ReleaseOld(base, dev, PPC_LOAD_U32(dev + kOffIndices), out)) return false;
      out.Add(dev + kOffIndices, 4, ctx.r4.u32);
      return true;
    case StateSetter::kTexture: {
      const uint32_t stage = ctx.r4.u32, tex = ctx.r5.u32;
      if (stage >= kTextureStages) return false;
      const uint32_t slot = dev + (stage + 48) * 24;
      if (!ReleaseOld(base, dev, PPC_LOAD_U32(dev + kOffTextureSlots + stage * 4), out)) return false;
      if (tex) {
        TexIn in{};
        in.t28 = PPC_LOAD_U32(tex + 28); in.t32 = PPC_LOAD_U32(tex + 32); in.t36 = PPC_LOAD_U32(tex + 36);
        in.t40 = PPC_LOAD_U32(tex + 40); in.t44 = PPC_LOAD_U32(tex + 44); in.t48 = PPC_LOAD_U32(tex + 48);
        in.w0 = PPC_LOAD_U32(slot + 0); in.w4 = PPC_LOAD_U32(slot + 4); in.w12 = PPC_LOAD_U32(slot + 12);
        in.w16 = PPC_LOAD_U32(slot + 16); in.w20 = PPC_LOAD_U32(slot + 20);
        in.min_clamp = PPC_LOAD_U8(dev + stage + kOffMinClamp);
        in.max_clamp = PPC_LOAD_U8(dev + stage + kOffMaxClamp);
        const TexOut o = MergeTextureFetch(in);
        out.Add(slot + 0, 4, o.w0); out.Add(slot + 4, 4, o.w4); out.Add(slot + 8, 4, o.w8);
        out.Add(slot + 12, 4, o.w12); out.Add(slot + 16, 4, o.w16); out.Add(slot + 20, 4, o.w20);
        out.Add(dev + 24, 8, PPC_LOAD_U64(dev + 24) | ctx.r6.u64);
      } else {
        out.Add(slot, 4, PPC_LOAD_U32(slot) & 0xFFFFFFFCu);
      }
      out.Add(dev + kOffTextureSlots + stage * 4, 4, tex);
      return true;
    }
    case StateSetter::kStream: {
      const uint32_t stream = ctx.r4.u32, buffer = ctx.r5.u32, offset = ctx.r6.u32, stride = ctx.r7.u32;
      if (stream > 17) return false;
      if (buffer) {
        out.Add(dev + StreamFetchAddrOffset(stream), 4, StreamFetchAddr(PPC_LOAD_U32(buffer + 24), offset));
        out.Add(dev + StreamFetchSizeOffset(stream), 4, PPC_LOAD_U32(buffer + 28) - offset);
        out.Add(dev + 24, 8, PPC_LOAD_U64(dev + 24) | ctx.r8.u64);
      }
      if (!ReleaseOld(base, dev, PPC_LOAD_U32(dev + kOffStreamSlots + stream * 4), out)) return false;
      out.Add(dev + kOffStreamSlots + stream * 4, 4, buffer);
      out.Add(dev + kOffStride + stream, 1, (stride >> 2) & 0xFFu);
      const uint32_t dwords = stride >> 2;
      if (dwords && dwords != PPC_LOAD_U8(dev + kOffDeclStride + stream))
        out.Add(dev + 16, 8, PPC_LOAD_U64(dev + 16) | kDirtyDecl);
      return true;
    }
  }
  return false;
}

// Per-byte tally of device changes the prediction does not explain. Control = the same snapshot + compare with no setter
// running: what changes there is another thread, not the setter. An offset seen only in setter windows is unexplained.
struct OffsetTally {
  uint32_t setter[4][kDeviceCheckBytes] = {};
  uint32_t control[kDeviceCheckBytes] = {};
  uint64_t setter_calls_changed[4] = {};
  uint64_t control_changed = 0;
};
OffsetTally& Tally() {
  static OffsetTally* t = new OffsetTally();
  return *t;
}

void CheckState(PPCContext& ctx, uint8_t* base, StateSetter which, const Stores& st, GuestBody original) {
  const uint32_t dev = ctx.r3.u32;
  OffsetTally& tally = Tally();
  thread_local uint8_t before[kDeviceCheckBytes];
  std::memcpy(before, PPC_RAW_ADDR(dev), kDeviceCheckBytes);
  if (std::memcmp(before, PPC_RAW_ADDR(dev), kDeviceCheckBytes) != 0) {
    ++tally.control_changed;
    for (uint32_t i = 0; i < kDeviceCheckBytes; ++i)
      if (before[i] != PPC_RAW_ADDR(dev)[i]) ++tally.control[i];
    std::memcpy(before, PPC_RAW_ADDR(dev), kDeviceCheckBytes);
  }
  for (uint32_t i = 0; i < st.n; ++i) {
    const Stores::S& x = st.s[i];
    if (x.addr < dev || x.addr + x.size > dev + kDeviceCheckBytes) continue;
    uint8_t* d = before + (x.addr - dev);
    if (x.size == 1) *d = uint8_t(x.value);
    else if (x.size == 4) { const uint32_t v = __builtin_bswap32(uint32_t(x.value)); std::memcpy(d, &v, 4); }
    else { const uint64_t v = Bswap64(x.value); std::memcpy(d, &v, 8); }
  }
  original(ctx, base);
  // 1. Each predicted store must hold exactly its value (a wrong prediction).
  bool bad = false;
  for (uint32_t i = 0; i < st.n; ++i) {
    const Stores::S& x = st.s[i];
    if (x.size == 1) bad |= PPC_LOAD_U8(x.addr) != uint8_t(x.value);
    else if (x.size == 4) bad |= PPC_LOAD_U32(x.addr) != uint32_t(x.value);
    else bad |= PPC_LOAD_U64(x.addr) != x.value;
  }
  if (bad && g_st_bad.fetch_add(1, std::memory_order_relaxed) < 16)
    REXLOG_WARN("[d3d-native-state] CHECK mismatch setter={} predicted store differs", int(which));
  // 2. Anything else that changed: tallied by offset against the control.
  if (std::memcmp(before, PPC_RAW_ADDR(dev), kDeviceCheckBytes) != 0) {
    ++tally.setter_calls_changed[int(which)];
    for (uint32_t i = 0; i < kDeviceCheckBytes; ++i)
      if (before[i] != PPC_RAW_ADDR(dev)[i]) ++tally.setter[int(which)][i];
  }
}

// mismatch_unexplained = device bytes changed during a setter but never in a control window.
void LogTally() {
  OffsetTally& t = Tally();
  uint32_t unexplained = 0;
  std::string list;
  for (uint32_t i = 0; i < kDeviceCheckBytes; ++i) {
    const uint32_t n = t.setter[0][i] + t.setter[1][i] + t.setter[2][i] + t.setter[3][i];
    if (!n || t.control[i]) continue;
    if (++unexplained <= 12)
      list += " dev+" + std::to_string(i) + "(" + std::to_string(t.setter[0][i]) + "/" + std::to_string(t.setter[1][i]) + "/" +
              std::to_string(t.setter[2][i]) + "/" + std::to_string(t.setter[3][i]) + ")";
  }
  std::string seen;
  uint32_t shown = 0;
  for (uint32_t i = 0; i < kDeviceCheckBytes && shown < 12; ++i)
    if (t.control[i]) { seen += " dev+" + std::to_string(i) + "=" + std::to_string(t.control[i]); ++shown; }
  REXLOG_INFO("[d3d-native-state] CHECK tally: other_changes decl={} indices={} stream={} texture={} control={} mismatch_unexplained={}{} | control:{}",
              t.setter_calls_changed[0], t.setter_calls_changed[1], t.setter_calls_changed[2], t.setter_calls_changed[3],
              t.control_changed, unexplained,
              list, seen);
}

}  // namespace

bool TryState(PPCContext& ctx, uint8_t* base, StateSetter which, uint32_t frame, GuestBody original) {
  const StateConfig& cfg = StateCfg(which);
  if (cfg.mode == 0) return false;
  if (cfg.mode == 1 && !AbNewArm(frame, cfg.ab)) return false;
  if (!CaptureOnProducerThread()) return false;
  Stores st;
  const bool native = PredictState(ctx, base, which, st);
  const uint64_t calls = g_st_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!native) g_st_fallback.fetch_add(1, std::memory_order_relaxed);
  if ((calls & 0x3FFFFu) == 0)
    REXLOG_INFO("[d3d-native-state] {} calls={} original_path={} mismatch_calls={}", cfg.mode == 2 ? "CHECK" : "ON", calls,
                g_st_fallback.load(std::memory_order_relaxed), g_st_bad.load(std::memory_order_relaxed));
  if (cfg.mode == 2 && (calls & 0x3FFFFu) == 0) LogTally();
  if (!native) return false;
  // The normal hook re-reads the slot for the tag-guided capture (a NULL bind clears type bits with no tag): keep that.
  if (which == StateSetter::kTexture) CaptureNoteSetTexture(ctx);
  if (cfg.mode == 2) {
    CheckState(ctx, base, which, st, original);
    return true;
  }
  for (uint32_t i = 0; i < st.n; ++i) ApplyStore(base, st.s[i]);
  return true;
}

bool InterpFast(uint32_t frame) {
  static const uint32_t cfg = [] {  // bit 31 = on, low bits = A/B block
    if (NativeMode("WWE2K14_D3D_NATIVE_INTERP") != 1) return 0u;
    uint32_t v = 0x80000000u;
    if (const char* ab = std::getenv("WWE2K14_D3D_NATIVE_INTERP_AB"); ab && *ab) {
      const long n = std::strtol(ab, nullptr, 10);
      if (n >= 1 && n <= 1000000) v |= uint32_t(n);
    }
    REXLOG_INFO("[d3d-native-interp] on ab={}", v & 0x7FFFFFFFu);
    return v;
  }();
  return cfg && AbNewArm(frame, cfg & 0x7FFFFFFFu) && CaptureOnProducerThread();
}

}  // namespace wwe2k14::d3d::native_setters
