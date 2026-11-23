#include "wwe15_install.h"
#include <sstream>

#include "internal/util.h"
#include "internal/xdvdfs.h"
#include "wwe13_install.h"
#include "wwe15_looks.h"
#include "game_search.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <set>
#include <string>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef CopyFile  // windows.h maps it to CopyFileA; XdvdfsReader::CopyFile is meant
#endif

namespace wwe2k14::launcher {
namespace {

namespace fs = std::filesystem;
constexpr uint64_t kMib = 1024ull * 1024ull;

// ---- WWE 2K15 disc images: read in place with the launcher's XDVDFS reader (the one the WWE 2K14 disc install uses)
using internal::XdvdfsEntry;

bool IsDiscImageName(const fs::path& path) {
  const std::string extension = internal::LowerAscii(internal::PathToUtf8(path.extension()));
  return extension == ".iso" || extension == ".img";
}

// XDVDFS names are case-insensitive.
const XdvdfsEntry* DiscChild(const std::vector<XdvdfsEntry>& entries, const std::string& name, bool directory) {
  const std::string want = internal::LowerAscii(name);
  for (const auto& entry : entries)
    if (entry.directory == directory && internal::LowerAscii(entry.name) == want) return &entry;
  return nullptr;
}

// Opens `iso` and returns its pac folder when it is WWE 2K15 and the pac folder passes the folder check's contents test
// (IsWWE15PacFolder: ch/chEtc.pac, m3.pac, createmode.pac, ch424, ch398, menuHD2, menuSD2).
const XdvdfsEntry* OpenWWE15Disc(const fs::path& iso, internal::XdvdfsReader* reader) {
  if (ReadDiscImageTitleId(iso) != kWwe2k15TitleId) return nullptr;
  std::string error;
  if (!reader->Open(iso, &error)) return nullptr;
  const XdvdfsEntry* pac = DiscChild(reader->root_entries(), "pac", true);
  if (!pac) return nullptr;
  const XdvdfsEntry* ch = DiscChild(pac->children, "ch", true);
  const XdvdfsEntry* menu = DiscChild(pac->children, "menu", true);
  if (!ch || !menu) return nullptr;
  for (const char* name : {"m3.pac", "createmode.pac"})
    if (!DiscChild(pac->children, name, false)) return nullptr;
  for (const char* name : {"chEtc.pac", "ch424.pac", "ch398.pac"})
    if (!DiscChild(ch->children, name, false)) return nullptr;
  for (const char* name : {"menuHD2.pac", "menuSD2.pac"})
    if (!DiscChild(menu->children, name, false)) return nullptr;
  return pac;
}

// Lower-case names of the files in the disc's pac/ch folder (empty when it is not a WWE 2K15 disc image).
std::set<std::string> DiscModelNames(const fs::path& iso) {
  std::set<std::string> names;
  try {
    internal::XdvdfsReader reader;
    const XdvdfsEntry* pac = OpenWWE15Disc(iso, &reader);
    if (!pac) return names;
    for (const auto& entry : DiscChild(pac->children, "ch", true)->children)
      if (!entry.directory) names.insert(internal::LowerAscii(entry.name));
  } catch (...) {
  }
  return names;
}


uint64_t TreeBytes(const fs::path& root) {
  uint64_t total = 0;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e;
    if (it->is_regular_file(e) && !e) total += it->file_size(e);
  }
  return total;
}

bool EnoughSpace(const fs::path& where, uint64_t need, std::string* error) {
  std::error_code ec;
  const fs::space_info info = fs::space(where, ec);
  if (ec) { *error = "Could not check free disk space for " + internal::PathToUtf8(where) + ". Nothing has been changed."; return false; }
  if (info.available >= need) return true;
  *error = "Not enough free disk space at " + internal::PathToUtf8(where) + ". The WWE 2K15 import needs about " +
           std::to_string((need + kMib - 1) / kMib) + " MiB free there; " + std::to_string(info.available / kMib) +
           " MiB is available. Nothing has been changed.";
  return false;
}

}  // namespace

bool IsWWE15DiscImage(const fs::path& iso) {
  try {
    internal::XdvdfsReader reader;
    return OpenWWE15Disc(iso, &reader) != nullptr;
  } catch (...) {
    return false;
  }
}

fs::path FindWWE15DiscImage(const fs::path& selected) {
  std::error_code ec;
  if (fs::is_regular_file(selected, ec))
    return IsDiscImageName(selected) && IsWWE15DiscImage(selected) ? selected : fs::path{};
  for (fs::recursive_directory_iterator it(selected, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
    std::error_code e;
    if (it->is_regular_file(e) && !e && IsDiscImageName(it->path()) && IsWWE15DiscImage(it->path())) return it->path();
  }
  return {};
}

bool ExtractWWE15DiscFiles(const fs::path& iso, const fs::path& dest, const std::vector<uint32_t>& model_ids,
                           const ProgressFn& progress, const CancelFlag* cancel, std::string* error) {
  auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  auto stopped = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
  const std::string cancelled = "The WWE 2K15 import was cancelled. Nothing has been changed.";
  try {
    internal::XdvdfsReader reader;
    const XdvdfsEntry* pac = OpenWWE15Disc(iso, &reader);
    if (!pac) return fail("The disc image " + internal::PathToUtf8(iso) + " could not be read as WWE 2K15. Nothing has been changed.");
    // Exactly what the import reads (wwe15_pipeline/moves/portraits/looks): see the header.
    std::vector<std::pair<const XdvdfsEntry*, fs::path>> plan;
    for (const auto& entry : pac->children)
      if (!entry.directory) plan.emplace_back(&entry, fs::path("pac") / internal::PathFromUtf8(entry.name));
    if (const XdvdfsEntry* evt = DiscChild(pac->children, "evt", true)) {
      for (const auto& entry : evt->children) {
        if (entry.directory) continue;
        const bool wanted = entry.name.rfind("Entrance", 0) == 0 || entry.name.rfind("entrance", 0) == 0 ||
                            entry.name.rfind("Winning", 0) == 0 || internal::LowerAscii(entry.name) == "evtobj.pac";
        if (wanted) plan.emplace_back(&entry, fs::path("pac") / "evt" / internal::PathFromUtf8(entry.name));
      }
    }
    const XdvdfsEntry* menu = DiscChild(pac->children, "menu", true);
    for (const char* name : {"menuHD2.pac", "menuSD2.pac"})
      plan.emplace_back(DiscChild(menu->children, name, false), fs::path("pac") / "menu" / name);
    std::set<std::string> models{"chetc.pac", "ch424.pac", "ch398.pac"};
    for (const uint32_t id : model_ids) models.insert("ch" + std::to_string(id) + ".pac");
    for (const auto& entry : DiscChild(pac->children, "ch", true)->children)
      if (!entry.directory && models.count(internal::LowerAscii(entry.name)))
        plan.emplace_back(&entry, fs::path("pac") / "ch" / internal::PathFromUtf8(entry.name));
    // The wrestlers' own titantrons (GitHub #155): movies/titantron/ent_NNNN_0_0.bk2 of the chosen IDs.
    if (const XdvdfsEntry* movies = DiscChild(reader.root_entries(), "movies", true))
      if (const XdvdfsEntry* trons = DiscChild(movies->children, "titantron", true))
        for (const uint32_t id : model_ids) {
          char name[32];
          std::snprintf(name, sizeof(name), "ent_%04u_0_0.bk2", id);
          if (const XdvdfsEntry* entry = DiscChild(trons->children, name, false))
            plan.emplace_back(entry, fs::path("movies") / "titantron" / name);
        }
    // team films (data/wwe2k15-ports.tsv titantron=tag_NNNN), small: all of them
    if (const XdvdfsEntry* movies = DiscChild(reader.root_entries(), "movies", true))
      if (const XdvdfsEntry* trons = DiscChild(movies->children, "titantron", true))
        for (const auto& entry : trons->children)
          if (!entry.directory && internal::LowerAscii(entry.name).rfind("tag_", 0) == 0)
            plan.emplace_back(&entry, fs::path("movies") / "titantron" / internal::PathFromUtf8(entry.name));
    // The wrestlers' own entrance themes (RunWWE15ImportMusic): events in Game_Sound_E_S.pck, songs in Game_Sound_M.pck.
    if (const XdvdfsEntry* sound = DiscChild(reader.root_entries(), "sound", true))
      for (const char* name : {"Game_Sound_E_S.pck", "Game_Sound_M.pck"})
        if (const XdvdfsEntry* entry = DiscChild(sound->children, name, false)) plan.emplace_back(entry, fs::path("sound") / name);

    uint64_t total = 0;
    for (const auto& item : plan) total += item.first->size;
    std::error_code ec;
    fs::create_directories(dest / "pac", ec);
    if (ec) return fail("Could not create the folder " + internal::PathToUtf8(dest) + ". Nothing has been changed.");
    const fs::space_info space = fs::space(dest, ec);
    if (!ec && space.available < total + 64 * kMib)
      return fail("Not enough free disk space at " + internal::PathToUtf8(dest) + ". Reading WWE 2K15 from its disc image needs about " +
                  std::to_string((total + 64 * kMib + kMib - 1) / kMib) + " MiB free there; " +
                  std::to_string(space.available / kMib) + " MiB is available. Nothing has been changed.");
    uint64_t done = 0;
    const std::string label = "Reading WWE 2K15 files from the disc image…";
    for (const auto& [entry, relative] : plan) {
      if (stopped()) return fail(cancelled);
      const fs::path output_path = dest / relative;
      fs::create_directories(output_path.parent_path(), ec);
      std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
      if (!output) return fail("Could not write " + internal::PathToUtf8(output_path) + ". Nothing has been changed.");
      std::string copy_error;
      const bool copied = reader.CopyFile(*entry, output, [&](uint64_t chunk) {
        done += chunk;
        if (progress) progress(done, total, label);
        return !stopped();
      }, &copy_error);
      if (!copied) {
        if (stopped()) return fail(cancelled);
        return fail("Could not read pac/" + relative.generic_string() + " from the WWE 2K15 disc image" +
                    (copy_error.empty() ? std::string() : " (" + copy_error + ")") + ". Nothing has been changed.");
      }
      output.close();
      if (!output) return fail("Could not write " + internal::PathToUtf8(output_path) + ". Nothing has been changed.");
    }
    return true;
  } catch (const std::exception&) {
    return fail("The WWE 2K15 disc image could not be read. Nothing has been changed.");
  }
}

fs::path SearchWWE15PacFolder(const Paths& launcher_paths, const ProgressFn& progress, const CancelFlag* cancel,
                              bool* limit_reached, uint64_t* directories_visited) {
  // By names only (game_search.h): a folder holding "pac" whose contents pass the check, or a large .iso/.img that is
  // WWE 2K15. Nothing else is opened.
  fs::path found;
  const GameSearchStats stats = WalkGameFiles(GameSearchRoots(launcher_paths), [&](const fs::path& folder,
                                                                              const std::vector<GameSearchEntry>& entries) {
    for (const GameSearchEntry& entry : entries)
      if (entry.directory && entry.lower_name == "pac" && IsWWE15PacFolder(folder / "pac")) {
        found = folder / "pac";
        return true;
      }
    for (const GameSearchEntry& entry : entries) {
      if (!IsGameDiscImageCandidate(entry)) continue;
      if (progress) progress(0, 0, "Checking " + internal::PathToUtf8(entry.path));
      if (IsWWE15DiscImage(entry.path)) {
        found = entry.path;
        return true;
      }
    }
    return false;
  }, true, progress, cancel);
  if (limit_reached) *limit_reached = stats.limit_reached;
  if (directories_visited) *directories_visited = stats.folders;
  return found;
}

WWE15ImportScan ScanWWE15Import(const Paths& launcher_paths, const fs::path& selected, const ProgressFn& progress,
                                const CancelFlag* cancel) {
  WWE15ImportScan scan;
  std::vector<WWE15ImportPort> recipes;
  if (!ReadWWE15ImportRecipes(launcher_paths.exe_dir / "data" / "wwe2k15-ports.tsv", &recipes, &scan.error)) return scan;
  fs::path found;
  if (selected.empty()) {
    scan.searched_pc = true;
    found = SearchWWE15PacFolder(launcher_paths, progress, cancel, &scan.search_limit_reached, &scan.directories_visited);
    scan.cancelled = found.empty() && cancel && cancel->load();
  } else {
    found = FindWWE15PacFolder(selected);
    if (found.empty()) found = FindWWE15DiscImage(selected);  // extracted files first, then a disc image
  }
  std::error_code file_error;
  if (!found.empty() && fs::is_regular_file(found, file_error)) scan.disc_image = found;
  else scan.pac_folder = found;
  if (found.empty()) {
    if (scan.cancelled)
      scan.error = "Search stopped. Press Choose a disc image and pick your WWE 2K15 .iso, or Choose a folder with the "
                   "extracted disc, or Search again.";
    else if (scan.searched_pc)
      scan.error = std::string(scan.search_limit_reached ? "The search stopped after five minutes without finding" : "Could not find") +
                   " WWE 2K15 on this PC. Press Choose a disc image and pick your WWE 2K15 .iso, or Choose a folder with "
                   "the extracted disc (its pac folder has ch/chEtc.pac, m3.pac and createmode.pac).";
    else if (std::error_code ec; fs::is_regular_file(selected, ec))
      scan.error = internal::PathToUtf8(selected.filename()) + " is not a WWE 2K15 (Xbox 360) disc image. Choose your "
                   "WWE 2K15 .iso or .img file, or Choose a folder with the extracted disc.";
    else
      scan.error = "No WWE 2K15 game files were found in " + internal::PathToUtf8(selected) +
                   ". Press Choose a disc image and pick your WWE 2K15 .iso file, or Choose a folder with the extracted disc "
                   "(its pac folder has ch/chEtc.pac, m3.pac and createmode.pac).";
    return scan;
  }
  const std::set<std::string> disc_models = scan.disc_image.empty() ? std::set<std::string>{} : DiscModelNames(scan.disc_image);
  for (const auto& r : recipes) {
    WWE15ImportWrestler w;
    w.source_id = r.source_id;
    w.target_id = r.target_id;
    w.name = r.name;
    const std::string model_name = "ch" + std::to_string(r.source_id) + ".pac";
    if (scan.disc_image.empty()) {
      std::error_code ec;
      w.available = fs::is_regular_file(scan.pac_folder / "ch" / model_name, ec) && !ec;
    } else {
      w.available = disc_models.count(model_name) > 0;
    }
    if (!w.available)
      w.missing_reason = "pac/ch/" + model_name + " is missing in the WWE 2K15 " + (scan.disc_image.empty() ? "folder" : "disc image");
    scan.wrestlers.push_back(std::move(w));
  }
  return scan;
}

Result RunWWE15Import(const Paths& launcher_paths, const fs::path& game_folder, const WWE15ImportScan& scan,
                      const std::vector<uint32_t>& selected_source_ids, const ProgressFn& progress, CancelFlag& cancel,
                      WWE15ImportSummary* summary, const WWE15ImportOptions& options) {
  if (summary) *summary = {};
  // Windows stops at 260 characters and imports nest deep (same guard as the WWE '13 imports).
  if (internal::PathToUtf8(launcher_paths.exe_dir).size() > 125)
    return Result::Fail("This game folder's location is too long for Windows. Move the folder closer to the top of the drive, for example C:\\Games, and try again. Nothing has been changed.");
  if (HasWWE15Import(launcher_paths)) return Result::Fail("Undo the WWE 2K15 import before importing WWE 2K15 wrestlers again.");
  if (HasWWE13ArenaImport(launcher_paths))
    return Result::Fail("Undo the imported WWE '13 arenas first: they changed some of the same game files. Import them again afterwards.");
  if (!scan.error.empty()) return Result::Fail(scan.error);
  if (scan.pac_folder.empty() && scan.disc_image.empty()) return Result::Fail("Choose the folder with your WWE 2K15 files first.");
  if (selected_source_ids.empty()) return Result::Fail("Choose at least one WWE 2K15 wrestler to import.");

  std::vector<WWE15ImportPort> recipes, chosen;
  std::string error;
  if (!ReadWWE15ImportRecipes(launcher_paths.exe_dir / "data" / "wwe2k15-ports.tsv", &recipes, &error)) return Result::Fail(error);
  const std::set<uint32_t> want(selected_source_ids.begin(), selected_source_ids.end());
  for (const auto& r : recipes) if (want.count(r.source_id)) chosen.push_back(r);
  if (chosen.size() != want.size()) return Result::Fail("A chosen WWE 2K15 wrestler is not in the import list (data/wwe2k15-ports.tsv).");
  for (const auto& w : scan.wrestlers)
    if (want.count(w.source_id) && !w.available) return Result::Fail(w.name + ": " + w.missing_reason + ".");

  std::error_code ec;
  fs::path temp = fs::temp_directory_path(ec);
  if (ec) temp = launcher_paths.exe_dir / "w15tmp";
  // staging: about 0.6 GiB of rewritten add-on and game files plus ~60 MiB per wrestler (largest 2K15 model 32 MiB);
  // the optional 2K15 look stage adds about 480 MB, so require 512 MiB more when it is on.
  const uint64_t looks_need = options.existing_wrestler_looks ? 512 * kMib : 0;
  // own themes: the rebuilt entrance music pack (up to ~400 MiB) and, from a disc image, the two 2K15 sound files (~700 MiB)
  const uint64_t music_need = options.audio ? (scan.disc_image.empty() ? 512 : 1280) * kMib : 0;
  if (!EnoughSpace(temp, 1024 * kMib + chosen.size() * 80 * kMib + looks_need + music_need, &error)) return Result::Fail(error);
  const fs::path work = temp / ("w15-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{work};

  // A disc image: copy just the files the import reads into the work folder first (removed with it afterwards).
  fs::path wwe15_root = scan.pac_folder;
  if (!scan.disc_image.empty()) {
    std::vector<uint32_t> models(want.begin(), want.end());
    if (options.existing_wrestler_looks)
      models.insert(models.end(), WWE15ExistingLookIds().begin(), WWE15ExistingLookIds().end());
    if (!ExtractWWE15DiscFiles(scan.disc_image, work / "disc", models, progress, &cancel, &error)) return Result::Fail(error);
    wwe15_root = work / "disc";
  }

  WWE15ImportPaths paths;
  paths.wwe15_root = wwe15_root;
  paths.wwe14_root = game_folder;
  paths.userdata_root = launcher_paths.userdata_dir;
  paths.own_slots = launcher_paths.exe_dir / "own-slots.txt";
  paths.moves_data = launcher_paths.exe_dir / "data" / "wwe2k15-moves.tsv";
  {
    fs::path converter = launcher_paths.exe_dir / "wwe2k14-bink-convert";
#ifdef _WIN32
    converter += ".exe";
#endif
    std::error_code converter_error;
    if (fs::is_regular_file(converter, converter_error) && !converter_error) paths.bink_converter_executable = converter;
    paths.titantron_cache = launcher_paths.exe_dir / "w15cache";
  }
  // A failure that belongs to one wrestler leaves him out and prepares the rest again from scratch (nothing is applied
  // until a whole run succeeds, so the game files never hold a half import). Owner 11-13.
  WWE15ImportRunResult staged;
  std::vector<std::string> skipped;
  fs::path stage_dir;
  for (size_t attempt = 0;; ++attempt) {
    stage_dir = work / ("stage-" + std::to_string(attempt));
    staged = {};
    error.clear();
    if (RunWWE15ImportStages(paths, chosen, stage_dir, options, &staged, &error, progress, &cancel)) break;
    const std::string reason = error.empty() ? "The WWE 2K15 import could not be prepared." : error;
    const auto failed = std::find_if(chosen.begin(), chosen.end(),
                                     [&](const WWE15ImportPort& c) { return c.source_id == staged.failed_source_id; });
    if (cancel.load() || staged.failed_source_id == 0 || failed == chosen.end()) {
      if (skipped.empty()) return Result::Fail(reason);
      std::string message = reason;
      for (const std::string& line : skipped) message += " Also left out: " + line;
      return Result::Fail(message);
    }
    skipped.push_back(reason);
    chosen.erase(failed);
    std::error_code remove_error;
    fs::remove_all(stage_dir, remove_error);
    if (chosen.empty()) {
      std::string message = "None of the chosen WWE 2K15 wrestlers could be imported. Nothing has been changed.";
      for (const std::string& line : skipped) message += " " + line;
      return Result::Fail(message);
    }
  }
  // the record a later WWE '13 import uses to put these wrestlers back (installed and undone with the import)
  {
    std::string text = "source=" + internal::PathToUtf8(scan.disc_image.empty() ? scan.pac_folder : scan.disc_image) + "\n";
    text += "version=" + std::to_string(kWWE15ImportVersion) + "\n";
    text += std::string("looks=") + (options.existing_wrestler_looks ? "1" : "0") + "\nids=";
    for (size_t i = 0; i < chosen.size(); ++i) text += (i ? "," : "") + std::to_string(chosen[i].source_id);
    text += "\n";
    const fs::path record = stage_dir / "files" / "launcher" / "mods" / "wwe15" / "import.txt";
    std::error_code record_error;
    fs::create_directories(record.parent_path(), record_error);
    std::ofstream out(record, std::ios::binary | std::ios::trunc);
    out << text;
    if (record_error || !out) return Result::Fail("Could not prepare the WWE 2K15 import record. Nothing has been changed.");
  }
  // applying keeps a copy of every replaced file for Undo: the staged size again on each target volume
  const uint64_t staged_bytes = TreeBytes(stage_dir / "files");
  for (const fs::path& where : {game_folder, launcher_paths.userdata_dir, launcher_paths.exe_dir})
    if (!EnoughSpace(where, 2 * staged_bytes + 256 * kMib, &error)) return Result::Fail(error);
  const Result applied = ApplyWWE15StagedFiles(launcher_paths, game_folder, stage_dir / "files", progress, cancel,
                                               staged.removal_keys);
  if (!applied.ok) return applied;
  // Owner 11-15: how close each imported moveset is to WWE 2K15 (own moves of the total, and every move WWE 2K14 cannot play
  // with what replaced it). Plain text next to the launcher logs; rewritten by each import. A failed write never fails the import.
  {
    std::error_code ignored;
    fs::create_directories(launcher_paths.logs_dir, ignored);
    std::ofstream report(launcher_paths.logs_dir / "wwe2k15-moves-report.txt", std::ios::binary | std::ios::trunc);
    if (report) {
      report << "WWE 2K15 import: moves per wrestler (own WWE 2K15 moves of the total; lines below = moves WWE 2K14 cannot\n"
                "play and what took their place)\n";
      for (const std::string& line : staged.move_notes) report << line << "\n";
    }
  }
  if (summary) {
    summary->imported_wrestlers = chosen.size();
    for (const auto& c : chosen) summary->lines.push_back(c.name + " -> slot " + std::to_string(c.target_id));
    summary->lines.insert(summary->lines.end(), staged.notices.begin(), staged.notices.end());
    summary->skipped = skipped;
  }
  return Result::Ok();
}

bool ReadWWE15ImportRecord(const Paths& launcher_paths, WWE15ImportRecord* record) {
  std::ifstream in(launcher_paths.exe_dir / "mods" / "wwe15" / "import.txt", std::ios::binary);
  if (!in || !record) return false;
  WWE15ImportRecord r;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("source=", 0) == 0) r.source = internal::PathFromUtf8(line.substr(7));
    else if (line == "looks=1") r.existing_wrestler_looks = true;
    else if (line.rfind("version=", 0) == 0) r.version = static_cast<uint32_t>(std::strtoul(line.c_str() + 8, nullptr, 10));
    else if (line.rfind("ids=", 0) == 0) {
      std::stringstream ids(line.substr(4));
      std::string item;
      while (std::getline(ids, item, ','))
        if (!item.empty()) r.source_ids.push_back(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 10)));
    }
  }
  if (r.source.empty() || r.source_ids.empty()) return false;
  *record = std::move(r);
  return true;
}

bool WWE15ImportOutdated(const Paths& launcher_paths) {
  if (!HasWWE15Import(launcher_paths)) return false;
  WWE15ImportRecord record;
  return !ReadWWE15ImportRecord(launcher_paths, &record) || record.version < kWWE15ImportVersion;
}

Result RefreshWWE15Import(const Paths& launcher_paths, const fs::path& game_folder, const ProgressFn& progress,
                          CancelFlag& cancel, WWE15ImportSummary* summary) {
  if (!HasWWE15Import(launcher_paths)) return Result::Fail("There is no WWE 2K15 import to update.");
  WWE15ImportRecord record;
  if (!ReadWWE15ImportRecord(launcher_paths, &record)) {
    // Imports before 1.6.1 have no record: their wrestlers are the own-slots.txt ids that are WWE 2K15 import slots
    // (data/wwe2k15-ports.tsv) and not WWE '13 imports (mods/wwe13/roster.tsv); the two slot sets do not overlap.
    std::vector<WWE15ImportPort> recipes;
    std::string error;
    if (!ReadWWE15ImportRecipes(launcher_paths.exe_dir / "data" / "wwe2k15-ports.tsv", &recipes, &error)) return Result::Fail(error);
    std::map<uint32_t, uint32_t> source_of;
    for (const WWE15ImportPort& p : recipes) source_of[p.target_id] = p.source_id;
    const auto ids_in = [](const fs::path& file) {
      std::set<uint32_t> ids;
      std::ifstream in(file, std::ios::binary);
      std::string line;
      while (std::getline(in, line))
        if (!line.empty() && line[0] >= '0' && line[0] <= '9') ids.insert(static_cast<uint32_t>(std::strtoul(line.c_str(), nullptr, 10)));
      return ids;
    };
    const std::set<uint32_t> from13 = ids_in(launcher_paths.exe_dir / "mods" / "wwe13" / "roster.tsv");
    for (const uint32_t target : ids_in(launcher_paths.exe_dir / "own-slots.txt")) {
      const auto it = source_of.find(target);
      if (it != source_of.end() && !from13.count(target)) record.source_ids.push_back(it->second);
    }
    if (record.source_ids.empty())
      return Result::Fail("The launcher could not tell which WWE 2K15 wrestlers are imported. Undo the WWE 2K15 import and import "
                          "them again. Nothing has been changed.");
  }
  if (progress) progress(0, 1, "Checking your WWE 2K15 files…");
  const WWE15ImportScan scan = ScanWWE15Import(launcher_paths, record.source, progress, &cancel);
  if (!scan.error.empty() || (scan.disc_image.empty() && scan.pac_folder.empty()))
    return Result::Fail("WWE 2K15 was not found" + (record.source.empty() ? std::string(" on this PC") : " at " + internal::PathToUtf8(record.source)) +
                        ". Undo the WWE 2K15 import and import again from where WWE 2K15 is now. Nothing has been changed.");
  for (const WWE15ImportWrestler& w : scan.wrestlers)
    if (!w.available && std::find(record.source_ids.begin(), record.source_ids.end(), w.source_id) != record.source_ids.end())
      return Result::Fail(w.name + " is not in the WWE 2K15 files found. Nothing has been changed.");
  const Result out = UndoWWE15Import(launcher_paths, game_folder, progress, cancel);
  if (!out.ok) return out;
  CancelFlag keep_going{false};  // the old wrestlers are already out: finish putting them back
  WWE15ImportOptions options;
  options.existing_wrestler_looks = record.existing_wrestler_looks;
  const Result back = RunWWE15Import(launcher_paths, game_folder, scan, record.source_ids, progress, keep_going, summary, options);
  if (!back.ok)
    return Result::Fail("The old WWE 2K15 import was taken out, but importing again failed: " + back.error +
                        " Use Import from WWE 2K15 to bring them back.");
  return Result::Ok();
}

}  // namespace wwe2k14::launcher
