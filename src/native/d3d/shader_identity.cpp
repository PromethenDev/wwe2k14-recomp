// WP5-impl: see shader_identity.h. Observe only; nothing here writes guest memory or SDK state.
#include "shader_identity.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/string/buffer.h>

#include "capture.h"       // GuestRangeOk
#include "device_state.h"  // kOffPixelShader / kOffVertexShader
#include "hooks.h"

namespace wwe2k14::d3d {
namespace {

constexpr uint32_t kOffRingCursor = 48;            // dev+48: address of the last dword written (WP3 FACT)
constexpr uint32_t kImmHdrBase = 0xC0002B00u;      // IM_LOAD_IMMEDIATE header without the count field (bits 16..29)
constexpr uint32_t kPairHdr = 0xC0003B00u;         // header of the dword pair the VS writer stores before the 0x2B packet
constexpr uint32_t kPairVal = 256u;                // ... and its payload (meaning not decoded; validated, not used)
constexpr uint32_t kMaxLoadDwords = 4096;          // sanity bound of one microcode span
constexpr uint32_t kPhysLimit = 0x1FFF0000u;
constexpr uint32_t kPhysViewBase = 0xA0000000u;    // guest view of physical memory
constexpr uint32_t kMaxDiffLogs = 40;
constexpr uint64_t kMaxFailDumps = 8;

inline uint32_t Be32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

uint64_t FnvRaw(const uint8_t* p, size_t n) {  // guest-order (big-endian) bytes
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

uint64_t QuickHash(const uint8_t* p, size_t n) {  // memo pre-key only; entries are confirmed with memcmp
  uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
  }
  for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h ^ (h >> 32);
}

struct Entry {
  uint64_t fnv = 0;
  uint8_t stage = 0;
  std::vector<uint8_t> raw;        // guest order
  std::vector<uint32_t> dwords;    // host order
  std::unique_ptr<rex::graphics::Shader> shader;
  bool in_manifest = false;
  bool analyzed_ok = false;        // AnalyzeUcode completed; false = `shader` is unusable (reset) and must never be handed out
};

struct Stats {
  std::mutex mu;
  // game-thread flush side
  uint64_t flushes = 0, flush_loads = 0, flush_noload = 0;
  uint64_t loads[2] = {0, 0}, new_keys[2] = {0, 0}, memo_hits = 0;
  // Exact capture. vs_*: post-hook on the inline VS writer sub_82CC1B00. ps_*: device-state IM_LOAD source in the flush hook.
  uint64_t vs_exact = 0, vs_reject = 0, vs_noemit = 0, ps_exact = 0, ps_reject = 0, ps_none = 0;
  // D7b: VS by pointer IM_LOAD (flush tail), derived from device state.
  uint64_t vsp_exact = 0, vsp_reject = 0, vsp_flush = 0;
  uint64_t resident_obj_mismatch = 0;           // resident key != last key uploaded for the bound object (diagnostic)
  uint64_t obj_match = 0, obj_nomatch = 0, obj_null = 0, obj_rebind_change = 0;
  uint64_t key_changed_for_object = 0;  // same object, different bytes than its previous upload (runtime patch)
  uint64_t manifest_hit[2] = {0, 0}, manifest_miss[2] = {0, 0};
  uint64_t analyze_ns = 0, analyzed = 0, analyze_fail = 0;
  uint64_t query_ok[2] = {0, 0}, query_unavail[2] = {0, 0};
  // compare side (any thread)
  uint64_t cmp_equal[2] = {0, 0}, cmp_diff[2] = {0, 0}, cmp_unavail[2] = {0, 0}, cmp_nr_none[2] = {0, 0};
  uint64_t cmp_diff_known[2] = {0, 0};  // different, but the nr key IS a WP5-captured key (other version / timing)
  uint64_t diff_logged = 0;
  uint64_t last_print_flush = 0, last_print_cmp = 0;
  // data
  std::unordered_map<uint64_t, std::unique_ptr<Entry>> by_fnv[2];                  // fnv -> entry (per stage)
  std::unordered_map<uint64_t, Entry*> by_quick[2];                                // QuickHash^size -> entry
  ShaderKey resident[2];                       // what the CP holds per stage = last IM_LOAD/IM_LOAD_IMMEDIATE in ring order
  bool resident_valid[2] = {false, false};
  std::unordered_map<uint32_t, ShaderKey> obj_key[2];                              // bound object -> latest key
  std::unordered_set<uint64_t> manifest[2];
  bool manifest_loaded = false;
  const uint8_t* base = nullptr;
  uint32_t dev = 0;
  rex::string::StringBuffer disasm;
};

Stats& S() {
  static Stats s;
  return s;
}

bool ReadDeviceWord(const uint8_t* base, uint32_t dev, uint32_t off, uint32_t* out) {
  const uint64_t addr64 = uint64_t(dev) + off;
  if (!base || !out || addr64 > UINT32_MAX || !GuestRangeOk(uint32_t(addr64), 4)) return false;
  *out = Be32(base, uint32_t(addr64));
  return true;
}

bool ReadEntryBindings(const uint8_t* base, uint32_t dev, uint32_t* vs, uint32_t* ps) {
  return ReadDeviceWord(base, dev, kOffVertexShader, vs) && ReadDeviceWord(base, dev, kOffPixelShader, ps);
}

thread_local bool t_have_pre = false;
thread_local uint32_t t_entry_mask = 0;      // flush entry r4 (dirty mask)
thread_local uint32_t t_entry_vs = 0;        // flush entry device+13072
thread_local uint32_t t_entry_ps = 0;        // flush entry device+13068
thread_local bool t_entry_bindings_readable = false;
thread_local uint32_t t_entry_sp = 0;        // flush entry r1; used to read VS-tail local [sp-272+92]
thread_local bool t_c58_vs = false;          // sub_82CC1C58 was called with r4 == VS object + 872 inside this flush
thread_local bool t_inline_after_c58 = false;  // an inline VS upload (sub_82CC1B00) followed that call
thread_local uint32_t t_vs_uploads = 0;      // inline VS uploads (sub_82CC1B00 calls that emitted) inside the current flush

void LoadManifest(Stats& s) {
  s.manifest_loaded = true;
  std::string path;
  if (const char* m = std::getenv("WWE2K14_D3D_MANIFEST")) path = m;
  else if (const char* d = std::getenv("WWE2K14_NR_SHADERS")) path = std::string(d) + "/manifest.tsv";
  if (path.empty()) return;
  std::ifstream f(path);
  std::string line;
  uint64_t n = 0;
  while (std::getline(f, line)) {
    const size_t t1 = line.find('\t');
    if (t1 == std::string::npos) continue;
    const size_t t2 = line.find('\t', t1 + 1);
    const std::string key = line.substr(0, t1);
    const std::string stage = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
    const int st = stage == "vs" ? 0 : stage == "ps" ? 1 : -1;
    if (st < 0) continue;
    s.manifest[st].insert(std::strtoull(key.c_str(), nullptr, 16));
    ++n;
  }
  REXLOG_INFO("[d3d-shader] manifest {}: {} rows ({}), FNV key == manifest key for both stages", path, n,
              f.is_open() ? "ok" : "NOT FOUND");
}

// Effective address / size of the span the game's flush computes from a bound object (WP5 analysis formula), for the
// sub-block records i = 0..3. Returns true and fills the outputs when one record reproduces the emitted packet.
bool MatchObjectRecord(const uint8_t* base, uint32_t obj, uint32_t pkt_phys, uint32_t pkt_dwords, uint32_t* guest_addr) {
  if (obj < 0x10000u || obj >= 0xE0000000u) return false;
  const uint32_t code_base = Be32(base, obj + 32);
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t off = Be32(base, obj + 896 + 8 * i);
    if (off < 0x380u || off >= 0x8000u || (off & 3)) continue;
    const uint32_t r = obj + off;
    const uint32_t a = code_base + Be32(base, r + 872);
    const uint32_t bytes = Be32(base, r + 876);
    const uint32_t phys = (a & 0x1FFFFFFFu) + ((((a >> 20) & 0xFFFu) + 512u) & 0x1000u);
    if ((phys & ~3u) == (pkt_phys & ~3u) && (bytes >> 2) == pkt_dwords) {
      *guest_addr = a;
      return true;
    }
  }
  return false;
}

// Failed AnalyzeUcode (the SDK disassembler throws fmt::format_error on undefined vertex fetch formats, i.e. on bytes
// that are not a valid shader): count, rate-limited log, raw dump of the first kMaxFailDumps blobs.
void ReportAnalyzeFail(Stats& s, uint32_t stage, uint64_t fnv, const std::vector<uint8_t>& raw, const char* what) {
  const uint64_t n = ++s.analyze_fail;
  if (n <= 8 || (n & (n - 1)) == 0)  // first 8, then powers of two
    REXLOG_WARN("[d3d-shader] AnalyzeUcode failed #{} stage={} fnv={:016X} bytes={}: {}", n,
                stage ? "ps" : "vs", fnv, raw.size(), what);
  if (n > kMaxFailDumps) return;
  const char* dir = std::getenv("WWE2K14_NR_OUT");
  char name[64];
  std::snprintf(name, sizeof(name), "d3d-shader-fail-%s-%016llX.bin", stage ? "ps" : "vs", (unsigned long long)fnv);
  const std::string path = (dir && *dir) ? std::string(dir) + "/" + name : std::string(name);
  std::ofstream f(path, std::ios::binary);
  if (f) f.write(reinterpret_cast<const char*>(raw.data()), std::streamsize(raw.size()));
}

Entry* Intern(Stats& s, uint32_t stage, const uint8_t* raw, uint32_t bytes, bool* is_new) {
  const uint64_t q = QuickHash(raw, bytes) ^ (uint64_t(bytes) << 48);
  auto it = s.by_quick[stage].find(q);
  if (it != s.by_quick[stage].end() && it->second->raw.size() == bytes && !std::memcmp(it->second->raw.data(), raw, bytes)) {
    ++s.memo_hits;
    *is_new = false;
    return it->second;
  }
  const uint64_t fnv = FnvRaw(raw, bytes);
  auto f = s.by_fnv[stage].find(fnv);
  if (f != s.by_fnv[stage].end()) {  // same key, different quick-hash bucket (collision on q): reuse the entry
    *is_new = false;
    return f->second.get();
  }
  auto e = std::make_unique<Entry>();
  e->fnv = fnv;
  e->stage = uint8_t(stage);
  e->raw.assign(raw, raw + bytes);
  e->dwords.resize(bytes / 4);
  for (uint32_t i = 0; i < bytes / 4; ++i) {
    uint32_t v;
    std::memcpy(&v, raw + 4 * i, 4);
    e->dwords[i] = __builtin_bswap32(v);
  }
  e->in_manifest = s.manifest[stage].count(fnv) != 0;
  const auto t0 = std::chrono::steady_clock::now();
  e->shader = std::make_unique<rex::graphics::Shader>(
      stage == 0 ? rex::graphics::xenos::ShaderType::kVertex : rex::graphics::xenos::ShaderType::kPixel, fnv,
      reinterpret_cast<const uint32_t*>(e->raw.data()), bytes / 4, std::endian::big);
  try {
    e->shader->AnalyzeUcode(s.disasm);
    e->analyzed_ok = true;
  } catch (const std::exception& ex) {
    ReportAnalyzeFail(s, stage, fnv, e->raw, ex.what());
  } catch (...) {
    ReportAnalyzeFail(s, stage, fnv, e->raw, "unknown exception");
  }
  if (!e->analyzed_ok) e->shader.reset();  // half-analysed Shader is never exposed; the key/bytes stay valid identity
  s.analyze_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  ++s.analyzed;
  Entry* raw_e = e.get();
  s.by_fnv[stage][fnv] = std::move(e);
  s.by_quick[stage][q] = raw_e;
  ++s.new_keys[stage];
  if (raw_e->in_manifest) ++s.manifest_hit[stage]; else ++s.manifest_miss[stage];
  *is_new = true;
  return raw_e;
}

// Interns the microcode of one exact load and makes it the stage's resident key (s.mu held). `code_addr` is the physical
// address of a pointer load (PS) or the ring address of the inline code (VS); `object` the bound D3D shader object.
void RecordLoad(Stats& s, uint32_t stage, const uint8_t* code, uint32_t dwords, uint32_t code_addr, uint32_t object,
                uint32_t guest_addr, bool imm) {
  ++s.flush_loads;
  ++s.loads[stage];
  bool is_new = false;
  Entry* e = Intern(s, stage, code, dwords * 4, &is_new);
  ShaderKey k;
  k.fnv = e->fnv;
  k.object = object;
  k.code_addr = code_addr;
  k.bytes = dwords * 4;
  k.stage = uint8_t(stage);
  k.immediate = imm ? 1 : 0;
  if (!object) {
    ++s.obj_null;
  } else if (imm) {
    // inline code is a patched copy: it never equals the object's record span; not counted as nomatch
  } else {
    k.guest_addr = guest_addr;  // derived from the bound object's record, so the span matches by construction
    k.obj_span_match = 1;
    ++s.obj_match;
  }
  s.resident[stage] = k;  // CP semantics: the last load in execution order is what runs, whatever object is bound
  s.resident_valid[stage] = true;
  if (object) {
    auto it = s.obj_key[stage].find(object);
    if (it != s.obj_key[stage].end() && it->second.fnv != k.fnv) ++s.key_changed_for_object;
    s.obj_key[stage][object] = k;
  }
  if (s.flush_loads <= 12)
    REXLOG_INFO("[d3d-shader] load #{} stage={} obj=0x{:08X} phys=0x{:08X} bytes={} fnv={:016X} new={} manifest={} "
                "obj_span_match={} guest=0x{:08X} imm={}",
                s.flush_loads, stage ? "ps" : "vs", k.object, code_addr, k.bytes, k.fnv, is_new ? 1 : 0,
                e->in_manifest ? 1 : 0, k.obj_span_match, k.guest_addr, imm ? 1 : 0);
}

void PrintSummaryLocked(Stats& s, const char* why) {
  REXLOG_INFO(
      "[d3d-shader] {} flushes={} with_load={} no_load={} | exact vs={} vs_reject={} "
      "vs_noemit={} ps={} ps_reject={} ps_none={} | vs_ptr exact={} reject={} flushes={} | loads vs={} ps={} unique_keys vs={} ps={} "
      "memo_hits={} manifest(hit/miss) vs={}/{} ps={}/{} | obj_span_match={} nomatch={} null_obj={} object_bytes_changed={} "
      "resident!=bound_obj_key={} analyze={}x/{:.1f}ms analyze_fail={} | CurrentShaderKey ok vs={} ps={} unavail vs={} ps={} | "
      "COMPARE vs equal={} diff={} (nr_key_known={}) unavail={} nr_none={} ; ps equal={} diff={} (nr_key_known={}) unavail={} "
      "nr_none={}",
      why, s.flushes, s.flush_loads, s.flush_noload,
      s.vs_exact, s.vs_reject, s.vs_noemit, s.ps_exact, s.ps_reject, s.ps_none,
      s.vsp_exact, s.vsp_reject, s.vsp_flush, s.loads[0], s.loads[1], s.new_keys[0],
      s.new_keys[1], s.memo_hits, s.manifest_hit[0],
      s.manifest_miss[0], s.manifest_hit[1], s.manifest_miss[1], s.obj_match, s.obj_nomatch, s.obj_null,
      s.key_changed_for_object, s.resident_obj_mismatch, s.analyzed, double(s.analyze_ns) / 1e6, s.analyze_fail,
      s.query_ok[0], s.query_ok[1], s.query_unavail[0], s.query_unavail[1], s.cmp_equal[0], s.cmp_diff[0],
      s.cmp_diff_known[0], s.cmp_unavail[0], s.cmp_nr_none[0], s.cmp_equal[1], s.cmp_diff[1], s.cmp_diff_known[1],
      s.cmp_unavail[1], s.cmp_nr_none[1]);
}

}  // namespace

bool ShaderIdentityEnabled() { return ObserveEnabled(); }

// Ordering of CurrentShaderKey ("what the CP executes for the next draw") across the two sources:
//  - VS: the inline upload is written and recorded at its own call (post-hook of sub_82CC1B00, below), i.e. in the order
//    the game appends it to the ring, once per draw whose VS needs patching; a memo hit in the game emits nothing and the
//    resident VS persists. The flush calls the writer after its PS block, so the VS record is already final at flush exit.
//  - PS: a pointer IM_LOAD built from the bound PS object's record. The flush emits at most one per call, so it is
//    recorded once per flush, at flush exit, from the same device state the game used. VS and PS are separate stages;
//    CurrentShaderKey is queried after the draw emitter returned, i.e. after both sources of the preceding flush are in.
void ShaderIdentityPreFlush(const uint8_t* guest_base, uint32_t device, uint32_t entry_mask, uint32_t entry_sp) {
  if (!ObserveEnabled()) return;
  t_entry_sp = entry_sp;
  t_c58_vs = false;
  t_inline_after_c58 = false;
  t_entry_mask = entry_mask;
  t_entry_bindings_readable = ReadEntryBindings(guest_base, device, &t_entry_vs, &t_entry_ps);
  t_vs_uploads = 0;
  t_have_pre = true;
}

void ShaderIdentityOnFlush(const uint8_t* guest_base, uint32_t device) {
  if (!ObserveEnabled()) return;
  Stats& s = S();
  const bool have_pre = t_have_pre;
  const uint32_t mask = t_entry_mask;
  const uint32_t entry_vs = t_entry_vs;
  const uint32_t entry_ps = t_entry_ps;
  const bool entry_bindings_readable = t_entry_bindings_readable;
  t_have_pre = false;
  const uint32_t vs_uploads = t_vs_uploads;

  std::lock_guard<std::mutex> lk(s.mu);
  if (!s.manifest_loaded) LoadManifest(s);
  s.base = guest_base;
  s.dev = device;
  ++s.flushes;
  const uint64_t loads_before = s.flush_loads;

  const bool should_load_ps = have_pre && entry_bindings_readable && ps_load::ShouldLoad(mask, entry_vs, entry_ps);
  if (have_pre && entry_bindings_readable && (mask & ps_load::kPsDirty) && !entry_ps) ++s.ps_none;

  if (should_load_ps) {
    // PS IM_LOAD source from device state (85:47035-47123): obj = dev+13068, sub = obj + obj[+64];
    // dword0 = fixup(sub[+40] + obj[+24]) | 1, dword1 = sub[+44] >> 2; fixup(v) = (v & 0x1FFFFFFE) + ((((v >> 20) & 0xFFF) + 512) & 0x1000).
    const uint32_t obj = entry_ps;
    bool ok = false;
    if (!obj) {
      ++s.ps_none;
    } else if (obj >= 0x10000u && obj < 0xE0000000u && GuestRangeOk(obj, 128)) {
      const uint32_t off = Be32(guest_base, obj + 64);
      if (off >= 0x40u && off < 0x10000u && !(off & 3) && GuestRangeOk(obj + off, 48)) {
        const uint32_t sub = obj + off;
        const uint32_t v = Be32(guest_base, sub + 40) + Be32(guest_base, obj + 24);
        const uint32_t phys = ((v & 0x1FFFFFFEu) + ((((v >> 20) & 0xFFFu) + 512u) & 0x1000u)) & 0x1FFFFFFCu;
        const uint32_t dwords = (Be32(guest_base, sub + 44) >> 2) & 0x3FFFFFFFu;
        if (dwords && dwords <= kMaxLoadDwords && phys < kPhysLimit && GuestRangeOk(kPhysViewBase + phys, 4ull * dwords)) {
          ok = true;
          ++s.ps_exact;
          RecordLoad(s, 1, guest_base + kPhysViewBase + phys, dwords, phys, obj, v, false);
        }
      }
    }
    if (obj && !ok) ++s.ps_reject;
  }
  // VS pointer IM_LOADs of the flush tail (85:47985-48210), derived from device state. Facts:
  //  - r15 (sub-block index) is -1 unless the VS block ended with a memo hit (85:47471) or the no-decl / record-flag-0x40
  //    path (85:47545); in both no inline upload follows the VS sub_82CC1C58 call (85:47424). Then r15 = r23, which is 1
  //    only when the PS object is null and obj[+872] & 0x20 (85:46954-46962), else 0 (85:46889).
  //  - [sp+92] (second variant index, -1 or 1) is a stack local of the dead flush frame (85:46846, 85:47531).
  //  - tail: if dev[11068] & 0x40 (85:47690): the [sp+92] load (if != -1) then the r15 load (if != -1); else only r15.
  //  - load of index i: phys = fixup(obj[+32] + rec[+872]), dwords = rec[+876] >> 2, rec = obj + obj[+896+8*i].
  {
    const uint32_t vobj = Be32(guest_base, device + kOffVertexShader);
    int idx[2] = {-1, -1};
    uint32_t nidx = 0;
    int r15 = -1, s92 = -1;
    if (vobj >= 0x10000u && vobj < 0xE0000000u && GuestRangeOk(vobj, 1024)) {
      if (t_c58_vs && !t_inline_after_c58) {
        const bool ps_null = Be32(guest_base, device + kOffPixelShader) == 0;
        r15 = (ps_null && (Be32(guest_base, vobj + 872) & 0x20)) ? 1 : 0;
      }
      if (t_entry_sp > 0x10000u && GuestRangeOk(t_entry_sp - 272, 272)) {
        const int32_t v = int32_t(Be32(guest_base, t_entry_sp - 272 + 92));
        if (v >= 0 && v < 4) s92 = v;
      }
      const bool flag40 = (guest_base[device + 11068] & 0x40) != 0;
      if (flag40 && s92 >= 0) idx[nidx++] = s92;
      if (r15 >= 0) idx[nidx++] = r15;
    }
    for (uint32_t k = 0; k < nidx; ++k) {
      const uint32_t off = Be32(guest_base, vobj + 896 + 8 * uint32_t(idx[k]));
      bool ok = false;
      if (off >= 0x40u && off < 0x10000u && !(off & 3) && GuestRangeOk(vobj + off + 872, 8)) {
        const uint32_t v = Be32(guest_base, vobj + 32) + Be32(guest_base, vobj + off + 872);
        const uint32_t phys = ((v & 0x1FFFFFFFu) + ((((v >> 20) & 0xFFFu) + 512u) & 0x1000u)) & 0x1FFFFFFCu;
        const uint32_t dwords = (Be32(guest_base, vobj + off + 876) >> 2) & 0x3FFFFFFFu;
        if (dwords && dwords <= kMaxLoadDwords && phys < kPhysLimit && GuestRangeOk(kPhysViewBase + phys, 4ull * dwords)) {
          ok = true;
          ++s.vsp_exact;
          RecordLoad(s, 0, guest_base + kPhysViewBase + phys, dwords, phys, vobj, v, false);
        }
      }
      if (!ok) ++s.vsp_reject;
    }
    if (nidx) ++s.vsp_flush;
  }
  if (s.flush_loads == loads_before && !vs_uploads) ++s.flush_noload;
  if (s.flushes - s.last_print_flush >= 20000) {
    s.last_print_flush = s.flushes;
    PrintSummaryLocked(s, "summary");
  }
}

// Post-hook on the inline VS writer sub_82CC1B00 (85:46327; called only from the flush, 85:47493 / 85:47610).
// Entry args: r3 dev, r5 VS object, r10 sub-block index. The writer reserves len+5 dwords in one segment (sub_82CBA9D0),
// then stores 0xC0003B00, 256, 0xC0002B00|((len+1)<<16), 0, len, copies the code, patches it in place and stores
// dev+48 = address of the last code dword (85:46361-46437). Hence start = cursor - 4*len - 16, and the GPU-executed
// (patched) code is the len dwords after the d1 word.
void ShaderIdentityHookedVSUpload(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  if (t_c58_vs) t_inline_after_c58 = true;
  const uint32_t dev = ctx.r3.u32, obj = ctx.r5.u32, idx = ctx.r10.u32;
  const uint32_t cursor_before = Be32(base, dev + kOffRingCursor);
  uint32_t len = 0;  // sub[+876] >> 2, the game's own length source (85:46347-46351)
  if (obj >= 0x10000u && obj < 0xE0000000u && idx < 4 && GuestRangeOk(obj + 896, 40)) {
    const uint32_t off = Be32(base, obj + 896 + 8 * idx);
    if (off >= 0x40u && off < 0x10000u && !(off & 3) && GuestRangeOk(obj + off + 872, 8)) len = Be32(base, obj + off + 876) >> 2;
  }
  original(ctx, base);
  const uint32_t cursor = Be32(base, dev + kOffRingCursor);

  Stats& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  if (cursor == cursor_before) {  // the reserve failed or nothing was written
    ++s.vs_noemit;
    return;
  }
  ++t_vs_uploads;
  const uint32_t pkt = len && len <= kMaxLoadDwords ? cursor - 4 * len - 16 : 0;  // first dword of the 0x3B pair
  if (!pkt || (cursor & 3) || cursor < 4 * len + 0x2000u || !GuestRangeOk(pkt, 4ull * (len + 5)) ||
      Be32(base, pkt) != kPairHdr || Be32(base, pkt + 4) != kPairVal ||
      Be32(base, pkt + 8) != (kImmHdrBase | (((len + 1) & 0x3FFFu) << 16)) || Be32(base, pkt + 12) != 0 ||
      Be32(base, pkt + 16) != (len & 0x3FFFu)) {
    ++s.vs_reject;
    return;
  }
  if (!s.manifest_loaded) LoadManifest(s);
  s.base = base;
  s.dev = dev;
  ++s.vs_exact;
  const uint32_t code = pkt + 20;
  RecordLoad(s, 0, base + code, len, code & 0x1FFFFFFCu, Be32(base, dev + kOffVertexShader), 0, true);
}

// sub_82CC1C58 (constant-block loader, header 0xC0022F00): called by the flush for the PS (r4 = PS obj + 40) and for the VS
// (r4 = VS obj + 872, 85:47424 / 85:47590). Only the VS call marks the VS block.
void ShaderIdentityHookedConstLoad(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  if (t_have_pre) {
    const uint32_t vobj = Be32(base, ctx.r3.u32 + kOffVertexShader);
    if (vobj && ctx.r4.u32 == vobj + 872) {
      t_c58_vs = true;
      t_inline_after_c58 = false;
    }
  }
  if (ShadowEnabled()) ShadowConstLoad(ctx, base);  // card S3-58: literals from this call (reads r4/r5 memory only)
  original(ctx, base);
}

void ShaderIdentityHookedFlush(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t dev = ctx.r3.u32;
  ShaderIdentityPreFlush(base, dev, ctx.r4.u32, ctx.r1.u32);
  original(ctx, base);
  ShaderIdentityOnFlush(base, dev);
}

bool CurrentShaderKey(uint32_t stage, ShaderKey* out) {
  if (!ObserveEnabled() || stage > 1) return false;
  Stats& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  if (!s.base) {
    ++s.query_unavail[stage & 1];
    return false;
  }
  if (!s.resident_valid[stage]) {
    ++s.query_unavail[stage];
    return false;
  }
  ++s.query_ok[stage];
  ShaderKey k = s.resident[stage];
  const uint32_t obj = Be32(s.base, s.dev + (stage ? kOffPixelShader : kOffVertexShader));
  auto it = obj ? s.obj_key[stage].find(obj) : s.obj_key[stage].end();
  if (it == s.obj_key[stage].end() || it->second.fnv != k.fnv) ++s.resident_obj_mismatch;
  k.object = obj;  // bound object at query time (informational; identity = what the CP last loaded)
  if (out) *out = k;
  return true;
}

const std::vector<uint32_t>* ShaderBytes(uint64_t fnv, uint32_t stage) {
  if (stage > 1) return nullptr;
  Stats& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  auto it = s.by_fnv[stage].find(fnv);
  return it == s.by_fnv[stage].end() ? nullptr : &it->second->dwords;
}

const rex::graphics::Shader* ShaderObject(uint64_t fnv, uint32_t stage) {
  if (stage > 1) return nullptr;
  Stats& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  auto it = s.by_fnv[stage].find(fnv);
  return it == s.by_fnv[stage].end() ? nullptr : it->second->shader.get();
}

void ShaderForEachRaw(const std::function<void(uint32_t, uint64_t, const uint8_t*, uint32_t)>& cb) {
  Stats& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  for (uint32_t st = 0; st < 2; ++st)
    for (const auto& [fnv, e] : s.by_fnv[st]) cb(st, fnv, e->raw.data(), uint32_t(e->raw.size()));
}

void ShaderReplayIntern(uint32_t stage, const uint8_t* raw, uint32_t bytes) {
  if (stage > 1 || !bytes) return;
  Stats& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  bool is_new = false;
  Intern(s, stage, raw, bytes, &is_new);
}

uint64_t ShaderFnv1a64(const uint32_t* dwords, size_t count) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t v = dwords[i];
    const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    for (uint8_t c : b) h = (h ^ c) * 0x100000001B3ull;
  }
  return h;
}

void ShaderIdentityCompare(bool captured_valid, const ShaderKey& captured, uint32_t stage, uint64_t nr_fnv) {
  if (!ObserveEnabled() || stage > 1) return;
  Stats& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  if (!nr_fnv) {
    ++s.cmp_nr_none[stage];
  } else if (!captured_valid) {
    ++s.cmp_unavail[stage];
  } else if (captured.fnv == nr_fnv) {
    ++s.cmp_equal[stage];
  } else {
    ++s.cmp_diff[stage];
    const bool known = s.by_fnv[stage].count(nr_fnv) != 0;
    if (known) ++s.cmp_diff_known[stage];
    if (s.diff_logged++ < kMaxDiffLogs)
      REXLOG_INFO("[d3d-shader] DIFF stage={} wp5_fnv={:016X} nr_fnv={:016X} obj=0x{:08X} phys=0x{:08X} bytes={} "
                  "obj_span_match={} nr_key_seen_by_wp5={}",
                  stage ? "ps" : "vs", captured.fnv, nr_fnv, captured.object, captured.code_addr, captured.bytes,
                  captured.obj_span_match, known ? 1 : 0);
  }
  const uint64_t n = s.cmp_equal[0] + s.cmp_equal[1] + s.cmp_diff[0] + s.cmp_diff[1] + s.cmp_unavail[0] + s.cmp_unavail[1];
  if (n - s.last_print_cmp >= 20000) {
    s.last_print_cmp = n;
    PrintSummaryLocked(s, "summary(compare)");
  }
}

void ShaderIdentityCompareDwords(bool captured_valid, const ShaderKey& captured, uint32_t stage,
                                 const uint32_t* sdk_host_dwords, size_t count) {
  ShaderIdentityCompare(captured_valid, captured, stage,
                        sdk_host_dwords && count ? ShaderFnv1a64(sdk_host_dwords, count) : 0);
}

void ShaderIdentitySummary(const char* why) {
  if (!ObserveEnabled()) return;
  Stats& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  PrintSummaryLocked(s, why ? why : "summary");
}

}  // namespace wwe2k14::d3d
