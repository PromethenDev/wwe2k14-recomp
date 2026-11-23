#ifdef NDEBUG
#undef NDEBUG
#endif

#include "launcher_core.h"

#include "internal/util.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {

Paths MakePaths(const fs::path& root) {
  Paths paths = ResolvePaths(root / "wwe2k14-launcher");
  paths.exe_dir = root;
  paths.game_exe = root / "wwe2k14";
  paths.settings_file = root / "wwe2k14-enhanced.ini";
  paths.userdata_dir = root / "userdata";
  paths.music_dir = root / "music";
  paths.logs_dir = root / "logs";
  paths.backups_dir = root / "backups";
  paths.default_game_folder = root / "WWE 2K14";
  return paths;
}

// Test fixtures beyond MAX_PATH need the extended-length form on Windows (the importer uses it internally).
fs::path Long(const fs::path& path) {
#ifdef _WIN32
  const std::wstring text = fs::absolute(path).lexically_normal().make_preferred().wstring();
  if (text.rfind(L"\\\\?\\", 0) == 0) return path;
  return fs::path(L"\\\\?\\" + text);
#else
  return path;
#endif
}

void Write(const fs::path& short_path, std::string_view text) {
  const fs::path path = Long(short_path);
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  assert(!error);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.close();
  assert(output.good());
}

void SetSearchRoots(const fs::path& root) {
  const std::string value = internal::PathToUtf8(root);
#ifdef _WIN32
  assert(_putenv_s("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", value.c_str()) == 0);
#else
  assert(setenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", value.c_str(), 1) == 0);
#endif
}

void ClearSearchRoots() {
#ifdef _WIN32
  assert(_putenv_s("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", "") == 0);
#else
  assert(unsetenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS") == 0);
#endif
}

std::string Read(const fs::path& path) {
  std::string text;
  assert(internal::ReadTextFile(Long(path), text));
  return text;
}

void TestReleaseFolderImport(const fs::path& root) {
  const fs::path source_wrapper = root / "release-1.0.1-zip";
  const fs::path source = source_wrapper / "WWE2K14-Recomp-x64";
  const fs::path destination_root = root / "new-install";
  const Paths paths = MakePaths(destination_root);
  fs::create_directories(paths.exe_dir);

  const fs::path source_save = source / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat";
  const fs::path source_dlc = source / "userdata" / "0000000000000000" / "545408B2" / "00000002" / "DLC1" / "content.bin";
  const fs::path source_cache = source / "userdata" / "cache" / "nr" / "pipelines-vulkan.keys";
  Write(source_save, "imported-save-v1.0.1");
  Write(source_dlc, "imported-dlc-v1.0.1");
  Write(source_cache, "source-build-cache-must-not-import");
  Write(source / "music" / "theme.wav", "imported-song");
  Write(source / "music" / "bonus.ogg", "second-song");
  Write(source / "wwe2k14.toml", "keybind_a = \"A\"\n");
  Write(source / "WWE 2K14" / "default.xex", "game-files-are-not-copied");
  Write(source / "wwe2k14-enhanced.ini",
        "[Settings]\r\nschema=7\r\ngame_folder=old-relative-game\r\ngpu=10de:2684\r\n"
        "display=windowed\r\nstretch_to_fill_wide_screens=1\r\n");
  Write(source / "bin" / "wwe2k14", "do-not-copy-native-game");

  const fs::path old_save = paths.userdata_dir / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat";
  const fs::path old_dlc = paths.userdata_dir / "0000000000000000" / "545408B2" / "00000002" / "OLD" / "old.bin";
  const fs::path prewarm = paths.userdata_dir / "cache" / "nr" / "pipelines-vulkan.keys";
  Write(old_save, "old-save-to-back-up");
  Write(old_dlc, "old-dlc-to-back-up");
  Write(prewarm, "new-build-prewarm-list");
  Write(paths.music_dir / "theme.wav", "old-song-to-back-up");
  Write(paths.music_dir / "keep-existing.wav", "not-in-source");

  InstallationImportInfo info;
  const Result scanned = ScanInstallationImport(paths, source_wrapper, &info);
  assert(scanned.ok);
  assert(info.source_root == fs::weakly_canonical(source));
  assert(info.save_files == 1 && info.dlc_files == 1 && info.music_files == 2);
  assert(info.has_ini && info.has_keyboard_settings && info.backup_required);
  assert(info.save_bytes > 0 && info.dlc_bytes > 0 && info.music_bytes > 0);

  CancelFlag cancel{false};
  fs::path backup;
  uint64_t last_done = 0;
  const Result imported = ImportInstallation(paths, info,
      [&](uint64_t done, uint64_t, const std::string&) { last_done = done; }, cancel, &backup);
  assert(imported.ok);
  assert(!backup.empty() && fs::is_directory(backup));
  assert(last_done > 0);

  assert(Read(paths.userdata_dir / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat") ==
         "imported-save-v1.0.1");
  assert(Read(paths.userdata_dir / "0000000000000000" / "545408B2" / "00000002" / "DLC1" / "content.bin") ==
         "imported-dlc-v1.0.1");
  assert(Read(paths.userdata_dir / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat") != "old-save-to-back-up");
  assert(Read(prewarm) == "new-build-prewarm-list");
  assert(!fs::exists(paths.userdata_dir / "cache" / "nr" / "source-build-cache-must-not-import"));
  assert(Read(paths.music_dir / "theme.wav") == "imported-song");
  assert(Read(paths.music_dir / "keep-existing.wav") == "not-in-source");
  assert(Read(paths.exe_dir / "wwe2k14.toml") == "keybind_a = \"A\"\n");
  assert(!fs::exists(paths.exe_dir / "bin" / "wwe2k14"));

  const Settings settings = LoadSettings(paths);
  assert(settings.game_folder == fs::weakly_canonical(source / "WWE 2K14"));
  assert(settings.gpu_id == "10DE:2684");
  assert(settings.display == DisplayMode::kWindowed);
  assert(settings.stretch_to_fill_wide_screens);
  assert(settings.renderer == Renderer::kNative);  // v1.0.1 has no renderer key; destination default remains.

  assert(Read(source_save) == "imported-save-v1.0.1");
  assert(Read(source_dlc) == "imported-dlc-v1.0.1");
  assert(Read(source_cache) == "source-build-cache-must-not-import");
  assert(Read(source / "music" / "theme.wav") == "imported-song");
  assert(Read(source / "wwe2k14.toml") == "keybind_a = \"A\"\n");
  assert(Read(backup / "music" / "theme.wav") == "old-song-to-back-up");
  assert(Read(backup / "userdata" / "0000000000000000" / "545408B2" / "00000002" / "OLD" / "old.bin") ==
         "old-dlc-to-back-up");
  assert(Read(backup / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat") ==
         "old-save-to-back-up");
  assert(!fs::exists(backup / "userdata" / "cache"));
  assert(!ListBackups(paths).empty());  // Existing Back Up Now archive mechanism also ran.
}

void TestNativeTestPackageLayout(const fs::path& root) {
  const fs::path source = root / "WWE2K14-Native-1023N-TEST";
  Paths paths = MakePaths(root / "test-package-destination");
  paths.game_exe = paths.exe_dir / "bin" / "wwe2k14";
  fs::create_directories(paths.exe_dir);
  Write(source / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat", "test-package-save");
  Write(source / "wwe2k14-enhanced.ini", "[Settings]\ngame_folder=/shared/WWE 2K14\n");
  Write(source / "wwe2k14.toml", "keybind_a = \"A\"\n");
  Write(source / "bin" / "wwe2k14", "not-a-user-file");

  InstallationImportInfo info;
  assert(ScanInstallationImport(paths, source, &info).ok);
  assert(info.source_root == fs::weakly_canonical(source));
  CancelFlag cancel{false};
  assert(ImportInstallation(paths, info, {}, cancel).ok);
  assert(Read(paths.userdata_dir / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat") == "test-package-save");
  assert(!fs::exists(paths.exe_dir / "bin" / "wwe2k14"));
  assert(Read(paths.exe_dir / "bin" / "wwe2k14.toml") == "keybind_a = \"A\"\n");
  assert(LoadSettings(paths).game_folder == fs::path("/shared/WWE 2K14"));
}

void TestLinuxZipRootAndSelfRefusal(const fs::path& root) {
  const fs::path archive_root = root / "linux-zip-root";
  const fs::path source = archive_root / "Linux-x86_64";
  Write(archive_root / "README.txt", "outer archive marker");
  Write(source / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat", "linux-zip-save");
  Write(source / "music" / "theme.flac", "linux-zip-song");
  Write(source / "wwe2k14-enhanced.ini", "[Settings]\ndisplay=fullscreen\n");

  const Paths paths = MakePaths(root / "linux-destination");
  fs::create_directories(paths.exe_dir);
  InstallationImportInfo info;
  assert(ScanInstallationImport(paths, archive_root, &info).ok);
  assert(info.source_root == fs::weakly_canonical(source));
  CancelFlag cancel{false};
  assert(ImportInstallation(paths, info, {}, cancel).ok);
  assert(Read(paths.userdata_dir / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat") == "linux-zip-save");
  assert(Read(paths.music_dir / "theme.flac") == "linux-zip-song");
  assert(Read(source / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat") == "linux-zip-save");

  InstallationImportInfo self_info;
  const Result self = ScanInstallationImport(paths, paths.exe_dir, &self_info);
  assert(!self.ok && self.error.find("not this one") != std::string::npos);
}

void TestSelectedFolderNormalization(const fs::path& root) {
  const fs::path archive = root / "old-linux-zip";
  const fs::path source = archive / "Linux-x86_64";
  const fs::path save = source / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat";
  Write(save, "userdata-only-source");
  const Paths paths = MakePaths(root / "normalization-destination");
  fs::create_directories(paths.exe_dir);

  const std::vector<fs::path> selections = {
      source,
      source / "userdata",
      source / "userdata" / "B13EBABEBABEBABE",
      archive,
  };
  for (const fs::path& selection : selections) {
    InstallationImportInfo info;
    const Result scanned = ScanInstallationImport(paths, selection, &info);
    assert(scanned.ok);
    assert(info.source_root == fs::weakly_canonical(source));
    assert(info.has_userdata && info.save_files == 1);
    assert(!info.has_music && !info.has_ini && !info.has_keyboard_settings);
  }

  InstallationImportInfo info;
  assert(ScanInstallationImport(paths, source / "userdata", &info).ok);
  CancelFlag cancel{false};
  assert(ImportInstallation(paths, info, {}, cancel).ok);
  assert(Read(paths.userdata_dir / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat") ==
         "userdata-only-source");
}

void TestAutomaticInstallationFinder(const fs::path& root) {
  const fs::path search_root = root / "search-tree";
  const fs::path current_root = search_root / "current-install";
  const Paths paths = MakePaths(current_root);
  fs::create_directories(paths.exe_dir);

  const fs::path v101 = search_root / "Downloads" / "release-1.0.1-zip" / "WWE2K14-Recomp-x64";
  const fs::path test_package = search_root / "Desktop" / "WWE2K14-Native-1023N-TEST";
  const fs::path linux_layout = search_root / "Documents" / "Linux-x86_64";
  const fs::path decoy = search_root / "decoy-without-saves";
  Write(v101 / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat", "old-release-save");
  Write(v101 / "userdata" / "0000000000000000" / "545408B2" / "00000002" / "DLC-1" / "content.bin",
        "old-release-dlc");
  Write(v101 / "WWE 2K14 Launcher.exe", "launcher");
  Write(test_package / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat", "test-package-save");
  Write(test_package / "bin" / "wwe2k14.exe", "game");
  Write(linux_layout / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat", "linux-save");
  Write(linux_layout / "userdata" / "0000000000000000" / "545408B2" / "00000002" / "LINUX-DLC" /
            "content.bin",
        "linux-dlc");
  Write(linux_layout / "wwe2k14-launcher", "launcher");
  Write(decoy / "userdata" / "other-account" / "empty-marker.txt", "not a save or DLC");
  Write(decoy / "wwe2k14-launcher", "launcher");
  Write(current_root / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat", "current-save");
  Write(current_root / "wwe2k14-launcher", "current-launcher");

  const auto now = fs::file_time_type::clock::now();
  std::error_code error;
  fs::last_write_time(v101 / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat",
                      now - std::chrono::hours(8), error);
  assert(!error);
  fs::last_write_time(test_package / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat",
                      now - std::chrono::hours(4), error);
  assert(!error);
  fs::last_write_time(linux_layout / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat",
                      now - std::chrono::hours(1), error);
  assert(!error);

  SetSearchRoots(search_root);
  CancelFlag cancel{false};
  std::vector<std::string> progress_labels;
  const std::vector<FoundInstallationImport> found = FindInstallationImports(
      paths, [&](uint64_t, uint64_t, const std::string& label) { progress_labels.push_back(label); }, cancel);
  ClearSearchRoots();

  assert(found.size() == 3);
  assert(found[0].folder == fs::weakly_canonical(linux_layout));
  assert(found[1].folder == fs::weakly_canonical(test_package));
  assert(found[2].folder == fs::weakly_canonical(v101));
  assert(found[0].saves == 1 && found[0].dlc_packs == 1);
  assert(found[1].saves == 1 && found[1].dlc_packs == 0);
  assert(found[2].saves == 1 && found[2].dlc_packs == 1);
  assert(found[0].bytes > 0 && !found[0].newest_save_when.empty());
  assert(!progress_labels.empty() && progress_labels.front().find("Searching...") == 0);
}


// Owner bug 2026-10-23: a v1.1.0 release folder has saves at its root and a Linux-x86_64/ part whose userdata holds
// only the shipped prewarm list; the import must take the folder with the saves, and a folder with only the
// prewarm list must not count as something to bring over.
void TestReleaseFolderWithEmptyLinuxPart(const fs::path& root) {
  const fs::path destination = root / "destination";
  const fs::path source = root / "WWE2K14-Recomp-v1.1.0-TEST2";
  Write(source / "WWE 2K14 Launcher.exe", "launcher");
  Write(source / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat", "root-save");
  Write(source / "userdata" / "cache" / "nr" / "pipelines-vulkan.keys", "keys");
  Write(source / "Linux-x86_64" / "wwe2k14-launcher", "linux launcher");
  Write(source / "Linux-x86_64" / "wwe2k14.toml", "keys = 1");
  Write(source / "Linux-x86_64" / "userdata" / "cache" / "nr" / "pipelines-vulkan.keys", "keys");
  const Paths paths = MakePaths(destination);
  fs::create_directories(destination);
  InstallationImportInfo info;
  assert(ScanInstallationImport(paths, source, &info).ok);
  assert(info.source_root.filename() == "WWE2K14-Recomp-v1.1.0-TEST2" && info.save_files == 1);

  const fs::path empty = root / "fresh-release";
  Write(empty / "WWE 2K14 Launcher.exe", "launcher");
  Write(empty / "userdata" / "cache" / "nr" / "pipelines-vulkan.keys", "keys");
  Write(empty / "Linux-x86_64" / "userdata" / "cache" / "nr" / "pipelines-vulkan.keys", "keys");
  InstallationImportInfo empty_info;
  assert(!ScanInstallationImport(paths, empty, &empty_info).ok);
}

// Owner 2026-10-28: importing into a nested folder (...\\WWE2K14-Recomp-v1.4.2-TEST\\WWE2K14-Recomp-x64) failed with
// "A folder could not be created": DLC paths (~114 characters below the install) in the temporary import folder passed
// Windows' 260-character MAX_PATH. The destination here is deep enough that the staged DLC path is > 300 characters.
void TestDeepNestedDestination(const fs::path& root) {
  const fs::path source = root / "old-install";
  const fs::path destination_root = root / "Downloads-wwe13-win-63f610e-wwe13-win-63f610e" /
                                    "WWE2K14-Recomp-v1.4.2-TEST-nested-for-path-length" / "WWE2K14-Recomp-x64";
  const Paths paths = MakePaths(destination_root);
  fs::create_directories(Long(paths.exe_dir));
  const fs::path dlc_relative = fs::path("0000000000000000") / "545408B2" / "00000002" /
                                "A09DECBF32B71FF36D0CBE0AF56B1618CA47CEEB54" / "pac" / "edit" / "casThumb_dlc05.pac";
  Write(source / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat", "deep-save");
  Write(source / "userdata" / dlc_relative, "deep-dlc");
  Write(source / "wwe2k14.toml", "keybind_a = \"A\"\n");
  Write(source / "WWE 2K14" / "default.xex", "game-files-are-not-copied");
  Write(source / "wwe2k14-enhanced.ini", "[Settings]\r\nschema=7\r\n");
  Write(source / "wwe2k14.exe", "old-game");
  const size_t staged_length = internal::PathToUtf8(fs::absolute(paths.exe_dir)).size() + 45 +
                               internal::PathToUtf8(fs::path("userdata") / dlc_relative).size();
  std::cout << "deep destination: staged DLC path ~" << staged_length << " characters\n";
  assert(staged_length > 260);

  InstallationImportInfo info;
  const Result scanned = ScanInstallationImport(paths, source, &info);
  if (!scanned.ok) std::cerr << "scan: " << scanned.error << "\n";
  assert(scanned.ok);
  CancelFlag cancel{false};
  const Result imported = ImportInstallation(paths, info, {}, cancel);
  if (!imported.ok) std::cerr << "import: " << imported.error << "\n";
  assert(imported.ok);
  assert(Read(paths.userdata_dir / dlc_relative) == "deep-dlc");
  assert(Read(paths.userdata_dir / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat") == "deep-save");
}

}  // namespace

void TestModStateCarry(const fs::path& root) {
  // GitHub #132: own-slots.txt, mods/ and the undo sets live next to the launcher, not in userdata/.
  const fs::path source = root / "old" / "WWE2K14-Recomp-x64";
  Write(source / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat", "save");
  Write(source / "own-slots.txt", "# Written by the launcher's Mods page.\n162\n294\n");
  Write(source / "mods" / "wwe13" / "roster.tsv", "#game WWE '13\n294\tBrie Bella\tF\n");
  Write(source / "mods" / "wwe13" / "arenas.txt", "source-arenas");
  Write(source / "mods" / "wwe13" / "theme-levels.tsv", "source-themes");
  Write(source / "backups" / "wwe13-import" / "current.txt", "set1");
  Write(source / "backups" / "wwe13-import" / "sets" / "set1" / "manifest.tsv", "manifest");
  Write(source / "backups" / "wwe13-arenas" / "current.txt", "arena-set");
  Write(source / "backups" / "other-save-backup" / "x.bin", "not-carried");

  // Case 1: fresh target gets everything.
  const Paths fresh = MakePaths(root / "fresh");
  fs::create_directories(fresh.exe_dir);
  InstallationImportInfo info;
  assert(ScanInstallationImport(fresh, source, &info).ok);
  CancelFlag cancel{false};
  assert(ImportInstallation(fresh, info, {}, cancel).ok);
  assert(Read(fresh.exe_dir / "own-slots.txt") == "# Written by the launcher's Mods page.\n162\n294\n");
  assert(Read(fresh.exe_dir / "mods" / "wwe13" / "roster.tsv").find("Brie Bella") != std::string::npos);
  assert(Read(fresh.exe_dir / "backups" / "wwe13-import" / "sets" / "set1" / "manifest.tsv") == "manifest");
  assert(Read(fresh.exe_dir / "backups" / "wwe13-arenas" / "current.txt") == "arena-set");
  assert(!fs::exists(fresh.exe_dir / "backups" / "other-save-backup"));
  assert(Read(source / "own-slots.txt").find("162") != std::string::npos);  // source untouched

  // Case 2: target already has slots and a mods folder: ids merge, the target's own files stay, missing files arrive.
  const Paths merged = MakePaths(root / "merged");
  Write(merged.exe_dir / "own-slots.txt", "# mine\n400\n294\n");
  Write(merged.exe_dir / "mods" / "wwe13" / "roster.tsv", "#game WWE '13\n400\tTarget Guy\tM\n");
  Write(merged.exe_dir / "backups" / "wwe13-import" / "current.txt", "target-set");
  InstallationImportInfo info2;
  assert(ScanInstallationImport(merged, source, &info2).ok);
  assert(ImportInstallation(merged, info2, {}, cancel).ok);
  const std::set<uint32_t> ids = ReadOwnSlotIds(merged.exe_dir / "own-slots.txt");
  assert((ids == std::set<uint32_t>{162, 294, 400}));
  assert(Read(merged.exe_dir / "own-slots.txt").rfind("# Written by the launcher's Mods page.\n", 0) == 0);
  assert(Read(merged.exe_dir / "mods" / "wwe13" / "roster.tsv").find("Target Guy") != std::string::npos);
  assert(Read(merged.exe_dir / "mods" / "wwe13" / "roster.tsv").find("Brie") == std::string::npos);
  assert(Read(merged.exe_dir / "mods" / "wwe13" / "arenas.txt") == "source-arenas");
  assert(Read(merged.exe_dir / "backups" / "wwe13-import" / "current.txt") == "target-set");
  assert(Read(merged.exe_dir / "backups" / "wwe13-arenas" / "current.txt") == "arena-set");

  // Case 3: the player switched every import off (empty own-slots.txt): the new folder gets the empty file, so the
  // startup repair does not switch them all back on (Fable review 11-15).
  const fs::path empty_source = root / "old-empty" / "WWE2K14-Recomp-x64";
  Write(empty_source / "userdata" / "B13EBABEBABEBABE" / "545408B2" / "SaveData.Dat", "save");
  Write(empty_source / "own-slots.txt", "# Written by the launcher's Mods page.\n");
  Write(empty_source / "mods" / "wwe13" / "roster.tsv", "294\tBrie Bella\tF\n");
  const Paths emptied = MakePaths(root / "emptied");
  fs::create_directories(emptied.exe_dir);
  InstallationImportInfo info3;
  assert(ScanInstallationImport(emptied, empty_source, &info3).ok);
  assert(ImportInstallation(emptied, info3, {}, cancel).ok);
  assert(fs::exists(emptied.exe_dir / "own-slots.txt") && ReadOwnSlotIds(emptied.exe_dir / "own-slots.txt").empty());
  assert(RepairOwnSlotsFromRosters(emptied) == 0 && ReadOwnSlotIds(emptied.exe_dir / "own-slots.txt").empty());

  // Case 4 (Fable 1.6.1 item 9): the target keeps its own list AND every import record -> nothing from the source's list is
  // merged (a removed id must not come back).
  const Paths owned = MakePaths(root / "owned");
  Write(owned.exe_dir / "own-slots.txt", "# mine\n400\n");
  Write(owned.exe_dir / "backups" / "wwe13-import" / "current.txt", "target-set");
  Write(owned.exe_dir / "backups" / "wwe13-arenas" / "current.txt", "target-arenas");
  InstallationImportInfo info4;
  assert(ScanInstallationImport(owned, source, &info4).ok);
  assert(ImportInstallation(owned, info4, {}, cancel).ok);
  assert((ReadOwnSlotIds(owned.exe_dir / "own-slots.txt") == std::set<uint32_t>{400}));
  assert(Read(owned.exe_dir / "backups" / "wwe13-import" / "current.txt") == "target-set");

  // Case 5 (item 9): an EMPTY leftover backups/wwe13-import folder does not block the source's record.
  const Paths leftover = MakePaths(root / "leftover");
  fs::create_directories(leftover.exe_dir / "backups" / "wwe13-import");
  InstallationImportInfo info5;
  assert(ScanInstallationImport(leftover, source, &info5).ok);
  assert(ImportInstallation(leftover, info5, {}, cancel).ok);
  assert(Read(leftover.exe_dir / "backups" / "wwe13-import" / "sets" / "set1" / "manifest.tsv") == "manifest");
  assert((ReadOwnSlotIds(leftover.exe_dir / "own-slots.txt") == std::set<uint32_t>{162, 294}));

  // Startup repair: missing own-slots.txt is rebuilt from the roster lists; an existing one is never touched.
  const Paths repair = MakePaths(root / "repair");
  Write(repair.exe_dir / "mods" / "wwe13" / "roster.tsv", "294\tBrie Bella\tF\n162\tHunico\tM\n");
  assert(RepairOwnSlotsFromRosters(repair) == 2);
  assert((ReadOwnSlotIds(repair.exe_dir / "own-slots.txt") == std::set<uint32_t>{162, 294}));
  Write(repair.exe_dir / "own-slots.txt", "# Written by the launcher's Mods page.\n");  // player removed everyone
  assert(RepairOwnSlotsFromRosters(repair) == 0 && ReadOwnSlotIds(repair.exe_dir / "own-slots.txt").empty());
  const Paths none = MakePaths(root / "none");
  fs::create_directories(none.exe_dir);
  assert(RepairOwnSlotsFromRosters(none) == 0 && !fs::exists(none.exe_dir / "own-slots.txt"));
}

int main() {
  // WWE2K14_IMPORT_TEST_ROOT lets the Windows binary run on a real Windows machine (MAX_PATH applies there only).
  const char* root_override = std::getenv("WWE2K14_IMPORT_TEST_ROOT");
  const fs::path test_root = root_override && *root_override
                                 ? fs::path(root_override)
                                 : fs::path(WWE2K14_LAUNCHER_TESTDATA).parent_path() / "installation-import";
  std::error_code error;
  fs::remove_all(test_root, error);
  error.clear();
  fs::create_directories(test_root, error);
  assert(!error);
  // WWE2K14_IMPORT_TEST_DEEP_ONLY=1: the Windows MAX_PATH case alone (the other cases expect POSIX paths).
  const char* deep_only = std::getenv("WWE2K14_IMPORT_TEST_DEEP_ONLY");
  if (deep_only && *deep_only == '1') {
    TestDeepNestedDestination(test_root / "deep-nested-destination");
    std::cout << "PASS deep nested destination import\n";
    return 0;
  }
  TestReleaseFolderImport(test_root / "v1.0.1");
  TestNativeTestPackageLayout(test_root / "native-test-package");
  TestLinuxZipRootAndSelfRefusal(test_root / "linux-zip");
  TestSelectedFolderNormalization(test_root / "selected-folder-normalization");
  TestAutomaticInstallationFinder(test_root / "automatic-installation-finder");
  TestReleaseFolderWithEmptyLinuxPart(test_root / "release-folder-empty-linux-part");
  TestDeepNestedDestination(test_root / "deep-nested-destination");
  TestModStateCarry(test_root / "mod-state-carry");
  std::cout << "PASS installation import layouts, folder normalization, automatic finder, cache exclusion, backup, "
               "settings, source preservation, and self-refusal\n";
  return 0;
}
