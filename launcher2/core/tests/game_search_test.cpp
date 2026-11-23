// The PC search's walk (game_search.h): names only, each folder once, links never followed, cancel honoured.
#include "../game_search.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void Touch(const fs::path& path, size_t size = 16) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << std::string(size, 'x');
}

}  // namespace

int main() {
  try {
    const fs::path root = fs::temp_directory_path() /
        ("game-search-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{root};
    Touch(root / "a" / "b" / "WWE 2K15.iso");
    Touch(root / "a" / "game" / "default.xex");
    fs::create_directories(root / "a" / "game" / "pac" / "ch");
    Touch(root / "a" / "game" / "pac" / "ch" / "inside.pac");  // inside pac: never walked
    Touch(root / "Windows" / "hidden.iso");                    // system folder: never walked
    std::error_code link_error;
    fs::create_directory_symlink(root, root / "a" / "loop", link_error);  // a link back up: never followed

    std::set<std::string> folders, files;
    CancelFlag cancel{false};
    // Overlapping roots: root and root/a. Every folder must be listed once.
    const GameSearchStats stats = WalkGameFiles({root, root / "a"}, [&](const fs::path& folder, const auto& entries) {
      Check(folders.insert(folder.lexically_normal().generic_string()).second, "folder listed twice: " + folder.string());
      for (const GameSearchEntry& entry : entries)
        if (!entry.directory) files.insert(entry.lower_name);
      return false;
    }, false, {}, &cancel);
    Check(!stats.cancelled && !stats.limit_reached && !stats.stopped_early, "the walk did not run to the end");
    Check(files.contains("wwe 2k15.iso") && files.contains("default.xex"), "the walk missed candidate names");
    Check(!files.contains("inside.pac") && !files.contains("hidden.iso"), "the walk went inside pac or a system folder");
    for (const std::string& folder : folders)
      Check(folder.find("/loop") == std::string::npos, "the walk followed a link: " + folder);
    Check(stats.folders == folders.size(), "the folder count is wrong");

    // Stopping early and cancelling.
    const GameSearchStats early = WalkGameFiles({root}, [](const fs::path&, const auto&) { return true; }, false, {}, &cancel);
    Check(early.stopped_early && early.folders == 1, "the visitor could not stop the walk");
    cancel.store(true);
    const GameSearchStats cancelled = WalkGameFiles({root}, [](const fs::path&, const auto&) { return false; }, false, {}, &cancel);
    Check(cancelled.cancelled && cancelled.folders == 0, "a cancelled walk kept going");

    // Disc image candidates: by name and (on a real PC search) size; the test roots variable lifts the size floor.
    GameSearchEntry small{root / "a" / "b" / "WWE 2K15.iso", "wwe 2k15.iso", false, 16};
    GameSearchEntry other{root / "x.zip", "x.zip", false, 9000000000ull};
#ifdef _WIN32
    _wputenv_s(L"WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", L"");
#else
    unsetenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS");
#endif
    Check(!IsGameDiscImageCandidate(small), "a 16-byte .iso was opened by the PC search");
    Check(!IsGameDiscImageCandidate(other), "a .zip counted as a disc image");
#ifdef _WIN32
    _wputenv_s(L"WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", root.wstring().c_str());
#else
    setenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", root.string().c_str(), 1);
#endif
    Check(IsGameDiscImageCandidate(small), "the test size floor lift did not apply");
    Paths paths;
    paths.exe_dir = root / "launcher";
    const auto roots = GameSearchRoots(paths);
    Check(roots.size() == 1 && roots[0] == root.lexically_normal(), "test roots did not replace the search roots");
#ifdef _WIN32
    _wputenv_s(L"WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS", L"");
#else
    unsetenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS");
#endif
    std::cout << "game_search_test: PASS (names only, each folder once, links/pac/system folders skipped, stop, cancel, disc candidates)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "game_search_test: FAIL: " << error.what() << '\n';
    return 1;
  }
}
