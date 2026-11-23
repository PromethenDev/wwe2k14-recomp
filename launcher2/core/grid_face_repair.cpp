#include "grid_face_repair.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "grid_faces.h"
#include "path_case.h"
#include "wwe13_convert.h"
#include "wwe13_install.h"
#include "wwe13_pipeline.h"

namespace wwe2k14::launcher {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}

bool ReadAll(const fs::path& path, Bytes* out) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  out->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return true;
}

bool WriteAll(const fs::path& path, const Bytes& data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  output.flush();
  return static_cast<bool>(output);
}

void RenameRetry(const fs::path& from, const fs::path& to, std::error_code& ec) {
  for (int attempt = 0;; ++attempt) {  // antivirus can hold a just-written file for a moment
    ec.clear();
    fs::rename(from, to, ec);
    if (!ec || attempt >= 30) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  while (!s.empty() && (s.front() == ' ' || s.front() == '\0')) s.erase(s.begin());
  return s;
}

// Stamp of what the last successful check saw: the two menu PACs (size + time) and the wrestler ids.
std::string Stamp(const fs::path& hd, const fs::path& sd, const std::set<uint32_t>& ids) {
  std::ostringstream text;
  for (const fs::path& p : {hd, sd}) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    const auto time = fs::last_write_time(p, ec);
    text << (ec ? 0 : size) << ' ' << (ec ? 0 : time.time_since_epoch().count()) << '\n';
  }
  for (const uint32_t id : ids) text << id << ' ';
  text << '\n';
  return text.str();
}

// Every installed select portrait (SSFC, 256x256 DXT5) of the wanted ids, from the add-on packages' DLC_HD*.pac files.
std::map<uint32_t, Bytes> InstalledPortraits(const Paths& paths, const std::set<uint32_t>& wanted) {
  std::map<uint32_t, Bytes> out;
  const fs::path title = paths.userdata_dir / "0000000000000000" / "545408B2" / "00000002";
  std::error_code ec;
  std::vector<fs::path> files;
  for (fs::directory_iterator pack(title, ec), end; !ec && pack != end; pack.increment(ec)) {
    std::error_code inner;
    for (fs::directory_iterator f(pack->path() / "pac", inner), e2; !inner && f != e2; f.increment(inner)) {
      const std::string name = f->path().filename().string();
      if (name.rfind("DLC_HD", 0) == 0 && f->path().extension() == ".pac") files.push_back(f->path());
    }
  }
  std::sort(files.begin(), files.end());
  for (const fs::path& file : files) {
    Bytes pac;
    std::vector<WWE13PacGroupData> groups;
    std::string why;
    if (!ReadAll(file, &pac) || !ReadWWE13PacGroups(pac, &groups, &why)) continue;
    for (const auto& group : groups) {
      if (Trim(group.name) != "SSFC") continue;
      for (const auto& entry : group.entries) {
        const std::string name = Trim(entry.name);
        if (name.empty() || name.size() > 6 || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }))
          continue;
        const uint32_t id = uint32_t(std::stoul(name));
        if (!wanted.count(id) || out.count(id)) continue;
        Bytes dds;
        if (entry.data.size() >= 4 && std::memcmp(entry.data.data(), "BPE ", 4) == 0) {
          if (!DecodeWWE13Bpe(entry.data, &dds, &why)) continue;
        } else {
          dds = entry.data;
        }
        out[id] = std::move(dds);
      }
    }
  }
  return out;
}

struct PendingFile {
  std::string key;  // undo-record key, "game/..."
  fs::path target;
  Bytes data;
  fs::path temp, old;
  std::string old_hash, new_hash;
  bool swapped = false;
};


// A repair stopped part-way (power loss, killed launcher). Before any undo record or game file changes, the repair
// writes "<menu dir>/.faces-<token>.journal": one line per file, key + old hash + new hash. After every swap worked it
// writes ".faces-<token>.done". Recovery, at the next Play (any time is safe: it checks hashes, never assumes):
//   - with .done: only the cleanup was cut short -> remove the leftover copies.
//   - without .done, per file: target holds the NEW bytes (or is missing) and the old copy exists -> old copy back;
//     target holds the OLD bytes -> nothing to put back; anything else (an import or Undo changed the file since)
//     -> leave the file alone and drop the stale copy. Records naming the new hash go back to the old one only for
//     files that now hold the old bytes, so a record never points at bytes that are not there.
// Leftover copies without a journal (never shipped) are removed; a missing target gets its old copy back.
void RecoverInterruptedRepair(const Paths& paths, const fs::path& marker_dir,
                              const std::vector<std::pair<std::string, fs::path>>& files) {
  std::error_code ec;
  const auto ends_with = [](const std::string& n, const std::string& tail) {
    return n.size() >= tail.size() && n.compare(n.size() - tail.size(), tail.size(), tail) == 0;
  };
  std::map<std::string, fs::path> targets(files.begin(), files.end());
  std::set<std::string> tokens;
  for (fs::directory_iterator it(marker_dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().string();
    if (n.rfind(".faces-", 0) == 0 && ends_with(n, ".journal")) tokens.insert(n.substr(7, n.size() - 7 - 8));
  }
  ec.clear();
  for (const std::string& token : tokens) {
    const fs::path journal = marker_dir / (".faces-" + token + ".journal");
    const fs::path marker = marker_dir / (".faces-" + token + ".done");
    const bool finished = fs::exists(marker, ec);
    std::ifstream in(journal, std::ios::binary);
    std::string line;
    std::map<std::string, std::pair<std::string, std::string>> back;
    while (std::getline(in, line)) {
      std::istringstream fields(line);
      std::string key, old_hash, new_hash;
      if (!std::getline(fields, key, '\t') || !std::getline(fields, old_hash, '\t') || !std::getline(fields, new_hash, '\t'))
        continue;
      const auto t = targets.find(key);
      if (t == targets.end()) continue;
      const fs::path& target = t->second;
      const fs::path old = target.parent_path() / (target.filename().string() + ".faces-old-" + token + ".tmp");
      const fs::path temp = target.parent_path() / (target.filename().string() + ".faces-" + token + ".tmp");
      if (!finished) {
        const bool has_target = fs::exists(target, ec);
        const std::string now = has_target ? ImportFileHash(target) : std::string();
        const bool old_ok = fs::exists(old, ec) && ImportFileHash(old) == old_hash;
        if (fs::exists(old, ec) && (!has_target || (now == new_hash && old_ok))) {
          RenameRetry(old, target, ec);  // rename replaces the target; never remove it first
          if (!ec && old_ok) back[key] = {new_hash, old_hash};
        } else if (has_target && now == old_hash) {
          back[key] = {new_hash, old_hash};  // not swapped yet; its record may already name the new bytes
        }
      }
      if (fs::exists(target, ec)) {  // never delete the only copy
        fs::remove(old, ec);
        fs::remove(temp, ec);
      }
    }
    in.close();
    if (!back.empty()) {
      std::string ignored;
      RebaseImportRecords(paths, back, &ignored);
    }
    bool left = false;
    for (const auto& [key, target] : targets)
      for (const char* kind : {".faces-old-", ".faces-"})
        left = left || fs::exists(target.parent_path() / (target.filename().string() + kind + token + ".tmp"), ec);
    if (!left) {
      fs::remove(marker, ec);
      fs::remove(journal, ec);
    }
  }
  // Copies with no journal: a missing target gets its old copy back, everything else goes.
  for (const auto& [key, target] : targets) {
    for (fs::directory_iterator it(target.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
      const std::string n = it->path().filename().string();
      const std::string base = target.filename().string() + ".faces-";
      if (n.rfind(base, 0) != 0 || !ends_with(n, ".tmp")) continue;
      const bool is_old = n.rfind(base + "old-", 0) == 0;
      const std::string token = n.substr(base.size() + (is_old ? 4 : 0), n.size() - base.size() - (is_old ? 4 : 0) - 4);
      if (tokens.count(token)) continue;
      std::error_code one;
      if (is_old && !fs::exists(target, one)) RenameRetry(it->path(), target, one);
      else fs::remove(it->path(), one);
    }
    ec.clear();
  }
}
}  // namespace

bool RepairImportGridFaces(const Paths& paths, const fs::path& game_folder, GridFaceRepairReport* report,
                           std::string* error) {
  GridFaceRepairReport local;
  GridFaceRepairReport& r = report ? *report : local;
  r = {};
  struct CountsOnlyOnSuccess {  // a failed repair changed nothing: report no added/replaced faces
    GridFaceRepairReport& r;
    bool done = false;
    ~CountsOnlyOnSuccess() { if (!done) r.added = r.replaced = 0; }
  } counts{r};
  std::set<uint32_t> ids;
  for (const ModWrestler& w : LoadModWrestlers(paths)) ids.insert(w.id);
  if (ids.empty()) { r.unchanged = true; return true; }

  const fs::path hd_path = ResolvePathCase(game_folder, "pac/menu/menuHD.pac");
  const fs::path sd_path = ResolvePathCase(game_folder, "pac/menu/menuSD.pac");
  const fs::path stamp_file = paths.exe_dir / "grid-faces-check.txt";
  RecoverInterruptedRepair(paths, hd_path.parent_path(), {{"game/pac/menu/menuHD.pac", hd_path},
                                   {"game/pac/menu/menuSD.pac", sd_path},
                                   {"game/plist360.arc", ResolvePathCase(game_folder, "plist360.arc")},
                                   {"game/plist360_4x3.arc", ResolvePathCase(game_folder, "plist360_4x3.arc")}});
  {
    Bytes last;
    const std::string now = Stamp(hd_path, sd_path, ids);
    if (ReadAll(stamp_file, &last) && std::string(last.begin(), last.end()) == now) { r.unchanged = true; return true; }
  }

  std::vector<PendingFile> pending;
  std::map<uint32_t, Bytes> portraits;
  bool portraits_read = false;
  std::set<uint32_t> no_portrait;
  std::vector<std::pair<std::string, Bytes>> arcs;  // pac-list arcs, read once, synced for each changed PAC
  for (const char* arc_name : {"plist360.arc", "plist360_4x3.arc"}) {
    Bytes arc;
    const fs::path p = ResolvePathCase(game_folder, arc_name);
    if (!ReadAll(p, &arc)) return Fail(error, std::string("Could not read ") + arc_name + ".");
    arcs.push_back({arc_name, std::move(arc)});
  }
  bool arcs_changed = false;

  for (const auto& [kind, path] : {std::pair<std::string, fs::path>{"HD", hd_path}, {"SD", sd_path}}) {
    Bytes old;
    std::map<uint32_t, Bytes> faces;
    std::string why;
    if (!ReadAll(path, &old)) return Fail(error, "Could not read pac/menu/menu" + kind + ".pac.");
    if (!ReadGridFaces(old, &faces, &why) || faces.empty()) return Fail(error, "menu" + kind + ".pac: " + why);
    std::set<uint32_t> need, replace;
    for (const uint32_t id : ids) {
      const auto it = faces.find(id);
      if (it == faces.end()) need.insert(id);
      else if (GridFaceSeeThrough(it->second)) { need.insert(id); replace.insert(id); }
    }
    if (need.empty()) continue;
    if (!portraits_read) {
      std::set<uint32_t> all_needed = ids;  // read once for both PACs
      portraits = InstalledPortraits(paths, all_needed);
      portraits_read = true;
    }
    std::map<uint32_t, Bytes> stock_like;  // backdrop from the opaque faces only (stock + good imports)
    for (const auto& [id, dds] : faces)
      if (!GridFaceSeeThrough(dds)) stock_like[id] = dds;
    const Bytes backdrop = GridFaceBackdrop(stock_like);
    const Bytes& header_like = stock_like.empty() ? faces.begin()->second : stock_like.begin()->second;
    std::map<uint32_t, Bytes> made;
    for (const uint32_t id : need) {
      const auto portrait = portraits.find(id);
      Bytes face;
      if (portrait == portraits.end() || !MakeGridFaceFromPortrait(portrait->second, header_like, &face, &why, &backdrop)) {
        no_portrait.insert(id);
        continue;
      }
      made[id] = std::move(face);
    }
    if (made.empty()) continue;
    Bytes now, old_plain;
    size_t changed = 0;
    if (!AddGridFaces(old, made, &now, &changed, &why, &replace)) return Fail(error, "menu" + kind + ".pac: " + why);
    if (!changed) continue;
    if (!DecodeWWE13PacPlain(old, &old_plain, &why)) return Fail(error, "menu" + kind + ".pac: " + why);
    bool any = false;
    for (auto& [name, arc] : arcs) {
      bool synced = false;
      if (!SyncWWE13PacArchiveIfListed(&arc, old_plain, now, &synced, &why)) return Fail(error, name + ": " + why);
      any = any || synced;
    }
    if (!any) return Fail(error, "menu" + kind + ".pac is not in the game's pac lists.");
    arcs_changed = true;
    for (const auto& [id, face] : made) (replace.count(id) ? r.replaced : r.added) += 1;
    pending.push_back({"game/pac/menu/menu" + kind + ".pac", path, std::move(now)});
  }
  r.no_portrait = no_portrait.size();
  if (arcs_changed)
    for (auto& [name, arc] : arcs) pending.push_back({"game/" + name, ResolvePathCase(game_folder, name), std::move(arc)});

  if (!pending.empty()) {
    // 1. New bytes beside each file, 2. undo records point at the new bytes, 3. swap the files in (old copies kept until
    // every swap worked). Any failure puts everything back.
    const std::string token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::vector<std::string> adopted;  // keys that got a new undo record from this repair
    auto cleanup = [&](bool restore_records) {
      std::error_code ec;
      for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
        std::error_code exists_ec;
        if (it->swapped || (!fs::exists(it->target, exists_ec) && fs::exists(it->old, exists_ec))) {
          RenameRetry(it->old, it->target, ec);  // replaces the target; on failure the old copy stays for the next start
        }
        if (!ec) fs::remove(it->temp, ec);
      }
      if (restore_records) {
        std::map<std::string, std::pair<std::string, std::string>> back;
        for (const PendingFile& f : pending) back[f.key] = {f.new_hash, f.old_hash};
        std::string ignored;
        RebaseImportRecords(paths, back, &ignored);
        DropAdoptedImportRecords(paths, adopted, &ignored);
      }
    };
    std::map<std::string, std::pair<std::string, std::string>> changes;
    for (PendingFile& f : pending) {
      f.temp = f.target.parent_path() / (f.target.filename().string() + ".faces-" + token + ".tmp");
      f.old = f.target.parent_path() / (f.target.filename().string() + ".faces-old-" + token + ".tmp");
      f.old_hash = ImportFileHash(f.target);
      if (f.old_hash.empty() || !WriteAll(f.temp, f.data) || (f.new_hash = ImportFileHash(f.temp)).empty()) {
        cleanup(false);
        return Fail(error, "Could not write the new " + f.target.filename().string() + ".");
      }
      changes[f.key] = {f.old_hash, f.new_hash};
    }
    const fs::path journal = hd_path.parent_path() / (".faces-" + token + ".journal");
    {
      std::string text;
      for (const PendingFile& f : pending) text += f.key + "\t" + f.old_hash + "\t" + f.new_hash + "\n";
      if (!WriteAll(journal, Bytes(text.begin(), text.end()))) {
        cleanup(false);
        return Fail(error, "Could not write the grid-face repair journal.");
      }
    }
    std::string why;
    {
      // Imports made before 1.6.1 have no record for the menu PACs: save them in the import's record first.
      // Every file must be held by a record at its current bytes; a menu PAC no record holds at all is adopted. A record
      // holding a file at other bytes (another launcher copy's import, an older state) means Undo would refuse after
      // the repair: then nothing is changed.
      std::map<std::string, std::pair<fs::path, std::string>> files;
      std::string anchor_hash;
      for (const PendingFile& f : pending) {
        files[f.key] = {f.target, f.old_hash};
        if (f.key == "game/plist360.arc") anchor_hash = f.old_hash;
      }
      if (!AdoptImportRecords(paths, files, "game/plist360.arc", anchor_hash, &adopted, &why)) {
        cleanup(false);
        std::error_code jec;
        fs::remove(journal, jec);
        return Fail(error, why);
      }
    }
    if (!RebaseImportRecords(paths, changes, &why)) {
      cleanup(true);
      std::error_code jec;
      fs::remove(journal, jec);
      return Fail(error, why);
    }
    for (PendingFile& f : pending) {
      std::error_code ec;
      RenameRetry(f.target, f.old, ec);
      if (!ec) {
        RenameRetry(f.temp, f.target, ec);
        if (ec) { std::error_code back; RenameRetry(f.old, f.target, back); }
        else f.swapped = true;
      }
      if (ec) {
        cleanup(true);
        std::error_code jec;
        bool left = false;
        for (const PendingFile& p : pending) left = left || fs::exists(p.old, jec);
        if (!left) fs::remove(journal, jec);  // otherwise the next start finishes putting things back
        return Fail(error, "Could not replace " + f.target.filename().string() + " (" + ec.message() + ").");
      }
    }
    const fs::path marker = hd_path.parent_path() / (".faces-" + token + ".done");
    WriteAll(marker, Bytes{});  // the swap is complete: a cut-short cleanup must not roll it back
    std::error_code ec;
    for (const PendingFile& f : pending) fs::remove(f.old, ec);
    fs::remove(marker, ec);
    fs::remove(journal, ec);
  }
  // Remember this state only when every wrestler has a face now; otherwise check again next time.
  if (no_portrait.empty()) {
    const std::string stamp = Stamp(hd_path, sd_path, ids);
    WriteAll(stamp_file, Bytes(stamp.begin(), stamp.end()));
  }
  counts.done = true;
  return true;
}

}  // namespace wwe2k14::launcher
