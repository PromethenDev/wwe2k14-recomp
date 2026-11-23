#ifdef NDEBUG
#undef NDEBUG
#endif

#include "../wwe13_install.h"

#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {
using namespace std::string_literals;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void Write(const fs::path& path, const std::string& bytes) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  Check(!error, "could not create fixture directory");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  Check(static_cast<bool>(output), "could not write fixture file");
}

std::string Read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void Stage(const fs::path& root, const std::string& token, const std::string& game_bytes,
           const std::string& userdata_bytes, const std::string& slots, const std::string& roster) {
  const fs::path files = root / token / "files";
  Write(files / "game" / "pac" / "ch" / "ch152.pac", game_bytes);
  Write(files / "userdata" / "0000000000000000" / "545408B2" / "00000002" / "045B" / "port.bin",
        userdata_bytes);
  Write(files / "launcher" / "own-slots.txt", slots);
  Write(files / "launcher" / "mods" / "wwe13" / "roster.tsv", roster);
}

}  // namespace

int main() {
  try {
#ifdef WWE2K14_LAUNCHER_TESTDATA
    fs::path root = fs::path(WWE2K14_LAUNCHER_TESTDATA) / "wwe13-install";
#else
    fs::path root = fs::temp_directory_path() / "wwe13-install-test";
#endif
    // Cross-built Windows tests run on another PC: WWE2K14_TEST_ROOT picks a local folder there.
    if (const char* override_root = std::getenv("WWE2K14_TEST_ROOT"); override_root && *override_root)
      root = fs::path(override_root) / "wwe13-install";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    const fs::path launcher = root / "launcher";
    const fs::path game = root / "game";
    const fs::path userdata = launcher / "userdata";
    const fs::path target = game / "pac" / "ch" / "ch152.pac";
    const fs::path hardlink = root / "hardlink-observer.pac";
    Paths paths;
    paths.exe_dir = launcher;
    paths.game_exe = game / "wwe2k14";
    paths.userdata_dir = userdata;
    paths.backups_dir = launcher / "backups";
    fs::create_directories(target.parent_path());
    fs::create_directories(userdata);
    const std::string original = "original game bytes\0with binary"s;
    const std::string original_slots = "# player-owned slots\n200\n\n";
    Write(target, original);
    Write(launcher / "own-slots.txt", original_slots);
    fs::create_hard_link(target, hardlink);

    const fs::path real_input = root / "real-copy-source" / "large-source.bin";
    const std::string real_bytes(2 * 1024 * 1024, '\x5a');
    Write(real_input, real_bytes);
    const fs::path stage_one = root / "stage-one" / "files";
    const fs::path staged_game = stage_one / "game" / "pac" / "ch" / "ch152.pac";
    fs::create_directories(staged_game.parent_path());
    fs::copy_file(real_input, staged_game);
    Write(stage_one / "userdata" / "0000000000000000" / "545408B2" / "00000002" / "045B" / "port.bin",
          "userdata-one");
    Write(stage_one / "launcher" / "own-slots.txt", "# player-owned slots\n152\n200\n");
    Write(stage_one / "launcher" / "mods" / "wwe13" / "roster.tsv", "#game WWE '13\n152\tShane McMahon\tM\n");

    CancelFlag cancel{false};
    WWE13ImportSummary summary;
    Result applied = ApplyWWE13StagedFiles(paths, game, stage_one, {}, cancel, &summary);
    Check(applied.ok, applied.error.c_str());
    Check(HasWWE13Import(paths), "apply did not create a current undo record");
    Check(Read(target) == real_bytes, "the staged game file was not copied into place");
    Check(Read(hardlink) == original, "applying through a hard link changed another file in place");
    Check(Read(userdata / "0000000000000000" / "545408B2" / "00000002" / "045B" / "port.bin") == "userdata-one",
          "the staged userdata file was not installed");
    Check(Read(launcher / "mods" / "wwe13" / "roster.tsv").starts_with("#game WWE '13\n"),
          "the roster metadata header was not exact");

    const fs::path stage_two = root / "stage-two" / "files";
    Write(stage_two / "game" / "pac" / "ch" / "ch152.pac", "reimported game bytes");
    Write(stage_two / "userdata" / "0000000000000000" / "545408B2" / "00000002" / "045B" / "port.bin",
          "userdata-two");
    Write(stage_two / "launcher" / "own-slots.txt", "# player-owned slots\n152\n153\n200\n");
    Write(stage_two / "launcher" / "mods" / "wwe13" / "roster.tsv",
          "#game WWE '13\n152\tShane McMahon\tM\n153\tAJ Lee\tF\n");
    applied = ApplyWWE13StagedFiles(paths, game, stage_two, {}, cancel, &summary);
    Check(applied.ok, applied.error.c_str());
    Check(Read(target) == "reimported game bytes", "re-import did not replace the previous imported bytes");
    Check(Read(hardlink) == original, "re-import changed an external hard link");

#ifdef _WIN32
    {
      // Another program (virus scanner, indexer) holds the undo record open without delete sharing, as on the
      // Discord player's PC 11-08. A short hold must be waited out; a long one must fail cleanly with a reason.
      const fs::path pointer = paths.backups_dir / "wwe13-import" / "current.txt";
      auto hold = [&](int ms) {
        HANDLE h = CreateFileW(pointer.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(h != INVALID_HANDLE_VALUE, "could not open the undo record for the lock test");
        return std::thread([h, ms] { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); CloseHandle(h); });
      };
      std::thread short_hold = hold(800);
      applied = ApplyWWE13StagedFiles(paths, game, stage_two, {}, cancel, &summary);
      short_hold.join();
      Check(applied.ok, ("a briefly held undo record was not waited out: " + applied.error).c_str());
      const std::string before = Read(pointer);
      std::thread long_hold = hold(10000);
      const Result blocked = ApplyWWE13StagedFiles(paths, game, stage_two, {}, cancel, &summary);
      long_hold.join();
      Check(!blocked.ok, "a locked undo record unexpectedly succeeded");
      Check(blocked.error.find("undo record could not be activated (") != std::string::npos,
            ("the failure gives no reason: " + blocked.error).c_str());
      Check(Read(pointer) == before, "a failed activation changed the undo record");
      Check(Read(target) == "reimported game bytes", "a failed activation left the game file changed");
      std::cout << "lock test: " << blocked.error << "\n";
    }
#endif
    const Result undone = UndoWWE13Import(paths, game, {}, cancel);
    Check(undone.ok, undone.error.c_str());
    Check(!HasWWE13Import(paths), "undo left an active importer manifest");
    Check(Read(target) == original, "undo did not restore the exact original game bytes");
    Check(Read(hardlink) == original, "undo changed the external hard link");
    Check(Read(launcher / "own-slots.txt") == original_slots, "undo did not restore own-slots.txt byte-for-byte");
    Check(!fs::exists(userdata / "0000000000000000" / "545408B2" / "00000002" / "045B" / "port.bin"),
          "undo did not remove a newly created userdata file");
    Check(!fs::exists(launcher / "mods" / "wwe13" / "roster.tsv"), "undo did not remove WWE '13 roster metadata");

    // A file the import takes away (the host pack's old name): gone while imported, exact after undo, folder gone too.
    const fs::path old_pack_file = userdata / "0000000000000000" / "545408B2" / "00000002" / "745B" / "info" / "catalog.dlc";
    Write(old_pack_file, "old pack catalog");
    const fs::path stage_rename = root / "stage-rename" / "files";
    Write(stage_rename / "userdata" / "0000000000000000" / "545408B2" / "00000002" / "045B" / "info" / "catalog.dlc", "new");
    const std::vector<std::string> removals{"userdata/0000000000000000/545408B2/00000002/745B/info/catalog.dlc"};
    applied = ApplyWWE13StagedFiles(paths, game, stage_rename, {}, cancel, &summary, removals);
    Check(applied.ok, applied.error.c_str());
    Check(!fs::exists(old_pack_file), "the removed file is still there");
    Check(!fs::exists(old_pack_file.parent_path().parent_path()), "the removed pack left an empty folder");
    const Result undone_rename = UndoWWE13Import(paths, game, {}, cancel);
    Check(undone_rename.ok, undone_rename.error.c_str());
    Check(Read(old_pack_file) == "old pack catalog", "undo did not bring the removed file back exactly");
    Check(!fs::exists(userdata / "0000000000000000" / "545408B2" / "00000002" / "045B"), "undo left the new pack's folder");

    const fs::path stage_cancel = root / "stage-cancel" / "files";
    Write(stage_cancel / "game" / "pac" / "ch" / "ch152.pac", "cancelled");
    cancel.store(true);
    const Result cancelled = ApplyWWE13StagedFiles(paths, game, stage_cancel, {}, cancel, nullptr);
    Check(!cancelled.ok, "a pre-cancelled apply unexpectedly succeeded");
    Check(Read(target) == original, "a cancelled apply changed the original game file");

    // GitHub #141 automatic: launcher copy B undoes copy A's import in the shared game folder with A's undo record.
    {
      CancelFlag go{false};
      const fs::path r2 = root / "two-launchers";
      const fs::path game2 = r2 / "game", a_dir = r2 / "launcher-a", b_dir = r2 / "launcher-b", other_dir = r2 / "unrelated";
      auto make = [](const fs::path& dir) {
        Paths p;
        p.exe_dir = dir;
        p.userdata_dir = dir / "userdata";
        p.backups_dir = dir / "backups";
        fs::create_directories(p.userdata_dir);
        return p;
      };
      const Paths pa = make(a_dir), pb = make(b_dir);
      make(other_dir);
      const fs::path ch = game2 / "pac" / "ch" / "ch152.pac", added = game2 / "pac" / "ch" / "ch400.pac";
      Write(ch, "stock ch152");
      const fs::path st = r2 / "stage" / "files";
      Write(st / "game" / "pac" / "ch" / "ch152.pac", "imported ch152");
      Write(st / "game" / "pac" / "ch" / "ch400.pac", "new wrestler");
      Write(st / "userdata" / "0000000000000000" / "545408B2" / "00000002" / "045B" / "port.bin", "a userdata");
      Write(st / "launcher" / "own-slots.txt", "# player-owned slots\n400\n");
      const Result a_applied = ApplyWWE13StagedFiles(pa, game2, st, {}, go, nullptr);
      Check(a_applied.ok, a_applied.error.c_str());
      Check(Read(ch) == "imported ch152" && fs::exists(added), "copy A did not import");
      fs::path used;
      std::string restore_error;
      // B's fresh userdata has none of A's userdata files: still usable (those are already 'original' here).
      Check(RestoreWWE13ImportFromOtherLauncher(pb, game2, {other_dir, b_dir, a_dir}, {}, go, &used, &restore_error),
            restore_error.c_str());
      Check(used == a_dir, "copy B did not pick copy A's undo record");
      Check(Read(ch) == "stock ch152", "copy B did not put the original game file back");
      Check(!fs::exists(added), "copy B left a file the import had added");
      Check(Read(a_dir / "own-slots.txt") == "# player-owned slots\n400\n", "copy A's own launcher files were touched");
      // A game folder changed after that import (fingerprints differ) is never touched.
      const Result again = ApplyWWE13StagedFiles(pa, game2, st, {}, go, nullptr);
      Check(again.ok, again.error.c_str());
      Write(ch, "changed by something else");
      Check(RestoreWWE13ImportFromOtherLauncher(pb, game2, {a_dir}, {}, go, &used, &restore_error), restore_error.c_str());
      Check(used.empty(), "a non-matching undo record was used");
      Check(Read(ch) == "changed by something else" && fs::exists(added), "a non-matching folder was changed");
      std::cout << "wwe13_install_test: #141 restore from another launcher copy OK\n";
      // GitHub #160: that changed folder gets its ORIGINAL files back from A's backup (verified), A's record untouched.
      Check(RestoreWWE13OriginalsFromOtherBackup(pb, game2, {other_dir, b_dir, a_dir}, {}, go, &used, &restore_error),
            restore_error.c_str());
      Check(used == a_dir, "copy B did not use copy A's backup");
      Check(Read(ch) == "stock ch152", "the original file was not put back from the backup");
      Check(!fs::exists(added), "a file the import had added is still there");
      // A damaged backup is never used: corrupt A's saved original, nothing may change.
      Write(ch, "changed again");
      for (const auto& e : fs::recursive_directory_iterator(a_dir / "backups" / "wwe13-import" / "sets"))
        if (e.is_regular_file() && e.path().parent_path().filename() == "original" && Read(e.path()) == "stock ch152")
          Write(e.path(), "damaged backup");
      Check(RestoreWWE13OriginalsFromOtherBackup(pb, game2, {a_dir}, {}, go, &used, &restore_error), restore_error.c_str());
      Check(used.empty(), "a damaged backup was used");
      Check(Read(ch) == "changed again", "a folder was changed from a damaged backup");
      std::cout << "wwe13_install_test: #160 originals from another copy's backup OK\n";
    }

    // GitHub #141: a game folder already changed by an earlier import is found before anything is built.
    {
      std::vector<uint32_t> present;
      std::string registry_error;
      Check(!WWE13TargetsInDlcRegistry(std::vector<uint8_t>{1, 2, 3}, {120}, &present, &registry_error),
            "a broken DLC.pac was read as a registry");
      if (const char* stock = std::getenv("WWE2K14_STOCK_DLC_PAC"); stock && *stock) {
        std::ifstream input(stock, std::ios::binary);
        const std::vector<uint8_t> pac{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        Check(WWE13TargetsInDlcRegistry(pac, {120, 121, 124}, &present, &registry_error), registry_error.c_str());
        Check(present.empty(), "the stock DLC.pac already lists a WWE '13 target");
        std::vector<uint8_t> imported;
        Check(ConvertWWE13DlcRegistry(pac, {{120, 1, 1}, {124, 2, 2}}, "DLC.pac", &imported, &registry_error),
              registry_error.c_str());
        Check(WWE13TargetsInDlcRegistry(imported, {120, 121, 124}, &present, &registry_error), registry_error.c_str());
        Check(present == std::vector<uint32_t>{120, 124}, "the imported DLC.pac did not report 120 and 124");
        std::cout << "wwe13_install_test: #141 leftover-import check on the stock DLC.pac OK\n";
      }
    }

    fs::remove_all(root, ignored);
    std::cout << "wwe13_install_test: PASS (copy, re-import, hard-link safety, exact undo, cancel)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "wwe13_install_test: FAIL: " << error.what() << '\n';
    return 1;
  }
}
