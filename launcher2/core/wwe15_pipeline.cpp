#include "wwe15_pipeline.h"

#include "wwe13_audio.h"
#include "bink_convert.h"

#include "grid_faces.h"

#include "internal/util.h"
#include "path_case.h"
#include "wwe13_pipeline.h"
#include "wwe13_shared.h"
#include "wwe15_convert.h"
#include "wwe15_moves.h"
#include "wwe15_portraits.h"
#include "wwe15_looks.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace wwe2k14::launcher {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr size_t kDirRoom = 0x4000 - 0x800;              // porttool DIR_ROOM
constexpr size_t kProvenDir = 14328;                      // largest Entrance directory the game has loaded (cap4)
constexpr size_t kProvenEntranceFile = 74276864;          // largest Entrance_dlc0N.pac the game has loaded
constexpr const char* kUdp = "userdata/0000000000000000/545408B2/00000002/";

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}
uint16_t Le16(const Bytes& b, size_t at) { return uint16_t(b[at] | (b[at + 1] << 8)); }
uint32_t Le32(const Bytes& b, size_t at) {
  return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
}
void PutLe16(Bytes* b, size_t at, uint32_t v) { (*b)[at] = uint8_t(v); (*b)[at + 1] = uint8_t(v >> 8); }
void PutLe32(Bytes* b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) (*b)[at + i] = uint8_t(v >> (8 * i)); }
void PutBe16(Bytes* b, size_t at, uint32_t v) { (*b)[at] = uint8_t(v >> 8); (*b)[at + 1] = uint8_t(v); }
std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  return s;
}
std::string PyStrip(const std::string& s) {  // python str.strip(): whitespace only
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}
std::string Pad4(uint32_t v) { char t[16]; std::snprintf(t, sizeof(t), "%04u", v); return t; }
Bytes Ascii(const std::string& s) { return Bytes(s.begin(), s.end()); }
Bytes Ljust(const std::string& s, size_t n) { Bytes b = Ascii(s); b.resize(std::max(n, b.size()), ' '); return b; }

bool ReadFileBytes(const fs::path& path, Bytes* out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Fail(error, "Could not read " + internal::PathToUtf8(path) + ".");
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  in.seekg(0);
  out->resize(size_t(size));
  if (size > 0) in.read(reinterpret_cast<char*>(out->data()), size);
  if (!in) return Fail(error, "Could not read " + internal::PathToUtf8(path) + ".");
  return true;
}
bool ReadHead(const fs::path& path, size_t n, Bytes* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign(n, 0);
  in.read(reinterpret_cast<char*>(out->data()), std::streamsize(n));
  out->resize(size_t(in.gcount()));
  return true;
}
bool WriteFileBytes(const fs::path& path, const Bytes& data, std::string* error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  if (!out) return Fail(error, "Could not write " + internal::PathToUtf8(path) + ".");
  return true;
}
bool Unpack(const Bytes& raw, Bytes* out, std::string* error) {
  if (raw.size() >= 4 && std::memcmp(raw.data(), "ZLIB", 4) == 0) return DecodeWWE13ZlibLeaf(raw, out, error);
  return DecodeWWE13Bpe(raw, out, error);
}

// ---------------------------------------------------------------- PAC directory (arcmodel model)
struct DirEntry { Bytes name; uint32_t sector = 0, size = 0; };
struct DirGroup { Bytes type; uint32_t hi = 0; std::vector<DirEntry> entries; };
std::vector<DirGroup> GroupsFromPlain(const Bytes& plain) {
  std::vector<DirGroup> out;
  size_t p = 0x800;
  while (p + 12 <= 0x4000 && p + 12 <= plain.size() && !(plain[p] == 0 && plain[p + 1] == 0 && plain[p + 2] == 0 && plain[p + 3] == 0)) {
    DirGroup g;
    g.type.assign(plain.begin() + p, plain.begin() + p + 4);
    const uint32_t w = Le32(plain, p + 4);
    const uint32_t n = w & 0xffff;
    g.hi = w >> 16;
    const size_t st = g.hi ? 16 : 12;
    size_t e = p + 12;
    for (size_t i = 0; i < n * 4 / st && e + st <= plain.size(); ++i) {
      DirEntry d;
      d.name.assign(plain.begin() + e, plain.begin() + e + st - 8);
      d.sector = Le32(plain, e + st - 8);
      d.size = Le32(plain, e + st - 4);
      g.entries.push_back(std::move(d));
      e += st;
    }
    out.push_back(std::move(g));
    p = e;
  }
  return out;
}
size_t DirectoryBytes(const std::vector<DirGroup>& groups, bool by_type) {
  size_t n = 0;
  for (const auto& g : groups) {
    const std::string t(g.type.begin(), g.type.end());
    const size_t st = by_type ? ((t == "EVP " || t == "EVZ " || t == "EMD ") ? 16 : 12) : 16;
    n += 12 + g.entries.size() * st;
  }
  return n;
}
bool PlainOf(const Bytes& pac, Bytes* plain, std::string* error) {
  std::string d;
  if (!DecodeWWE13PacPlain(pac, plain, &d)) return Fail(error, d);
  return true;
}

// arcmodel.epac_rebuild: every group re-sorted by entry name, `add` appended to existing groups only,
// reserved word read from the source header at the output cursor, header size word kept.
using AddMap = std::map<std::string, std::vector<std::pair<Bytes, Bytes>>>;
using ReplMap = std::map<std::pair<std::string, std::string>, Bytes>;
bool EpacRebuild(const Bytes& pac, const AddMap& add, const ReplMap& repl, Bytes* output, std::string* error) {
  Bytes plain;
  if (!PlainOf(pac, &plain, error)) return false;
  const auto groups = GroupsFromPlain(plain);
  struct View { const uint8_t* p = nullptr; size_t n = 0; };
  auto slice = [&](const DirEntry& e) {
    const size_t at = 0x4000 + size_t(e.sector) * 0x800, len = size_t(e.size) * 0x100;
    const size_t a = std::min(at, plain.size()), b = std::min(at + len, plain.size());
    return View{plain.data() + a, b - a};
  };
  // python keeps one blob per (group, name): the last entry of a repeated name wins for every copy
  std::map<std::pair<Bytes, Bytes>, View> blobs;
  size_t total = 0x4000 + 0x800;
  for (const auto& g : groups) for (const auto& e : g.entries) blobs[{g.type, e.name}] = slice(e);
  struct Item { const Bytes* name; View data; };
  std::vector<std::vector<Item>> all;
  for (const auto& g : groups) {
    const std::string type = PyStrip(std::string(g.type.begin(), g.type.end()));
    std::vector<Item> items;
    for (const auto& e : g.entries) {
      const auto r = repl.find({type, PyStrip(std::string(e.name.begin(), e.name.end()))});
      items.push_back({&e.name, r != repl.end() ? View{r->second.data(), r->second.size()} : blobs[{g.type, e.name}]});
    }
    const auto ad = add.find(type);
    if (ad != add.end()) for (const auto& [nm, d] : ad->second) items.push_back({&nm, View{d.data(), d.size()}});
    std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return *x.name < *y.name; });
    for (const auto& it : items) total += (it.data.n + 0x7ff) & ~size_t(0x7ff);
    all.push_back(std::move(items));
  }
  Bytes out;
  out.reserve(total);
  out.assign(plain.begin(), plain.begin() + 0x4000);
  size_t q = 0x800, body = 0;
  for (size_t gi = 0; gi < groups.size(); ++gi) {
    const auto& g = groups[gi];
    const size_t st = g.hi ? 16 : 12;
    if (q + 12 + all[gi].size() * st > 0x4000) return Fail(error, "the rebuilt directory does not fit");
    std::copy(g.type.begin(), g.type.end(), out.begin() + q);
    PutLe32(&out, q + 4, uint32_t(all[gi].size() * st / 4) | (g.hi << 16));
    PutLe32(&out, q + 8, Le32(plain, q + 8));
    q += 12;
    for (const auto& it : all[gi]) {
      if (it.name->size() != st - 8) return Fail(error, "entry name '" + std::string(it.name->begin(), it.name->end()) + "' does not fit its group");
      std::copy(it.name->begin(), it.name->end(), out.begin() + q);
      PutLe32(&out, q + st - 8, uint32_t(body / 0x800));
      PutLe32(&out, q + st - 4, uint32_t((it.data.n + 0xff) / 0x100));
      out.insert(out.end(), it.data.p, it.data.p + it.data.n);
      body += it.data.n;
      const size_t padded = (body + 0x7ff) & ~size_t(0x7ff);
      out.resize(out.size() + (padded - body), 0);
      body = padded;
      q += st;
    }
  }
  std::fill(out.begin() + q, out.begin() + 0x4000, 0);
  PutLe32(&out, 8, uint32_t(body));
  out.resize(out.size() + 0x800, 0);
  *output = std::move(out);
  return true;
}

// arc model (big-endian): u32 ff000000, per pac its groups then ffff + index, ...
Bytes EmitArcBlock(const std::vector<DirGroup>& groups, uint16_t index) {
  Bytes out;
  for (const auto& g : groups) {
    const size_t st = g.hi ? 16 : 12;
    out.insert(out.end(), g.type.begin(), g.type.end());
    const uint32_t w = uint32_t((g.entries.size() * st / 4) << 20) | (g.hi << 12);
    for (int i = 3; i >= 0; --i) out.push_back(uint8_t(w >> (8 * i)));
    for (int i = 0; i < 4; ++i) out.push_back(0);
    for (const auto& e : g.entries) {
      out.insert(out.end(), e.name.begin(), e.name.end());
      for (int i = 3; i >= 0; --i) out.push_back(uint8_t(e.sector >> (8 * i)));
      for (int i = 3; i >= 0; --i) out.push_back(uint8_t(e.size >> (8 * i)));
    }
  }
  out.push_back(0xff); out.push_back(0xff);
  out.push_back(uint8_t(index >> 8)); out.push_back(uint8_t(index));
  return out;
}

// One PAC's group/entry lookup (trimmed names), first match wins.
bool FindIn(const Bytes& pac, const std::string& group, const std::string& name, Bytes* out) {
  std::vector<WWE13PacGroupData> groups;
  std::string ignored;
  if (!ReadWWE13PacGroups(pac, &groups, &ignored)) return false;
  for (const auto& g : groups)
    if (Trim(g.name) == group)
      for (const auto& e : g.entries)
        if (Trim(e.name) == name) { *out = e.data; return true; }
  return false;
}

// Directory-only PAC cache: uncompressed files are read entry by entry on demand; compressed ones
// (most WWE 2K15 files) keep their decompressed bytes until the cache is cleared.
struct LazyEntry { std::string name; uint32_t sector = 0, size = 0; };
struct LazyGroup { std::string name; bool wide = false; std::vector<LazyEntry> entries; };
struct LazyPac { fs::path path; bool compressed = false; Bytes plain; std::vector<LazyGroup> groups; uint64_t file_size = 0; };
struct PacCache {
  std::map<fs::path, LazyPac> pacs;
  const LazyPac& Get(const fs::path& path) {
    auto it = pacs.find(path);
    if (it != pacs.end()) return it->second;
    LazyPac pac;
    pac.path = path;
    Bytes head;
    std::error_code ec;
    pac.file_size = fs::file_size(path, ec);
    if (ec) pac.file_size = 0;
    const Bytes* dir = nullptr;
    if (ReadHead(path, 0x4000, &head) && head.size() >= 4 && (std::memcmp(head.data(), "EPAC", 4) == 0 || std::memcmp(head.data(), "EPK8", 4) == 0)) {
      dir = &head;
    } else {
      Bytes raw;
      std::string ignored;
      if (ReadFileBytes(path, &raw, &ignored) && DecodeWWE13PacPlain(raw, &pac.plain, &ignored)) { pac.compressed = true; dir = &pac.plain; }
    }
    if (dir && dir->size() >= 0x4000) {
      size_t at = 0x800;
      while (at + 12 <= 0x4000 && !((*dir)[at] == 0 && (*dir)[at + 1] == 0 && (*dir)[at + 2] == 0 && (*dir)[at + 3] == 0)) {
        const uint32_t word = Le32(*dir, at + 4);
        LazyGroup g;
        g.name.assign(dir->begin() + at, dir->begin() + at + 4);
        g.wide = ((word >> 16) & 1) != 0;
        const size_t rec = g.wide ? 16 : 12, nsz = g.wide ? 8 : 4;
        const size_t count = (word & 0xffff) / (rec / 4);
        at += 12;
        for (size_t i = 0; i < count && at + rec <= 0x4000; ++i, at += rec)
          g.entries.push_back({std::string(dir->begin() + at, dir->begin() + at + nsz), Le32(*dir, at + nsz), Le32(*dir, at + nsz + 4)});
        pac.groups.push_back(std::move(g));
      }
    }
    return pacs.emplace(path, std::move(pac)).first->second;
  }
  Bytes Data(const LazyPac& pac, const LazyEntry& e) {
    const uint64_t at = 0x4000 + uint64_t(e.sector) * 0x800, len = uint64_t(e.size) * 0x100;
    if (pac.compressed) {
      const size_t a = size_t(std::min<uint64_t>(at, pac.plain.size())), b = size_t(std::min<uint64_t>(at + len, pac.plain.size()));
      return Bytes(pac.plain.begin() + a, pac.plain.begin() + b);
    }
    if (at >= pac.file_size) return {};
    Bytes out(size_t(std::min<uint64_t>(len, pac.file_size - at)));
    std::ifstream in(pac.path, std::ios::binary);
    in.seekg(std::streamoff(at));
    in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
    return out;
  }
  // first file (in the given order) holding group/name
  bool Find(const std::vector<fs::path>& files_in_order, const std::string& group, const std::string& name, Bytes* out,
            std::string* where = nullptr) {
    for (const auto& f : files_in_order) {
      const LazyPac& pac = Get(f);
      for (const auto& g : pac.groups)
        if (Trim(g.name) == group)
          for (const auto& e : g.entries)
            if (Trim(e.name) == name) { *out = Data(pac, e); if (where) *where = internal::PathToUtf8(f.filename()); return true; }
    }
    return false;
  }
};

// sorted(glob(dir/prefix*.pac)) with python's byte order; `prefixes` matched case-insensitively (GitHub #135)
std::string AsciiLower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}
std::vector<fs::path> Glob(const fs::path& dir, const std::vector<std::string>& prefixes) {
  std::vector<std::string> names;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = internal::PathToUtf8(it->path().filename());
    const std::string nl = AsciiLower(n);
    if (n.size() < 4 || nl.substr(n.size() - 4) != ".pac") continue;
    for (const auto& p : prefixes) if (nl.rfind(AsciiLower(p), 0) == 0) { names.push_back(n); break; }
  }
  std::sort(names.begin(), names.end());
  std::vector<fs::path> out;
  for (const auto& n : names) out.push_back(dir / fs::u8path(n));
  return out;
}

std::string PyTitle(const std::string& s) {  // python str.title() on ASCII/latin1 bytes
  std::string out = s;
  bool prev_cased = false;
  for (char& c : out) {
    const unsigned char u = static_cast<unsigned char>(c);
    const bool alpha = std::isalpha(u) != 0;
    if (alpha) c = static_cast<char>(prev_cased ? std::tolower(u) : std::toupper(u));
    prev_cased = alpha;
  }
  return out;
}

struct Person {
  WWE15ImportPort port;
  uint32_t entrance_key = 0;
  WWE15MovePlan plan;
  uint32_t dat_index = 0;
  uint8_t cls = 0;
  WWE15Rows rows;
  uint16_t rank0 = 0, rank2 = 0;
};

}  // namespace

size_t CountWWE15OfferRoom(const std::vector<size_t>& current_offers, size_t per_catalog_cap) {
  size_t room = 0;
  for (size_t offers : current_offers) {
    if (offers < per_catalog_cap) room += per_catalog_cap - offers;
  }
  return room;
}

bool ReadWWE15ImportRecipes(const fs::path& file, std::vector<WWE15ImportPort>* ports, std::string* error) {
  Bytes raw;
  if (!ReadFileBytes(file, &raw, error)) return false;
  std::istringstream in(std::string(raw.begin(), raw.end()));
  std::string line;
  size_t number = 0;
  bool header = false;
  ports->clear();
  while (std::getline(in, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) {
      const size_t b = line.find('\t', a);
      f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
      if (b == std::string::npos) break;
      a = b + 1;
    }
    if (!header) { header = true; if (f.size() >= 10 && f[0] == "id15") continue; }
    if (f.size() < 10) return Fail(error, "data/wwe2k15-ports.tsv line " + std::to_string(number) + " has " + std::to_string(f.size()) + " columns; 10 expected.");
    try {
      WWE15ImportPort p;
      p.source_id = std::stoul(f[0]); p.target_id = std::stoul(f[1]); p.name = f[2]; p.handle = f[3];
      p.donor_id = std::stoul(f[4]); p.alternate = f[5] == "alt";
      p.original14_id = f[6].empty() ? 0 : std::stoul(f[6]);
      p.entrance_pack = std::stoul(f[7]);
      std::istringstream opts(f[8]);
      std::string o;
      while (std::getline(opts, o, ',')) {
        if (o.empty()) continue;
        if (o.rfind("victory15=", 0) == 0) p.victory15 = std::stoul(o.substr(10));
        else p.options.insert(o);
      }
      if (f[9] == "F") p.gender = "F";
      else if (f[9] != "M") return Fail(error, "data/wwe2k15-ports.tsv line " + std::to_string(number) + ": gender must be M or F.");
      if (p.entrance_pack < 3 || p.entrance_pack > 5) return Fail(error, "data/wwe2k15-ports.tsv line " + std::to_string(number) + ": entrance pack must be 3, 4 or 5.");
      ports->push_back(std::move(p));
    } catch (const std::exception&) {
      return Fail(error, "data/wwe2k15-ports.tsv line " + std::to_string(number) + ": a number could not be read.");
    }
  }
  return true;
}

bool IsWWE15PacFolder(const fs::path& pac) {
  std::error_code e;
  if (!fs::is_regular_file(pac / "ch" / "chEtc.pac", e) || !fs::is_regular_file(pac / "m3.pac", e) ||
      !fs::is_regular_file(pac / "createmode.pac", e)) return false;
  // WWE 2K15 content WWE 2K14 lacks: Rusev's (424) and Bray Wyatt's (398) models and the menu*2 portrait packs
  return fs::is_regular_file(pac / "ch" / "ch424.pac", e) && fs::is_regular_file(pac / "ch" / "ch398.pac", e) &&
         fs::is_regular_file(pac / "menu" / "menuHD2.pac", e) && fs::is_regular_file(pac / "menu" / "menuSD2.pac", e);
}

fs::path FindWWE15PacFolder(const fs::path& selected) {
  std::error_code ec;
  const auto looks_right = IsWWE15PacFolder;
  for (const fs::path& c : {selected / "pac", selected / "game" / "pac", selected})
    if (looks_right(c)) return c;
  for (fs::recursive_directory_iterator it(selected, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
    std::error_code e;
    if (it->is_directory(e) && it->path().filename() == "pac" && looks_right(it->path())) return it->path();
  }
  return {};
}

bool RunWWE15ImportStages(const WWE15ImportPaths& paths, const std::vector<WWE15ImportPort>& requested,
                          const fs::path& staging, const WWE15ImportOptions& options,
                          WWE15ImportRunResult* result, std::string* error, const ProgressFn& progress,
                          const CancelFlag* cancel) {
  if (error) error->clear();
  if (!result) return Fail(error, "The import result output is missing.");
  result->failed_source_id = 0;
  // A failure that belongs to one wrestler: remember who, so the caller can skip him and import the rest.
  auto person_fail = [&](const WWE15ImportPort& who, std::string message) {
    result->failed_source_id = who.source_id;
    return Fail(error, std::move(message));
  };
  std::error_code ec;
  if (staging.empty() || fs::exists(staging, ec)) return Fail(error, "The staging folder must be new.");
  bool keep_staging = false;
  struct Cleanup {
    fs::path path;
    bool* keep;
    ~Cleanup() { if (!*keep) { std::error_code e; fs::remove_all(path, e); } }
  } cleanup{staging, &keep_staging};
  auto cancelled = [&] { return cancel && cancel->load(); };
  static const CancelFlag no_cancel{false};
  auto stage = [&](uint64_t n, const std::string& label) { if (progress) progress(n, 10, label); };
  const fs::path g15 = FindWWE15PacFolder(paths.wwe15_root);
  if (g15.empty())
    return Fail(error, "No WWE 2K15 game files were found in " + internal::PathToUtf8(paths.wwe15_root) +
                           ". Choose the folder that holds the extracted WWE 2K15 disc (its pac folder with ch/chEtc.pac, "
                           "m3.pac and createmode.pac). Nothing has been changed.");
  const fs::path g14 = paths.wwe14_root;
  const fs::path title = paths.userdata_root / "0000000000000000" / "545408B2" / "00000002";

  // ---- packs, found by contents
  // The WWE '13 import renames Add-On 2 to a folder starting with '0' (045BEE31.., or 067E5576.. from the 4-offer
  // version) and adds its wrestlers there; found by contents: a pack-2 catalog and the import's chEtc_dlc02.pac.
  std::string host;
  for (fs::directory_iterator it(title, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = internal::PathToUtf8(it->path().filename());
    if (name.size() != 42 || name[0] != '0') continue;
    Bytes head;
    if (!ReadHead(ResolvePathCase(it->path(), "info/catalog.dlc"), 0x20, &head) || head.size() < 0x1c || std::memcmp(head.data(), "DLCC", 4) != 0 ||
        Le16(head, 8) != 2 || !fs::is_regular_file(ResolvePathCase(it->path(), "pac/ch/chEtc_dlc02.pac"), ec))
      continue;
    if (host.empty() || name < host) host = name;
  }
  // No WWE '13 import: make that folder here, the same way (Add-On 2 found by contents, renamed with '0' first,
  // its header rewritten); Undo restores the original Add-On 2 folder and header.
  std::string host_source;
  const bool create_host = host.empty();
  if (create_host) {
    host_source = FindWWE2K14HostPack(paths.userdata_root);
    if (host_source.empty()) {
      for (fs::directory_iterator it(title, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = internal::PathToUtf8(it->path().filename());
        if (name.size() == 42 && name[0] == '0')
          return Fail(error, "This userdata folder already holds an earlier import that the launcher has no record of (for "
                             "example, copied from another install): the folder " + name + " is there and the add-on it was "
                             "made from is not. Delete that folder, add your WWE 2K14 add-ons again on the Game files page "
                             "(Add TU or DLC), then import. Nothing has been changed.");
      }
      return Fail(error, "Importing needs one version of WWE 2K14 Add-On 2 (the add-on with the pre-order bonuses) and none "
                         "was found in the userdata folder. Add it on the Game files page first (Add TU or DLC). Nothing has "
                         "been changed.");
    }
    host = WWE13HostOutputPack(host_source);
    if (fs::exists(title / host, ec))
      return Fail(error, "The add-on folder " + host + " is already there but has no import tables. Delete it, add your WWE 2K14 "
                         "add-ons again on the Game files page, then import. Nothing has been changed.");
    if (!fs::is_regular_file(paths.userdata_root / "0000000000000000" / "545408B2" / "Headers" / "00000002" / (host_source + ".header"), ec))
      return Fail(error, "The WWE 2K14 add-on's header file was not found (" + host_source + ".header). Add the add-on again on "
                         "the Game files page (Add TU or DLC). Nothing has been changed.");
  }
  std::map<uint32_t, std::string> pack_of;  // 1, 3, 4, 5 -> folder
  for (fs::directory_iterator it(title, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = internal::PathToUtf8(it->path().filename());
    if (name.size() == 42 && name[0] == '0') continue;  // import outputs
    Bytes head;
    if (!ReadHead(ResolvePathCase(it->path(), "info/catalog.dlc"), 0x20, &head) || head.size() < 0x1c || std::memcmp(head.data(), "DLCC", 4) != 0) continue;
    const uint32_t id = Le16(head, 8);
    if (id == 1 || ((id == 3 || id == 4 || id == 5) && fs::is_regular_file(ResolvePathCase(it->path(), "pac/string_dlc0" + std::to_string(id) + ".pac"), ec))) {
      if (!pack_of.count(id) || name < pack_of[id]) pack_of[id] = name;
    }
  }
  for (uint32_t id : {3u, 4u, 5u})
    if (!pack_of.count(id))
      return Fail(error, "WWE 2K14 Add-On " + std::to_string(id) + " was not found in the userdata folder (looked for a pack whose "
                         "catalog says pack " + std::to_string(id) + " and that has pac/string_dlc0" + std::to_string(id) +
                         ".pac). Add it on the Game files page first. Nothing has been changed.");
  auto U = [&](const std::string& pack, const std::string& rel) { return std::string(kUdp) + pack + "/" + rel; };
  auto upath = [&](const std::string& pack, const std::string& rel) { return ResolvePathCase(title / pack, rel); };

  // staged files are written as soon as they are made (memory stays near one wrestler's files)
  std::set<std::string> staged_names;
  auto put = [&](const std::string& rel, const Bytes& d) -> bool {
    if (rel.size() > 4 && rel.substr(rel.size() - 4) == ".pac" && d.size() >= 0x4000 &&
        (std::memcmp(d.data(), "EPAC", 4) == 0 || std::memcmp(d.data(), "EPK8", 4) == 0)) {
      const size_t dl = DirectoryBytes(GroupsFromPlain(d), true);
      if (dl > kDirRoom) return Fail(error, rel + ": its directory would be " + std::to_string(dl) + " bytes; " + std::to_string(kDirRoom) + " fit.");
    }
    if (!WriteFileBytes(staging / "files" / fs::u8path(rel), d, error)) return false;
    staged_names.insert(rel);
    return true;
  };
  auto read = [&](const fs::path& p, Bytes* b) { return ReadFileBytes(p, b, error); };

  // ---- people
  if (requested.empty()) return Fail(error, "Choose at least one WWE 2K15 wrestler.");
  std::vector<Person> people;
  std::set<uint32_t> targets;
  for (const auto& p : requested) {
    if (!targets.insert(p.target_id).second) return Fail(error, "Slot " + std::to_string(p.target_id) + " is chosen twice.");
    Person x;
    x.port = p;
    people.push_back(std::move(x));
  }
  std::vector<uint32_t> own_base;
  {
    Bytes raw;
    std::string ignored;
    if (!paths.own_slots.empty() && ReadFileBytes(paths.own_slots, &raw, &ignored)) {
      std::istringstream in(std::string(raw.begin(), raw.end()));
      uint32_t v = 0;
      while (in >> v) own_base.push_back(v);
    }
  }
  for (const auto& p : people)
    if (std::find(own_base.begin(), own_base.end(), p.port.target_id) != own_base.end())
      return person_fail(p.port, p.port.name + " (WWE 2K15 " + std::to_string(p.port.source_id) + "): slot " + std::to_string(p.port.target_id) +
                         " is already used by an imported wrestler. Nothing has been changed.");

  // select-screen room first (each wrestler needs one offer: Add-On 3's catalog to 64, then pack 1's to 64)
  {
    std::vector<size_t> catalog_offers;
    for (uint32_t id : {3u, 1u}) {
      if (!pack_of.count(id)) continue;
      Bytes head;
      if (ReadHead(upath(pack_of[id], "info/catalog.dlc"), 0x20, &head) && head.size() >= 14)
        catalog_offers.push_back(Le16(head, 12));
    }
    const size_t room = CountWWE15OfferRoom(catalog_offers);
    if (people.size() > room)
      return Fail(error, std::to_string(people.size()) + " wrestlers were chosen but only " + std::to_string(room) +
                         " select-screen places are left (Add-On 3 and pack 1 hold 64 each). Choose fewer wrestlers. Nothing has been changed.");
  }
  stage(1, "Reading WWE 2K15 wrestler rows…");
  Bytes c15, c14;
  WWE13RawRowMap dat15, pro15, dat14, pro14;
  if (!read(g15 / "ch" / "chEtc.pac", &c15) || !read(g14 / "pac" / "ch" / "chEtc.pac", &c14)) return false;
  if (!ReadWWE13PacRows(c15, "CHAR", "DAT", &dat15, error) || !ReadWWE13PacRows(c15, "CHAR", "PRO", &pro15, error) ||
      !ReadWWE13PacRows(c14, "CHAR", "DAT", &dat14, error) || !ReadWWE13PacRows(c14, "CHAR", "PRO", &pro14, error))
    return Fail(error, "The chEtc tables could not be read: " + *error);
  for (auto& p : people) {
    const auto& s = p.port;
    if (!pro15.count(s.source_id) || pro15.at(s.source_id).size() < 586 || !dat15.count(s.source_id))
      return person_fail(s, s.name + ": WWE 2K15 has no complete row for wrestler " + std::to_string(s.source_id) + " (PRO " +
                             std::to_string(pro15.count(s.source_id) ? pro15.at(s.source_id).size() : 0) + " bytes; at least 586 expected).");
    if (!pro14.count(s.donor_id) || !dat14.count(s.donor_id))
      return person_fail(s, s.name + ": the WWE 2K14 donor " + std::to_string(s.donor_id) + " has no chEtc row.");
    p.entrance_key = Le16(pro15.at(s.source_id), 584);
    p.cls = dat15.at(s.source_id).size() > 26 ? dat15.at(s.source_id)[26] : 0;
  }

  // ---- move facts and plans
  stage(2, "Planning move lists…");
  WWE15MoveFacts facts;
  for (const auto& f : Glob(g14 / "pac", {"dlc_mot", "mot"})) {
    Bytes raw;
    if (!read(f, &raw) || !ScanWWE15MotionIds(raw, &facts.base14, error)) return false;
  }
  for (const auto& [id, pack] : pack_of) {
    for (const auto& f : Glob(ResolvePathCase(title / pack, "pac"), {"dlc_mot"})) {
      Bytes raw;
      if (!read(f, &raw) || !ScanWWE15MotionIds(raw, &facts.addon14, error)) return false;
    }
  }
  for (const auto& f : Glob(g15, {"dlc_mot", "mot"})) {
    Bytes raw;
    if (!read(f, &raw) || !ScanWWE15MotionIds(raw, &facts.all15, error)) return false;
  }
  const std::vector<std::string> mot15 = {"mot00", "mot01", "mot02", "mot03", "mot05", "mot06", "mot07", "mot08"};
  for (const auto& n : mot15) {
    Bytes raw;
    if (fs::is_regular_file(g15 / (n + ".pac"), ec) && (!read(g15 / (n + ".pac"), &raw) || !ScanWWE15MotionParts(raw, &facts.even15, &facts.odd15, error))) return false;
  }
  Bytes m3_15, cm15, waze15;
  std::map<uint32_t, Bytes> prm15;
  if (!read(g15 / "m3.pac", &m3_15) || !ReadWWE15PrmTables(m3_15, &prm15, error) || !read(g15 / "createmode.pac", &cm15)) return false;
  facts.prm15_rows = prm15[0];
  if (!FindIn(cm15, "MOVS", "WAZE", &waze15) || !Unpack(Bytes(waze15), &waze15, error) || waze15.size() < kWWE15WazeHeader)
    return Fail(error, "WWE 2K15 createmode.pac has no readable move list (MOVS/WAZE).");
  for (size_t i = 0; i < Le32(waze15, 4) && kWWE15WazeHeader + kWWE15WazeRecord * (i + 1) <= waze15.size(); ++i)
    facts.waze15.insert(Le16(waze15, kWWE15WazeHeader + kWWE15WazeRecord * i + 152));
  for (const auto& [id, r] : pro14) {
    if (r.size() != 1276 || Le16(r, 576) == 255) continue;
    for (size_t s = 0; s < 262; ++s) if (Le16(r, 4 + 2 * s)) facts.playable14.insert(Le16(r, 4 + 2 * s));
  }
  WWE15MoveRules rules;
  {
    Bytes raw;
    if (!read(paths.moves_data, &raw) || !ReadWWE15MoveRules(std::string(raw.begin(), raw.end()), &rules, error)) return false;
  }
  for (auto& p : people) {
    if (!PlanWWE15Moves(p.port.source_id, pro15.at(p.port.source_id), pro14.at(p.port.donor_id), facts, rules, &p.plan, error)) return false;
    const Bytes& row15 = pro15.at(p.port.source_id);
    size_t total = 0, own = 0;
    for (size_t s = 3; s < 262; ++s) {
      const uint16_t v = Le16(row15, 4 + 2 * s);
      if (!v) continue;
      ++total;
      if (p.plan.slots[s] == v) ++own;
    }
    for (size_t s = 0; s < 22; ++s) {
      const uint16_t v = Le16(row15, 528 + 2 * s);
      if (!v) continue;
      ++total;
      if (p.plan.extras[s] == v) ++own;
    }
    result->move_notes.push_back(p.port.name + " (2K15 id " + std::to_string(p.port.source_id) + "): " + std::to_string(own) +
                                 " of " + std::to_string(total) + " moves are the wrestler's own 2K15 moves");
    for (const auto& n : p.plan.notes) result->move_notes.push_back("  " + n);
  }
  if (cancelled()) return Fail(error, "The import was canceled.");

  // ---- own entrance themes (owner 11-16: "their 2K15 themes converted over just like WWE '13 imports")
  std::set<uint32_t> own_song;
  if (options.audio) {
    stage(3, "Adding the wrestlers' WWE 2K15 entrance themes…");
    std::vector<WWE15MusicRequest> songs;
    for (const auto& p : people)
      if (!p.port.options.count("donor-music"))
        songs.push_back({p.port.source_id, p.port.target_id, Le16(pro15.at(p.port.source_id), 576)});
    WWE15ImportMusicResult music;
    if (!RunWWE15ImportMusic(g15.parent_path(), g14, paths.userdata_root, songs, staging / "files", cancel ? *cancel : no_cancel,
                             &music, error))
      return Fail(error, "WWE 2K15 entrance themes: " + *error);
    for (const auto& f : music.files) staged_names.insert(f);
    own_song = music.targets_with_song;
    result->theme_levels = music.theme_levels;
    // GitHub #155: name who keeps a borrowed WWE 2K14 theme, instead of source IDs.
    std::string borrowed;
    for (const auto& p : people)
      if (!own_song.count(p.port.target_id)) borrowed += (borrowed.empty() ? "" : ", ") + p.port.name;
    if (!borrowed.empty())
      result->notices.push_back("No WWE 2K15 theme was found for " + borrowed + ": they keep a WWE 2K14 theme (choose another in "
                                "Create an Entrance if you like).");
  }
  if (cancelled()) return Fail(error, "The import was canceled.");

  // ---- rows
  stage(3, "Building wrestler rows…");
  std::set<uint16_t> used;
  {
    std::vector<fs::path> etc{g14 / "pac" / "ch" / "chEtc.pac"};
    if (!create_host) etc.push_back(upath(host, "pac/ch/chEtc_dlc02.pac"));
    for (uint32_t n : {3u, 4u, 5u}) etc.push_back(upath(pack_of[n], "pac/ch/chEtc_dlc0" + std::to_string(n) + ".pac"));
    for (const auto& f : etc) {
      Bytes raw;
      WWE13RawRowMap d;
      if (!read(f, &raw)) return false;
      if (!ReadWWE13PacRows(raw, "CHAR", "DAT", &d, error)) return Fail(error, internal::PathToUtf8(f.filename()) + ": " + *error);
      for (const auto& [id, r] : d) if (r.size() >= 8) used.insert(Le16(r, 6));
    }
  }
  std::vector<WWE13ConvertedRows> converted;
  for (auto& p : people) {
    uint32_t idx = 1;
    // 1..999: see core/wwe13_rows.cpp (owner 2026-11-14, #141; 1000..1049 are special in the game).
    while (idx < 1000 && used.count(uint16_t(idx))) ++idx;
    if (idx >= 1000) return person_fail(p.port, "No free wrestler index (1-999) is left for " + p.port.name + ".");
    used.insert(uint16_t(idx));
    p.dat_index = idx;
    WWE15RowsPort rp;
    rp.source_id = p.port.source_id; rp.target_id = p.port.target_id; rp.donor_id = p.port.donor_id; rp.dat_index = uint16_t(idx);
    rp.alternate = p.port.alternate; rp.original14_id = p.port.original14_id;
    // Own 2K15 theme added to the music pack: the selectors stay on the wrestler's own slot; else the donor's stock theme.
    rp.donor_music = !own_song.count(p.port.target_id);
    rp.slots = p.plan.slots; rp.extras = p.plan.extras;
    if (!ConvertWWE15Rows(rp, dat15.at(p.port.source_id), dat14.at(p.port.donor_id), pro14.at(p.port.donor_id), &p.rows, error)) return false;
    WWE13ConvertedRows c;
    c.target_id = p.port.target_id; c.dat_index = uint16_t(idx); c.dat = p.rows.dat; c.dsbd = p.rows.dsbd; c.pro = p.rows.pro;
    converted.push_back(std::move(c));
  }

  // ---- names and registry ranks
  WWE13StringKeyMap names;
  for (const auto& p : people) {
    const Bytes n = Ascii(p.port.name), h = Ascii(p.port.handle);
    for (uint32_t base : {60000u, 62000u, 64000u}) names[base + p.port.target_id] = n;
    names[79000 + p.port.target_id] = h;
  }
  std::map<uint32_t, std::string> all_names;
  {
    std::vector<fs::path> files{g14 / "pac" / "string.pac"};
    for (uint32_t n : {3u, 4u, 5u}) files.push_back(upath(pack_of[n], "pac/string_dlc0" + std::to_string(n) + ".pac"));
    for (const auto& f : files) {
      Bytes raw;
      WWE13StringKeyMap values;
      if (!read(f, &raw) || !ReadWWE13StringKeyValues(raw, &values, error)) return false;
      for (const auto& [k, v] : values) {
        if (k < 60000 || k >= 60500 || all_names.count(k)) continue;
        Bytes c = v;
        while (!c.empty() && c.back() == 0) c.pop_back();
        all_names[k] = std::string(c.begin(), c.end());
      }
    }
  }
  Bytes base_dlc;
  if (!read(g14 / "pac" / "DLC.pac", &base_dlc)) return false;
  std::vector<std::array<uint16_t, 4>> r0, r2;
  if (!WWE13SharedRegistryRows(base_dlc, "DLC.pac", 0, &r0, error) || !WWE13SharedRegistryRows(base_dlc, "DLC.pac", 2, &r2, error)) return false;
  std::set<uint16_t> t0, t2;
  for (const auto& r : r0) t0.insert(r[2]);
  for (const auto& r : r2) t2.insert(r[2]);
  std::vector<WWE13RegistryChar> chars;
  for (auto& p : people) {
    p.rank0 = WWE13SharedAutoRank(r0, all_names, p.port.name, t0); t0.insert(p.rank0);
    p.rank2 = WWE13SharedAutoRank(r2, all_names, p.port.name, t2); t2.insert(p.rank2);
    chars.push_back({p.port.target_id, p.rank0, p.rank2});
  }
  std::vector<WWE13AttireColorPort> att;
  for (const auto& p : people) att.push_back({p.port.target_id, p.port.donor_id});

  // ---- moves: routes
  std::set<uint16_t> all_ports;
  for (const auto& p : people) all_ports.insert(p.plan.port.begin(), p.plan.port.end());
  const std::vector<uint16_t> ids(all_ports.begin(), all_ports.end());
  std::vector<uint16_t> d004;
  std::map<uint16_t, uint16_t> owners;
  std::map<uint16_t, WWE15MotionRecord> records;
  if (!ids.empty()) {
    std::map<std::tuple<std::string, std::string, std::string, uint32_t, uint16_t>, std::set<uint32_t>> parity;
    std::vector<fs::path> route_pacs = Glob(g14 / "pac", {"mot"});
    for (fs::directory_iterator it(title, ec), end; !ec && it != end; it.increment(ec))
      for (const auto& f : Glob(ResolvePathCase(it->path(), "pac"), {"dlc_mot"})) route_pacs.push_back(f);
    std::set<uint16_t> found;
    for (const auto& f : route_pacs) {
      Bytes raw;
      std::vector<WWE15MotionHit> hits;
      if (!read(f, &raw) || !ScanWWE15MotionRoutes(raw, all_ports, &hits, error)) return false;
      for (const auto& h : hits) { parity[{internal::PathToUtf8(f), h.group, h.entry, h.bank, h.id}].insert(h.part & 1); found.insert(h.id); }
    }
    std::set<uint16_t> complete;
    for (const auto& [k, v] : parity) if (v.size() == 2) complete.insert(std::get<4>(k));
    for (uint16_t id : found)
      if (!complete.count(id)) return Fail(error, "Move " + std::to_string(id) + " is in a WWE 2K14 motion archive with only one of its two parts.");
    for (uint16_t id : ids) if (!complete.count(id)) d004.push_back(id);
    const std::set<uint16_t> want(d004.begin(), d004.end());
    for (const auto& n : mot15) {
      Bytes raw;
      if (fs::is_regular_file(g15 / (n + ".pac"), ec) && (!read(g15 / (n + ".pac"), &raw) || !CollectWWE15MotionRecords(raw, want, &records, error))) return false;
    }
    for (uint16_t id : ids) {
      uint16_t owner = 0;
      if (!ReadWWE15MoveOwner(waze15, id, &owner)) return Fail(error, "Move " + std::to_string(id) + " has no WWE 2K15 move-list record.");
      if (owner == 0x0200) continue;
      std::vector<const Person*> users;
      for (const auto& p : people) if (std::binary_search(p.plan.port.begin(), p.plan.port.end(), id)) users.push_back(&p);
      owners[id] = users.size() == 1 && users[0]->port.source_id == owner ? uint16_t(users[0]->port.target_id) : uint16_t(0x0200);
    }
  }

  // ---- base game files
  Bytes new_base_string, new_base_dlc, new_base_etc;  // a new host folder's own tables are these (as the WWE '13 import makes)
  stage(4, "Updating WWE 2K14 game files…");
  std::vector<Bytes> base_arcs(2);
  if (!read(g14 / "plist360.arc", &base_arcs[0]) || !read(g14 / "plist360_4x3.arc", &base_arcs[1])) return false;
  auto put_base = [&](const std::string& rel, const Bytes& old_raw, const Bytes& now) -> bool {
    Bytes old_plain;
    if (!PlainOf(old_raw, &old_plain, error)) return Fail(error, rel + ": " + *error);
    if (!SyncWWE13PacArchives(old_plain, now, &base_arcs, error, true)) return Fail(error, rel + " is not listed once in the game's pac lists: " + *error);
    return put("game/" + rel, now);
  };
  if (options.existing_wrestler_looks) {
    WWE15LooksFiles looks;
    if (!BuildWWE15LooksFiles(g14, g15, WWE15ExistingLookIds(), &base_arcs, &looks, error)) return false;
    for (const auto& [relative, bytes] : looks.files)
      if (!put("game/" + relative, bytes)) return false;
    result->notices.push_back("Updated WWE 2K15 looks for " + std::to_string(looks.reports.size()) +
                              " existing WWE 2K14 wrestlers.");
    for (const WWE15LooksReport& report : looks.reports) {
      if (!report.only15.empty()) {
        std::string names;
        for (const std::string& name : report.only15) names += (names.empty() ? "" : ", ") + name;
        result->notices.push_back("WWE 2K15 look " + std::to_string(report.id) +
                                  ": 2K15-only attires not added: " + names + ".");
      }
      for (const auto& [name, dropped] : report.dropped_leaves) {
        std::string leaves;
        for (uint32_t leaf : dropped) leaves += (leaves.empty() ? "" : ", ") + std::to_string(leaf);
        result->notices.push_back("WWE 2K15 look " + std::to_string(report.id) + " " + name +
                                  ": dropped leaves unknown to WWE 2K14: " + leaves + ".");
      }
    }
  }
  {
    Bytes old, now;
    size_t inserted = 0;
    if (!read(g14 / "pac" / "string.pac", &old) || !ConvertWWE13StringKeys(old, names, &now, &inserted, error) || !put_base("pac/string.pac", old, now)) return false;
    if (create_host) new_base_string = now;
    if (!ConvertWWE13DlcRegistry(base_dlc, chars, "DLC.pac", &now, error) || !put_base("pac/DLC.pac", base_dlc, now)) return false;
    if (create_host) new_base_dlc = now;
    if (!WWE13SharedPatchChEtc(c14, converted, &now, error) || !put_base("pac/ch/chEtc.pac", c14, now)) return false;
    if (create_host) new_base_etc = now;
    if (!read(g14 / "pac" / "misc01_Start.pac", &old) || !ConvertWWE13AttireColors(old, att, &now, error) || !put_base("pac/misc01_Start.pac", old, now)) return false;
  }
  // DSC entrance camera: the donor's, else a same-class stock one
  {
    const std::map<uint8_t, uint32_t> dsc_class{{3, 318}, {2, 219}, {4, 119}};
    const auto dsc_files = Glob(g14 / "pac" / "evtdsc", {""});
    PacCache dsc;
    auto has_dsc = [&](uint32_t id) {
      for (const auto& f : dsc_files)
        for (const auto& g : dsc.Get(f).groups)
          if (Trim(g.name).rfind("DSC", 0) == 0)
            for (const auto& e : g.entries) if (Trim(e.name) == "M" + std::to_string(id)) return true;
      return false;
    };
    std::vector<std::string> order;
    std::map<std::string, AddMap> adds;
    for (const auto& p : people) {
      const uint32_t t = p.port.target_id;
      std::optional<uint32_t> src;
      if (has_dsc(p.port.donor_id)) src = p.port.donor_id;
      else if (dsc_class.count(p.cls)) src = dsc_class.at(p.cls);
      if (has_dsc(t) || !src) continue;
      bool done = false;
      for (const auto& f : dsc_files) {
        const LazyPac& pac = dsc.Get(f);
        for (const auto& g : pac.groups) {
          if (Trim(g.name).rfind("DSC", 0) != 0) continue;
          for (const auto& e : g.entries)
            if (Trim(e.name) == "M" + std::to_string(*src)) {
              const std::string fname = internal::PathToUtf8(f.filename());
              if (!adds.count(fname)) order.push_back(fname);
              adds[fname][Trim(g.name)].push_back({Ascii("M" + std::to_string(t)), dsc.Data(pac, e)});
              done = true;
              break;
            }
          if (done) break;
        }
        if (done) break;
      }
    }
    for (const auto& fname : order) {
      Bytes old, now;
      if (!read(g14 / "pac" / "evtdsc" / fs::u8path(fname), &old) || !EpacRebuild(old, adds[fname], {}, &now, error) ||
          !put_base("pac/evtdsc/" + fname, old, now)) return Fail(error, "pac/evtdsc/" + fname + ": " + *error);
    }
  }
  if (!ids.empty()) {
    if (!fs::create_directories(staging / "files", ec) && ec) return Fail(error, "The staging folder could not be created.");
    Bytes old, now;
    WWE15PrmReport rep;
    if (!read(g14 / "pac" / "m3_patch.pac", &old) || !ExtendWWE15Prm6(old, m3_15, ids, &now, &rep, error) || !put_base("pac/m3_patch.pac", old, now)) return false;
    size_t sets = 0;
    if (!read(g14 / "pac" / "createmode.pac", &old) || !UpdateWWE15MoveMenus(old, waze15, ids, owners, d004, &now, &sets, error) ||
        !put_base("pac/createmode.pac", old, now)) return false;
  }
  if (cancelled()) return Fail(error, "The import was canceled.");

  // ---- ring-announcer names (owner 11-16 "call names"): 2K15 RASS key per slot; 2K15 name clips where 2K14 lacks the name
  if (options.audio) {
    stage(5, "Adding the ring announcer's names…");
    std::vector<WWE15NameRequest> names;
    for (const auto& p : people) names.push_back({p.port.source_id, p.port.target_id});
    Bytes old_sound_pac;
    if (!read(g14 / "pac" / "sound.pac", &old_sound_pac)) return false;
    WWE15AnnouncerResult announcer;
    if (!BuildWWE15AnnouncerNames(g15.parent_path(), g14 / "sound" / "Game_Sound_E_S.pck", old_sound_pac, names, &announcer, error))
      return Fail(error, "Ring-announcer names: " + *error);
    if (!announcer.es_pack.empty() && !put("game/sound/Game_Sound_E_S.pck", announcer.es_pack)) return false;
    if (!announcer.sound_pac.empty() && !put_base("pac/sound.pac", old_sound_pac, announcer.sound_pac)) return false;
    for (const auto& n : announcer.notes) result->notices.push_back(n);
    result->notices.push_back("Ring-announcer names for " + std::to_string(announcer.named.size()) + " of " +
                              std::to_string(people.size()) + " wrestlers.");
  }
  if (cancelled()) return Fail(error, "The import was canceled.");

  // ---- entrances (2K15 EVP2 converted, else the donor's family) and their event objects
  stage(5, "Converting entrances…");
  PacCache cache;
  const auto ent14 = Glob(g14 / "pac" / "evt", {"Entrance"});
  const auto ent15 = Glob(ResolvePathCase(g15, "evt"), {"Entrance", "entrance"});
  const auto win15 = Glob(ResolvePathCase(g15, "evt"), {"Winning"});
  const auto win14 = Glob(g14 / "pac" / "evt", {"Winning"});
  std::set<std::string> used_sobj;
  const fs::path obj14_path = g14 / "pac" / "evt" / "evtobj.pac", obj15_path = g15 / "evt" / "evtobj.pac";
  for (const auto& g : cache.Get(obj14_path).groups)
    if (PyStrip(g.name) == "SOBJ") for (const auto& e : g.entries) used_sobj.insert(Trim(e.name));
  for (uint32_t n : {3u, 4u, 5u}) {
    const fs::path f = upath(pack_of[n], "pac/evt/evtobj_dlc0" + std::to_string(n) + ".pac");
    if (fs::is_regular_file(f, ec))
      for (const auto& g : cache.Get(f).groups) if (PyStrip(g.name) == "SOBJ") for (const auto& e : g.entries) used_sobj.insert(Trim(e.name));
  }
  // event object by ID: python looks up str(id) first, then the 4-digit form; the last entry of a name wins
  auto sobj_of = [&](const fs::path& f, uint32_t v, Bytes* out) {
    const LazyPac& pac = cache.Get(f);
    for (const std::string key : {std::to_string(v), Pad4(v)}) {
      const LazyEntry* hit = nullptr;
      for (const auto& g : pac.groups)
        if (PyStrip(g.name) == "SOBJ") for (const auto& e : g.entries) if (Trim(e.name) == key) hit = &e;
      if (hit) { *out = cache.Data(pac, *hit); if (!out->empty()) return true; }
    }
    return false;
  };
  std::vector<uint32_t> free_sobj;
  for (uint32_t i = 3960; i < 4400; ++i) if (!used_sobj.count(Pad4(i)) && !used_sobj.count(std::to_string(i))) free_sobj.push_back(i);
  std::map<uint32_t, uint32_t> sobj_map;
  std::map<uint32_t, Bytes> sobj_new;
  std::map<uint32_t, AddMap> ent_add;
  for (auto& p : people) {
    const auto& s = p.port;
    const uint32_t t = s.target_id, d = s.donor_id;
    std::map<std::string, std::map<std::string, Bytes>> fam;  // group -> key -> data
    for (const std::string g : {"EVP", "EVZ"})
      for (const auto& f : ent14)
        for (const auto& grp : cache.Get(f).groups) {
          if (PyStrip(grp.name) != g) continue;
          for (const auto& e : grp.entries) {
            const std::string n = Trim(e.name);
            const std::string head = n.substr(0, 2);
            const bool five = n.size() == 5 && n.substr(2) == std::to_string(d) &&
                              (head == "10" || head == "11" || head == "12" || head == "13" || head == "14");
            if (n != std::to_string(d) && !five) continue;
            const std::string k = five ? n.substr(0, n.size() - 3) + std::to_string(t) : std::to_string(t);
            if (fam[g].count(k)) continue;
            if (g == "EVP") {
              Bytes r;
              if (!WWE13SharedRetargetEvp(cache.Data(cache.Get(f), e), std::stoul(k), std::stoul(n), &r, error))
                return person_fail(s, s.name + ": his entrance is built from WWE 2K14's " + internal::PathToUtf8(f.filename()) +
                                          ", and that file has been changed (by a mod or an older port). Restore your original "
                                          "WWE 2K14 game files to import him.");
              fam[g][k] = std::move(r);
            } else fam[g][k] = cache.Data(cache.Get(f), e);
          }
        }
    auto keys_of = [](const std::map<std::string, Bytes>& m) { std::set<std::string> k; for (const auto& [a, b] : m) k.insert(a); return k; };
    if (!fam["EVP"].count(std::to_string(t)) || keys_of(fam["EVP"]) != keys_of(fam["EVZ"]))
      return person_fail(s, s.name + ": the WWE 2K14 donor " + std::to_string(d) + " has no complete entrance (EVP and EVZ) to start from.");
    Bytes x, z;
    std::string where;
    const std::string ekey = std::to_string(p.entrance_key);
    if (!s.options.count("keep-donor-entrance") && cache.Find(ent15, "EVP2", ekey, &x, &where) && cache.Find(ent15, "EVZ", ekey, &z)) {
      Bytes plain, q, ref;
      WWE15Evp2Info info;
      std::vector<std::pair<uint32_t, uint32_t>> filled;
      std::vector<std::string> notes;
      ref = fam["EVP"][std::to_string(t)];
      if (!(ref.size() >= 4 && std::memcmp(ref.data(), "0FOP", 4) == 0) && !Unpack(Bytes(ref), &ref, error)) return false;
      if (!Unpack(x, &plain, error) || !ConvertWWE15Evp2(plain, ekey, &q, &info, error) || !FillWWE15WrestlerTracks(&q, ref, &filled, error) ||
          !RetargetWWE15Entrance(&q, t, s.source_id, p.entrance_key, s.original14_id, &notes, error))
        return person_fail(s, s.name + ": " + *error);
      std::vector<WWE15EntranceProp> props;
      if (!ListWWE15EntranceProps(q, &props, error)) return false;
      for (const auto& prop : props) {
        const uint32_t v = prop.object_id;
        Bytes b15v, b14v;
        if (!sobj_of(obj15_path, v, &b15v)) continue;
        const Bytes* b15 = &b15v;
        if (sobj_of(obj14_path, v, &b14v) && b14v == b15v) continue;
        if (!sobj_map.count(v)) {
          if (free_sobj.empty()) return person_fail(s, s.name + ": no free event-object ID (3960-4399) is left for entrance prop " + std::to_string(v) + ".");
          sobj_map[v] = free_sobj.front();
          free_sobj.erase(free_sobj.begin());
          sobj_new[sobj_map[v]] = *b15;
        }
        if (!SetWWE15EntranceProp(&q, prop.track, sobj_map[v], error)) return false;
      }
      fam["EVP"][std::to_string(t)] = StoreWWE13BpeLiteral(q);
      fam["EVZ"][std::to_string(t)] = z;
      result->notices.push_back(s.name + ": own WWE 2K15 entrance from " + where);
    } else {
      result->notices.push_back(s.name + ": the WWE 2K15 entrance " + ekey + (s.options.count("keep-donor-entrance") ? " is parked" : " was not found") +
                                "; the entrance of WWE 2K14 wrestler " + std::to_string(d) + " is used.");
    }
    for (const std::string g : {"EVP", "EVZ"}) {
      std::vector<std::pair<std::string, Bytes>> items(fam[g].begin(), fam[g].end());
      std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return std::stoul(a.first) < std::stoul(b.first); });
      for (auto& [k, v] : items) ent_add[s.entrance_pack][g].push_back({Ljust(k, 8), v});
    }
  }

  // the decompressed WWE 2K15 entrance and event-object files are not needed again
  for (const auto& f : ent15) cache.pacs.erase(f);
  cache.pacs.erase(obj15_path);
  cache.pacs.erase(obj14_path);

  // ---- retail add-on packs 3, 4, 5
  stage(6, "Updating add-on packs…");
  for (uint32_t n : {3u, 4u, 5u}) {
    const std::string pk = pack_of[n], sn = std::to_string(n);
    std::vector<Bytes> arcs(2);
    if (!read(upath(pk, "info/catalog_hd.arc"), &arcs[0]) || !read(upath(pk, "info/catalog_sd.arc"), &arcs[1])) return false;
    std::vector<std::pair<std::string, std::pair<Bytes, Bytes>>> todo;
    Bytes o, now;
    size_t inserted = 0;
    if (!read(upath(pk, "pac/string_dlc0" + sn + ".pac"), &o) || !ConvertWWE13StringKeys(o, names, &now, &inserted, error)) return false;
    todo.push_back({"pac/string_dlc0" + sn + ".pac", {o, now}});
    if (!read(upath(pk, "pac/DLC_dlc0" + sn + ".pac"), &o) || !ConvertWWE13DlcRegistry(o, chars, "DLC_dlc0" + sn + ".pac", &now, error)) return false;
    todo.push_back({"pac/DLC_dlc0" + sn + ".pac", {o, now}});
    if (!read(upath(pk, "pac/ch/chEtc_dlc0" + sn + ".pac"), &o) || !WWE13SharedPatchChEtc(o, converted, &now, error)) return false;
    todo.push_back({"pac/ch/chEtc_dlc0" + sn + ".pac", {o, now}});
    Bytes m;
    if (!read(upath(pk, "pac/misc_dlc0" + sn + ".pac"), &o) || !ConvertWWE13AttireColors(o, att, &m, error)) return false;
    if (!ids.empty()) {
      size_t sets = 0;
      if (!UpdateWWE15MoveMenus(m, waze15, ids, owners, d004, &now, &sets, error)) return false;
      m = now;
    }
    todo.push_back({"pac/misc_dlc0" + sn + ".pac", {o, m}});
    if (n == 4 && !d004.empty()) {
      if (!read(upath(pk, "pac/dlc_mot04.pac"), &o) || !MergeWWE15D004Archive(o, d004, records, &now, error)) return false;
      todo.push_back({"pac/dlc_mot04.pac", {o, now}});
    }
    if (ent_add.count(n)) {
      const std::string rel = "pac/evt/Entrance_dlc0" + sn + ".pac";
      Bytes plain;
      if (!read(upath(pk, rel), &o) || !PlainOf(o, &plain, error)) return false;
      const auto current = GroupsFromPlain(plain);
      std::set<std::pair<std::string, std::string>> existing;
      for (const auto& g : current) for (const auto& e : g.entries) existing.insert({PyStrip(std::string(g.type.begin(), g.type.end())), PyStrip(std::string(e.name.begin(), e.name.end()))});
      size_t adding = 0;
      for (const auto& [g, items] : ent_add[n])
        for (const auto& [nm, b] : items) {
          ++adding;
          if (existing.count({g, PyStrip(std::string(nm.begin(), nm.end()))}))
            return Fail(error, "Add-On " + sn + "'s entrance file already has " + g + " " + PyStrip(std::string(nm.begin(), nm.end())) + ". Nothing has been changed.");
        }
      size_t dir = 0;
      for (const auto& g : current) dir += 12 + g.entries.size() * 16;
      if (dir + 16 * adding > kProvenDir)
        return Fail(error, "Add-On " + sn + "'s entrance file has no room: its directory would be " + std::to_string(dir + 16 * adding) + " bytes (" +
                           std::to_string(dir) + " now); the game is proven up to " + std::to_string(kProvenDir) + ". Choose fewer wrestlers. Nothing has been changed.");
      if (!EpacRebuild(o, ent_add[n], {}, &now, error)) return Fail(error, rel + ": " + *error);
      Bytes np;
      PlainOf(now, &np, error);
      const size_t dl = DirectoryBytes(GroupsFromPlain(np), false);
      if (dl > (n == 4 ? kProvenDir : kDirRoom) || (n == 4 && now.size() > kProvenEntranceFile))
        return Fail(error, "Add-On " + sn + "'s entrance file would be " + std::to_string(now.size()) + " bytes with a " + std::to_string(dl) +
                           "-byte directory, beyond what the game is proven to load. Choose fewer wrestlers. Nothing has been changed.");
      todo.push_back({rel, {o, now}});
    }
    for (const auto& [rel, pair] : todo) {
      Bytes old_plain;
      if (!PlainOf(pair.first, &old_plain, error) || !SyncWWE13PacArchives(old_plain, pair.second, &arcs, error, true))
        return Fail(error, "Add-On " + sn + " " + rel + " is not listed once in its catalog: " + *error);
      if (!put(U(pk, rel), pair.second)) return false;
    }
    if (!put(U(pk, "info/catalog_hd.arc"), arcs[0]) || !put(U(pk, "info/catalog_sd.arc"), arcs[1])) return false;
  }
  if (cancelled()) return Fail(error, "The import was canceled.");

  // ---- host pack (the WWE '13 import's add-on folder)
  stage(7, "Building the wrestlers' add-on files…");
  std::map<std::string, std::vector<DirGroup>> H;  // host files made by this import -> their directory (bytes are written at once)
  auto host_base = [&](const std::string& rel, const AddMap& add, Bytes* out) -> bool {
    if (!create_host) return read(upath(host, rel), out);
    std::vector<WWE13PacGroupData> groups;
    auto group = [&](const char* name, bool wide) { WWE13PacGroupData g; g.name = name; g.wide = wide; groups.push_back(g); };
    if (rel.find("DLC_HD") != std::string::npos || rel.find("DLC_SD") != std::string::npos) { group("SSFA", false); group("SSFB", false); group("SSFC", false); }
    else if (rel.find("Winning") != std::string::npos) { group("EVP ", true); group("EVZ ", true); }
    else if (rel.find("evtmov") != std::string::npos) { group("MVSW", false); group("MVSB", false); }
    else if (rel.find("gm_") != std::string::npos) { group("GMEV", false); if (add.count("GMCH") && !add.at("GMCH").empty()) group("GMCH", false); }
    else if (rel.find("prtcls") != std::string::npos) group("PCE6", false);
    else return Fail(error, "no empty form for " + rel);
    return BuildWWE13PacGroups(groups, out, error);
  };
  auto hput = [&](const std::string& rel, const Bytes& d) -> bool {
    H[rel] = GroupsFromPlain(d);
    return put(U(host, rel), d);
  };
  {
    Bytes o, now;
    size_t inserted = 0;
    if (create_host) now = new_base_string;
    else if (!read(upath(host, "pac/string_dlc02.pac"), &o) || !ConvertWWE13StringKeys(o, names, &now, &inserted, error)) return false;
    if (!hput("pac/string_dlc02.pac", now)) return false;
    if (create_host) now = new_base_dlc;
    else if (!read(upath(host, "pac/DLC_dlc02.pac"), &o) || !ConvertWWE13DlcRegistry(o, chars, "DLC_dlc02.pac", &now, error)) return false;
    if (!hput("pac/DLC_dlc02.pac", now)) return false;
    if (create_host) now = new_base_etc;
    else if (!read(upath(host, "pac/ch/chEtc_dlc02.pac"), &o) || !WWE13SharedPatchChEtc(o, converted, &now, error)) return false;
    if (!hput("pac/ch/chEtc_dlc02.pac", now)) return false;
  }
  // portraits
  std::map<uint32_t, Bytes> ssfc_hd;  // target id -> 256x256 select portrait (fallback source for the grid face)
  {
    std::map<std::string, Bytes> menus;
    for (const std::string kind : {"HD", "SD"})
      if (!read(g15 / "menu" / ("menu" + kind + "2.pac"), &menus[kind])) return false;
    std::map<std::string, AddMap> add;
    std::vector<const Person*> sorted_people;
    for (const auto& p : people) sorted_people.push_back(&p);
    std::sort(sorted_people.begin(), sorted_people.end(), [](auto a, auto b) { return a->port.target_id < b->port.target_id; });
    for (const Person* p : sorted_people) {
      uint32_t src = p->port.source_id;
      auto have_all = [&](uint32_t id) {
        Bytes b;
        for (const std::string kind : {"HD", "SD"})
          for (const std::string g : {"SFAG", "SFBG", "SFCG"}) if (!FindIn(menus[kind], g, Pad4(id), &b)) return false;
        return true;
      };
      if (!have_all(src)) {
        if (!have_all(p->port.donor_id))
          return person_fail(p->port, p->port.name + ": WWE 2K15 has no menu portrait for " + std::to_string(src) + " or the donor " + std::to_string(p->port.donor_id) + ".");
        src = p->port.donor_id;
      }
      for (const std::string kind : {"HD", "SD"})
        for (const auto& [g15n, g14n] : std::vector<std::pair<std::string, std::string>>{{"SFAG", "SSFA"}, {"SFBG", "SSFB"}, {"SFCG", "SSFC"}}) {
          Bytes raw, plain, dds;
          FindIn(menus[kind], g15n, Pad4(src), &raw);
          const int w = g14n == "SSFC" ? 256 : 512, h = g14n == "SSFC" ? 256 : (kind == "HD" ? 512 : 256);
          if (!Unpack(raw, &plain, error) || !ConvertWWE15Portrait(plain, w, h, &dds, error))
            return person_fail(p->port, p->port.name + ": portrait " + kind + " " + g15n + " " + Pad4(src) + ": " + *error);
          add[kind][g14n].push_back({Ascii(Pad4(p->port.target_id)), dds});
          if (kind == "HD" && g14n == "SSFC") ssfc_hd[p->port.target_id] = dds;
        }
    }
    for (const std::string kind : {"HD", "SD"}) {
      Bytes o, now;
      const std::string rel = "pac/DLC_" + kind + "_dlc02.pac";
      if (!host_base(rel, add[kind], &o) || !EpacRebuild(o, add[kind], {}, &now, error)) return Fail(error, rel + ": " + *error);
      if (!hput(rel, now)) return false;
    }
  }
  // Main select grid faces (owner 2026-11-16; core/grid_faces.h): each wrestler's own 2K15 grid face (menu/menuHD.pac
  // MENU/SSFD, same 64x64 DXT5 format as 2K14) goes into the 2K14 menuHD/SD.pac under his slot; a wrestler without one gets
  // a head crop of his select portrait. Cosmetic: any problem here is a notice, never a failed import.
  {
    std::map<uint32_t, Bytes> own15;
    Bytes m15;
    std::string why;
    if (!ReadFileBytes(g15 / "menu" / "menuHD.pac", &m15, &why) || !ReadGridFaces(m15, &own15, &why)) own15.clear();
    size_t own = 0, cropped = 0;
    for (const std::string kind : {"HD", "SD"}) {
      const std::string rel = "pac/menu/menu" + kind + ".pac";
      Bytes old, now;
      std::map<uint32_t, Bytes> stock, faces;
      std::string detail;
      if (!ReadFileBytes(ResolvePathCase(g14 / "pac" / "menu", "menu" + kind + ".pac"), &old, &detail) ||
          !ReadGridFaces(old, &stock, &detail) || stock.empty()) {
        result->notices.push_back("Select-grid faces skipped (" + rel + ": " + detail + ").");
        continue;
      }
      own = cropped = 0;
      const GridFaceBytes backdrop = GridFaceBackdrop(stock);  // opaque tile background for portrait crops (owner 11-16)
      for (const auto& p : people) {
        const uint32_t target = p.port.target_id;
        if (stock.count(target)) continue;
        const auto mine = own15.find(p.port.source_id);
        if (mine != own15.end() && IsGridFace(mine->second)) { faces[target] = mine->second; ++own; continue; }
        const auto portrait = ssfc_hd.find(target);
        Bytes face;
        if (portrait != ssfc_hd.end() && MakeGridFaceFromPortrait(portrait->second, stock.begin()->second, &face, &detail, &backdrop)) {
          faces[target] = face;
          ++cropped;
        }
      }
      size_t added = 0;
      if (!AddGridFaces(old, faces, &now, &added, &detail)) {
        result->notices.push_back("Select-grid faces skipped (" + rel + ": " + detail + ").");
        continue;
      }
      if (!added) continue;
      // Each menu PAC is listed in ONE of the two pac lists (16:9 lists HD, 4:3 lists SD): sync where listed.
      Bytes old_plain;
      bool any_listed = false;
      if (!PlainOf(old, &old_plain, &detail)) { result->notices.push_back("Select-grid faces skipped (" + rel + ": " + detail + ")."); continue; }
      std::vector<Bytes> next_arcs = base_arcs;
      bool synced_ok = true;
      for (Bytes& arc : next_arcs) {
        bool synced = false;
        if (!SyncWWE13PacArchiveIfListed(&arc, old_plain, now, &synced, &detail)) { synced_ok = false; break; }
        any_listed = any_listed || synced;
      }
      if (!synced_ok || !any_listed) {
        result->notices.push_back("Select-grid faces skipped (" + rel + " is not in the game's pac lists" +
                                  (detail.empty() ? "" : ": " + detail) + ").");
        continue;
      }
      base_arcs = std::move(next_arcs);
      if (!put("game/" + rel, now)) return false;
    }
    result->notices.push_back("Select-grid faces: " + std::to_string(own) + " from WWE 2K15, " + std::to_string(cropped) +
                              " made from the select portrait.");
  }
  // victory, event moves, GMEV/GMCH, PCE6
  {
    AddMap win, mv, gm, pce;
    Bytes gm15, pr15;
    if (!read(g15 / "gm.pac", &gm15) || !read(g15 / "prtcls.pac", &pr15)) return false;
    const fs::path gm14 = g14 / "pac" / "gm.pac", pr14 = g14 / "pac" / "prtcls.pac", mov14 = g14 / "pac" / "evt" / "evtmov.pac";
    for (auto& p : people) {
      const auto& s = p.port;
      const uint32_t t = s.target_id;
      bool ok = false;
      std::vector<uint32_t> keys{s.source_id};
      if (p.entrance_key != s.source_id) keys.push_back(p.entrance_key);
      if (s.victory15 && std::find(keys.begin(), keys.end(), s.victory15) == keys.end()) keys.push_back(s.victory15);
      const std::string vkey = std::to_string(6000 + t);
      for (uint32_t k : keys) {
        Bytes x, z, plain, q;
        if (!cache.Find(win15, "EVP2", "6" + std::to_string(k), &x) || !cache.Find(win15, "EVZ", "6" + std::to_string(k), &z)) continue;
        WWE15Evp2Info info;
        if (!Unpack(x, &plain, error) || !ConvertWWE15Evp2(plain, "6" + std::to_string(k), &q, &info, error)) return person_fail(s, s.name + ": " + *error);
        PutBe16(&q, 32, 6000 + t);
        win["EVP"].push_back({Ljust(vkey, 8), StoreWWE13BpeLiteral(q)});
        win["EVZ"].push_back({Ljust(vkey, 8), z});
        ok = true;
        break;
      }
      for (uint32_t from : {s.original14_id, s.donor_id}) {
        if (ok || !from) continue;
        Bytes e, z, r;
        if (!cache.Find(win14, "EVP", "6" + std::to_string(from), &e) || !cache.Find(win14, "EVZ", "6" + std::to_string(from), &z)) continue;
        if (!WWE13SharedRetargetEvp(e, 6000 + t, 6000 + from, &r, error)) return person_fail(s, s.name + ": victory " + std::to_string(6000 + from) + ": " + *error);
        win["EVP"].push_back({Ljust(vkey, 8), r});
        win["EVZ"].push_back({Ljust(vkey, 8), z});
        ok = true;
      }
      if (!ok && !s.options.count("allow-no-victory"))
        return person_fail(s, s.name + ": no victory celebration was found (WWE 2K15 6" + std::to_string(s.source_id) + ", WWE 2K14 6" + std::to_string(s.donor_id) + ").");
      for (const std::string g : {"MVSW", "MVSB"}) {
        Bytes b;
        if (!cache.Find({mov14}, g, "M" + std::to_string(s.donor_id), &b)) {
          const uint32_t fallback = p.cls == 0 ? 290 : p.cls == 2 ? 219 : 319;
          if (!cache.Find({mov14}, g, "M" + std::to_string(fallback), &b))
            return person_fail(s, s.name + ": no event-move entry " + g + " for donor " + std::to_string(s.donor_id) + " or " + std::to_string(fallback) + ".");
        }
        mv[g].push_back({Ascii("M" + std::to_string(t)), b});
      }
      Bytes b;
      if (!FindIn(gm15, "GMEV", Pad4(s.source_id), &b) && !cache.Find({gm14}, "GMEV", Pad4(s.donor_id), &b))
        return person_fail(s, s.name + ": no GMEV entry in WWE 2K15 or for the donor.");
      gm["GMEV"].push_back({Ascii(Pad4(t)), b});
      if (cache.Find({gm14}, "GMCH", "5" + std::to_string(s.donor_id), &b)) gm["GMCH"].push_back({Ascii("5" + std::to_string(t)), b});
      if (!FindIn(pr15, "PCE6", Pad4(s.source_id), &b) && !cache.Find({pr14}, "PCE6", Pad4(s.donor_id), &b))
        return person_fail(s, s.name + ": no PCE6 entry in WWE 2K15 or for the donor.");
      pce["PCE6"].push_back({Ascii(Pad4(t)), b});
    }
    for (const auto& [rel, add] : std::vector<std::pair<std::string, const AddMap*>>{{"pac/evt/Winning_dlc02.pac", &win}, {"pac/evt/evtmov_dlc02.pac", &mv},
                                                                                     {"pac/gm_dlc02.pac", &gm}, {"pac/prtcls_dlc02.pac", &pce}}) {
      Bytes o, now;
      if (!host_base(rel, *add, &o) || !EpacRebuild(o, *add, {}, &now, error)) return Fail(error, rel + ": " + *error);
      if (!hput(rel, now)) return false;
    }
  }
  cache = PacCache{};
  if (!sobj_new.empty()) {
    WWE13PacGroupData sobj, bobj, spmt;
    sobj.name = "SOBJ"; bobj.name = "BOBJ"; spmt.name = "SPMT";
    for (const auto& [i, b] : sobj_new) sobj.entries.push_back({Pad4(i), b});
    Bytes now;
    if (!BuildWWE13PacGroups({sobj, bobj, spmt}, &now, error)) return false;
    if (!hput("pac/evt/evtobj_dlc02.pac", now)) return false;
  }
  for (const auto& p : people) {
    Bytes src, model;
    std::vector<std::string> notes;
    if (!read(g15 / "ch" / ("ch" + std::to_string(p.port.source_id) + ".pac"), &src) ||
        !ConvertWWE15CharacterPac(src, p.port.source_id, p.port.target_id, &model, &notes, error)) return false;
    if (!hput("pac/ch/ch" + std::to_string(p.port.target_id) + "_dlc02.pac", model)) return false;
  }
  if (cancelled()) return Fail(error, "The import was canceled.");

  // ---- host catalog: same file order, films sorted in, new files appended, arcs rebuilt
  stage(8, "Updating the add-on catalog…");
  {
    Bytes cat;
    const fs::path cat_path = create_host ? upath(host_source, "info/catalog.dlc") : upath(host, "info/catalog.dlc");
    const std::string cat_host = internal::PathToUtf8(cat_path);
    if (!read(cat_path, &cat)) {
      if (error && error->empty()) return Fail(error, "Could not read the add-on catalog " + cat_host + ".");
      return false;
    }
    if (cat.size() < 0x1c)
      return Fail(error, "The add-on catalog " + cat_host + " is too short (" + std::to_string(cat.size()) +
                         " bytes).");
    const uint16_t nof = Le16(cat, 12), nf = Le16(cat, 18);
    size_t p = 0x1c + 0x40 * size_t(nof);
    struct Rec { std::string rel; std::string kind; };
    std::vector<Rec> files;
    for (uint16_t i = 0; i < nf; ++i) {
      if (p + 7 > cat.size()) return Fail(error, "The add-on catalog " + host + " is truncated at file record " + std::to_string(i) + ".");
      const uint16_t l = Le16(cat, p), a = Le16(cat, p + 2), b = Le16(cat, p + 4);
      const uint8_t ln = cat[p + 6];
      std::string rel(cat.begin() + p + 7, cat.begin() + p + 7 + ln);
      std::replace(rel.begin(), rel.end(), '\\', '/');
      files.push_back({rel, a == 0xffff && b == 0xffff ? "mov" : b == 0xffff ? "hd" : a == 0xffff ? "sd" : "both"});
      p += l;
    }
    const Bytes tail(cat.begin() + std::min(p, cat.size()), cat.end());
    std::set<std::string> have;
    for (const auto& r : files) have.insert(r.rel);
    std::vector<Rec> movies;
    size_t i0 = 0;
    while (i0 < files.size() && files[i0].kind == "mov") movies.push_back(files[i0++]);
    for (const auto& pp : people) movies.push_back({"movies/titantron/" + std::to_string(pp.port.target_id) + ".bik2", "mov"});
    auto num = [](const std::string& rel) { const std::string f = rel.substr(rel.rfind('/') + 1); return std::stoul(f.substr(0, f.find('.'))); };
    std::stable_sort(movies.begin(), movies.end(), [&](const Rec& a, const Rec& b) { return num(a.rel) < num(b.rel); });
    std::vector<Rec> all = movies;
    all.insert(all.end(), files.begin() + i0, files.end());
    for (const auto& [rel, d] : H)
      if (!have.count(rel))
        all.push_back({rel, rel.find("DLC_HD_") != std::string::npos ? "hd" : rel.find("DLC_SD_") != std::string::npos ? "sd" : "both"});
    Bytes recs;
    uint16_t hd = 0, sd = 0;
    std::vector<std::vector<DirGroup>> hd_dirs, sd_dirs;
    for (const auto& r : all) {
      std::string path = r.rel;
      std::replace(path.begin(), path.end(), '/', '\\');
      const size_t at = recs.size();
      recs.resize(at + 7, 0);
      PutLe16(&recs, at, uint16_t(7 + path.size()));
      if (r.kind == "mov") PutLe32(&recs, at + 2, 0xffffffffu);
      else {
        uint16_t a = 0xffff, b = 0xffff;
        Bytes plain;
        std::vector<DirGroup> dir;
        const auto h = H.find(r.rel);
        if (h != H.end()) dir = h->second;
        else {
          Bytes head;
          if (!ReadHead(upath(host, r.rel), 0x4000, &head)) return Fail(error, "The add-on file " + r.rel + " listed in " + host + "'s catalog is missing.");
          if (head.size() >= 4 && head[0] == 0x0F && head[1] == 0xF5) { if (!read(upath(host, r.rel), &head) || !PlainOf(head, &plain, error)) return false; }
          else plain = head;
          dir = GroupsFromPlain(plain);
        }
        if (r.kind == "hd" || r.kind == "both") { a = hd++; hd_dirs.push_back(dir); }
        if (r.kind == "sd" || r.kind == "both") { b = sd++; sd_dirs.push_back(dir); }
        PutLe16(&recs, at + 2, a);
        PutLe16(&recs, at + 4, b);
      }
      recs[at + 6] = uint8_t(path.size());
      recs.insert(recs.end(), path.begin(), path.end());
    }
    Bytes catn(cat.begin(), cat.begin() + 0x1c);
    PutLe16(&catn, 18, uint16_t(all.size()));
    catn.insert(catn.end(), cat.begin() + 0x1c, cat.begin() + 0x1c + 0x40 * size_t(nof));
    catn.insert(catn.end(), recs.begin(), recs.end());
    catn.insert(catn.end(), tail.begin(), tail.end());
    if (!put(U(host, "info/catalog.dlc"), catn)) return false;
    for (const auto& [name, dirs] : std::vector<std::pair<std::string, const std::vector<std::vector<DirGroup>>*>>{{"info/catalog_hd.arc", &hd_dirs}, {"info/catalog_sd.arc", &sd_dirs}}) {
      Bytes arc{0xff, 0, 0, 0};
      for (size_t k = 0; k < dirs->size(); ++k) {
        const Bytes block = EmitArcBlock((*dirs)[k], k + 1 < dirs->size() ? uint16_t(k + 1) : uint16_t(0xffff));
        arc.insert(arc.end(), block.begin(), block.end());
      }
      if (!put(U(host, name), arc)) return false;
    }
  }

  // ---- offers: Add-On 3's catalog to 64, then pack 1's
  {
    std::vector<const Person*> todo;
    for (const auto& p : people) todo.push_back(&p);
    Bytes c3;
    if (!read(upath(pack_of[3], "info/catalog.dlc"), &c3)) return false;
    // Offer template: the python route's (wrestler 350, present after a WWE '13 import), else any wrestler offer as the
    // WWE '13 import picks it (Add-On 3, Add-On 2, then packs 4 and 5). The bytes the import does not overwrite are the
    // same in all of them (checked 2026-11-11: stock 383, Add-On 2's 338, imported 350).
    std::optional<Bytes> tpl;
    auto pick = [&](const Bytes& c, bool want_350) {
      if (tpl || c.size() < 0x1c) return;
      for (uint16_t i = 0; i < Le16(c, 12) && 0x1c + 0x40 * size_t(i + 1) <= c.size(); ++i) {
        const size_t at = 0x1c + 0x40 * size_t(i);
        const uint16_t id = Le16(c, at + 16);
        const bool wrestler = Le16(c, at) == 0x40 && Le16(c, at + 4) == 1 && id != 0 && Le16(c, at + 18) == 0;
        if (want_350 ? id == 350 : wrestler) { tpl = Bytes(c.begin() + at, c.begin() + at + 0x40); return; }
      }
    };
    pick(c3, true);
    pick(c3, false);
    for (const std::string& pk : {create_host ? host_source : host, pack_of[4], pack_of[5]}) {
      Bytes c;
      if (!tpl && ReadFileBytes(upath(pk, "info/catalog.dlc"), &c, error)) pick(c, false);
    }
    if (!tpl) return Fail(error, "No wrestler unlock entry was found in your WWE 2K14 add-ons to copy for the imported wrestlers. Nothing has been changed.");
    auto add_offers = [&](const std::string& pack, const Bytes& c, uint16_t pid) -> bool {
      if (Le16(c, 8) != pid) return Fail(error, "The catalog of " + pack + " says pack " + std::to_string(Le16(c, 8)) + "; " + std::to_string(pid) + " expected.");
      const uint16_t n = Le16(c, 12);
      uint16_t no = 0;
      for (uint16_t i = 0; i < n; ++i) no = std::max<uint16_t>(no, Le16(c, 0x1c + 0x40 * size_t(i) + 2));
      ++no;
      Bytes add;
      uint16_t k = 0;
      while (!todo.empty() && n + k < 64) {  // offers per pack: see kAddOnOfferCap (wwe13_pipeline.cpp)
        const Person* p = todo.front();
        todo.erase(todo.begin());
        Bytes rr = *tpl;
        PutLe16(&rr, 2, no + k); PutLe16(&rr, 6, pid); PutLe16(&rr, 8, n + k); PutLe16(&rr, 10, no + k); PutLe16(&rr, 16, p->port.target_id);
        std::string title_name = PyTitle(p->port.name).substr(0, 31);
        std::fill(rr.begin() + 32, rr.begin() + 64, 0);
        std::copy(title_name.begin(), title_name.end(), rr.begin() + 32);
        add.insert(add.end(), rr.begin(), rr.end());
        ++k;
      }
      Bytes out(c.begin(), c.begin() + 0x1c);
      PutLe16(&out, 12, n + k);
      out.insert(out.end(), c.begin() + 0x1c, c.begin() + 0x1c + 0x40 * size_t(n));
      out.insert(out.end(), add.begin(), add.end());
      out.insert(out.end(), c.begin() + 0x1c + 0x40 * size_t(n), c.end());
      return put(U(pack, "info/catalog.dlc"), out);
    };
    if (!add_offers(pack_of[3], c3, 3)) return false;
    if (!todo.empty()) {
      if (!pack_of.count(1)) return Fail(error, "Add-On 3's select-screen places are full and no pack-1 catalog was found for the other " + std::to_string(todo.size()) + " wrestlers.");
      Bytes c1;
      if (!read(upath(pack_of[1], "info/catalog.dlc"), &c1) || !add_offers(pack_of[1], c1, 1)) return false;
    }
    if (!todo.empty())
      return Fail(error, std::to_string(people.size()) + " wrestlers were chosen but only " + std::to_string(people.size() - todo.size()) +
                         " select-screen places are left. Choose fewer wrestlers. Nothing has been changed.");
  }

  if (!put("game/plist360.arc", base_arcs[0]) || !put("game/plist360_4x3.arc", base_arcs[1])) return false;
  {
    std::string slots;
    {
      Bytes raw;
      std::string ignored;
      if (!paths.own_slots.empty() && ReadFileBytes(paths.own_slots, &raw, &ignored)) slots.assign(raw.begin(), raw.end());
    }
    while (!slots.empty() && slots.back() == '\n') slots.pop_back();
    if (!slots.empty()) slots += "\n";
    for (const auto& p : people) slots += std::to_string(p.port.target_id) + "\n";
    if (!put("launcher/own-slots.txt", Bytes(slots.begin(), slots.end()))) return false;
  }
  if (!result->theme_levels.empty()) {  // Mods page: 0 on a 2K15 wrestler's theme volume slider = this level
    std::string levels;
    for (const auto& [id, db] : result->theme_levels) levels += std::to_string(id) + "\t" + std::to_string(db) + "\n";
    if (!put("launcher/mods/wwe15/theme-levels.tsv", Bytes(levels.begin(), levels.end()))) return false;
  }
  // Mods page: roster.tsv lists imported 2K15 wrestlers so LoadModWrestlers can show them on the Mods page.
  {
    std::string roster = "#game WWE 2K15\n";
    for (const auto& p : people) roster += std::to_string(p.port.target_id) + "\t" + p.port.name + "\t" + p.port.gender + "\n";
    if (!put("launcher/mods/wwe15/roster.tsv", Bytes(roster.begin(), roster.end()))) return false;
  }
  // ---- titantrons (GitHub #155): the wrestler's own WWE 2K15 film (Bink 2 KB2i, converted to a film WWE 2K14 plays by
  //      wwe2k14-bink-convert, cached by source ID + size), else the 2K14 original's film for alternates, the donor's
  //      where the recipe says so, else the stand-in. A failed conversion falls back the same way and is named.
  if (options.titantrons) {
    std::string stand_ins;
    for (const auto& p : people) {
      const auto& s = p.port;
      const fs::path dir = g14 / "movies" / "titantron";
      fs::path f;
      if (s.alternate && s.original14_id) f = dir / (std::to_string(s.original14_id) + ".bik2");
      else if (s.options.count("donor-titantron") && fs::is_regular_file(dir / (std::to_string(s.donor_id) + ".bik2"), ec)) f = dir / (std::to_string(s.donor_id) + ".bik2");
      if (f.empty() || !fs::is_regular_file(f, ec)) f = dir / "144.bik2";
      Bytes film;
      bool own = false;
      char own_name[32];
      std::snprintf(own_name, sizeof(own_name), "ent_%04u_0_0.bk2", s.source_id);
      fs::path own_source = ResolvePathCase(g15.parent_path(), std::string("movies/titantron/") + own_name);
      if (!fs::is_regular_file(own_source, ec))  // a team member without his own film: the team's (titantron=tag_NNNN)
        for (const std::string& o : s.options)
          if (o.rfind("titantron=", 0) == 0 && o.size() > 10 && o.size() < 24 &&
              std::all_of(o.begin() + 10, o.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }))
            own_source = ResolvePathCase(g15.parent_path(), "movies/titantron/" + o.substr(10) + "_0_0.bk2");
      if (!paths.bink_converter_executable.empty() && fs::is_regular_file(own_source, ec)) {
        fs::path cached;
        const auto size = fs::file_size(own_source, ec);
        if (!ec && !paths.titantron_cache.empty())
          cached = paths.titantron_cache / ("2k15-" + own_source.stem().string() + "-" + std::to_string(size) + ".bik2");
        if (!cached.empty() && fs::is_regular_file(cached, ec) && ReadFileBytes(cached, &film, error)) {
          own = true;
        } else {
          const fs::path out = staging / ("titantron15-" + std::to_string(s.target_id) + ".bik2");
          WWE13TitantronResult converted;
          const Result r = ConvertWWE13TitantronOrStandIn(paths.bink_converter_executable, own_source, f, out, s.name, {},
                                                          cancel ? *cancel : no_cancel, &converted);
          if (!r.ok) return Fail(error, r.error);
          if (!read(out, &film)) return false;
          own = !converted.used_stand_in;
          std::error_code rm;
          if (own && !cached.empty()) {
            fs::create_directories(cached.parent_path(), rm);
            fs::copy_file(out, cached, fs::copy_options::overwrite_existing, rm);
          }
          fs::remove(out, rm);
        }
      }
      if (!own) {
        if (!read(f, &film)) return person_fail(s, s.name + ": the titantron " + internal::PathToUtf8(f) + " could not be read.");
        if (!(s.alternate && s.original14_id)) stand_ins += (stand_ins.empty() ? "" : ", ") + s.name;
      }
      const bool playable = film.size() >= 4 && (std::memcmp(film.data(), "KB2f", 4) == 0 || std::memcmp(film.data(), "BIKi", 4) == 0 ||
                                                 std::memcmp(film.data(), "BIKf", 4) == 0 || std::memcmp(film.data(), "BIKh", 4) == 0);
      if (!playable) return person_fail(s, s.name + ": the titantron is not a film WWE 2K14 opens.");
      if (!put(U(host, "movies/titantron/" + std::to_string(s.target_id) + ".bik2"), film)) return false;
    }
    if (!stand_ins.empty())
      result->notices.push_back("No WWE 2K15 titantron could be used for " + stand_ins + ": they show a WWE 2K14 titantron.");
  }

  if (create_host) {
    const fs::path old_pack = title / host_source;
    for (fs::recursive_directory_iterator it(old_pack, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e;
      if (!it->is_regular_file(e) || e) continue;
      const std::string rel = it->path().lexically_relative(old_pack).generic_string();
      if (!staged_names.count(U(host, rel))) {
        Bytes b;
        if (!read(it->path(), &b) || !put(U(host, rel), b)) return false;
      }
      result->removal_keys.push_back(U(host_source, rel));
    }
    if (ec) return Fail(error, "Could not read the WWE 2K14 add-on folder " + host_source + ".");
    const std::string headers = "userdata/0000000000000000/545408B2/Headers/00000002/";
    Bytes header;
    if (!read(paths.userdata_root / "0000000000000000" / "545408B2" / "Headers" / "00000002" / (host_source + ".header"), &header)) return false;
    constexpr size_t kNameAt = 0x108, kNameSize = 42;
    const bool name_matches = header.size() >= kNameAt + kNameSize && host_source.size() <= kNameSize &&
        std::equal(host_source.begin(), host_source.end(), header.begin() + kNameAt) &&
        std::all_of(header.begin() + kNameAt + host_source.size(), header.begin() + kNameAt + kNameSize, [](uint8_t x) { return x == 0; });
    if (!name_matches) return Fail(error, "The WWE 2K14 add-on's header (" + host_source + ".header) is not in the expected form. Nothing has been changed.");
    std::fill(header.begin() + kNameAt, header.begin() + kNameAt + kNameSize, 0);
    std::copy(host.begin(), host.end(), header.begin() + kNameAt);
    if (!put(headers + host + ".header", header)) return false;
    result->removal_keys.push_back(headers + host_source + ".header");
    result->notices.push_back("Add-On 2 (" + host_source + ") now hosts the imported wrestlers as " + host + ".");
  }
  result->files.assign(staged_names.begin(), staged_names.end());
  result->staging_directory = staging;
  keep_staging = true;
  stage(10, "WWE 2K15 files are ready to apply.");
  return true;
}

}  // namespace wwe2k14::launcher
