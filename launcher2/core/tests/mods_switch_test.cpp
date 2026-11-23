// Host test: Mods off clean view (owner 11-18). One WWE '13 import changes a game file, adds a game file and changes an
// add-on file; the clean view must show the originals, leave the added file out, keep the real folders untouched, copy
// the saves once and then keep them separate.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mods_switch.h"
#include "../wwe13_install.h"
#include "../internal/util.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << bytes;
}
std::string Read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
}  // namespace

int main() {
  try {
    fs::path root = fs::temp_directory_path() / "mods-switch-test";
    if (const char* o = std::getenv("WWE2K14_TEST_ROOT"); o && *o) root = fs::path(o) / "mods-switch";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    const fs::path launcher = root / "launcher", game = root / "game", userdata = launcher / "userdata";
    Paths paths;
    paths.exe_dir = launcher;
    paths.game_exe = launcher / "wwe2k14";
    paths.userdata_dir = userdata;
    paths.backups_dir = launcher / "backups";
    const fs::path content = userdata / "0000000000000000" / "545408B2" / "00000002" / "045B";
    Write(game / "pac" / "ch" / "ch152.pac", "original ch152");
    Write(game / "pac" / "menu" / "menu.pac", "untouched menu");
    Write(content / "port.bin", "original port");
    Write(userdata / "B13EBABEBABEBABE" / "545408B2" / "00000001" / "SaveData.dat", "modded save v1");

    const fs::path stage = root / "stage" / "files";
    Write(stage / "game" / "pac" / "ch" / "ch152.pac", "IMPORTED ch152");
    Write(stage / "game" / "pac" / "ch" / "ch900.pac", "IMPORTED new file");
    Write(stage / "userdata" / "0000000000000000" / "545408B2" / "00000002" / "045B" / "port.bin", "IMPORTED port");
    CancelFlag cancel{false};
    WWE13ImportSummary summary;
    const Result applied = ApplyWWE13StagedFiles(paths, game, stage, {}, cancel, &summary);
    Check(applied.ok, applied.error.c_str());
    Check(Read(game / "pac" / "ch" / "ch152.pac") == "IMPORTED ch152", "fixture import did not apply");

    ModsOffView view;
    Result r = PrepareModsOffView(paths, game, &view);
    Check(r.ok, r.error.c_str());
    Check(Read(view.game / "pac" / "ch" / "ch152.pac") == "original ch152", "changed game file not the original");
    Check(!fs::exists(view.game / "pac" / "ch" / "ch900.pac"), "file added by the import is in the clean view");
    Check(Read(view.game / "pac" / "menu" / "menu.pac") == "untouched menu", "unchanged game file missing");
    const fs::path view_content = view.userdata / "0000000000000000" / "545408B2" / "00000002" / "045B";
    Check(Read(view_content / "port.bin") == "original port", "changed add-on file not the original");
    const fs::path view_save = view.userdata / "B13EBABEBABEBABE" / "545408B2" / "00000001" / "SaveData.dat";
    Check(Read(view_save) == "modded save v1", "saves were not copied on first use");
    // Real folders untouched.
    Check(Read(game / "pac" / "ch" / "ch152.pac") == "IMPORTED ch152", "real game folder changed");
    Check(Read(game / "pac" / "ch" / "ch900.pac") == "IMPORTED new file", "real added file removed");
    Check(Read(content / "port.bin") == "IMPORTED port", "real add-on changed");

    // Separate saves: mods-off play writes its save; mods-on play writes another; neither leaks.
    Write(view_save, "clean save v2");
    Write(userdata / "B13EBABEBABEBABE" / "545408B2" / "00000001" / "SaveData.dat", "modded save v3");
    Write(game / "pac" / "menu" / "extra.pac", "new base file");
    r = PrepareModsOffView(paths, game, &view);
    Check(r.ok, r.error.c_str());
    Check(Read(view_save) == "clean save v2", "mods-off save was overwritten by a refresh");
    Check(Read(userdata / "B13EBABEBABEBABE" / "545408B2" / "00000001" / "SaveData.dat") == "modded save v3",
          "modded save changed");
    Check(Read(view.game / "pac" / "menu" / "extra.pac") == "new base file", "refresh missed a new base file");
    Check(Read(view.game / "pac" / "ch" / "ch152.pac") == "original ch152", "refresh lost the original");

    // A file removed from the game folder leaves the view; a damaged original refuses to start.
    fs::remove(game / "pac" / "menu" / "extra.pac");
    r = PrepareModsOffView(paths, game, &view);
    Check(r.ok && !fs::exists(view.game / "pac" / "menu" / "extra.pac"), "removed base file stayed in the view");
    std::map<std::string, ImportOriginal> originals;
    std::string error;
    Check(CollectImportOriginals(paths, &originals, &error), "originals unreadable");
    Check(originals.size() >= 3 && !originals.at("game/pac/ch/ch900.pac").exists, "original list wrong");
    fs::remove(view.game / "pac" / "ch" / "ch152.pac");
    Write(originals.at("game/pac/ch/ch152.pac").artifact, "tampered");
    r = PrepareModsOffView(paths, game, &view);
    Check(!r.ok, "a damaged original did not stop Mods off");

    // Launch plan: arguments point at the view.
    LaunchPlan plan;
    plan.arguments = {"/real/game", "--x=1", "--user_data_root=/real/userdata"};
    UseModsOffView(view, &plan);
    Check(fs::path(plan.arguments[0]) == view.game, "game argument not the view");
    Check(plan.arguments[2] == "--user_data_root=" + internal::PathToUtf8(view.userdata), "userdata argument not the view");
    // Mods off in the launch plan: every Mods-page option off, imported slots off.
    Settings settings;
    settings.mods_enabled = false;
    settings.hiac_weapon_wheel = true;
    settings.weapon_wheel_more = true;
    settings.unlock_all = true;
    settings.imports_in_grid = true;
    Write(launcher / "mods" / "wwe13" / "arenas.txt", "57\n");
    const LaunchPlan off = BuildLaunchPlan(paths, settings, {});
    auto env = [](const LaunchPlan& p, const std::string& k) -> const std::string* {
      const std::string* v = nullptr;
      for (const auto& [key, value] : p.environment) if (key == k) v = &value;
      return v;
    };
    Check(!env(off, "WWE2K14_HIAC_WHEEL"), "mods off still passes the weapon wheel");
    Check(env(off, "WWE2K14_OWN_SLOTS") && *env(off, "WWE2K14_OWN_SLOTS") == "0", "mods off leaves imported slots on");
    Check(env(off, "WWE2K14_OWN_GRID") && env(off, "WWE2K14_OWN_GRID")->empty(), "mods off leaves the grid option on");
    Check(env(off, "WWE2K14_UNLOCK_ALL") && env(off, "WWE2K14_UNLOCK_ALL")->empty(), "mods off leaves unlock all on");
    Check(env(off, "WWE2K14_ARENA_LIST") && env(off, "WWE2K14_ARENA_LIST")->empty(), "mods off passes the arena list");
    settings.mods_enabled = true;
    const LaunchPlan on = BuildLaunchPlan(paths, settings, {});
    Check(env(on, "WWE2K14_HIAC_WHEEL") && *env(on, "WWE2K14_HIAC_WHEEL") == "2", "mods on lost the weapon wheel");
    Check(!env(on, "WWE2K14_OWN_SLOTS"), "mods on sets the slot switch");
    Check(env(on, "WWE2K14_ARENA_LIST") && !env(on, "WWE2K14_ARENA_LIST")->empty(), "mods on lost the arena list");
    std::cout << "mods_switch_test PASS\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "mods_switch_test FAIL: " << e.what() << "\n";
    return 1;
  }
}
