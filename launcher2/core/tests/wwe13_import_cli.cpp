// Host tool: runs the real import job (the one behind the Mods page button) without the window.
// usage: wwe13_import_cli LAUNCHER_DIR GAME_DIR import WWE13_FOLDER all|ID[,ID...]
//        wwe13_import_cli LAUNCHER_DIR GAME_DIR undo
//        wwe13_import_cli LAUNCHER_DIR GAME_DIR arenas WWE13_BASE ID[,ID...] [BINK_CONVERTER]
//        wwe13_import_cli LAUNCHER_DIR GAME_DIR arena-undo
//        wwe13_import_cli LAUNCHER_DIR GAME_DIR arena-list WWE13_BASE
#include "../launcher_core.h"
#include "../wwe13_audio.h"
#include "../wwe13_install.h"
#include "../wwe13_arena_install.h"

#include <chrono>
#include <iostream>
#include <set>
#include <sstream>

using namespace wwe2k14::launcher;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc < 4) { std::cerr << "usage\n"; return 2; }
  const fs::path launcher = argv[1];
  const fs::path game = argv[2];
  const std::string action = argv[3];
  Paths paths = ResolvePaths(launcher / "wwe2k14-launcher");
  std::string last;
  const ProgressFn progress = [&](uint64_t done, uint64_t total, const std::string& label) {
    if (label != last) { std::cout << "[" << done << "/" << total << "] " << label << std::endl; last = label; }
  };
  CancelFlag cancel{false};
  const auto start = std::chrono::steady_clock::now();
  Result result = Result::Ok();
  if (action == "arena-undo") {
    result = UndoWWE13ArenaImport(paths, game, progress, cancel);
  } else if (action == "arena-list" && argc == 5) {
    std::string error;
    for (const WWE13ArenaItem& item : ListWWE13Arenas(argv[4], &error))
      std::cout << item.source_id << "\t" << item.name << "\t" << item.picture << "\t" << (item.available ? "available" : item.missing_reason) << "\n";
    std::cout << "free numbers:";
    for (uint32_t id : FreeWWE13ArenaNumbers(game)) std::cout << " " << id;
    std::cout << "\n";
    if (!error.empty()) std::cout << "note: " << error << "\n";
  } else if (action == "arenas" && (argc == 6 || argc == 7)) {
    std::vector<uint32_t> chosen;
    std::istringstream ids(argv[5]);
    std::string id;
    while (std::getline(ids, id, ',')) chosen.push_back(static_cast<uint32_t>(std::stoul(id)));
    WWE13ArenaImportSummary summary;
    result = RunWWE13ArenaImport(paths, game, argv[4], chosen, argc == 7 ? fs::path(argv[6]) : fs::path(), progress, cancel, &summary);
    for (const std::string& line : summary.lines) std::cout << "note: " << line << "\n";
  } else if (action == "undo") {
    result = UndoWWE13Import(paths, game, progress, cancel);
  } else if (action == "import" && argc == 6) {
    const WWE13ImportScan scan = ScanWWE13ImportSources(paths, argv[4], {}, cancel);
    std::cout << "scan base=" << scan.base_found << " packs=" << scan.packs.size() << " error=" << scan.error << "\n";
    std::vector<uint32_t> chosen;
    size_t available = 0;
    for (const WWE13ImportWrestler& wrestler : scan.wrestlers) {
      if (wrestler.available) ++available;
      else std::cout << "missing " << wrestler.source_id << " " << wrestler.name << ": " << wrestler.missing_reason << "\n";
    }
    std::cout << "available " << available << " of " << scan.wrestlers.size() << "\n";
    if (std::string(argv[5]) == "all") {
      for (const WWE13ImportWrestler& wrestler : scan.wrestlers) if (wrestler.available) chosen.push_back(wrestler.source_id);
    } else {
      std::istringstream ids(argv[5]);
      std::string id;
      while (std::getline(ids, id, ',')) chosen.push_back(static_cast<uint32_t>(std::stoul(id)));
    }
    WWE13ImportSummary summary;
    result = RunWWE13Import(paths, game, scan, chosen, progress, cancel, &summary);
    std::cout << "imported " << summary.imported_wrestlers << "\n";
    for (const std::string& line : summary.lines) std::cout << "note: " << line << "\n";
  } else if (action == "audio" && argc == 6) {  // audio step alone, into a fresh folder
    std::vector<WWE13ImportPort> ports;
    std::string error;
    if (!ReadWWE13ImportRecipes(paths.exe_dir / "data" / "wwe13-ports.tsv", &ports, &error)) { std::cerr << error << "\n"; return 1; }
    const WWE13ImportScan scan = ScanWWE13ImportSources(paths, argv[4], {}, cancel);
    WWE13ImportPaths sources;
    sources.wwe13_root = scan.base_folder;
    sources.wwe14_root = game;
    sources.userdata_root = paths.userdata_dir;
    for (const WWE13ImportPack& pack : scan.packs) if (!pack.package_file) sources.wwe13_pack_roots.push_back(pack.folder);
    WWE13ImportAudioResult audio;
    const bool ok = RunWWE13ImportAudio(sources, ports, argv[5], {}, cancel, &audio, &error);
    std::cout << "songs " << audio.songs << " names " << audio.announcer_names << "\nno song:";
    for (uint32_t id : audio.songs_without_audio) std::cout << " " << id;
    std::cout << "\nno name:";
    for (uint32_t id : audio.names_without_audio) std::cout << " " << id;
    std::cout << "\n";
    result = ok ? Result::Ok() : Result::Fail(error);
  } else {
    std::cerr << "usage\n";
    return 2;
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::cout << (result.ok ? "OK" : "FAILED: " + result.error) << " in " << seconds << " s\n";
  return result.ok ? 0 : 1;
}
