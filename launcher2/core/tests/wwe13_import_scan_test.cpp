#include "../launcher_core.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void PutBe32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value >> 24);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 16);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value);
}

void WriteXex(const fs::path& path, uint32_t title_id) {
  std::vector<uint8_t> bytes(0x40, 0);
  PutBe32(&bytes, 0, 0x58455832);       // XEX2
  PutBe32(&bytes, 8, 0x40);             // header size
  PutBe32(&bytes, 0x14, 1);             // one optional header
  PutBe32(&bytes, 0x18, 0x00040006);    // execution info
  PutBe32(&bytes, 0x1C, 0x30);          // execution info offset
  PutBe32(&bytes, 0x3C, title_id);      // title ID in execution info
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  Check(static_cast<bool>(output), "could not write synthetic XEX");
}

void WritePac(const fs::path& path, const std::array<uint8_t, 4>& magic = {'E', 'P', 'K', '8'}) {
  fs::create_directories(path.parent_path());
  std::vector<uint8_t> bytes(0x4000, 0);
  std::copy(magic.begin(), magic.end(), bytes.begin());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  Check(static_cast<bool>(output), "could not write synthetic PAC");
}

void InstallRecipes(const fs::path& launcher_dir) {
#if defined(WWE2K14_WWE13_RECIPE)
  fs::create_directories(launcher_dir / "data");
  std::error_code error;
  fs::copy_file(WWE2K14_WWE13_RECIPE, launcher_dir / "data" / "wwe13-ports.tsv",
                fs::copy_options::overwrite_existing, error);
  Check(!error, "could not copy runtime import recipes into the scanner fixture");
#else
  (void)launcher_dir;
#endif
}

const WWE13ImportWrestler* Find(const WWE13ImportScan& scan, uint32_t id) {
  for (const WWE13ImportWrestler& wrestler : scan.wrestlers)
    if (wrestler.source_id == id) return &wrestler;
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  try {
#if defined(WWE2K14_LAUNCHER_TESTDATA)
    // Its own subfolder: the test deletes its root at the end, and the shared testdata folder holds other tests'
    // fixtures (tu-folder-min, packages, ...) that core_test / disc_image_test / packages_test need (11-15).
    const fs::path default_root = fs::path(WWE2K14_LAUNCHER_TESTDATA) / "wwe13-import-scan";
#else
    const fs::path default_root = "out/l3/wwe13-import-scan";
#endif
    const fs::path root = fs::absolute(argc > 1 ? fs::path(argv[1]) : default_root);
    if (argc == 3 && std::string(argv[1]) == "--scan-read-only") {
      const fs::path selected = fs::absolute(argv[2]);
      Paths paths;
      paths.exe_dir = WWE2K14_LAUNCHER_TEST_BINARY_DIR;
      CancelFlag cancel{false};
      const WWE13ImportScan scan = ScanWWE13ImportSources(paths, selected, {}, cancel);
      const size_t available_count = static_cast<size_t>(std::count_if(
          scan.wrestlers.begin(), scan.wrestlers.end(),
          [](const WWE13ImportWrestler& wrestler) { return wrestler.available; }));
      std::cout << "base_found=" << scan.base_found << " packs=" << scan.packs.size()
                << " available_wrestlers=" << available_count << " directories=" << scan.directories_visited
                << " limit_reached=" << scan.search_limit_reached << '\n';
      if (scan.base_found) std::cout << "base_folder=" << scan.base_folder.string() << '\n';
      for (const WWE13ImportPack& pack : scan.packs)
        std::cout << "pack=" << pack.pack_number << " display=" << pack.display_name
                  << " wrestlers=" << pack.wrestler_ids.size() << " file=" << pack.package_file << '\n';
      return (scan.base_found || !scan.packs.empty()) ? 0 : 2;
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
    const fs::path base = root / "InstalledCopy-7";
    WriteXex(base / "default.xex", 0x545108B4);
    const std::array<uint8_t, 4> xcompress_magic{0x0F, 0xF5, 0x12, 0xED};
    const std::array<uint8_t, 4> epac_magic{'E', 'P', 'A', 'C'};
    WritePac(base / "pac" / "ch" / "chEtc.pac", xcompress_magic);
    WritePac(base / "pac" / "DLC.pac", xcompress_magic);
    WritePac(base / "pac" / "string.pac", epac_magic);
    WritePac(base / "pac" / "ch" / "ch100.pac", xcompress_magic);

    const fs::path pack = root / "Backups" / "arbitrary-package-name";
    WritePac(pack / "pac" / "ch" / "chEtc_dlc02.pac", epac_magic);
    WritePac(pack / "pac" / "ch" / "ch50_dlc02.pac");
    WritePac(pack / "pac" / "ch" / "ch51_dlc02.pac");

    Paths paths;
    paths.exe_dir = root / "launcher";
    InstallRecipes(paths.exe_dir);
    CancelFlag cancel{false};
    const WWE13ImportScan scan = ScanWWE13ImportSources(paths, root, {}, cancel);
    Check(scan.base_found, "the WWE '13 base game was not identified by XEX title ID and data files");
    Check(scan.base_folder == fs::absolute(base), "the scanner returned the wrong base game folder");
    Check(scan.packs.size() == 1 && scan.packs[0].pack_number == 2,
          "the WWE '13 DLC pack was not identified from content files");
    Check(scan.packs[0].folder == fs::absolute(pack), "the scanner returned the wrong DLC folder");
    Check(scan.wrestlers.size() == 106, "the known importable source roster was not returned");
    const WWE13ImportWrestler* base_wrestler = Find(scan, 100);
    Check(base_wrestler && base_wrestler->available && base_wrestler->selected &&
              base_wrestler->source_label == "Base game",
          "base-game wrestlers should be preselected when their model data exists");
    const WWE13ImportWrestler* dlc_wrestler = Find(scan, 50);
    Check(dlc_wrestler && dlc_wrestler->available && dlc_wrestler->selected &&
              dlc_wrestler->source_label == "DLC pack 02",
          "DLC wrestlers should be preselected and show their source pack");
    const WWE13ImportWrestler* missing_wrestler = Find(scan, 124);
    Check(missing_wrestler && !missing_wrestler->available && !missing_wrestler->selected,
          "wrestlers without source files must not be preselected");

    const fs::path wrong_game = root / "WWE13-Not-In-Name";
    WriteXex(wrong_game / "default.xex", 0x545408B2);
    WritePac(wrong_game / "pac" / "ch" / "chEtc.pac", xcompress_magic);
    WritePac(wrong_game / "pac" / "DLC.pac", xcompress_magic);
    WritePac(wrong_game / "pac" / "string.pac", epac_magic);
    const WWE13ImportScan wrong = ScanWWE13ImportSources(paths, wrong_game, {}, cancel);
    Check(!wrong.base_found, "a WWE 2K14 XEX must not be reported as WWE '13");
    Check(wrong.packs.empty(), "unidentified PAC files outside a WWE '13 root must not look like DLC");

    fs::remove_all(root, ignored);
    std::cout << "wwe13_import_scan_test: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "wwe13_import_scan_test: FAIL: " << error.what() << '\n';
    return 1;
  }
}
