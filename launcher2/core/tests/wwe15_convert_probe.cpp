// Host probe for the WWE 2K15 converters: runs one converter on the player's own files and
// writes the result, so it can be compared byte for byte with the python route's output.
// Not part of the launcher UI; never shipped with game data.
#include "../wwe15_convert.h"
#include "../wwe15_moves.h"
#include "../wwe15_portraits.h"
#include "../wwe15_pipeline.h"
#include "../wwe15_looks.h"
#include "../wwe15_install.h"
#include "../wwe13_install.h"
#include <cmath>
#include <chrono>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <tuple>
#include <sstream>
#include <algorithm>
#include <string>
#include <vector>

using namespace wwe2k14::launcher;
namespace fs = std::filesystem;

namespace {

bool ReadAll(const fs::path& path, WWE15Bytes* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

bool WriteAll(const fs::path& path, const WWE15Bytes& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  return s;
}

// A PAC entry, unpacked: BPE or ZLIB as the python route's _unz.
bool Unpack(const WWE15Bytes& raw, WWE15Bytes* out, std::string* error) {
  if (raw.size() >= 4 && std::equal(raw.begin(), raw.begin() + 4, "ZLIB"))
    return DecodeWWE13ZlibLeaf(raw, out, error);
  return DecodeWWE13Bpe(raw, out, error);
}

// First PAC (python sorted-glob order) in `dir` whose name matches one of `prefixes` and that has
// group/key; returns the raw entry.
bool FindEntry(const fs::path& dir, const std::vector<std::string>& prefixes, const std::string& group,
               const std::string& key, WWE15Bytes* raw, std::string* where) {
  std::vector<std::string> names;
  for (const auto& e : fs::directory_iterator(dir)) {
    const std::string n = e.path().filename().string();
    if (n.size() < 4 || n.substr(n.size() - 4) != ".pac") continue;
    for (const auto& prefix : prefixes)
      if (n.rfind(prefix, 0) == 0) { names.push_back(n); break; }
  }
  std::sort(names.begin(), names.end());
  for (const auto& n : names) {
    WWE15Bytes pac;
    if (!ReadAll(dir / n, &pac)) continue;
    std::vector<WWE13PacGroupData> groups;
    std::string error;
    if (!ReadWWE13PacGroups(pac, &groups, &error)) continue;
    for (const auto& g : groups) {
      if (Trim(g.name) != group) continue;
      for (const auto& entry : g.entries)
        if (Trim(entry.name) == key) { *raw = entry.data; *where = n; return true; }
    }
  }
  return false;
}

bool ReadGroupEntry(const WWE15Bytes& pac, const std::string& group, const std::string& key, WWE15Bytes* out,
                    std::string* error) {
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(pac, &groups, error)) return false;
  for (const auto& g : groups)
    if (Trim(g.name) == group)
      for (const auto& e : g.entries)
        if (Trim(e.name) == key) return Unpack(e.data, out, error);
  *error = "no " + group + "/" + key;
  return false;
}

int Usage() {
  std::cerr << "usage: wwe15_convert_probe looks G14_GAME_DIR G15_DIR OUT_DIR ID,ID,...\n"
               "       wwe15_convert_probe model SRC_CH_PAC SOURCE_ID TARGET_ID OUT_PAC\n"
               "       wwe15_convert_probe evp2 SRC_EVT_PAC OUT_DIR   (every EVP2 entry -> <pac>_<key>.evp)\n"
               "       wwe15_convert_probe rows G15_CHETC G14_CHETC BUILT_CHETC PEOPLE_TXT   (compare rows)\n"
               "       wwe15_convert_probe waze CM15 BASE_CM BUILT_CM PEOPLE_TXT   (lines: S T ported ids...)\n"
               "       wwe15_convert_probe prm M3_15 BASE_M3_PATCH BUILT_M3_PATCH IDS_TXT\n"
               "       wwe15_convert_probe createmode CM15 BASE_CM BUILT_CM PEOPLE_TXT D004_IDS_TXT   (WAZE+owners+MOTC, whole file)\n"
               "       wwe15_convert_probe d4 IDS_TXT G15_PAC_DIR BASE_DLC_MOT04 BUILT_DLC_MOT04 ROUTE_PAC...\n"
               "       wwe15_convert_probe plan G14_PAC_DIR ADDON_MOT_LIST G15_PAC_DIR MOVES_TSV PEOPLE_TXT(S DONOR)\n"
               "       wwe15_convert_probe misc CM15 BASE_MISC BUILT_MISC PEOPLE_TXT(S T D ports..) D004_IDS_TXT\n"
               "       wwe15_convert_probe portrait G15_MENU_DIR SOURCE TARGET BUILT_DLC_HD BUILT_DLC_SD\n"
               "       wwe15_convert_probe pipeline WWE15_ROOT G14 USERDATA OWN_SLOTS MOVES_TSV PORTS_TSV ID15,ID15.. STAGING\n"
               "       wwe15_convert_probe install LAUNCHER_DIR GAME WWE15_FOLDER ID15,..   (RunWWE15Import, as the button)\n"
               "       wwe15_convert_probe undo LAUNCHER_DIR GAME [USERDATA]   (UndoWWE15Import, as the button)\n"
               "       wwe15_convert_probe entrance G15_EVT_DIR KEY15 SOURCE TARGET ORIG14|0 G14_EVT_DIR DONOR_KEY OLD=NEW,..|- OUT_PREFIX\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return Usage();
  const std::string command = argv[1];
  if (command == "looks") {
    if (argc != 6) return Usage();
    std::vector<uint32_t> ids;
    std::stringstream list(argv[5]);
    std::string item;
    while (std::getline(list, item, ',')) {
      if (item.empty()) return Usage();
      char* end = nullptr;
      const unsigned long value = std::strtoul(item.c_str(), &end, 10);
      if (!end || *end != '\0' || value > 999999) return Usage();
      ids.push_back(static_cast<uint32_t>(value));
    }
    std::string error;
    if (!WriteWWE15LooksProbe(argv[2], argv[3], argv[4], ids, &error)) {
      std::cerr << "FAIL: " << error << "\n";
      return 1;
    }
    std::cout << "wrote WWE 2K15 looks for " << ids.size() << " IDs to " << argv[4] << "\n";
    return 0;
  }
  if (command == "model") {
    if (argc != 6) return Usage();
    WWE15Bytes source, output;
    if (!ReadAll(argv[2], &source)) { std::cerr << "cannot read " << argv[2] << "\n"; return 1; }
    std::vector<std::string> notes;
    std::string error;
    if (!ConvertWWE15CharacterPac(source, std::strtoul(argv[3], nullptr, 10),
                                  std::strtoul(argv[4], nullptr, 10), &output, &notes, &error)) {
      std::cerr << "FAIL: " << error << "\n";
      return 1;
    }
    if (!WriteAll(argv[5], output)) { std::cerr << "cannot write " << argv[5] << "\n"; return 1; }
    for (const std::string& note : notes) std::cout << "note: " << note << "\n";
    std::cout << "wrote " << output.size() << " bytes\n";
    return 0;
  }
  if (command == "evp2") {
    if (argc != 4) return Usage();
    WWE15Bytes source;
    if (!ReadAll(argv[2], &source)) { std::cerr << "cannot read " << argv[2] << "\n"; return 1; }
    std::vector<WWE13PacGroupData> groups;
    std::string error;
    if (!ReadWWE13PacGroups(source, &groups, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
    const std::string base = fs::path(argv[2]).filename().string();
    size_t ok = 0, failed = 0;
    for (const auto& group : groups) {
      if (Trim(group.name) != "EVP2") continue;
      for (const auto& entry : group.entries) {
        const std::string key = Trim(entry.name);
        WWE15Bytes plain, evp;
        WWE15Evp2Info info;
        if (!Unpack(entry.data, &plain, &error) || !ConvertWWE15Evp2(plain, key, &evp, &info, &error)) {
          std::cout << "error " << base << "_" << key << ": " << error << "\n";
          ++failed;
          continue;
        }
        WriteAll(fs::path(argv[3]) / (base + "_" + key + ".evp"), evp);
        std::cout << "ok " << base << "_" << key << " lists " << info.lists << " elems " << info.elements
                  << " dropped_nonzero " << info.dropped_nonzero << " pointers " << info.pointers
                  << " pad " << info.tail_padding << " resampled " << info.resampled << "\n";
        ++ok;
      }
    }
    std::cerr << base << ": converted " << ok << ", refused " << failed << "\n";
    return 0;
  }
  if (command == "rows") {
    // PEOPLE_TXT lines: S T D DAT_INDEX ALT(0/1) ORIG14 DONOR_MUSIC(0/1) then 262 slots and 22 extras.
    if (argc != 6) return Usage();
    WWE15Bytes c15, c14, built;
    if (!ReadAll(argv[2], &c15) || !ReadAll(argv[3], &c14) || !ReadAll(argv[4], &built)) { std::cerr << "cannot read inputs\n"; return 1; }
    std::string error;
    WWE13RawRowMap d15, d14, p14, bd, bs, bp;
    if (!ReadWWE13PacRows(c15, "CHAR", "DAT", &d15, &error) || !ReadWWE13PacRows(c14, "CHAR", "DAT", &d14, &error) ||
        !ReadWWE13PacRows(c14, "CHAR", "PRO", &p14, &error) || !ReadWWE13PacRows(built, "CHAR", "DAT", &bd, &error) ||
        !ReadWWE13PacRows(built, "CHAR", "DSBD", &bs, &error) || !ReadWWE13PacRows(built, "CHAR", "PRO", &bp, &error)) {
      std::cerr << "FAIL: " << error << "\n"; return 1;
    }
    std::ifstream in(argv[5]);
    std::string line;
    int same = 0, differ = 0;
    while (std::getline(in, line)) {
      std::istringstream f(line);
      WWE15RowsPort port;
      uint32_t idx = 0, alt = 0, dm = 0;
      f >> port.source_id >> port.target_id >> port.donor_id >> idx >> alt >> port.original14_id >> dm;
      port.dat_index = uint16_t(idx); port.alternate = alt; port.donor_music = dm;
      for (int k = 0; k < 262; ++k) { uint32_t v; f >> v; port.slots.push_back(uint16_t(v)); }
      for (int k = 0; k < 22; ++k) { uint32_t v; f >> v; port.extras.push_back(uint16_t(v)); }
      WWE15Rows rows;
      if (!ConvertWWE15Rows(port, d15[port.source_id], d14[port.donor_id], p14[port.donor_id], &rows, &error)) {
        std::cout << "FAIL " << port.source_id << ": " << error << "\n"; ++differ; continue;
      }
      const uint32_t t = port.target_id;
      const bool ok = rows.dat == bd[t] && rows.dsbd == bs[t] && rows.pro == bp[t];
      std::cout << (ok ? "IDENTICAL " : "DIFF ") << port.source_id << " -> " << t << " dat " << (rows.dat == bd[t])
                << " dsbd " << (rows.dsbd == bs[t]) << " pro " << (rows.pro == bp[t]) << "\n";
      (ok ? same : differ)++;
    }
    std::cout << "identical " << same << " differ " << differ << "\n";
    return 0;
  }
  if (command == "waze" || command == "createmode") {
    if (argc != (command == "waze" ? 6 : 7)) return Usage();
    WWE15Bytes c15, base, built, w15, w14, wb, out;
    std::string error;
    if (!ReadAll(argv[2], &c15) || !ReadAll(argv[3], &base) || !ReadAll(argv[4], &built)) { std::cerr << "cannot read\n"; return 1; }
    if (!ReadGroupEntry(c15, "MOVS", "WAZE", &w15, &error) || !ReadGroupEntry(base, "MOVS", "WAZE", &w14, &error) ||
        !ReadGroupEntry(built, "MOVS", "WAZE", &wb, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
    std::ifstream in(argv[5]);
    std::string line;
    std::vector<std::pair<uint32_t, uint32_t>> people;  // S, T
    std::map<uint16_t, std::vector<size_t>> users;
    std::vector<uint16_t> ids;
    while (std::getline(in, line)) {
      std::istringstream f(line);
      uint32_t s = 0, t = 0, id = 0;
      f >> s >> t;
      people.emplace_back(s, t);
      while (f >> id) { ids.push_back(uint16_t(id)); users[uint16_t(id)].push_back(people.size() - 1); }
    }
    std::vector<std::string> added, changes;
    if (!AddWWE15MoveRecords(w14, w15, ids, &out, &added, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
    std::map<uint16_t, uint16_t> owners;  // build.py: his slot if only he uses an owned move, else anyone
    for (const auto& [id, who] : users) {
      uint16_t owner = 0;
      if (!ReadWWE15MoveOwner(w15, id, &owner) || owner == 0x0200) continue;
      owners[id] = who.size() == 1 && people[who[0]].first == owner ? uint16_t(people[who[0]].second) : uint16_t(0x0200);
    }
    if (!FixWWE15MoveOwners(&out, owners, &changes, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
    if (command == "createmode") {
      std::ifstream din(argv[6]);
      std::vector<uint16_t> d004;
      uint32_t v = 0;
      while (din >> v) d004.push_back(uint16_t(v));
      WWE15Bytes motc, motc2, file;
      size_t sets = 0;
      if (!ReadGroupEntry(base, "MOVS", "MOTC", &motc, &error) || !AddWWE15D004Motc(motc, d004, &motc2, &sets, &error)) {
        std::cerr << "FAIL: " << error << "\n"; return 1;
      }
      motc2.resize((motc2.size() + 0xff) & ~size_t(0xff), 0);
      std::vector<WWE13PacGroupData> groups;
      if (!ReadWWE13PacGroups(base, &groups, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
      for (auto& g : groups)
        if (Trim(g.name) == "MOVS")
          for (auto& e : g.entries) {
            if (Trim(e.name) == "WAZE") e.data = out;
            if (Trim(e.name) == "MOTC") e.data = motc2;
          }
      if (!RebuildWWE13PacGroups(base, groups, &file, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
      std::cout << "WAZE added " << added.size() << " owners " << changes.size() << " MOTC sets " << sets << " file "
                << file.size() << " vs built " << built.size() << (file == built ? " IDENTICAL" : " DIFF") << "\n";
      return file == built ? 0 : 3;
    }
    std::cout << "added " << added.size() << " owner changes " << changes.size() << " size " << out.size() << " vs built "
              << wb.size() << (out == wb ? " IDENTICAL" : " DIFF") << "\n";
    for (const auto& c : changes) std::cout << "  owner " << c << "\n";
    return out == wb ? 0 : 3;
  }
  if (command == "prm") {
    if (argc != 6) return Usage();
    WWE15Bytes m15, base, built, out;
    if (!ReadAll(argv[2], &m15) || !ReadAll(argv[3], &base) || !ReadAll(argv[4], &built)) { std::cerr << "cannot read\n"; return 1; }
    std::ifstream in(argv[5]);
    std::vector<uint16_t> ids;
    uint32_t id = 0;
    while (in >> id) ids.push_back(uint16_t(id));
    WWE15PrmReport report;
    std::string error;
    if (!ExtendWWE15Prm6(base, m15, ids, &out, &report, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
    std::cout << "ids " << ids.size() << " already " << report.already.size() << " rows";
    for (const auto& [leaf, n] : report.rows_added) std::cout << " " << leaf << ":" << n;
    std::cout << " size " << out.size() << " vs built " << built.size() << (out == built ? " IDENTICAL" : " DIFF") << "\n";
    return out == built ? 0 : 3;
  }
  if (command == "d4") {
    if (argc < 7) return Usage();
    std::ifstream in(argv[2]);
    std::set<uint16_t> ids;
    uint32_t v = 0;
    while (in >> v) ids.insert(uint16_t(v));
    std::string error;
    // stock routes: complete even/odd records in one (archive, group, entry, bank)
    std::map<std::tuple<std::string, std::string, std::string, uint32_t, uint16_t>, std::set<uint32_t>> parity;
    std::set<uint16_t> found;
    for (int a = 6; a < argc; ++a) {
      WWE15Bytes pac;
      if (!ReadAll(argv[a], &pac)) continue;
      std::vector<WWE15MotionHit> hits;
      if (!ScanWWE15MotionRoutes(pac, ids, &hits, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
      for (const auto& h : hits) { parity[{argv[a], h.group, h.entry, h.bank, h.id}].insert(h.part & 1); found.insert(h.id); }
    }
    std::set<uint16_t> complete;
    for (const auto& [k, p] : parity) if (p.size() == 2) complete.insert(std::get<4>(k));
    std::vector<uint16_t> d004;
    for (uint16_t id : ids) if (!complete.count(id)) d004.push_back(id);
    std::cout << "stock routes";
    for (uint16_t id : complete) std::cout << " " << id;
    std::cout << "\nD004";
    for (uint16_t id : d004) std::cout << " " << id;
    std::cout << "\npartial " << (found.size() - complete.size()) << "\n";
    std::map<uint16_t, WWE15MotionRecord> records;
    const std::set<uint16_t> want(d004.begin(), d004.end());
    for (const char* n : {"mot00", "mot01", "mot02", "mot03", "mot05", "mot06", "mot07", "mot08"}) {
      WWE15Bytes pac;
      if (!ReadAll(fs::path(argv[3]) / (std::string(n) + ".pac"), &pac)) continue;
      if (!CollectWWE15MotionRecords(pac, want, &records, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
    }
    WWE15Bytes base, built, out;
    if (!ReadAll(argv[4], &base) || !ReadAll(argv[5], &built)) { std::cerr << "cannot read\n"; return 1; }
    if (!MergeWWE15D004Archive(base, d004, records, &out, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
    std::cout << "dlc_mot04 " << out.size() << " vs built " << built.size() << (out == built ? " IDENTICAL" : " DIFF") << "\n";
    return out == built ? 0 : 3;
  }
  if (command == "plan") {
    if (argc != 7) return Usage();
    std::string error;
    WWE15MoveFacts facts;
    auto list_pacs = [](const fs::path& dir, const std::string& must) {
      std::vector<fs::path> v;
      for (const auto& e : fs::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        if (n.size() > 4 && n.substr(n.size() - 4) == ".pac" && n.find(must) != std::string::npos) v.push_back(e.path());
      }
      std::sort(v.begin(), v.end());
      return v;
    };
    for (const auto& f : list_pacs(argv[2], "mot")) {
      WWE15Bytes pac;
      if (!ReadAll(f, &pac) || !ScanWWE15MotionIds(pac, &facts.base14, &error)) { std::cerr << "FAIL " << f << ": " << error << "\n"; return 1; }
    }
    {
      std::ifstream in(argv[3]);
      std::string path;
      while (std::getline(in, path)) {
        WWE15Bytes pac;
        if (!ReadAll(path, &pac) || !ScanWWE15MotionIds(pac, &facts.addon14, &error)) { std::cerr << "FAIL " << path << ": " << error << "\n"; return 1; }
      }
    }
    const fs::path g15 = argv[4];
    for (const auto& f : list_pacs(g15, "mot")) {
      WWE15Bytes pac;
      if (!ReadAll(f, &pac) || !ScanWWE15MotionIds(pac, &facts.all15, &error)) { std::cerr << "FAIL " << f << ": " << error << "\n"; return 1; }
    }
    for (const char* n : {"mot00", "mot01", "mot02", "mot03", "mot05", "mot06", "mot07", "mot08"}) {
      WWE15Bytes pac;
      if (ReadAll(g15 / (std::string(n) + ".pac"), &pac) && !ScanWWE15MotionParts(pac, &facts.even15, &facts.odd15, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
    }
    WWE15Bytes m3, cm15, c15, c14;
    std::map<uint32_t, WWE15Bytes> prm;
    WWE15Bytes w15;
    if (!ReadAll(g15 / "m3.pac", &m3) || !ReadWWE15PrmTables(m3, &prm, &error) || !ReadAll(g15 / "createmode.pac", &cm15) ||
        !ReadGroupEntry(cm15, "MOVS", "WAZE", &w15, &error) || !ReadAll(g15 / "ch" / "chEtc.pac", &c15) ||
        !ReadAll(fs::path(argv[2]) / "ch" / "chEtc.pac", &c14)) { std::cerr << "FAIL inputs: " << error << "\n"; return 1; }
    facts.prm15_rows = prm[0];
    for (size_t i = 0; i < (w15.size() - kWWE15WazeHeader) / kWWE15WazeRecord && i < uint32_t(w15[4] | (w15[5] << 8) | (w15[6] << 16)); ++i)
      facts.waze15.insert(uint16_t(w15[kWWE15WazeHeader + kWWE15WazeRecord * i + 152] | (w15[kWWE15WazeHeader + kWWE15WazeRecord * i + 153] << 8)));
    WWE13RawRowMap p15, p14;
    if (!ReadWWE13PacRows(c15, "CHAR", "PRO", &p15, &error) || !ReadWWE13PacRows(c14, "CHAR", "PRO", &p14, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
    for (const auto& [id, r] : p14) {
      if (r.size() != 1276 || (r[576] | (r[577] << 8)) == 255) continue;
      for (size_t s = 0; s < 262; ++s) { const uint16_t m = uint16_t(r[4 + 2 * s] | (r[5 + 2 * s] << 8)); if (m) facts.playable14.insert(m); }
    }
    std::ifstream rin(argv[5]);
    std::string rules_text((std::istreambuf_iterator<char>(rin)), std::istreambuf_iterator<char>());
    WWE15MoveRules rules;
    if (!ReadWWE15MoveRules(rules_text, &rules, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
    std::cerr << "facts: base14 " << facts.base14.size() << " addon14 " << facts.addon14.size() << " all15 " << facts.all15.size()
              << " even15 " << facts.even15.size() << " waze15 " << facts.waze15.size() << " playable14 " << facts.playable14.size() << "\n";
    std::ifstream pin(argv[6]);
    uint32_t s = 0, d = 0;
    while (pin >> s >> d) {
      WWE15MovePlan plan;
      if (!PlanWWE15Moves(s, p15[s], p14[d], facts, rules, &plan, &error)) { std::cout << s << " FAIL " << error << "\n"; continue; }
      std::cout << s << " |";
      for (uint16_t v : plan.slots) std::cout << " " << v;
      std::cout << " |";
      for (uint16_t v : plan.extras) std::cout << " " << v;
      std::cout << " |";
      for (uint16_t v : plan.port) std::cout << " " << v;
      std::cout << "\n";
      for (const auto& n : plan.notes) std::cerr << "  " << s << " " << n << "\n";
    }
    return 0;
  }
  if (command == "misc") {
    if (argc != 7) return Usage();
    WWE15Bytes c15, base, built, w15, att, out;
    std::string error;
    if (!ReadAll(argv[2], &c15) || !ReadAll(argv[3], &base) || !ReadAll(argv[4], &built) ||
        !ReadGroupEntry(c15, "MOVS", "WAZE", &w15, &error)) { std::cerr << "FAIL inputs " << error << "\n"; return 1; }
    std::ifstream in(argv[5]);
    std::string line;
    std::vector<WWE13AttireColorPort> ports;
    std::vector<std::pair<uint32_t, uint32_t>> people;
    std::map<uint16_t, std::vector<size_t>> users;
    std::vector<uint16_t> ids;
    while (std::getline(in, line)) {
      std::istringstream f(line);
      uint32_t s = 0, t = 0, dn = 0, id = 0;
      f >> s >> t >> dn;
      ports.push_back({t, dn});
      people.emplace_back(s, t);
      while (f >> id) { ids.push_back(uint16_t(id)); users[uint16_t(id)].push_back(people.size() - 1); }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::map<uint16_t, uint16_t> owners;
    for (const auto& [id, who] : users) {
      uint16_t owner = 0;
      if (!ReadWWE15MoveOwner(w15, id, &owner) || owner == 0x0200) continue;
      owners[id] = who.size() == 1 && people[who[0]].first == owner ? uint16_t(people[who[0]].second) : uint16_t(0x0200);
    }
    std::ifstream din(argv[6]);
    std::vector<uint16_t> d004;
    uint32_t v = 0;
    while (din >> v) d004.push_back(uint16_t(v));
    size_t sets = 0;
    if (!ConvertWWE13AttireColors(base, ports, &att, &error) ||
        (!ids.empty() && !UpdateWWE15MoveMenus(att, w15, ids, owners, d004, &out, &sets, &error))) {
      std::cerr << "FAIL: " << error << "\n"; return 1;
    }
    if (ids.empty()) out = att;
    std::cout << "ATT " << ports.size() << " moves " << ids.size() << " D004 sets " << sets << " file " << out.size()
              << " vs built " << built.size() << (out == built ? " IDENTICAL" : " DIFF") << "\n";
    return out == built ? 0 : 3;
  }
  if (command == "portrait") {
    if (argc != 7) return Usage();
    const std::string src = std::string(4 - std::min<size_t>(4, std::string(argv[3]).size()), '0') + argv[3];
    const std::string dst = std::string(4 - std::min<size_t>(4, std::string(argv[4]).size()), '0') + argv[4];
    int worst = 0;
    bool format_ok = true;
    for (const std::string kind : {"HD", "SD"}) {
      WWE15Bytes menu, built;
      std::string error;
      if (!ReadAll(fs::path(argv[2]) / ("menu" + kind + "2.pac"), &menu) || !ReadAll(kind == "HD" ? argv[5] : argv[6], &built)) {
        std::cerr << "cannot read\n"; return 1;
      }
      for (const auto& [g15, g14] : std::vector<std::pair<std::string, std::string>>{{"SFAG", "SSFA"}, {"SFBG", "SSFB"}, {"SFCG", "SSFC"}}) {
        WWE15Bytes raw, ref, mine, a, b;
        if (!ReadGroupEntry(menu, g15, src, &raw, &error) || !ReadGroupEntry(built, g14, dst, &ref, &error)) {
          std::cerr << "FAIL " << kind << " " << g15 << ": " << error << "\n"; return 1;
        }
        const int ww = g14 == "SSFC" ? 256 : 512, wh = g14 == "SSFC" ? 256 : (kind == "HD" ? 512 : 256);
        if (!ConvertWWE15Portrait(raw, ww, wh, &mine, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
        ref.resize(std::min(ref.size(), mine.size() + (ref.size() - mine.size()) * 0));  // entries are 256-byte padded
        int w1, h1, m1, w2, h2, m2;
        if (!DecodeWWE15Dxt5Dds(mine, &w1, &h1, &m1, &a, &error) || !DecodeWWE15Dxt5Dds(ref, &w2, &h2, &m2, &b, &error)) {
          std::cerr << "FAIL decode: " << error << "\n"; return 1;
        }
        const bool header_same = std::equal(mine.begin(), mine.begin() + 128, ref.begin());
        const bool size_same = ref.size() >= mine.size() && std::all_of(ref.begin() + mine.size(), ref.end(), [](uint8_t x) { return x == 0; });
        // colour error only where the sheet is visible (alpha >= 128 in both); alpha error everywhere
        double csum = 0, asum = 0;
        int cmax = 0, amax = 0;
        size_t cn = 0, cover8 = 0;
        for (size_t i = 0; i < a.size(); i += 4) {
          const int da = std::abs(int(a[i + 3]) - int(b[i + 3]));
          asum += da; amax = std::max(amax, da);
          if (a[i + 3] < 128 || b[i + 3] < 128) continue;
          for (int c = 0; c < 3; ++c) {
            const int d = std::abs(int(a[i + c]) - int(b[i + c]));
            csum += d; cmax = std::max(cmax, d); ++cn; if (d > 16) ++cover8;
          }
        }
        const double mean_c = cn ? csum / cn : 0, mean_a = asum / (a.size() / 4);
        {  // each against the source pixels (resized), visible pixels only, per channel
          int side = 0;
          WWE15Bytes src_rgba;
          DecodeWWE15PortraitPixels(raw, &side, &src_rgba, &error);
          const WWE15Bytes truth = ResizeWWE15Box(src_rgba, side, side, w1, h1);
          double em[4] = {0, 0, 0, 0}, ep[4] = {0, 0, 0, 0};
          size_t n = 0;
          for (size_t i = 0; i < truth.size(); i += 4) {
            if (truth[i + 3] < 128) continue;
            ++n;
            for (int c = 0; c < 4; ++c) { em[c] += std::abs(int(a[i + c]) - int(truth[i + c])); ep[c] += std::abs(int(b[i + c]) - int(truth[i + c])); }
          }
          std::printf("   vs source: ours R%.1f G%.1f B%.1f A%.1f | python R%.1f G%.1f B%.1f A%.1f\n", em[0] / n, em[1] / n, em[2] / n, em[3] / n,
                      ep[0] / n, ep[1] / n, ep[2] / n, ep[3] / n);
        }
        const int mx = std::max(cmax, amax);
        format_ok = format_ok && header_same && size_same && w1 == w2 && h1 == h2 && m1 == m2;
        worst = std::max(worst, mx);
        std::printf("%s %s %dx%d mips %d header %s size %s | colour (visible) mean %.2f max %d >16: %.3f%% | alpha mean %.2f max %d\n",
                    kind.c_str(), g14.c_str(), w1, h1, m1, header_same ? "same" : "DIFF", size_same ? "same" : "DIFF", mean_c, cmax,
                    cn ? 100.0 * cover8 / cn : 0.0, mean_a, amax);
      }
    }
    return format_ok ? 0 : 3;
  }
  if (command == "pipeline") {
    if (argc != 10) return Usage();
    WWE15ImportPaths paths;
    paths.wwe15_root = argv[2]; paths.wwe14_root = argv[3]; paths.userdata_root = argv[4];
    paths.own_slots = argv[5]; paths.moves_data = argv[6];
    std::vector<WWE15ImportPort> recipes, chosen;
    std::string error;
    if (!ReadWWE15ImportRecipes(argv[7], &recipes, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
    std::set<uint32_t> want;
    std::stringstream in(argv[8]);
    std::string item;
    while (std::getline(in, item, ',')) want.insert(std::strtoul(item.c_str(), nullptr, 10));
    for (const auto& r : recipes) if (want.count(r.source_id)) chosen.push_back(r);
    WWE15ImportOptions options;
    options.audio = std::getenv("P15_AUDIO_OFF") == nullptr;  // python-equal music selectors unless asked
    options.titantrons = true;
    WWE15ImportRunResult result;
    const auto t0 = std::chrono::steady_clock::now();
    if (!RunWWE15ImportStages(paths, chosen, argv[9], options, &result, &error,
                              [](uint64_t d, uint64_t t, const std::string& l) {
                                std::ifstream st("/proc/self/status");
                                std::string line, rss;
                                while (std::getline(st, line)) if (line.rfind("VmHWM", 0) == 0) rss = line;
                                std::cerr << "[" << d << "/" << t << "] " << l << "  " << rss << "\n";
                              })) {
      for (const auto& n : result.move_notes) std::cout << "moves: " << n << "\n";
      std::cerr << "FAIL: " << error << "\n"; return 1;
    }
    std::cout << "staged " << result.files.size() << " files in "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s\n";
    for (const auto& n : result.notices) std::cout << "notice: " << n << "\n";
    for (const auto& n : result.move_notes) std::cout << "moves: " << n << "\n";
    return 0;
  }
  if (command == "install" || command == "undo") {
    // the launcher folder layout of the real app (userdata, backups, own-slots beside the launcher)
    if (argc < 4) return Usage();
    Paths paths = ResolvePaths(fs::path(argv[2]) / "wwe2k14-launcher");
    if (command == "undo" && argc == 5) paths.userdata_dir = argv[4];  // a fixture whose userdata sits elsewhere
    const fs::path game = argv[3];
    CancelFlag cancel{false};
    auto progress = [](uint64_t, uint64_t, const std::string&) {};
    if (command == "install") {
      if (argc != 6) return Usage();
      const WWE15ImportScan scan = ScanWWE15Import(paths, argv[4]);
      std::vector<uint32_t> ids;
      std::stringstream in(argv[5]);
      std::string item;
      while (std::getline(in, item, ',')) ids.push_back(std::strtoul(item.c_str(), nullptr, 10));
      WWE15ImportSummary summary;
      const auto t0 = std::chrono::steady_clock::now();
      WWE15ImportOptions options;
      if (const char* e = std::getenv("WWE15_PROBE_LOOKS")) options.existing_wrestler_looks = (*e == '1');  // the tick box
      const Result r = RunWWE15Import(paths, game, scan, ids, progress, cancel, &summary, options);
      std::cout << "2K15 import: " << (r.ok ? "OK" : "REFUSED/FAILED: " + r.error) << " ("
                << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s)\n";
      for (const auto& l : summary.lines) std::cout << "  " << l << "\n";
      for (const auto& l : summary.skipped) std::cout << "  SKIPPED " << l << "\n";
      return r.ok ? 0 : 1;
    }
    const Result r = UndoWWE15Import(paths, game, progress, cancel);
    std::cout << "2K15 undo: " << (r.ok ? "OK" : "REFUSED/FAILED: " + r.error) << "\n";
    return r.ok ? 0 : 1;
  }
  if (command == "entrance") {
    if (argc != 11) return Usage();
    const fs::path g15 = argv[2], g14 = argv[7];
    const std::string key15 = argv[3], donor_key = argv[8], map_text = argv[9];
    const uint32_t source = std::strtoul(argv[4], nullptr, 10), target = std::strtoul(argv[5], nullptr, 10),
                   orig14 = std::strtoul(argv[6], nullptr, 10);
    WWE15Bytes x, z, donor_raw, plain, evp, donor;
    std::string where, where_z, where_d, error;
    if (!FindEntry(g15, {"Entrance", "entrance"}, "EVP2", key15, &x, &where) ||
        !FindEntry(g15, {"Entrance", "entrance"}, "EVZ", key15, &z, &where_z)) {
      std::cerr << "FAIL: no 2K15 EVP2+EVZ " << key15 << "\n"; return 1;
    }
    if (!FindEntry(g14, {"Entrance"}, "EVP", donor_key, &donor_raw, &where_d)) {
      std::cerr << "FAIL: no 2K14 donor EVP " << donor_key << "\n"; return 1;
    }
    WWE15Evp2Info info;
    std::vector<std::pair<uint32_t, uint32_t>> filled;
    std::vector<std::string> notes;
    if (!Unpack(x, &plain, &error) || !ConvertWWE15Evp2(plain, key15, &evp, &info, &error) ||
        !Unpack(donor_raw, &donor, &error) || !FillWWE15WrestlerTracks(&evp, donor, &filled, &error) ||
        !RetargetWWE15Entrance(&evp, target, source, std::strtoul(key15.c_str(), nullptr, 10), orig14, &notes, &error)) {
      std::cerr << "FAIL: " << error << "\n"; return 1;
    }
    std::map<uint32_t, uint32_t> remap;
    if (map_text != "-") {
      std::stringstream in(map_text);
      std::string item;
      while (std::getline(in, item, ','))
        remap[std::strtoul(item.c_str(), nullptr, 10)] = std::strtoul(item.substr(item.find('=') + 1).c_str(), nullptr, 10);
    }
    std::vector<WWE15EntranceProp> props;
    if (!ListWWE15EntranceProps(evp, &props, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
    for (const auto& prop : props) {
      const auto hit = remap.find(prop.object_id);
      if (hit != remap.end() && !SetWWE15EntranceProp(&evp, prop.track, hit->second, &error)) {
        std::cerr << "FAIL: " << error << "\n"; return 1;
      }
    }
    WriteAll(std::string(argv[10]) + ".evp", StoreWWE13BpeLiteral(evp));
    WriteAll(std::string(argv[10]) + ".evz", z);
    std::cout << "from " << where << " donor " << where_d << " filled " << filled.size() << " resampled "
              << info.resampled << " props " << props.size();
    for (const auto& n : notes) std::cout << " note: " << n;
    std::cout << "\n";
    return 0;
  }
  return Usage();
}
