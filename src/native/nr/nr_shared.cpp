// Native renderer (nr) - shared, input-neutral pieces (Stage 2 WP0, decision 0006). See nr_shared.h.
// The bodies below were moved out of class Frontend in nr_frontend.cpp without behaviour changes.
// Card RT1: the runtime shader translator core (rts_core) depends on the standard library only; a host test compiles
// it alone with -DNR_RTS_CORE_ONLY (#include this file).
#ifndef NR_RTS_CORE_ONLY
#include "nr_shared.h"
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <istream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "nr_constants32.h"  // card G56 B2: RT1 keeps the Int64 constants ABI
#include "rts_wait.h"

#ifndef NR_RTS_CORE_ONLY
#include <rex/graphics/flags.h>
#include <rex/math.h>
#include <rex/logging.h>
#endif

// ---- card RT1: runtime shader translator core (std only) ------------------------------------------------------
// The offline recipe (tools/nr/raw-container.py + patched XenosRecomp + DXC, tools/nr/build-shaders.sh) run as child
// processes, so a runtime contract is byte-identical to an offline one.
namespace nr::rts_core {

std::vector<uint8_t> ReadAll(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint32_t Field(const std::string& line, const char* name) {
  const std::string key = std::string(" ") + name + "=";
  const size_t at = line.find(key);
  return at == std::string::npos ? 0 : uint32_t(std::strtol(line.c_str() + at + key.size(), nullptr, 10));
}

// Contract metadata from the converter's HLSL comments (the one parser for shipped and runtime contracts).
template <typename E>
void ParseContract(std::istream& in, E* e) {
  std::string h;
  while (std::getline(in, h)) {
    if (h.find("// NR_VFETCH address=") != std::string::npos) {
      e->vfetch.push_back({Field(h, "location"), Field(h, "usage"), Field(h, "fetch"), Field(h, "stride"),
                           Field(h, "offset"), Field(h, "format"), Field(h, "signed"), Field(h, "integer")});
    } else if (h.find("// NR_UNSUPPORTED") != std::string::npos) {
      e->unsupported = h.substr(h.find("NR_UNSUPPORTED") + 15);
    } else if (h.find("// NR_TFETCH slot=") != std::string::npos) {
      e->texture_slots.insert(Field(h, "slot"));
      e->texture_dims[Field(h, "slot")] = Field(h, "dim");
    }
  }
}

// build-shaders.sh: a VS whose every vertex fetch has zero stride/offset/format was captured before the D3D library
// patched it; the GPU never runs that microcode.
bool UnpatchedVfetch(const std::string& hlsl) {
  bool any = false;
  for (size_t at = hlsl.find("// NR_VFETCH address="); at != std::string::npos;
       at = hlsl.find("// NR_VFETCH address=", at + 1)) {
    any = true;
    const size_t eol = hlsl.find('\n', at);
    if (hlsl.substr(at, eol == std::string::npos ? std::string::npos : eol - at).find("stride=0 offset=0 format=0 ") ==
        std::string::npos)
      return false;
  }
  return any;
}

uint64_t Fnv(const uint8_t* p, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

// Port of tools/nr/raw-container.py: minimal Xbox 360 shader container (no constant names / declaration /
// interpolator table; marked raw with fieldC 'NR') around big-endian microcode.
std::vector<uint8_t> RawContainer(const uint32_t* host_dwords, size_t count, bool vs) {
  std::vector<uint8_t> b;
  auto be = [&b](uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
  };
  const uint32_t code_bytes = uint32_t(count * 4);
  const uint32_t ct_off = 36, sh_off = ct_off + 32, sh_size = vs ? 36 : 32;
  const uint32_t virt = (sh_off + sh_size + 15) & ~15u;
  for (uint32_t v : {0x102A1100u | (vs ? 1u : 0u), virt, code_bytes, 0x4E520000u, ct_off, 0u, sh_off, 0u, 0u}) be(v);
  for (uint32_t v : {28u, 28u, 0u, 0u, 0u, 0u, 0u, 0u}) be(v);                    // empty ConstantTableContainer
  for (uint32_t v : {0u, code_bytes, 0u, 0u, 0u, 0u}) be(v);                       // physicalOffset, size, ...
  for (uint32_t i = 0; i < (vs ? 3u : 2u); ++i) be(0);                             // VS field18/elements/field20 | PS
  b.resize(virt, 0);
  for (size_t i = 0; i < count; ++i) be(host_dwords[i]);
  return b;
}

// Card RT1 (review rv1): bump on any change to the raw-container layout, the dxc flags or the vfetch handling; mixed
// into the toolchain stamp, so cached contracts of an older recipe are never reused.
constexpr uint64_t kRecipeVersion = 2;
// Child process limit (ms); a tool still running then is killed and the job fails (not marked deterministic).
int g_child_timeout_ms = 60000;

// UTF-8 text of a path for logs and messages (path::string() is the ANSI code page on Windows and can throw).
std::string U8(const std::filesystem::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

// A path from the environment, wide on Windows (getenv is the ANSI code page there). Empty when unset or empty.
std::filesystem::path EnvPath(const char* name) {
#ifdef _WIN32
  const std::wstring w(name, name + std::strlen(name));
  const wchar_t* v = _wgetenv(w.c_str());
#else
  const char* v = std::getenv(name);
#endif
  return v && *v ? std::filesystem::path(v) : std::filesystem::path();
}

// kToolFailed: the tool ran and exited non-zero (deterministic for the same input and toolchain). kError: it could not
// be started, crashed, timed out or could not be waited for.
enum class RunResult { kOk, kToolFailed, kError };
// Runs argv[0] with argv, waits up to g_child_timeout_ms. Arguments are paths so Windows passes them as UTF-16
// (CreateProcessW) and Linux as bytes. lib_dir: shared-library search dir for the child (Linux LD_LIBRARY_PATH; Windows
// finds DLLs next to the exe). log: stdout+stderr (Linux only). win_cwd: the child's current directory (Windows only;
// empty = inherit). Defined at the end of the file.
RunResult RunProcess(const std::vector<std::filesystem::path>& argv, const std::filesystem::path& lib_dir,
                     const std::filesystem::path& log, std::string* err, const std::filesystem::path& win_cwd = {});
std::filesystem::path ExeDir();
unsigned long ProcessId();

struct Toolchain {
  std::filesystem::path dir, xenos, dxc, dxcompiler, common;
  uint64_t stamp = 0;
  std::string why;
};

bool FindToolchain(const std::filesystem::path& dir, Toolchain* t) {
  namespace fs = std::filesystem;
  std::error_code ec;
  t->dir = dir;
#ifdef _WIN32
  t->xenos = dir / "XenosRecomp.exe";
  t->dxc = dir / "dxc.exe";
  t->dxcompiler = dir / "dxcompiler.dll";  // dxil.dll is not used (not shipped): never part of the stamp
#else
  t->xenos = dir / "XenosRecomp";
  t->dxc = fs::exists(dir / "dxc", ec) ? dir / "dxc" : dir / "dxc-linux";
  t->dxcompiler = dir / "libdxcompiler.so";
#endif
  t->common = dir / "shader_common_nr.h";
  for (const fs::path* p : {&t->xenos, &t->dxc, &t->dxcompiler, &t->common}) {
    if (!fs::is_regular_file(*p, ec)) {
      t->why = "missing " + U8(*p);
      return false;
    }
  }
  uint64_t h = Fnv(reinterpret_cast<const uint8_t*>(&kRecipeVersion), sizeof(kRecipeVersion));
  for (const fs::path* p : {&t->xenos, &t->dxc, &t->dxcompiler, &t->common}) {
    const std::vector<uint8_t> bytes = ReadAll(*p);
    h = Fnv(bytes.data(), bytes.size(), h);
  }
  t->stamp = h;
  return true;
}

// Last non-empty line of a tool log (<= 200 chars), " | line", or "" (the work dir is removed after every job).
std::string LogTail(const std::filesystem::path& log) {
  const std::vector<uint8_t> b = ReadAll(log);
  std::string s(b.begin(), b.end());
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  if (s.empty()) return "";
  const size_t nl = s.find_last_of('\n');
  s = nl == std::string::npos ? s : s.substr(nl + 1);
  return " | " + s.substr(0, 200);
}

// Microcode (host-order dwords) -> HLSL contract + SPIR-V, in `work` (created; left for the caller to remove).
// permanent: set true when the failure is deterministic (a tool exited non-zero, unpatched vfetch).
bool Translate(const Toolchain& t, const uint32_t* host_dwords, size_t count, bool vs, const std::filesystem::path& work,
               std::string* hlsl, std::vector<uint8_t>* spirv, std::string* err, bool* permanent = nullptr) {
  namespace fs = std::filesystem;
  if (permanent) *permanent = false;
  auto fail = [&](const std::string& e, bool perm) {
    *err = e;
    if (permanent) *permanent = perm;
    return false;
  };
  std::error_code ec;
  fs::create_directories(work, ec);
  const fs::path in = work / "in.bin", out_hlsl = work / "out.hlsl", out_spv = work / "out.spv", log = work / "log.txt";
  {
    const std::vector<uint8_t> c = RawContainer(host_dwords, count, vs);
    std::ofstream f(in, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(c.data()), std::streamsize(c.size()));
    f.close();
    if (!f) return fail("cannot write " + U8(in), false);
  }
  std::string e;
#ifdef _WIN32
  // XenosRecomp reads its arguments in the ANSI code page (a path with other characters, e.g. a CJK user name, fails
  // with 0xC0000409 even though CreateProcessW passed UTF-16): it runs in `work` with relative names and a copy of the
  // common header there (the header path only reaches an HLSL comment).
  fs::copy_file(t.common, work / "shader_common_nr.h", fs::copy_options::overwrite_existing, ec);
  if (ec) return fail("cannot copy " + U8(t.common) + ": " + ec.message(), false);
  const fs::path cwd = work, x_in = "in.bin", x_out = "out.hlsl", x_common = "shader_common_nr.h";
#else
  const fs::path cwd, x_in = in, x_out = out_hlsl, x_common = t.common;
#endif
  RunResult rr = RunProcess({t.xenos, x_in, x_out, x_common}, t.dir, log, &e, cwd);
  if (rr != RunResult::kOk) return fail("XenosRecomp: " + e + LogTail(log), rr == RunResult::kToolFailed);
  const std::vector<uint8_t> h = ReadAll(out_hlsl);
  if (h.empty()) return fail("XenosRecomp wrote no HLSL", false);
  hlsl->assign(h.begin(), h.end());
  if (vs && UnpatchedVfetch(*hlsl)) return fail("unpatched-vfetch", true);
  std::vector<fs::path> dxc = {t.dxc, "-HV", "2021", "-all-resources-bound", "-spirv", "-fvk-use-dx-layout",
                               "-Qstrip_debug", "-I", t.dir, "-T", vs ? "vs_6_0" : "ps_6_0"};
  if (vs) dxc.push_back("-DNR_VS=1");
  for (const fs::path& a : {fs::path("-Fo"), out_spv, out_hlsl}) dxc.push_back(a);
  rr = RunProcess(dxc, t.dir, log, &e, cwd);
  if (rr != RunResult::kOk) return fail("dxc: " + e + LogTail(log), rr == RunResult::kToolFailed);
  *spirv = ReadAll(out_spv);
  if (spirv->empty()) return fail("dxc wrote no SPIR-V", false);
  return true;
}

// ---- disk cache: <dir>/<KEY>.<stage>.spv, then <KEY>.<stage>.hlsl (its presence marks a complete entry) whose last
// line records the .spv length and FNV-1a 64; <KEY>.<stage>.fail = an earlier deterministic failure (holds the error).
std::atomic<uint64_t> g_tmp_counter{0};
constexpr const char* kSpvMarker = "// NR_RTS_SPV bytes=";

// Unique temp file (pid + counter) + rename. false (temp removed) on any write/flush/close/rename error.
bool WriteFileAtomic(const std::filesystem::path& dst, const void* p, size_t n) {
  namespace fs = std::filesystem;
  fs::path tmp = dst;
  tmp += "." + std::to_string(ProcessId()) + "." + std::to_string(g_tmp_counter.fetch_add(1)) + ".tmp";
  std::error_code ec;
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (f) {
      f.write(static_cast<const char*>(p), std::streamsize(n));
      f.flush();
    }
    const bool good = bool(f);
    f.close();
    if (!good || f.fail()) {
      fs::remove(tmp, ec);
      return false;
    }
  }
  fs::rename(tmp, dst, ec);
  if (ec) {  // some runtimes do not replace an existing target on Windows: remove it, then rename again
    std::error_code ec2;
    fs::remove(dst, ec2);
    ec.clear();
    fs::rename(tmp, dst, ec);
  }
  if (ec) {
    std::error_code ec2;
    fs::remove(tmp, ec2);
    return false;
  }
  return true;
}

bool CachePut(const std::filesystem::path& dir, const std::string& name, const std::string& hlsl,
              const std::vector<uint8_t>& spirv) {
  if (!WriteFileAtomic(dir / (name + ".spv"), spirv.data(), spirv.size())) return false;
  char marker[96];
  std::snprintf(marker, sizeof(marker), "%s%zu fnv=%016llX\n", kSpvMarker, spirv.size(),
                (unsigned long long)Fnv(spirv.data(), spirv.size()));
  std::string h = hlsl;
  if (!h.empty() && h.back() != '\n') h += '\n';
  h += marker;
  return WriteFileAtomic(dir / (name + ".hlsl"), h.data(), h.size());
}

enum class CacheResult { kMiss, kHit, kBad };
// kBad: an entry exists but its .spv length / hash does not match the marker (or the marker is missing); both files
// were removed so the caller translates again. `why` says what failed.
CacheResult CacheGet(const std::filesystem::path& dir, const std::string& name, std::string* hlsl,
                     std::vector<uint8_t>* spirv, std::string* why) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path ph = dir / (name + ".hlsl"), ps = dir / (name + ".spv");
  if (!fs::exists(ph, ec)) return CacheResult::kMiss;
  const std::vector<uint8_t> h = ReadAll(ph);
  hlsl->assign(h.begin(), h.end());
  *spirv = ReadAll(ps);
  const size_t at = hlsl->rfind(kSpvMarker);
  unsigned long long bytes = 0, fnv = 0;
  bool parsed = false;
  if (at != std::string::npos) {
    char* end = nullptr;
    bytes = std::strtoull(hlsl->c_str() + at + std::strlen(kSpvMarker), &end, 10);
    if (end && std::strncmp(end, " fnv=", 5) == 0) {
      const char* hex = end + 5;
      fnv = std::strtoull(hex, &end, 16);
      parsed = end == hex + 16;
    }
  }
  if (!parsed) {
    *why = "no spv marker";
  } else if (bytes != spirv->size()) {
    *why = "spv length " + std::to_string(spirv->size()) + " != " + std::to_string(bytes);
  } else if (fnv != Fnv(spirv->data(), spirv->size())) {
    *why = "spv hash mismatch";
  } else {
    return CacheResult::kHit;
  }
  fs::remove(ph, ec);
  fs::remove(ps, ec);
  hlsl->clear();
  spirv->clear();
  return CacheResult::kBad;
}

std::filesystem::path FailMarker(const std::filesystem::path& dir, const std::string& name) {
  return dir / (name + ".fail");
}
bool FailMarked(const std::filesystem::path& dir, const std::string& name) {
  std::error_code ec;
  return std::filesystem::exists(FailMarker(dir, name), ec);
}
bool MarkFailed(const std::filesystem::path& dir, const std::string& name, const std::string& err) {
  const std::string text = err + "\n";
  return WriteFileAtomic(FailMarker(dir, name), text.data(), text.size());
}

}  // namespace nr::rts_core

#ifndef NR_RTS_CORE_ONLY
namespace nr::shared {

using namespace rex::graphics;

namespace {
using rts_core::ReadAll;
}  // namespace

// ---- endian helpers -------------------------------------------------------------------------------------------

void SwapDwords(uint8_t* p, size_t bytes, uint32_t endian) {
  for (size_t i = 0; i + 4 <= bytes; i += 4) {
    uint32_t v;
    std::memcpy(&v, p + i, 4);
    switch (endian) {
      case 1: v = (v & 0x00FF00FFu) << 8 | (v & 0xFF00FF00u) >> 8; break;
      case 2: v = Swap32(v); break;
      case 3: v = v << 16 | v >> 16; break;
      default: break;
    }
    std::memcpy(p + i, &v, 4);
  }
}

void CopySwapDwords(uint8_t* dst, const uint8_t* src, size_t bytes, uint32_t endian) {
  const size_t n = bytes / 4;
  switch (endian) {
    case 1:
      for (size_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        v = (v & 0x00FF00FFu) << 8 | (v & 0xFF00FF00u) >> 8;
        std::memcpy(dst + i * 4, &v, 4);
      }
      break;
    case 2:
      for (size_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        v = __builtin_bswap32(v);
        std::memcpy(dst + i * 4, &v, 4);
      }
      break;
    case 3:
      for (size_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, src + i * 4, 4);
        v = v << 16 | v >> 16;
        std::memcpy(dst + i * 4, &v, 4);
      }
      break;
    default:
      std::memcpy(dst, src, n * 4);
      break;
  }
  if (bytes > n * 4) std::memcpy(dst + n * 4, src + n * 4, bytes - n * 4);  // a partial last dword is copied unswapped
}


uint64_t Fnv1a64BigEndian(const uint32_t* dwords, size_t count) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t v = dwords[i];
    const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    for (uint8_t c : b) h = (h ^ c) * 0x100000001B3ull;
  }
  return h;
}

// ---- shader set -----------------------------------------------------------------------------------------------

ShaderSet& ShaderSet::Get() {
  static ShaderSet set;
  return set;
}

size_t ShaderSet::Load(const std::filesystem::path& dir, const std::filesystem::path& dir_b, bool f24_conservative) {
  const bool dxil = nb::BackendWantsDxil();
  if (loaded_ && dir_ == dir && dir_b_ == dir_b && f24c_ == f24_conservative && dxil_ == dxil) return map_.size();
  loaded_ = true;
  dir_ = dir;
  dir_b_ = dir_b;
  f24c_ = f24_conservative;
  dxil_ = dxil;
  saw_manifest_row_ = false;
  format_matches_backend_ = true;
  map_.clear();
  f24c_loaded_ = set_b_loaded_ = set_b_missing_ = 0;
  std::filesystem::path primary_dir = dir;
  const char* dxil_fix = std::getenv("WWE2K14_NR_DXIL_FIX");
  if (dxil_ && (!dxil_fix || std::strcmp(dxil_fix, "0") != 0)) {
    auto fix_dir = dir;
    fix_dir += "-fix";
    if (std::filesystem::exists(fix_dir) && std::filesystem::exists(fix_dir / "manifest.tsv")) {
      primary_dir = fix_dir;
      REXLOG_INFO("[nr] DXIL fix set in use: {}", rts_core::U8(primary_dir));
    } else {
      REXLOG_INFO("[nr] DXIL fix set not found, using {}", rts_core::U8(dir));
    }
  }
  std::ifstream manifest(primary_dir / "manifest.tsv");
  std::string line;
  while (std::getline(manifest, line)) {
    std::istringstream in(line);
    std::string key, stage, spv, source;
    in >> key >> stage >> spv >> source;
    ShaderEntry e;
    const bool row_dxil = spv.size() > 5 && spv.compare(spv.size() - 5, 5, ".dxil") == 0;
    // Load only the artifact format selected for the backend; never substitute SPIR-V for a D3D12 shader or vice versa.
    if (row_dxil != dxil_) {
      format_matches_backend_ = false;
      continue;
    }
    saw_manifest_row_ = true;
    e.spirv = ReadAll(primary_dir / spv);
    if (e.spirv.empty()) {
      // A manifest row without a SPIR-V file is not a contract: leave the key missing so the draw takes the
      // missing-contract path (skipped + microcode dump) instead of creating a shader from no data.
      REXLOG_WARN("[nr] manifest lists {} {} but {} is missing or empty; not loaded", key, stage, spv);
      continue;
    }
    if (stage == "ps" && dxil_) {
      // Ported DXIL siblings and transformed contract fallback from
      // <wwe13-recomp-nr>/src/nr/nr_frontend.cpp:974-994.
      const std::string base = key + ".ps";
      auto load = [&](const std::string& name, std::vector<uint8_t>* out) {
        if (std::filesystem::exists(primary_dir / name)) *out = ReadAll(primary_dir / name);
      };
      load(base + ".f24.s0.dxil", &e.spirv_f24);
      load(base + ".s2.dxil", &e.spirv_s2);
      load(base + ".f24.s2.dxil", &e.spirv_f24_s2);
    } else if (stage == "ps") {
      if (key == "00000000F24D0001" && f24_conservative) {  // card P8: depth-only library shader
        e.spirv_f24c = ReadAll(dir / (key + ".ps.f24c.spv"));
        if (!e.spirv_f24c.empty()) ++f24c_loaded_;
      }
      if (std::filesystem::exists(dir / (key + ".ps.f24.spv"))) {
        e.spirv_f24 = ReadAll(dir / (key + ".ps.f24.spv"));
        // Card P8 (WWE2K14_NR_F24_CONSERVATIVE=1): same float24 depth, declared SV_DepthLessEqual
        // (tools/nr/f24c-variants.sh) so the GPU may reject fragments before the shader runs.
        if (f24_conservative && std::filesystem::exists(dir / (key + ".ps.f24c.spv"))) {
          e.spirv_f24c = ReadAll(dir / (key + ".ps.f24c.spv"));
          if (!e.spirv_f24c.empty()) ++f24c_loaded_;
        }
      }
    }
    e.source = source;
    std::ifstream hlsl(primary_dir / (key + "." + stage + ".hlsl"));
    // DXIL contracts are transformed `.s0.hlsl` sources under `src/`; all formats share the v1.4.4 parser.
    if (!hlsl.is_open() && dxil_) hlsl.open(primary_dir / "src" / (key + "." + stage + ".s0.hlsl"));
    rts_core::ParseContract(hlsl, &e);
    if (!dir_b.empty()) {  // card P27: same file names in the second set; a missing file keeps the first set's
      auto load_b = [&](const std::vector<uint8_t>& a, std::vector<uint8_t>* b, const std::string& name) {
        if (a.empty()) return;
        *b = ReadAll(dir_b / name);
        if (b->empty()) ++set_b_missing_;
        else ++set_b_loaded_;
      };
      load_b(e.spirv, &e.spirv_b, spv);
      load_b(e.spirv_f24, &e.spirv_f24_b,
             dxil_ ? key + ".ps.f24.s0.dxil" : key + ".ps.f24.spv");
      load_b(e.spirv_f24c, &e.spirv_f24c_b, key + ".ps.f24c.spv");
      if (dxil_ && stage == "ps") {
        load_b(e.spirv_s2, &e.spirv_s2_b, key + ".ps.s2.dxil");
        load_b(e.spirv_f24_s2, &e.spirv_f24_s2_b, key + ".ps.f24.s2.dxil");
      }
    }
    map_[std::strtoull(key.c_str(), nullptr, 16) ^ (stage == "vs" ? 1ull : 0ull)] = std::move(e);
  }
  REXLOG_INFO("[nr] {} shader contracts from {} (conservative float24 variants: {})", map_.size(),
              dir.string(), f24c_loaded_);
  if (!dir_b.empty())
    REXLOG_INFO("[nr] shader set B {}: {} SPIR-V loaded, {} missing (those keep set A)", dir_b.string(),
                set_b_loaded_, set_b_missing_);
  return map_.size();
}

bool VertexFormat(const VFetch& f, nb::VtxFormat* out, bool* half_words) {
  using V = nb::VtxFormat;
  *half_words = false;
  const bool s = f.is_signed, integer = f.integer;
  switch (xenos::VertexFormat(f.format)) {
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10: *out = V::kR32_UINT; return true;
    case xenos::VertexFormat::k_32_FLOAT: *out = V::kR32_SFLOAT; return true;
    case xenos::VertexFormat::k_32_32_FLOAT: *out = V::kR32G32_SFLOAT; return true;
    case xenos::VertexFormat::k_32_32_32_FLOAT: *out = V::kR32G32B32_SFLOAT; return true;
    case xenos::VertexFormat::k_32_32_32_32_FLOAT: *out = V::kR32G32B32A32_SFLOAT; return true;
    case xenos::VertexFormat::k_16_16_FLOAT: *out = V::kR16G16_SFLOAT; return true;
    case xenos::VertexFormat::k_16_16_16_16_FLOAT: *out = V::kR16G16B16A16_SFLOAT; return true;
    case xenos::VertexFormat::k_8_8_8_8:
      *out = integer ? (s ? V::kR8G8B8A8_SINT : V::kR8G8B8A8_UINT) : (s ? V::kR8G8B8A8_SNORM : V::kR8G8B8A8_UNORM);
      return true;
    case xenos::VertexFormat::k_16_16:
      *out = integer ? (s ? V::kR16G16_SINT : V::kR16G16_UINT) : (s ? V::kR16G16_SNORM : V::kR16G16_UNORM);
      return true;
    case xenos::VertexFormat::k_16_16_16_16:
      *out = integer ? (s ? V::kR16G16B16A16_SINT : V::kR16G16B16A16_UINT)
                     : (s ? V::kR16G16B16A16_SNORM : V::kR16G16B16A16_UNORM);
      return true;
    case xenos::VertexFormat::k_32: if (!integer) return false; *out = V::kR32_UINT; return !s;
    case xenos::VertexFormat::k_32_32: if (!integer) return false; *out = V::kR32G32_UINT; return !s;
    case xenos::VertexFormat::k_32_32_32_32: if (!integer) return false; *out = V::kR32G32B32A32_UINT; return !s;
    default: return false;
  }
}

// ---- render target / clear values -----------------------------------------------------------------------------

bool HostTargetFormat(xenos::ColorRenderTargetFormat f, nb::TargetFormat* out) {
  switch (f) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8: *out = nb::TargetFormat::kRGBA8; return true;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: *out = nb::TargetFormat::kRGBA16F; return true;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: *out = nb::TargetFormat::kRG16F; return true;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT: *out = nb::TargetFormat::kR32F; return true;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: *out = nb::TargetFormat::kRG32F; return true;
    // 16-bit fixed (-32...32): signed-normalized host targets holding value/32 (the SDK's full-range mode)
    case xenos::ColorRenderTargetFormat::k_16_16: *out = nb::TargetFormat::kRG16S; return true;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16: *out = nb::TargetFormat::kRGBA16S; return true;
    default: return false;
  }
}

float DepthClearZ(uint32_t depth_format, uint32_t rb_depth_clear) {
  const uint32_t g = (rb_depth_clear >> 8) & 0xFFFFFF;
  return depth_format == uint32_t(xenos::DepthRenderTargetFormat::kD24S8) ? xenos::UNorm24To32(g)
                                                                         : xenos::Float20e4To32(g) * 0.5f;
}
bool ColorClearValue(xenos::ColorRenderTargetFormat cf, uint32_t clear, uint32_t clear_lo, float rgba[4],
                     bool* integer, std::string* why) {
  const uint64_t v = uint64_t(clear) | uint64_t(clear_lo) << 32;
  for (int c = 0; c < 4; ++c) rgba[c] = 0;
  *integer = true;
  switch (cf) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      *integer = false;
      for (int c = 0; c < 4; ++c) rgba[c] = float((v >> (8 * c)) & 0xFF) * (1.0f / 255.0f);
      return true;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      // Integer transfer view: the raw 16-bit components.
      for (int c = 0; c < 4; ++c) {
        const uint32_t u = uint32_t((v >> (16 * c)) & 0xFFFF);
        std::memcpy(&rgba[c], &u, 4);
      }
      return true;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      for (int c = 0; c < 2; ++c) {
        const uint32_t u = uint32_t(v >> (32 * c));
        std::memcpy(&rgba[c], &u, 4);
      }
      return true;
    default:
      *why = "colour clear for format " + std::to_string(uint32_t(cf));
      return false;
  }
}

// RenderTargetCache::PrepareHostRenderTargetsResolveClear + the clears of

// ---- samplers and textures --------------------------------------------------------------------------------------

nb::SamplerDesc SamplerFromFetch(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t slot, const Shader* sdk_vs,
                                 const Shader* sdk_ps) {
  // Filters as the SDK's VulkanTextureCache::ComputeSamplerParameters: the fetch
  // instruction's override if any, else the fetch constant.
  const ParsedTextureFetchInstruction::Attributes* attr = nullptr;
  for (const Shader* s : {sdk_vs, sdk_ps}) {
    if (!s) continue;
    for (const auto& b : s->texture_bindings()) {
      if (b.fetch_constant == slot) attr = &b.fetch_instr.attributes;
    }
  }
  auto pick = [](xenos::TextureFilter instr, xenos::TextureFilter f) {
    return instr == xenos::TextureFilter::kUseFetchConst ? f : instr;
  };
  xenos::TextureFilter mag = pick(attr ? attr->mag_filter : xenos::TextureFilter::kUseFetchConst, fetch.mag_filter);
  xenos::TextureFilter min = pick(attr ? attr->min_filter : xenos::TextureFilter::kUseFetchConst, fetch.min_filter);
  xenos::TextureFilter mip = pick(attr ? attr->mip_filter : xenos::TextureFilter::kUseFetchConst, fetch.mip_filter);
  xenos::AnisoFilter aniso = attr && attr->aniso_filter != xenos::AnisoFilter::kUseFetchConst ? attr->aniso_filter
                                                                                               : fetch.aniso_filter;
  nb::SamplerDesc s;
  s.mag_linear = mag == xenos::TextureFilter::kLinear;
  s.min_linear = min == xenos::TextureFilter::kLinear;
  s.mip_linear = mip == xenos::TextureFilter::kLinear;
  xenos::ClampMode cx, cy, cz;
  texture_util::GetClampModesForDimension(fetch, cx, cy, cz);
  auto addr = [](xenos::ClampMode m) -> uint32_t {
    switch (m) {
      case xenos::ClampMode::kRepeat: return 0;
      case xenos::ClampMode::kMirroredRepeat: return 1;
      case xenos::ClampMode::kClampToEdge: return 2;
      case xenos::ClampMode::kMirrorClampToEdge: return 3;
      case xenos::ClampMode::kClampToHalfway: return 2;
      case xenos::ClampMode::kMirrorClampToHalfway: return 3;
      case xenos::ClampMode::kClampToBorder: return 4;
      default: return 3;  // kMirrorClampToBorder
    }
  };
  s.address_u = addr(cx);
  s.address_v = addr(cy);
  s.address_w = addr(cz);
  s.border = fetch.border_color == xenos::BorderColor::k_ABGR_White ? 2 : 0;
  uint32_t mip_min = 0, mip_max = 0;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr, &mip_min,
                                                 &mip_max);
  s.min_lod = float(mip_min);
  s.max_lod = mip == xenos::TextureFilter::kBaseMap ? float(mip_min) + 0.25f : 1000.0f;
  // The SDK's anisotropic_override (default 3 = 4x in the shipped game) forces anisotropy on
  // mipmapped, min/mag-linear, point/linear-mip textures (ComputeSamplerParameters).
  const int32_t override_aniso = REXCVAR_GET(anisotropic_override);
  if (override_aniso > -1 && override_aniso < 6 && mip_max > mip_min && mip != xenos::TextureFilter::kBaseMap &&
      s.mag_linear && s.min_linear &&
      (mip == xenos::TextureFilter::kPoint || mip == xenos::TextureFilter::kLinear)) {
    aniso = xenos::AnisoFilter(override_aniso);
  }
  if (aniso > xenos::AnisoFilter::kMax_16_1) aniso = xenos::AnisoFilter::kMax_16_1;  // device max 16
  if (aniso != xenos::AnisoFilter::kDisabled && aniso != xenos::AnisoFilter::kUseFetchConst) {
    s.max_anisotropy = 1u << (uint32_t(aniso) - uint32_t(xenos::AnisoFilter::kMax_1_1));
    s.mag_linear = s.min_linear = s.mip_linear = true;
  }
  s.lod_bias = float(fetch.lod_bias) / 32.0f + (attr ? attr->lod_bias : 0.0f);
  return s;
}

bool PlanTextureFormat(const xenos::xe_gpu_texture_fetch_t& fetch, const TextureKeys::TextureKey& key,
                       TexFormatPlan* out, std::string* why) {
  // Signs (as the SDK's host format pair): all unsigned or all signed only; gamma / biased not yet.
  const xenos::TextureSign signs4[4] = {fetch.sign_x, fetch.sign_y, fetch.sign_z, fetch.sign_w};
  bool any_signed = false, any_other = false;
  for (auto sg : signs4) {
    any_signed |= sg == xenos::TextureSign::kSigned;
    any_other |= sg == xenos::TextureSign::kUnsignedBiased || sg == xenos::TextureSign::kGamma;
  }
  out->any_signed = any_signed;
  if (any_other) {
    *why = "gamma/biased texture sign";
    return false;
  }
  nb::TexFormat format;
  uint32_t block = 1, bpb = 4;
  bool one_component = false, two_component = false, depth24 = false, depth_float = false, rgba4 = false;
  switch (key.format) {
    case xenos::TextureFormat::k_8_8_8_8: format = any_signed ? nb::TexFormat::kRGBA8S : nb::TexFormat::kRGBA8; break;
    case xenos::TextureFormat::k_16_16_16_16:
      format = any_signed ? nb::TexFormat::kRGBA16S : nb::TexFormat::kRGBA16;
      bpb = 8;
      break;
    case xenos::TextureFormat::k_16_16:
      // SDK: R16G16_UNORM / R16G16_SNORM, host swizzle RGGG.
      format = any_signed ? nb::TexFormat::kRG16S : nb::TexFormat::kRG16;
      two_component = true;
      break;
    case xenos::TextureFormat::k_8:
      if (any_signed) {
        *why = "signed k_8";
        return false;
      }
      format = nb::TexFormat::kR8; bpb = 1; one_component = true;
      break;
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
      // SDK: R32 float from the 24-bit depth in bits 8-31 (UNorm24To32 / Float20e4To32).
      format = nb::TexFormat::kR32F; one_component = true; depth24 = true;
      depth_float = key.format == xenos::TextureFormat::k_24_8_FLOAT;
      break;
    case xenos::TextureFormat::k_4_4_4_4:
      // The SDK loads it as B4G4R4A4 (R in guest bits 0-3 ... A in 12-15); expanded here to RGBA8,
      // n * 17 = the same n / 15 texel values.
      if (any_signed) {
        *why = "signed k_4_4_4_4";
        return false;
      }
      format = nb::TexFormat::kRGBA8; bpb = 2; rgba4 = true;
      break;
    case xenos::TextureFormat::k_DXT1: format = nb::TexFormat::kBC1; block = 4; bpb = 8; break;
    case xenos::TextureFormat::k_DXT2_3: format = nb::TexFormat::kBC2; block = 4; bpb = 16; break;
    case xenos::TextureFormat::k_DXT4_5: format = nb::TexFormat::kBC3; block = 4; bpb = 16; break;
    case xenos::TextureFormat::k_32_FLOAT: format = nb::TexFormat::kR32F; one_component = true; break;
    default:
      *why = "texture format " + std::to_string(uint32_t(key.format));
      return false;
  }
  out->format = format;
  out->block = block;
  out->bpb = bpb;
  out->one_component = one_component;
  out->two_component = two_component;
  out->depth24 = depth24;
  out->depth_float = depth_float;
  out->rgba4 = rgba4;
  return true;
}

xenos::xe_gpu_texture_fetch_t TextureKeyFetch(const xenos::xe_gpu_texture_fetch_t& fetch,
                                              const TextureKeys::TextureKey& key, bool nosampler) {
  xenos::xe_gpu_texture_fetch_t key_fetch = fetch;
  if (nosampler) {
    key_fetch.clamp_x = key_fetch.clamp_y = key_fetch.clamp_z = xenos::ClampMode(0);
    key_fetch.mag_filter = key_fetch.min_filter = xenos::TextureFilter(0);
    key_fetch.aniso_filter = xenos::AnisoFilter(0);
    key_fetch.arbitrary_filter = xenos::ArbitraryFilter(0);
    key_fetch.vol_mag_filter = key_fetch.vol_min_filter = 0;
    key_fetch.mag_aniso_walk = key_fetch.min_aniso_walk = 0;
    key_fetch.lod_bias = 0;
    key_fetch.grad_exp_adjust_h = key_fetch.grad_exp_adjust_v = 0;
    key_fetch.border_color = xenos::BorderColor(0);
    key_fetch.tri_clamp = 0;
    key_fetch.aniso_bias = 0;
    // The type field (texture vs "invalid texture", both accepted by BindingInfoFromFetchConstant once is_valid)
    // does not change the image, and neither does the mip filter of a single-level texture (the key above was
    // already built from the real fetch). Seen: two fetches of one 512x512 k_16_16_16_16 copy per frame that
    // differ only in these (trace 43912 census).
    key_fetch.type = xenos::FetchConstantType(0);
    // The level-count field is cleared with it, so a masked key can only equal another single-level key (a
    // multi-level fetch keeps its real mip_filter and mip_max_level).
    if (key.mip_max_level == 0) {
      key_fetch.mip_filter = xenos::TextureFilter(0);
      key_fetch.mip_max_level = 0;
    }
  }
  return key_fetch;
}

void TextureSwizzle(const xenos::xe_gpu_texture_fetch_t& fetch, const TexFormatPlan& plan, uint8_t swizzle[4]) {
  for (int c = 0; c < 4; ++c) {
    uint8_t sw = uint8_t((fetch.swizzle >> (3 * c)) & 7);
    if (plan.one_component && sw >= 1 && sw <= 3) sw = 0;  // the SDK's RRRR host swizzle for 1-component formats
    if (plan.two_component && sw >= 2 && sw <= 3) sw = 1;  // RGGG for 2-component formats
    swizzle[c] = sw;
  }
}

LevelGeom ComputeLevelGeom(const TextureKeys::TextureKey& key, const texture_util::TextureGuestLayout& layout,
                           const TexFormatPlan& plan, uint32_t level) {
  LevelGeom g;
  const uint32_t block = plan.block, bpb = plan.bpb;
  g.bpb_log2 = bpb == 16 ? 4 : bpb == 8 ? 3 : bpb == 4 ? 2 : bpb == 2 ? 1 : 0;
  const uint32_t width = key.GetWidth(), height = key.GetHeight();
  g.lw = std::max(1u, width >> level);
  g.lh = std::max(1u, height >> level);
  g.bw = (g.lw + block - 1) / block;
  g.bh = (g.lh + block - 1) / block;
  const texture_util::TextureGuestLayout::Level& ll = level ? layout.mips[level] : layout.base;
  PackedLevelOffset(layout, width, height, key.format, level, &g.off_x, &g.off_y);
  if (level == 0) {
    g.address = key.base_page << 12;
  } else {
    const bool packed = layout.packed_level != UINT32_MAX && level >= layout.packed_level;
    g.address = (key.mip_page << 12) + layout.mip_offsets_bytes[packed ? layout.packed_level : level];
  }
  g.lay = (level && layout.packed_level != UINT32_MAX && level >= layout.packed_level) ? &layout.mips[layout.packed_level]
                                                                                       : &ll;
  g.pitch_blocks = g.lay->row_pitch_bytes / bpb;
  return g;
}

nb::GpuLevel MakeGpuLevel(const TextureKeys::TextureKey& key, const TexFormatPlan& plan, const LevelGeom& g,
                          uint32_t level, uint32_t face) {
  const uint32_t bpb = plan.bpb;
  nb::GpuLevel gl;
  gl.width = g.lw;
  gl.height = g.lh;
  gl.mip = level;
  gl.face = face;
  gl.p.src_base = g.address + face * g.lay->array_slice_stride_bytes;
  gl.p.row_pitch = g.lay->row_pitch_bytes;
  gl.p.pitch_blocks = g.pitch_blocks;
  gl.p.off_x = g.off_x;
  gl.p.off_y = g.off_y;
  gl.p.bw = g.bw;
  gl.p.bh = g.bh;
  gl.p.bpb_log2 = g.bpb_log2;
  gl.p.tiled = key.tiled ? 1 : 0;
  gl.p.endian = uint32_t(key.endianness);
  gl.p.mode = plan.rgba4 ? 1 : plan.depth24 ? (plan.depth_float ? 3 : 2) : 0;
  gl.p.out_size = g.bw * g.bh * (plan.rgba4 ? 4 : bpb);
  return gl;
}

// Card S3-49: the detile loops called the out-of-line SDK texture_util::GetTiledOffset2D once per texel (14 % of the
// UHD D3D worker's busy time); they now use the inlinable TiledOffset2D (nr_shared.h, same arithmetic).
// Card S3-49: texels [x, x + run) with x a multiple of run are contiguous in tiled memory (offset grows by bpb per
// texel) for run = 16 / bpb (bpb 2..16: micro & 0xF holds them) and run = 8 for bpb 1 (the x & 7 bits). 0 = no runs.
inline uint32_t TiledRunTexels(uint32_t bpb) {
  switch (bpb) {
    case 1: return 8;
    case 2: return 8;
    case 4: return 4;
    case 8: return 2;
    case 16: return 1;
    default: return 0;
  }
}

std::atomic<ReadObserver> g_read_observer{nullptr};
void SetReadObserver(ReadObserver fn) { g_read_observer.store(fn, std::memory_order_release); }

bool DetileLevelCpu(const TextureKeys::TextureKey& key, const TexFormatPlan& plan, const LevelGeom& g, uint32_t level,
                    uint32_t face, const uint8_t* src, nb::LevelData* out) {
  const uint32_t bpb = plan.bpb, bw = g.bw, bh = g.bh, off_x = g.off_x, off_y = g.off_y, bpb_log2 = g.bpb_log2;
  const uint32_t pitch_blocks = g.pitch_blocks;
  const uint32_t pitch_tiled = rex::align(pitch_blocks, xenos::kTextureTileWidthHeight);
  const bool rgba4 = plan.rgba4, depth24 = plan.depth24, depth_float = plan.depth_float;
  const auto& lay = *g.lay;
  if (const ReadObserver obs = g_read_observer.load(std::memory_order_acquire); obs && bw && bh) {
    // The bytes the loops below read: tiled = whole 32-row macro-tile rows (TiledOffset2D: row r of macro tiles spans
    // [r, r + 1) x pitch_tiled x 32 blocks); linear = first to last texel row.
    const uint64_t block = uint64_t(1) << bpb_log2;
    uint64_t lo, hi;
    if (key.tiled) {
      lo = uint64_t(off_y >> 5) * pitch_tiled * 32 * block;
      hi = (uint64_t((off_y + bh - 1) >> 5) + 1) * pitch_tiled * 32 * block;
    } else {
      const uint64_t tb = rgba4 ? 2 : bpb;
      lo = uint64_t(off_y) * lay.row_pitch_bytes + uint64_t(off_x) * tb;
      hi = uint64_t(off_y + bh - 1) * lay.row_pitch_bytes + uint64_t(off_x + bw) * tb;
    }
    obs(src + lo, hi - lo);
  }
  nb::LevelData& data = *out;
  data.width = g.lw;
  data.height = g.lh;
  data.mip = level;
  data.face = face;
  data.bytes.resize(size_t(bw) * bh * (rgba4 ? 4 : bpb));
  auto src_offset = [&](uint32_t x, uint32_t y) -> size_t {
    return key.tiled ? size_t(TiledOffset2D(int32_t(x + off_x), int32_t(y + off_y), pitch_tiled, bpb_log2))
                     : size_t(y + off_y) * lay.row_pitch_bytes + size_t(x + off_x) * bpb;
  };
  for (uint32_t y = 0; y < bh && rgba4; ++y) {
    for (uint32_t x = 0; x < bw; ++x) {
      const size_t so = src_offset(x, y);
      // 16-bit texel in host order for the endian swap of its guest word.
      uint32_t v;
      switch (key.endianness) {
        case xenos::Endian::k8in16: v = src[so ^ 1] | src[so] << 8; break;
        case xenos::Endian::k8in32: v = src[so ^ 3] | src[so ^ 2] << 8; break;
        case xenos::Endian::k16in32: v = src[so ^ 2] | src[(so ^ 2) + 1] << 8; break;
        default: v = src[so] | src[so + 1] << 8; break;
      }
      uint8_t* d = &data.bytes[(size_t(y) * bw + x) * 4];
      for (int c = 0; c < 4; ++c) d[c] = uint8_t(((v >> (4 * c)) & 0xF) * 17);
    }
  }
  // 1 byte per texel: the endian swap moves the byte within its word (8in16: ^1, 8in32: ^3).
  const size_t xr = bpb == 1 ? (key.endianness == xenos::Endian::k8in16 ? 1 : key.endianness == xenos::Endian::k8in32 ? 3 : 0)
                             : 0;
  // Card S3-49: copy runs of texels contiguous in the source (a whole row when linear) instead of one memcpy per texel.
  const uint32_t run = key.tiled ? TiledRunTexels(bpb) : bw;
  for (uint32_t y = 0; y < bh && !rgba4; ++y) {
    uint8_t* drow = &data.bytes[size_t(y) * bw * bpb];
    uint32_t x = 0;
    while (x < bw) {
      const uint32_t n = run ? std::min(run - (key.tiled ? (x + off_x) % run : 0), bw - x) : 1;
      const size_t so = src_offset(x, y);
      if (xr) {
        for (uint32_t i = 0; i < n; ++i) drow[x + i] = src[(so + i) ^ xr];
      } else {
        std::memcpy(drow + size_t(x) * bpb, src + so, size_t(n) * bpb);
      }
      x += n;
    }
  }
  if (rgba4) {
    // already host order
  } else if (bpb == 1) {
    if (key.endianness == xenos::Endian::k16in32) {
      return false;
    }
  } else {
    SwapDwords(data.bytes.data(), data.bytes.size(), uint32_t(key.endianness));
  }
  if (depth24) {
    for (size_t i = 0; i + 4 <= data.bytes.size(); i += 4) {
      uint32_t v;
      std::memcpy(&v, &data.bytes[i], 4);
      const float f = depth_float ? xenos::Float20e4To32(v >> 8) : xenos::UNorm24To32(v >> 8);
      std::memcpy(&data.bytes[i], &f, 4);
    }
  }
  return true;
}

XXH128_hash_t TextureHash(XXH3_state_t* st, const std::string& cache_key, const TextureKeys::TextureKey& key,
                          const texture_util::TextureGuestLayout& layout, rex::memory::Memory* memory) {
  XXH3_128bits_reset(st);
  XXH3_128bits_update(st, cache_key.data(), cache_key.size());
  const uint32_t base_len = layout.base.level_data_extent_bytes;
  if (base_len) {
    const uint32_t base_start = uint32_t(key.base_page) << 12;
    if (const ReadObserver obs = g_read_observer.load(std::memory_order_acquire))
      obs(memory->TranslatePhysical<const uint8_t*>(base_start), base_len);
    XXH3_128bits_update(st, memory->TranslatePhysical<const uint8_t*>(base_start), base_len);
  }
  const uint32_t mip_len = layout.mips_total_extent_bytes;
  if (mip_len) {
    const uint32_t mip_start = uint32_t(key.mip_page) << 12;
    if (const ReadObserver obs = g_read_observer.load(std::memory_order_acquire))
      obs(memory->TranslatePhysical<const uint8_t*>(mip_start), mip_len);
    XXH3_128bits_update(st, memory->TranslatePhysical<const uint8_t*>(mip_start), mip_len);
  }
  return XXH3_128bits_digest(st);
}

// ---- texture / buffer write watch -------------------------------------------------------------------------------

void TextureWatch::MarkWritten(uint32_t start, uint32_t length, bool bypass) {
  if (!length || start >= 0x20000000u) return;
  const uint32_t first = start >> 12;
  const uint32_t last = std::min<uint64_t>(uint64_t(start) + length - 1, 0x1FFFFFFFu) >> 12;
  uint64_t armed = 0;
  for (uint32_t pg = first; pg <= last; ++pg) {
    gen_[pg].fetch_add(1, std::memory_order_relaxed);  // always: generation advances on any write
    if (armed_[pg].exchange(0, std::memory_order_relaxed)) ++armed;
  }
  if (bypass) bypass_pages.fetch_add(last - first + 1, std::memory_order_relaxed);
  else if (armed) watch_faults.fetch_add(armed, std::memory_order_relaxed);
}

void TextureWatch::Arm(rex::memory::Memory* memory, uint32_t start, size_t length) {
  if (!length || start >= 0x20000000u) return;
  if (ensure_) ensure_(ensure_ctx_);
  const uint32_t first = start >> 12;
  const uint32_t last = uint32_t(std::min<uint64_t>(uint64_t(start) + length - 1, 0x1FFFFFFFu) >> 12);
  for (uint32_t pg = first; pg <= last; ++pg) armed_[pg].store(1, std::memory_order_relaxed);
  memory->EnablePhysicalMemoryAccessCallbacks(first << 12, (last - first + 1) << 12, true, false);
}

uint64_t TextureWatch::GenSum(const std::array<uint32_t, 4>& range) const {
  uint64_t sum = 0;
  for (uint32_t r = 0; r < 4; r += 2) {
    if (range[r + 1] <= range[r]) continue;  // empty span
    for (uint32_t pg = range[r] >> 12; pg <= (range[r + 1] - 1) >> 12; ++pg) {
      sum += gen_[pg].load(std::memory_order_relaxed);
    }
  }
  return sum;
}

bool TextureWatch::Clean(const std::array<uint32_t, 4>& range, bool gen_valid, uint64_t gen_sum) const {
  if (!gen_valid) return false;
  for (uint32_t r = 0; r < 4; r += 2) {
    if (range[r + 1] <= range[r]) continue;  // empty span
    for (uint32_t pg = range[r] >> 12; pg <= (range[r + 1] - 1) >> 12; ++pg) {
      if (!armed_[pg].load(std::memory_order_relaxed)) return false;
    }
  }
  return GenSum(range) == gen_sum;
}

// ---- vertex / index buffer cache --------------------------------------------------------------------------------

bool VbCache::Clean(const Entry& e) const {
  uint64_t sum = 0;
  for (uint32_t pg = e.first_pg; pg <= e.last_pg; ++pg) {
    if (!watch_->Armed(pg)) return false;
    sum += watch_->Gen(pg);
  }
  return sum == e.gen_sum;
}

void VbCache::EvictIfFull(uint64_t max_bytes) {
  if (bytes <= max_bytes) return;
  map_.clear();
  bytes = 0;
  flush = true;
}

bool VbCache::WouldHit(const Key& key, uint32_t phys_start, uint32_t nbytes, uint64_t frames, uint32_t* id) const {
  if (!nbytes || uint64_t(phys_start) + nbytes > 0x20000000u) return false;
  const auto it = map_.find(key);
  if (it == map_.end()) return false;
  const Entry& e = it->second;
  if (e.dyn_until > frames) return false;
  if (!(e.built && e.backend_has && Clean(e))) return false;
  if (id) *id = e.id;
  return true;
}

int VbCache::Probe(const Key& key, uint32_t phys_start, uint32_t nbytes, uint64_t frames, uint32_t* id) const {
  // V17d: the result Lookup would return when it is a pure function of current state (counters only): kHit (built,
  // backend has it, pages clean) or kNo for an entry in its dynamic window. -1 = Lookup would change state.
  if (!nbytes || uint64_t(phys_start) + nbytes > 0x20000000u) return kNo;
  const auto it = map_.find(key);
  if (it == map_.end()) return -1;
  const Entry& e = it->second;
  if (e.dyn_until > frames) return kNo;
  if (e.built && e.backend_has && Clean(e)) {
    if (id) *id = e.id;
    return kHit;
  }
  return -1;
}

int VbCache::Lookup(rex::memory::Memory* memory, const Key& key, uint32_t phys_start, uint32_t nbytes, bool index,
                    uint64_t frames, bool trust_replay, Entry** out) {
  uint64_t& c_first = index ? ibc_first : vbc_first;
  uint64_t& c_dyn = index ? ibc_dyn : vbc_dyn;
  if (!nbytes || uint64_t(phys_start) + nbytes > 0x20000000u) return kNo;
  auto [it, inserted] = map_.try_emplace(key);
  Entry& e = it->second;
  *out = &e;
  if (inserted) {
    ++c_first;
    return kNo;
  }
  if (e.dyn_until > frames) {
    ++c_dyn;
    return kNo;
  }
  // TRUST_REPLAY (measurement only): the replay harness rewrites identical bytes into the watched pages every frame,
  // which fires the real write watch; treat a built entry as valid forever to measure the steady-state ceiling.
  if (e.built && e.backend_has && (trust_replay || Clean(e))) {
    (index ? ibc_hit : vbc_hit)++;
    (index ? ibc_hit_bytes : vbc_hit_bytes) += e.bytes;
    return kHit;
  }
  if (e.built && e.backend_has) {  // written since the last build
    e.strikes = frames - e.last_build <= 2 ? e.strikes + 1 : 0;
    if (e.strikes >= 3) {
      e.dyn_until = frames + 240;
      e.strikes = 0;
      e.built = false;
      ++c_dyn;
      return kNo;
    }
  }
  if (!e.counted) {
    e.counted = true;
    e.bytes = nbytes;
    e.id = next_id++;
    bytes += nbytes;
  }
  watch_->Arm(memory, phys_start, nbytes);
  e.first_pg = phys_start >> 12;
  e.last_pg = uint32_t((uint64_t(phys_start) + nbytes - 1) >> 12);
  uint64_t sum = 0;
  for (uint32_t pg = e.first_pg; pg <= e.last_pg; ++pg) sum += watch_->Gen(pg);
  e.gen_sum = sum;
  e.built = true;
  e.backend_has = false;
  e.last_build = frames;
  ++vbc_build;
  if (!index) vbc_build_bytes += nbytes;
  else ++ibc_build;
  return kBuild;
}

// ---- card RT1: runtime contracts ------------------------------------------------------------------------------

namespace {

std::filesystem::path g_rts_cache_dir;  // set before the first draw (wwe2k14_app.h)

struct RtsJob {
  uint64_t key = 0;
  bool vs = true, check = false;
  std::vector<uint32_t> dwords;
  const ShaderEntry* shipped = nullptr;  // check jobs: node-stable pointer into ShaderSet
};
struct RtsDone {
  uint64_t key = 0;
  bool vs = true;
  ShaderEntry e;
};

struct Rts {
  std::once_flag once;
  bool enabled = false, check = false;
  bool stop = false;  // (mu) shutdown began: the worker no longer logs, publishes or reads shipped entries
  rts_core::Toolchain tc;
  std::filesystem::path cache;  // <root>/<toolchain stamp>
  std::filesystem::path work;   // <cache>/work/<pid>: this process's job dirs
  std::mutex mu;
  std::condition_variable cv;
  std::deque<RtsJob> fallback_q, check_q;
  std::vector<RtsDone> done;
  std::unordered_set<uint64_t> pending;  // (mu) stage-discriminated keys with a fallback translation in flight
  std::thread::id translator_id;  // (mu) dedicated RtsThread; independent of the D3D draw worker
  // owner (D3D draw worker) thread only
  std::unordered_set<uint64_t> requested, check_seen, waited;
  // counters (mu)
  uint64_t translated = 0, failed = 0, cache_hits = 0, inserted = 0, key_mismatch = 0, cache_bad = 0, fail_skips = 0;
  uint64_t waits = 0, wait_ms = 0;
  uint64_t checks = 0, mismatches = 0, spirv_diff = 0, check_failed = 0;
  double max_ms = 0, sum_ms = 0;
};

// Immortal (review rv1): the detached worker may still run a job while the process exits; a function-local static
// would be destroyed under it.
Rts& R() {
  static Rts& r = *new Rts;
  return r;
}

const char* StageName(bool vs) { return vs ? "vs" : "ps"; }

std::string Counters(const Rts& r) {
  return fmt::format("translated={} failed={} cache_hits={} cache_bad={} fail_skips={} inserted={} key_mismatch={} "
                     "waits={} wait_ms={} avg_ms={:.0f} max_ms={:.0f} check: checks={} mismatches={} spirv_diff={} "
                     "check_failed={}",
                     r.translated, r.failed, r.cache_hits, r.cache_bad, r.fail_skips, r.inserted, r.key_mismatch,
                     r.waits, r.wait_ms, r.translated ? r.sum_ms / double(r.translated) : 0.0, r.max_ms, r.checks,
                     r.mismatches, r.spirv_diff, r.check_failed);
}

uint32_t RuntimeShaderWaitLimitMs() {
  static const uint32_t value = rts_wait::ParseLimitMs(std::getenv("WWE2K14_RTS_WAIT_MS"));
  return value;
}

const ShaderEntry* RuntimeShaderWait(ShaderSet& set, uint64_t fnv, bool vertex, Rts& r,
                                     RuntimeShaderWaitStats* stats) {
  const uint64_t stored_key = fnv ^ (vertex ? 1ull : 0ull);
  const uint32_t limit_ms = RuntimeShaderWaitLimitMs();
  if (!limit_ms || !r.waited.insert(stored_key).second) return nullptr;

  const auto start = std::chrono::steady_clock::now();
  rts_wait::Result result = rts_wait::Result::kReady;
  uint64_t elapsed_ms = 0;
  bool blocked = false;
  {
    std::unique_lock<std::mutex> lk(r.mu);
    if (r.pending.count(stored_key) && !r.stop) {
      const auto ready = [&] { return r.stop || !r.pending.count(stored_key); };
      result = rts_wait::WaitUntil(r.cv, lk, ready, std::chrono::milliseconds(limit_ms),
                                   std::this_thread::get_id(), r.translator_id);
      blocked = result != rts_wait::Result::kWouldDeadlock;
      if (blocked) {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        elapsed_ms = uint64_t(std::ceil(std::chrono::duration<double, std::milli>(elapsed).count()));
        ++r.waits;
        r.wait_ms += elapsed_ms;
        if (stats) {
          ++stats->waits;
          stats->wait_ms += elapsed_ms;
        }
        REXLOG_INFO("[nr-rts] waited {} {} ms (limit {} ms) | {}", fmt::format("{:016X}.{}", fnv, StageName(vertex)),
                    elapsed_ms, limit_ms, Counters(r));
      }
    }
    // The translator publishes only RtsDone. Insertion into the worker-owned set stays on this worker.
    for (RtsDone& d : r.done) set.Insert(d.key, d.vs, std::move(d.e)), ++r.inserted;
    r.done.clear();
  }
  return set.Find(fnv, vertex);
}

std::string ContractDiff(const ShaderEntry& a, const ShaderEntry& b) {
  std::string d;
  bool vf = a.vfetch.size() == b.vfetch.size();
  for (size_t i = 0; vf && i < a.vfetch.size(); ++i) vf = !std::memcmp(&a.vfetch[i], &b.vfetch[i], sizeof(VFetch));
  if (!vf) d += fmt::format(" vfetch({} vs {})", a.vfetch.size(), b.vfetch.size());
  if (a.texture_slots != b.texture_slots) d += fmt::format(" tslots({} vs {})", a.texture_slots.size(), b.texture_slots.size());
  if (a.texture_dims != b.texture_dims) d += " tdims";
  if (a.unsupported != b.unsupported) d += " unsupported";
  return d;
}

void RtsJobRun(Rts& r, RtsJob& j) {
  const std::string name = fmt::format("{:016X}.{}", j.key, StageName(j.vs));
  const std::filesystem::path work = r.work / (name + (j.check ? ".check" : ""));
  const auto t0 = std::chrono::steady_clock::now();
  std::string hlsl, err;
  std::vector<uint8_t> spirv;
  bool permanent = false;
  const bool ok =
      rts_core::Translate(r.tc, j.dwords.data(), j.dwords.size(), j.vs, work, &hlsl, &spirv, &err, &permanent);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::error_code ec;
  std::filesystem::remove_all(work, ec);  // failures: the error (+ the tool log's last line on Linux) is logged instead
  ShaderEntry e;
  if (ok) {
    e.spirv = std::move(spirv);
    e.source = "runtime";
    std::istringstream in(hlsl);
    rts_core::ParseContract(in, &e);
  }
  if (j.check) {
    std::lock_guard<std::mutex> lk(r.mu);
    if (r.stop) return;  // j.shipped may already be gone
    if (!ok) {
      ++r.check_failed;
      REXLOG_WARN("[nr-rts] CHECK {} translation failed: {} | {}", name, err, Counters(r));
      return;
    }
    ++r.checks;
    const std::string d = ContractDiff(e, *j.shipped);
    const bool spv_same = e.spirv == j.shipped->spirv;
    if (!d.empty()) ++r.mismatches;
    if (!spv_same) ++r.spirv_diff;
    if (!d.empty() || !spv_same)
      REXLOG_WARN("[nr-rts] CHECK MISMATCH {}:{}{} ({:.0f} ms) | {}", name, d, spv_same ? "" : " spirv-bytes", ms,
                  Counters(r));
    else if (r.checks <= 4 || (r.checks & 15) == 0)
      REXLOG_INFO("[nr-rts] CHECK {} equal ({:.0f} ms) | {}", name, ms, Counters(r));
    return;
  }
  bool stored = false, marked = false;
  if (ok) stored = rts_core::CachePut(r.cache, name, hlsl, e.spirv);
  else if (permanent) marked = rts_core::MarkFailed(r.cache, name, err);  // skipped by later launches (same stamp)
  std::lock_guard<std::mutex> lk(r.mu);
  if (r.stop) return;
  if (!ok) {
    ++r.failed;
    r.pending.erase(j.key ^ (j.vs ? 1ull : 0ull));
    REXLOG_WARN("[nr-rts] {} translation failed ({:.0f} ms): {}{} | {}", name, ms, err,
                permanent ? (marked ? " (deterministic: marked, skipped by later runs)" : " (deterministic; marker write failed)")
                          : "",
                Counters(r));
    r.cv.notify_all();
    return;
  }
  ++r.translated;
  r.sum_ms += ms;
  r.max_ms = std::max(r.max_ms, ms);
  REXLOG_INFO("[nr-rts] translated {} in {:.0f} ms ({} dwords, {} vfetch, {} textures{}){} | {}", name, ms,
              j.dwords.size(), e.vfetch.size(), e.texture_slots.size(),
              e.unsupported.empty() ? "" : ", unsupported:" + e.unsupported,
              stored ? "" : " (disk cache write failed: not cached)", Counters(r));
  r.done.push_back({j.key, j.vs, std::move(e)});
  r.pending.erase(j.key ^ (j.vs ? 1ull : 0ull));
  r.cv.notify_all();
}

void RtsThread() {
  Rts& r = R();
  {
    std::lock_guard<std::mutex> lk(r.mu);
    r.translator_id = std::this_thread::get_id();
  }
  r.cv.notify_all();
  try {
    for (;;) {
      RtsJob j;
      {
        std::unique_lock<std::mutex> lk(r.mu);
        r.cv.wait(lk, [&] { return r.stop || !r.fallback_q.empty() || !r.check_q.empty(); });
        if (r.stop) return;
        std::deque<RtsJob>& q = r.fallback_q.empty() ? r.check_q : r.fallback_q;  // fallback jobs first
        j = std::move(q.front());
        q.pop_front();
      }
      try {
        RtsJobRun(r, j);
      } catch (...) {  // an exception escaping a detached thread terminates the process: drop the job instead
        {
          std::lock_guard<std::mutex> lk(r.mu);
          if (j.check) ++r.check_failed;
          else {
            ++r.failed;
            r.pending.erase(j.key ^ (j.vs ? 1ull : 0ull));
          }
        }
        r.cv.notify_all();
      }
    }
  } catch (...) {
  }
}

// Shutdown began (OnShutdown, or exit as a backstop): the worker stops logging / publishing; a running child process
// is left to finish (or the process exit ends the thread).
void RtsStop() {
  Rts& r = R();
  {
    std::lock_guard<std::mutex> lk(r.mu);
    r.stop = true;
  }
  r.cv.notify_all();
}

void RtsInit(Rts& r) {
  // Card G56 B2: RT1 does not implement the 32-bit constants ABI. Stop before toolchain,
  // cache and thread discovery when the validated opt-in selects it.
  if (!nr_constants32::Plan(nr_constants32::SwitchRequested(std::getenv(nr_constants32::kSwitch)))
           .rt1_eligible) {
    REXLOG_INFO("[nr-rts] runtime shader translation unavailable for the 32-bit constants ABI "
                "(WWE2K14_NR_CONSTANTS32=1; RT1 keeps the Int64 ABI)");
    return;
  }
  auto env = [](const char* n) -> const char* {
    const char* v = std::getenv(n);
    return v && *v ? v : nullptr;
  };
  const char* on = env("WWE2K14_NR_RUNTIME_SHADERS");
  if (on && std::strcmp(on, "0") == 0) {
    REXLOG_INFO("[nr-rts] runtime shader translation off (WWE2K14_NR_RUNTIME_SHADERS=0)");
    return;
  }
  if (nr::backend::ShaderFormatIsDxil()) {
    REXLOG_INFO("[nr-rts] runtime shader translation unavailable on the DXIL backend");
    return;
  }
  std::filesystem::path tools = rts_core::EnvPath("WWE2K14_NR_RUNTIME_SHADERS_TOOLS");
  if (tools.empty()) tools = rts_core::ExeDir() / "shadertools";
  if (!rts_core::FindToolchain(tools, &r.tc)) {
    REXLOG_WARN("[nr-rts] runtime shader translation off: {}", r.tc.why);
    return;
  }
  std::filesystem::path root = rts_core::EnvPath("WWE2K14_NR_RUNTIME_SHADERS_CACHE");
  if (root.empty()) root = g_rts_cache_dir;
  if (root.empty()) {
    const std::filesystem::path out = rts_core::EnvPath("WWE2K14_NR_OUT");
    root = (out.empty() ? std::filesystem::path(".") : out) / "runtime-shaders";
  }
  r.cache = root / fmt::format("{:016X}", r.tc.stamp);
  r.work = r.cache / "work" / std::to_string(rts_core::ProcessId());
  std::error_code ec;
  std::filesystem::create_directories(r.work, ec);
  if (ec) {
    REXLOG_WARN("[nr-rts] runtime shader translation off: cannot create {}: {}", rts_core::U8(r.work), ec.message());
    return;
  }
  r.enabled = true;
  r.check = env("WWE2K14_NR_RUNTIME_SHADERS_CHECK") && std::strcmp(env("WWE2K14_NR_RUNTIME_SHADERS_CHECK"), "1") == 0;
  std::atexit(RtsStop);  // backstop; OnShutdown calls RuntimeShaderShutdown before the logger shuts down
  std::thread(RtsThread).detach();
  REXLOG_INFO("[nr-rts] runtime shader translation on: tools {} cache {} (recipe {}) wait_ms={}{}",
              rts_core::U8(tools), rts_core::U8(r.cache), rts_core::kRecipeVersion, RuntimeShaderWaitLimitMs(),
              r.check ? " CHECK=1" : "");
}

Rts& RtsReady() {
  Rts& r = R();
  std::call_once(r.once, [&r] { RtsInit(r); });
  return r;
}

}  // namespace

void SetRuntimeShaderCacheDir(const std::filesystem::path& dir) { g_rts_cache_dir = dir; }

void RuntimeShaderShutdown() {
  if (R().enabled) RtsStop();
}

const ShaderEntry* RuntimeShaderResolve(ShaderSet& set, uint64_t fnv, bool vertex, const uint32_t* host_dwords,
                                        size_t count, RuntimeShaderWaitStats* wait_stats) {
  Rts& r = RtsReady();
  if (!r.enabled) return nullptr;
  {
    std::lock_guard<std::mutex> lk(r.mu);
    for (RtsDone& d : r.done) set.Insert(d.key, d.vs, std::move(d.e)), ++r.inserted;
    r.done.clear();
  }
  if (const ShaderEntry* e = set.Find(fnv, vertex)) return e;
  const uint64_t stored_key = fnv ^ (vertex ? 1ull : 0ull);
  if (!r.requested.insert(stored_key).second)  // pending, or failed this run
    return RuntimeShaderWait(set, fnv, vertex, r, wait_stats);
  if (!host_dwords || !count || Fnv1a64BigEndian(host_dwords, count) != fnv) {
    std::lock_guard<std::mutex> lk(r.mu);
    ++r.key_mismatch;
    REXLOG_WARN("[nr-rts] {:016X}.{}: microcode does not hash to its key; not translated", fnv, StageName(vertex));
    return nullptr;
  }
  const std::string name = fmt::format("{:016X}.{}", fnv, StageName(vertex));
  {  // translated by an earlier run (verified: .spv length + hash recorded in the .hlsl)
    std::string hlsl, why;
    ShaderEntry e;
    const rts_core::CacheResult c = rts_core::CacheGet(r.cache, name, &hlsl, &e.spirv, &why);
    if (c == rts_core::CacheResult::kHit) {
      e.source = "runtime-cache";
      std::istringstream in(hlsl);
      rts_core::ParseContract(in, &e);
      std::lock_guard<std::mutex> lk(r.mu);
      ++r.cache_hits;
      ++r.inserted;
      REXLOG_INFO("[nr-rts] {} from the disk cache | {}", name, Counters(r));
      return set.Insert(fnv, vertex, std::move(e));
    }
    if (c == rts_core::CacheResult::kBad) {
      std::lock_guard<std::mutex> lk(r.mu);
      ++r.cache_bad;
      REXLOG_WARN("[nr-rts] {} disk cache entry rejected ({}): removed, translating again | {}", name, why,
                  Counters(r));
    }
  }
  if (rts_core::FailMarked(r.cache, name)) {  // deterministic failure in an earlier run with this toolchain stamp
    std::lock_guard<std::mutex> lk(r.mu);
    ++r.fail_skips;
    REXLOG_INFO("[nr-rts] {} skipped: failed deterministically in an earlier run ({}) | {}", name,
                rts_core::U8(rts_core::FailMarker(r.cache, name)), Counters(r));
    return nullptr;
  }
  RtsJob j;
  j.key = fnv;
  j.vs = vertex;
  j.dwords.assign(host_dwords, host_dwords + count);
  {
    std::lock_guard<std::mutex> lk(r.mu);
    r.pending.insert(stored_key);
    r.fallback_q.push_back(std::move(j));
  }
  r.cv.notify_one();
  REXLOG_INFO("[nr-rts] {} has no contract: queued for translation", name);
  return RuntimeShaderWait(set, fnv, vertex, r, wait_stats);
}

bool RuntimeShaderCheckEnabled() {
  const Rts& r = RtsReady();
  return r.enabled && r.check;
}

void RuntimeShaderCheck(uint64_t fnv, bool vertex, const uint32_t* host_dwords, size_t count, const ShaderEntry* shipped) {
  Rts& r = RtsReady();
  if (!r.enabled || !r.check || !shipped || !host_dwords || !count) return;
  if (!r.check_seen.insert(fnv ^ (vertex ? 1ull : 0ull)).second) return;
  if (Fnv1a64BigEndian(host_dwords, count) != fnv) return;
  RtsJob j;
  j.key = fnv;
  j.vs = vertex;
  j.check = true;
  j.shipped = shipped;
  j.dwords.assign(host_dwords, host_dwords + count);
  {
    std::lock_guard<std::mutex> lk(r.mu);
    r.check_q.push_back(std::move(j));
  }
  r.cv.notify_one();
}

}  // namespace nr::shared
#endif  // NR_RTS_CORE_ONLY

// ---- card RT1: child processes (kept last: the Windows header's macros reach no other code) ---------------------
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace nr::rts_core {

#ifdef _WIN32
namespace {
// CommandLineToArgvW quoting of one argument.
std::wstring QuoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring q = L"\"";
  for (size_t i = 0;; ++i) {
    size_t bs = 0;
    while (i < a.size() && a[i] == L'\\') ++i, ++bs;
    if (i == a.size()) {
      q.append(bs * 2, L'\\');
      break;
    }
    if (a[i] == L'"') q.append(bs * 2 + 1, L'\\');
    else q.append(bs, L'\\');
    q.push_back(a[i]);
  }
  return q + L"\"";
}
}  // namespace

RunResult RunProcess(const std::vector<std::filesystem::path>& argv, const std::filesystem::path&,
                     const std::filesystem::path&, std::string* err, const std::filesystem::path& win_cwd) {
  std::wstring cmd;  // UTF-16 throughout: no ANSI code page step for non-ASCII install paths
  for (const std::filesystem::path& a : argv) {
    if (!cmd.empty()) cmd += L' ';
    cmd += QuoteArg(a.native());
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(argv[0].c_str(), cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                      win_cwd.empty() ? nullptr : win_cwd.c_str(), &si, &pi)) {
    *err = "CreateProcess error " + std::to_string(GetLastError());
    return RunResult::kError;
  }
  const DWORD w = WaitForSingleObject(pi.hProcess, DWORD(g_child_timeout_ms));
  if (w != WAIT_OBJECT_0) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    *err = w == WAIT_TIMEOUT ? "timed out after " + std::to_string(g_child_timeout_ms) + " ms (killed)"
                             : "wait error " + std::to_string(GetLastError());
    return RunResult::kError;
  }
  DWORD code = 1;
  const bool got = GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (!got) {
    *err = "GetExitCodeProcess error " + std::to_string(GetLastError());
    return RunResult::kError;
  }
  if (code == 0) return RunResult::kOk;
  *err = "exit code " + std::to_string(code);
  // NTSTATUS-style codes (0xC0000005 access violation, ...) are crashes, not a verdict on the input.
  return code >= 0xC0000000u ? RunResult::kError : RunResult::kToolFailed;
}

unsigned long ProcessId() { return GetCurrentProcessId(); }

std::filesystem::path ExeDir() {
  wchar_t buf[32768];
  const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return n ? std::filesystem::path(std::wstring(buf, n)).parent_path() : std::filesystem::path(".");
}
#else
RunResult RunProcess(const std::vector<std::filesystem::path>& argv, const std::filesystem::path& lib_dir,
                     const std::filesystem::path& log, std::string* err, const std::filesystem::path&) {
  std::vector<std::string> env;
  for (char** e = environ; e && *e; ++e)
    if (std::strncmp(*e, "LD_LIBRARY_PATH=", 16) != 0 && std::strncmp(*e, "LD_PRELOAD=", 11) != 0) env.push_back(*e);
  env.push_back("LD_LIBRARY_PATH=" + lib_dir.native());
  std::vector<char*> av, ev;
  for (const std::filesystem::path& a : argv) av.push_back(const_cast<char*>(a.c_str()));
  for (const std::string& e : env) ev.push_back(const_cast<char*>(e.c_str()));
  av.push_back(nullptr);
  ev.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, av[0], &fa, nullptr, av.data(), ev.data());
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) {
    *err = std::string("posix_spawn: ") + std::strerror(rc);
    return RunResult::kError;
  }
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(g_child_timeout_ms);
  for (;;) {
    const pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) break;
    if (w < 0 && errno != EINTR) {
      *err = std::string("waitpid: ") + std::strerror(errno);
      return RunResult::kError;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      *err = "timed out after " + std::to_string(g_child_timeout_ms) + " ms (killed)";
      return RunResult::kError;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return RunResult::kOk;
  if (WIFEXITED(status)) {
    *err = "exit code " + std::to_string(WEXITSTATUS(status));
    return RunResult::kToolFailed;
  }
  *err = "signal " + std::to_string(WIFSIGNALED(status) ? WTERMSIG(status) : -1);
  return RunResult::kError;
}

unsigned long ProcessId() { return (unsigned long)getpid(); }

std::filesystem::path ExeDir() {
  std::error_code ec;
  const std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path(".") : p.parent_path();
}
#endif

}  // namespace nr::rts_core
