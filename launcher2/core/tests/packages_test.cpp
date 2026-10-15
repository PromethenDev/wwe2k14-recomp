#include "../launcher_core.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {

constexpr uint32_t kTitleId = 0x545408B2;

struct DlcCase {
  const char* source;
  const char* package_name;
  const char* display_name;
};

constexpr std::array<DlcCase, 3> kDlcCases = {{
    {"WWE 2K14 - DLC Pack 2 (World) (Addon)/545408B2/00000002/9FAF6BD4EADAD29A4AA3517843077FC877DDFA0WWE",
     "9FAF6BD4EADAD29A4AA3517843077FC877DDFA0WWE", "WWE 2K14 Pack 2"},
    {"WWE 2K14 - DLC Pack 3 (World) (Addon)/545408B2/00000002/AE201CFDEFFB0C988B4A1DC89EDF2B4F7491FB5354",
     "AE201CFDEFFB0C988B4A1DC89EDF2B4F7491FB5354", "WWE 2K14 Pack 3"},
    {"WWE 2K14 - Pack 1 (World) (DLC)/545408B2/00000002/B64239E5B81FC5EFE8CEF7E695F77223A9D39A0F54",
     "B64239E5B81FC5EFE8CEF7E695F77223A9D39A0F54", "WWE 2K14 Pack 1"},
}};

void Check(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

fs::path PathFromUtf8(const std::string& text) {
#if __cplusplus >= 202002L
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#else
  return fs::u8path(text.begin(), text.end());
#endif
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  Check(static_cast<bool>(input), "could not read file: " + path.string());
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

std::vector<uint8_t> WithWrongTitleId(std::vector<uint8_t> executable) {
  const auto be32 = [&](size_t offset) {
    return (static_cast<uint32_t>(executable[offset]) << 24) |
           (static_cast<uint32_t>(executable[offset + 1]) << 16) |
           (static_cast<uint32_t>(executable[offset + 2]) << 8) | executable[offset + 3];
  };
  Check(executable.size() >= 0x18, "base XEX fixture is too small");
  const uint32_t header_size = be32(8);
  const uint32_t count = be32(0x14);
  Check(header_size <= executable.size() && count <= (header_size - 0x18) / 8,
        "base XEX fixture header is invalid");
  for (uint32_t index = 0; index < count; ++index) {
    const size_t offset = 0x18 + static_cast<size_t>(index) * 8;
    if (be32(offset) == 0x00040006) {
      const uint32_t execution_info = be32(offset + 4);
      Check(execution_info <= header_size && header_size - execution_info >= 0x10,
            "base XEX fixture has invalid execution info");
      executable[execution_info + 0x0C] ^= 0x01;
      return executable;
    }
  }
  throw std::runtime_error("base XEX fixture has no execution info");
}

bool SameFile(const fs::path& left, const fs::path& right) {
  std::error_code error;
  const uint64_t left_size = fs::file_size(left, error);
  if (error) {
    return false;
  }
  const uint64_t right_size = fs::file_size(right, error);
  if (error || left_size != right_size) {
    return false;
  }
  std::ifstream first(left, std::ios::binary);
  std::ifstream second(right, std::ios::binary);
  std::array<char, 65536> a{}, b{};
  while (first && second) {
    first.read(a.data(), a.size());
    second.read(b.data(), b.size());
    const std::streamsize count = first.gcount();
    if (count != second.gcount() || !std::equal(a.begin(), a.begin() + count, b.begin())) {
      return false;
    }
  }
  return first.eof() && second.eof();
}

std::set<std::string> FileSet(const fs::path& root) {
  std::set<std::string> files;
  for (fs::recursive_directory_iterator iterator(root), end; iterator != end; ++iterator) {
    if (iterator->is_regular_file()) {
      files.insert(iterator->path().lexically_relative(root).generic_string());
    }
  }
  return files;
}

void CompareTrees(const fs::path& expected, const fs::path& actual) {
  const auto expected_files = FileSet(expected);
  const auto actual_files = FileSet(actual);
  Check(expected_files == actual_files, "installed package file set differs from expected tree");
  for (const auto& relative : expected_files) {
    Check(SameFile(expected / PathFromUtf8(relative), actual / PathFromUtf8(relative)),
          "installed package file differs: " + relative);
  }
}

void PutBe32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value >> 24);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 16);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value);
}

void PutLe16(std::vector<uint8_t>* bytes, size_t offset, uint16_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void PutLe24(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 16);
}

void PutLe32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 16);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value >> 24);
}

void WriteWritableStfs(const fs::path& path) {
  constexpr size_t header_size = 0x1000;
  const std::string contents = "writable block-separated test data\n";
  std::vector<uint8_t> package(header_size + 4 * 0x1000, 0);
  std::memcpy(package.data(), "LIVE", 4);
  PutBe32(&package, 0x340, header_size);
  PutBe32(&package, 0x344, 2);
  PutBe32(&package, 0x360, kTitleId);
  PutBe32(&package, 0x3A9, 0);
  const size_t descriptor = 0x379;
  package[descriptor] = 0x24;
  package[descriptor + 2] = 0;  // writable STFS uses two hash blocks per boundary
  PutLe16(&package, descriptor + 3, 1);
  PutLe24(&package, descriptor + 5, 0);
  PutBe32(&package, descriptor + 0x1C, 2);
  const std::string display = "Writable test DLC";
  for (size_t i = 0; i < display.size(); ++i) {
    package[0x411 + i * 2] = 0;
    package[0x411 + i * 2 + 1] = static_cast<uint8_t>(display[i]);
  }

  // One entry in the file table: test.txt, backed by logical data block 1.
  uint8_t* entry = package.data() + 0x1000 + 2 * 0x1000;
  const std::string filename = "test.txt";
  std::memcpy(entry, filename.data(), filename.size());
  entry[0x28] = static_cast<uint8_t>(filename.size());
  entry[0x2C] = 1;
  entry[0x2D] = 0;
  entry[0x2E] = 0;
  entry[0x2F] = 1;
  entry[0x30] = 0;
  entry[0x31] = 0;
  entry[0x32] = 0xFF;
  entry[0x33] = 0xFF;
  PutBe32(&package, 0x1000 + 2 * 0x1000 + 0x34, static_cast<uint32_t>(contents.size()));
  // Data block 1 follows hash table block 0 and its secondary block at 0x4000.
  std::memcpy(package.data() + 0x4000, contents.data(), contents.size());
  // Hash table records for the file table block and data block both end their chains.
  PutBe32(&package, 0x1000 + 0x14, 0x00FFFFFF);
  PutBe32(&package, 0x1000 + 0x18 + 0x14, 0x00FFFFFF);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(package.data()), package.size());
  Check(static_cast<bool>(output), "could not write writable STFS test package");
}

}  // namespace

int main(int argc, char** argv) {
  try {
#if defined(WWE2K14_LAUNCHER_TESTDATA)
    const fs::path default_testdata = WWE2K14_LAUNCHER_TESTDATA;
#else
    const fs::path default_testdata = "out/l1/testdata";
#endif
    Check(argc == 1 || argc == 3 ||
              (argc == 4 && (std::string(argv[3]) == "--cleanup" ||
                             std::string(argv[3]) == "--keep-output")),
          "usage: packages_test [TESTDATA_DIR OUTPUT_DIR [--keep-output|--cleanup]]");
    const fs::path testdata = fs::absolute(argc == 1 ? default_testdata : fs::path(argv[1]));
    const fs::path output_root = fs::absolute(argc == 1 ? testdata.parent_path() / "formats"
                                                        : fs::path(argv[2]));
    const fs::path output = output_root / "packages-test";
    std::error_code ignored;
    if (argc == 4 && std::string(argv[3]) == "--cleanup") {
      fs::remove_all(output, ignored);
      std::cout << "packages_test: cleaned " << output << '\n';
      return 0;
    }
    const bool keep_output = argc == 4 && std::string(argv[3]) == "--keep-output";
    fs::remove_all(output, ignored);
    fs::create_directories(output);

    const fs::path package_root = testdata / "packages";
    const fs::path expected_root = testdata / "expected-dlc-content" / "545408B2";
    const fs::path tu_package = package_root / "TU_1A5225K_0000008000000" /
                                "TU_1A5225K_0000008000000.0000000000082";
    const auto tu_info = InspectPackage(tu_package);
    Check(tu_info && tu_info->kind == PackageKind::kTitleUpdate && tu_info->title_id == kTitleId &&
              tu_info->display_name == "WWE 2K14 Title Update",
          "title update inspection returned incorrect metadata");

    const fs::path game_folder = output / "title-update-case" / "game";
    fs::create_directories(game_folder);
    fs::copy_file(testdata / "tu-folder-min" / "default.xex", game_folder / "default.xex");
    const fs::path stale_tu_stage = game_folder / ".wwe2k14-title-update.partial";
    fs::create_directories(stale_tu_stage / "stale-data");
    {
      std::ofstream stale(stale_tu_stage / "stale-data" / "marker.txt");
      stale << "interrupted title update";
    }
    Paths paths;
    paths.userdata_dir = output / "title-update-case" / "userdata";
    uint64_t tu_done = 0;
    uint64_t tu_total = 0;
    CancelFlag cancel{false};
    const Result tu_result = ImportPackage(
        tu_package, paths, game_folder,
        [&](uint64_t done, uint64_t total, const std::string&) {
          tu_done = done;
          tu_total = total;
        },
        cancel);
    Check(tu_result.ok, "title update import failed: " + tu_result.error);
    Check(!fs::exists(stale_tu_stage), "stale title-update stage was not replaced");
    Check(SameFile(testdata / "tu-folder-min" / "default.xexp", game_folder / "default.xexp"),
          "imported default.xexp differs from the ready-game fixture");
    Check(SameFile(testdata / "tu-folder-min" / "plist360.def", game_folder / "plist360.def"),
          "imported TU definition differs from the ready-game fixture");
    Check(SameFile(testdata / "tu-folder-min" / "plist360_4x3.def",
                   game_folder / "plist360_4x3.def"),
          "imported 4:3 TU definition differs from the ready-game fixture");
    Check(tu_done == tu_total && tu_total > 0, "title update progress did not reach its byte total");
    Check(fs::exists(game_folder / "pac" / "DLC.pac"), "title update data files were not imported");

    for (size_t index = 0; index < kDlcCases.size(); ++index) {
      const auto& test_case = kDlcCases[index];
      const fs::path package = package_root / test_case.source;
      const auto info = InspectPackage(package);
      Check(info && info->kind == PackageKind::kDlc && info->title_id == kTitleId &&
                info->display_name == test_case.display_name,
            std::string("DLC inspection failed for ") + test_case.package_name);
      Paths case_paths;
      case_paths.userdata_dir = output / ("dlc-" + std::to_string(index)) / "userdata";
      uint64_t last_done = 0;
      uint64_t last_total = 0;
      CancelFlag no_cancel{false};
      const Result result = ImportPackage(
          package, case_paths, game_folder,
          [&](uint64_t done, uint64_t total, const std::string&) {
            last_done = done;
            last_total = total;
          },
          no_cancel);
      Check(result.ok, std::string("DLC import failed for ") + test_case.package_name + ": " + result.error);
      Check(last_done == last_total && last_total > 0,
            std::string("DLC progress did not finish for ") + test_case.package_name);
      const fs::path actual_root = case_paths.userdata_dir / "0000000000000000" / "545408B2";
      const fs::path actual_package = actual_root / "00000002" / test_case.package_name;
      const fs::path expected_package = expected_root / "00000002" / test_case.package_name;
      CompareTrees(expected_package, actual_package);
      Check(SameFile(expected_root / "Headers" / "00000002" /
                         (std::string(test_case.package_name) + ".header"),
                     actual_root / "Headers" / "00000002" /
                         (std::string(test_case.package_name) + ".header")),
            std::string("DLC header differs for ") + test_case.package_name);
    }

    const fs::path writable_package = output / "WritableTestDLC";
    WriteWritableStfs(writable_package);
    const auto writable_info = InspectPackage(writable_package);
    Check(writable_info && writable_info->kind == PackageKind::kDlc &&
              writable_info->display_name == "Writable test DLC",
          "writable two-table STFS package inspection failed");
    Paths writable_paths;
    writable_paths.userdata_dir = output / "writable-case" / "userdata";
    CancelFlag no_cancel{false};
    const Result writable_result = ImportPackage(writable_package, writable_paths, game_folder, {}, no_cancel);
    Check(writable_result.ok, "writable STFS package import failed: " + writable_result.error);
    const fs::path writable_file = writable_paths.userdata_dir / "0000000000000000" / "545408B2" /
                                   "00000002" / "WritableTestDLC" / "test.txt";
  const std::string writable_contents = "writable block-separated test data\n";
  Check(ReadFile(writable_file) ==
            std::vector<uint8_t>(writable_contents.begin(), writable_contents.end()),
          "writable STFS data block did not extract correctly");

    const fs::path extracted_folder = output / "xenia-content" / "545408B2" / "00000002" / "ExtractedPack";
    fs::create_directories(extracted_folder / "info");
    {
      std::ofstream content(extracted_folder / "info" / "catalog.dlc", std::ios::binary);
      content << "folder-content";
    }
    const auto folder_info = InspectPackage(extracted_folder);
    Paths folder_paths;
    folder_paths.userdata_dir = output / "folder-case" / "userdata";
    if (!folder_info) {
      const Result diagnostic = ImportPackage(extracted_folder, folder_paths, game_folder, {}, no_cancel);
      throw std::runtime_error("extracted Xenia content folder was not inspected (import: " +
                               diagnostic.error + ")");
    }
    Check(folder_info->kind == PackageKind::kDlc && folder_info->title_id == kTitleId,
          "extracted Xenia content folder returned the wrong metadata (kind=" +
              std::to_string(static_cast<int>(folder_info->kind)) + ", title=" +
              std::to_string(folder_info->title_id) + ")");
    const Result folder_result = ImportPackage(extracted_folder, folder_paths, game_folder, {}, no_cancel);
    Check(folder_result.ok, "extracted content folder import failed: " + folder_result.error);
    Check(SameFile(extracted_folder / "info" / "catalog.dlc",
                   folder_paths.userdata_dir / "0000000000000000" / "545408B2" / "00000002" /
                       "ExtractedPack" / "info" / "catalog.dlc"),
          "extracted content folder file did not copy");

    const fs::path xenia_pack2 = expected_root / "00000002" / kDlcCases[0].package_name;
    const fs::path xenia_header = expected_root / "Headers" / "00000002" /
                                  (std::string(kDlcCases[0].package_name) + ".header");
    const auto xenia_info = InspectPackage(xenia_pack2);
    Check(xenia_info && xenia_info->kind == PackageKind::kDlc &&
              xenia_info->title_id == kTitleId,
          "Xenia-layout extracted DLC folder was not inspected");
    Paths xenia_paths;
    xenia_paths.userdata_dir = output / "xenia-header-case" / "userdata";
    const Result xenia_result = ImportPackage(xenia_pack2, xenia_paths, game_folder, {}, no_cancel);
    Check(xenia_result.ok, "Xenia-layout extracted DLC folder import failed: " + xenia_result.error);
    const fs::path imported_xenia_header =
        xenia_paths.userdata_dir / "0000000000000000" / "545408B2" / "Headers" / "00000002" /
        (std::string(kDlcCases[0].package_name) + ".header");
    Check(SameFile(xenia_header, imported_xenia_header),
          "Xenia-layout extracted DLC header was not copied byte-for-byte");

    const fs::path folder_tu = output / "xenia-tu" / "545408B2" / "000B0000" / "ExtractedTU";
    fs::create_directories(folder_tu);
    fs::copy_file(testdata / "tu-folder-min" / "default.xexp", folder_tu / "default.xexp");
    const auto folder_tu_info = InspectPackage(folder_tu);
    Check(folder_tu_info && folder_tu_info->kind == PackageKind::kTitleUpdate &&
              folder_tu_info->title_id == kTitleId,
          "extracted title update folder was not inspected");
    const fs::path folder_tu_game = output / "folder-tu-case" / "game";
    fs::create_directories(folder_tu_game);
    fs::copy_file(testdata / "tu-folder-min" / "default.xex", folder_tu_game / "default.xex");
    const Result folder_tu_result = ImportPackage(folder_tu, paths, folder_tu_game, {}, no_cancel);
    Check(folder_tu_result.ok && SameFile(folder_tu / "default.xexp", folder_tu_game / "default.xexp"),
          "extracted title update folder did not import");

    const fs::path cancelled_root = output / "cancelled-case";
    Paths cancelled_paths;
    cancelled_paths.userdata_dir = cancelled_root / "userdata";
    CancelFlag cancel_import{false};
    const Result cancelled = ImportPackage(
        package_root / kDlcCases[0].source, cancelled_paths, game_folder,
        [&](uint64_t done, uint64_t, const std::string&) {
          if (done) {
            cancel_import.store(true);
          }
        },
        cancel_import);
    Check(!cancelled.ok && cancelled.error.find("cancelled") != std::string::npos,
          "package cancellation was not reported");
    Check(!fs::exists(cancelled_root / "userdata" / "0000000000000000" / "545408B2" /
                      "00000002" / kDlcCases[0].package_name) &&
              !fs::exists(cancelled_root / "userdata" / "0000000000000000" / "545408B2" /
                          "Headers" / "00000002" /
                          (std::string(kDlcCases[0].package_name) + ".header")),
          "cancelled package import left installed data");

    const fs::path truncated = output / "truncated.package";
    {
      const auto bytes = ReadFile(package_root / kDlcCases[0].source);
      std::ofstream short_file(truncated, std::ios::binary | std::ios::trunc);
      short_file.write(reinterpret_cast<const char*>(bytes.data()), 128);
    }
    Check(!InspectPackage(truncated), "truncated package was accepted");
    Paths truncated_paths;
    truncated_paths.userdata_dir = output / "truncated-case" / "userdata";
    const Result truncated_result = ImportPackage(truncated, truncated_paths, game_folder, {}, no_cancel);
    Check(!truncated_result.ok, "truncated package import unexpectedly succeeded");

    const fs::path missing_base = output / "missing-base";
    fs::create_directories(missing_base);
    const Result missing_base_result = ImportPackage(tu_package, paths, missing_base, {}, no_cancel);
    Check(!missing_base_result.ok && !fs::exists(missing_base / "default.xexp"),
          "title update installed without a matching base game executable");

    const fs::path wrong_base = output / "wrong-base";
    fs::create_directories(wrong_base);
    const auto wrong_executable = WithWrongTitleId(ReadFile(testdata / "tu-folder-min" / "default.xex"));
    {
      std::ofstream executable(wrong_base / "default.xex", std::ios::binary | std::ios::trunc);
      executable.write(reinterpret_cast<const char*>(wrong_executable.data()), wrong_executable.size());
    }
    const Result wrong_base_result = ImportPackage(tu_package, paths, wrong_base, {}, no_cancel);
    Check(!wrong_base_result.ok && !fs::exists(wrong_base / "default.xexp"),
          "title update installed into a different game folder");

    if (keep_output) {
      std::cout << "packages_test: outputs retained under " << output << '\n';
    } else {
      fs::remove_all(output, ignored);
    }
    std::cout << "packages_test: PASS (TU; three DLC trees and headers; read-only and writable STFS; extracted folders; cancellation; truncated package)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "packages_test: FAIL: " << error.what() << '\n';
    return 1;
  }
}
