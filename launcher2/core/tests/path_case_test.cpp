// ResolvePathCase (path_case.h): joins a '/'-separated relative path onto a base, matching each
// component against disk ignoring ASCII letter case; exact case wins; missing parts are appended as given.
#include "../path_case.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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
        ("path-case-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{root};
    Touch(root / "PAC" / "CH" / "chEtc_dlc01.pac");

    // a) each component is matched against disk ignoring ASCII letter case.
    Check(ResolvePathCase(root, "pac/ch/chEtc_dlc01.pac") == root / "PAC" / "CH" / "chEtc_dlc01.pac",
          "the case-insensitive join did not land on the real names");

    // b) an exact-case match wins (skipped when the temp file system is case-insensitive).
    Touch(root / "info" / "catalog.dlc");
    Touch(root / "INFO" / "catalog.dlc");
    std::error_code error;
    if (!fs::exists(root / "Info", error)) {  // "Info" matches neither: the file system is case-sensitive.
      Check(ResolvePathCase(root, "info/catalog.dlc") == root / "info" / "catalog.dlc",
            "an exact-case name lost to a case-insensitive one");
    }

    // c) a missing component: it and the rest are appended unchanged.
    Check(ResolvePathCase(root, "pac/evt/x.pac") == root / "PAC" / "evt" / "x.pac",
          "a missing component was not appended as given");

    // d) a missing base folder: everything is appended unchanged.
    Check(ResolvePathCase(root / "nope", "a/b") == root / "nope" / "a" / "b",
          "a missing base was not preserved");

    // e) empty components from "//" are skipped.
    Check(ResolvePathCase(root, "pac//ch") == root / "PAC" / "CH",
          "an empty component broke the join");

    std::cout << "path_case_test: PASS (case-insensitive join, exact case wins, missing parts appended, empty components skipped)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "path_case_test: FAIL: " << error.what() << '\n';
    return 1;
  }
}
