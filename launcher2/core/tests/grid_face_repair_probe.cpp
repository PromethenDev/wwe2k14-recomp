// Probe for the automatic select-grid face repair (core/grid_face_repair.cpp) on a COPY of a launcher folder with imports.
// usage: grid_face_repair_probe <launcher folder>   (game folder = <launcher folder>/WWE 2K14)
// Runs the repair twice and checks: every imported wrestler has an opaque face in menuHD/SD.pac afterwards, the second run
// changes nothing, and every undo record's menu/pac-list entry still matches the file (what Undo checks).
// With --undo it then undoes every import (arenas, 2K15, '13) and checks each recorded file is back to its saved original
// (the menu PACs and pac lists of imports made before 1.6.1 included). No leftover repair .tmp files may remain.
// Prints PASS / FAIL. Never point it at a real installation: it changes the files.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

#include "grid_face_repair.h"
#include "grid_faces.h"
#include "launcher_core.h"
#include "wwe13_install.h"
#include "wwe15_install.h"

using namespace wwe2k14::launcher;
namespace fs = std::filesystem;

static std::vector<uint8_t> ReadBytes(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

static int Count(const fs::path& game, const std::vector<uint32_t>& ids, const char* label) {
  int bad = 0;
  for (const char* kind : {"HD", "SD"}) {
    std::map<uint32_t, GridFaceBytes> faces;
    std::string why;
    ReadGridFaces(ReadBytes(game / "pac" / "menu" / (std::string("menu") + kind + ".pac")), &faces, &why);
    int missing = 0, clear = 0, ok = 0;
    for (const uint32_t id : ids) {
      const auto it = faces.find(id);
      if (it == faces.end()) ++missing;
      else if (GridFaceSeeThrough(it->second)) ++clear;
      else ++ok;
    }
    std::printf("%s %s: %d ok, %d missing, %d see-through\n", label, kind, ok, missing, clear);
    bad += missing + clear;
  }
  return bad;
}

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) { std::printf("usage: grid_face_repair_probe <launcher folder copy> [--undo]\n"); return 2; }
  const bool undo = argc == 3 && std::string(argv[2]) == "--undo";
  const fs::path root = fs::u8path(argv[1]);
  const Paths paths = ResolvePaths(root / "wwe2k14-launcher.exe");
  const fs::path game = root / "WWE 2K14";
  std::vector<uint32_t> ids;
  for (const ModWrestler& w : LoadModWrestlers(paths)) ids.push_back(w.id);
  std::printf("imported wrestlers: %zu\n", ids.size());
  Count(game, ids, "before");
  bool pass = true;
  for (int run = 1; run <= 2; ++run) {
    GridFaceRepairReport r;
    std::string why;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = RepairImportGridFaces(paths, game, &r, &why);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("run %d: ok=%d unchanged=%d added=%zu replaced=%zu no_portrait=%zu %.0f ms %s\n", run, ok, r.unchanged, r.added,
                r.replaced, r.no_portrait, ms, why.c_str());
    if (!ok || (run == 2 && !r.unchanged)) pass = false;
  }
  if (Count(game, ids, "after") != 0) pass = false;
  // Stacked imports: only the top record of a key (the last import that changed it) must match the file; the one below
  // names the state the top one's Undo restores.
  std::map<std::string, bool> key_matches;
  for (const char* area : {"wwe13-import", "wwe13-arenas", "wwe15-import"}) {
    std::ifstream current(paths.backups_dir / area / "current.txt");
    std::string id;
    if (!std::getline(current, id) || id.empty()) continue;
    std::ifstream manifest(paths.backups_dir / area / "sets" / id / "manifest.tsv");
    std::string line;
    while (std::getline(manifest, line)) {
      std::istringstream fields(line);
      std::string key, a, b, c, applied;
      std::getline(fields, key, '\t'); std::getline(fields, a, '\t'); std::getline(fields, b, '\t');
      std::getline(fields, c, '\t'); std::getline(fields, applied, '\t');
      if (key.rfind("game/", 0) != 0 || (key.find("menu/menu") == std::string::npos && key.find("plist360") == std::string::npos))
        continue;
      const bool same = applied == ImportFileHash(game / fs::u8path(key.substr(5)));
      std::printf("undo record %s %s: %s\n", area, key.c_str(), same ? "matches file" : "below the top record");
      key_matches[key] = key_matches[key] || same;
    }
  }
  for (const auto& [key, ok] : key_matches)
    if (!ok) { std::printf("NO undo record matches %s\n", key.c_str()); pass = false; }
  for (const fs::path dir : {game, game / "pac" / "menu"})
    for (const auto& e : fs::directory_iterator(dir)) {
      const std::string n = e.path().filename().string();
      if (n.find(".faces-") != std::string::npos) { std::printf("leftover repair file %s\n", n.c_str()); pass = false; }
    }
  if (undo) {
    // key -> original hash, from every record before undo
    std::map<std::string, std::string> originals;  // the last area undone (WWE '13) names the final state
    for (const char* area : {"wwe13-arenas", "wwe15-import", "wwe13-import"}) {
      std::ifstream current(paths.backups_dir / area / "current.txt");
      std::string id;
      if (!std::getline(current, id) || id.empty()) continue;
      std::ifstream manifest(paths.backups_dir / area / "sets" / id / "manifest.tsv");
      std::string line;
      while (std::getline(manifest, line)) {
        std::istringstream fields(line);
        std::string key, exists, kind, original;
        std::getline(fields, key, '\t'); std::getline(fields, exists, '\t'); std::getline(fields, kind, '\t'); std::getline(fields, original, '\t');
        if (key.rfind("game/", 0) == 0 && exists == "1") originals[key] = original;
      }
    }
    CancelFlag cancel{false};
    if (HasWWE13ArenaImport(paths)) { const Result u = UndoWWE13ArenaImport(paths, game, {}, cancel); std::printf("undo arenas: %s\n", u.ok ? "ok" : u.error.c_str()); pass = pass && u.ok; }
    if (HasWWE15Import(paths)) { const Result u = UndoWWE15Import(paths, game, {}, cancel); std::printf("undo 2K15: %s\n", u.ok ? "ok" : u.error.c_str()); pass = pass && u.ok; }
    if (HasWWE13Import(paths)) { const Result u = UndoWWE13Import(paths, game, {}, cancel); std::printf("undo WWE '13: %s\n", u.ok ? "ok" : u.error.c_str()); pass = pass && u.ok; }
    for (const auto& [key, original] : originals) {
      if (key.find("menu/menu") == std::string::npos && key.find("plist360") == std::string::npos) continue;
      const bool same = ImportFileHash(game / fs::u8path(key.substr(5))) == original;
      std::printf("after undo %s: %s\n", key.c_str(), same ? "original" : "NOT ORIGINAL");
      if (!same) pass = false;
    }
  }
  std::printf("%s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
