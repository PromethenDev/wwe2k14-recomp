#ifdef NDEBUG
#undef NDEBUG
#endif

#include "../launcher_core.h"

#include "../internal/archive.h"
#include "../internal/creation_save.h"
#include "../internal/stfs.h"
#include "../internal/util.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {

constexpr std::string_view kAccount = "B13EBABEBABEBABE";
constexpr std::string_view kTitle = "545408B2";
constexpr size_t kMainContainerOffset = 0x2CA000;
constexpr size_t kMainPlainSize = 1'790'648;
constexpr size_t kCawPlainSize = 1'882'028;
constexpr size_t kArenaPlainSize = 626'764;
constexpr size_t kCawPayloadHeader = 0x1CA40F;
constexpr size_t kIndexedRegionCount = 4;
constexpr size_t kIndexedDataBase = 20;
constexpr size_t kIndexedDataStride = 0x2E47C;
constexpr size_t kAuxIndexedDataBase = 0xB9204;
constexpr size_t kAuxIndexedDataStride = 0x44484;
constexpr size_t kLargeSectionPlainSize = 5'068'336;
constexpr size_t kAdditionalSectionCount = 12;

Paths MakePaths(const fs::path& root) {
  Paths paths;
  paths.exe_dir = root;
  paths.userdata_dir = root / "userdata";
  paths.settings_file = root / "wwe2k14-enhanced.ini";
  paths.backups_dir = root / "backups";
  return paths;
}

void WriteLe32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 16);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value >> 24);
}

void WriteBe32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value >> 24);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 16);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value);
}

void WriteFixedName(std::vector<uint8_t>* bytes, size_t offset, size_t size, std::string_view name) {
  std::fill(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
            bytes->begin() + static_cast<std::ptrdiff_t>(offset + size), 0);
  std::copy(name.begin(), name.end(), bytes->begin() + static_cast<std::ptrdiff_t>(offset));
}

std::vector<uint8_t> PackContainer(const std::vector<uint8_t>& plain, size_t prefix_size) {
  const mz_ulong capacity = mz_compressBound(static_cast<mz_ulong>(plain.size()));
  std::vector<uint8_t> compressed(static_cast<size_t>(capacity));
  mz_ulong compressed_size = capacity;
  assert(mz_compress2(compressed.data(), &compressed_size, plain.data(),
                      static_cast<mz_ulong>(plain.size()), MZ_DEFAULT_COMPRESSION) == MZ_OK);
  compressed.resize(static_cast<size_t>(compressed_size));

  std::vector<uint8_t> file(prefix_size, 0);
  file.insert(file.end(), {'Z', 'L', 'I', 'B'});
  const size_t header = file.size();
  file.resize(header + 12);
  WriteLe32(&file, header, 0x3012);
  WriteLe32(&file, header + 4, static_cast<uint32_t>(compressed.size()));
  WriteLe32(&file, header + 8, static_cast<uint32_t>(plain.size()));
  file.insert(file.end(), compressed.begin(), compressed.end());
  return file;
}

std::vector<uint8_t> MakeSyntheticCawPayload() {
  std::vector<uint8_t> plain(kCawPlainSize, 0);
  WriteBe32(&plain, 0, 55);
  WriteBe32(&plain, 4, 42);
  WriteBe32(&plain, 8, 23);
  WriteFixedName(&plain, kCawPayloadHeader + 0xD1, 0x2A, "F2 ATTIRE TEST");
  WriteFixedName(&plain, kCawPayloadHeader + 0x115, 0x2A, "F2 NICKNAME");
  WriteFixedName(&plain, kCawPayloadHeader + 0x13D, 0x2A, "F2 ATTIRE TEST");
  WriteBe32(&plain, 0x0C, 0x78);
  WriteBe32(&plain, 0x1CB068, 0x78);
  WriteBe32(&plain, 0x1CB7A8, 0x78);

  // Four indexed data regions follow the repeated layout covered by the verified CAS checksum.
  // Distinct markers make any dropped or reordered region visible in this synthetic round trip.
  for (size_t region = 0; region < kIndexedRegionCount; ++region) {
    plain[kIndexedDataBase + region * kIndexedDataStride + 0x100] = static_cast<uint8_t>(0xA0 + region);
    plain[kAuxIndexedDataBase + region * kAuxIndexedDataStride + 0x100] = static_cast<uint8_t>(0xB0 + region);
  }

  std::vector<uint8_t> payload = PackContainer(plain, 0);
  // Slot 62 in the copied userdata fixture has twelve additional sequential ZLIB sections and a 16-byte tail.
  // Model their size/order without including any game-derived bytes in the test source.
  for (size_t section = 0; section < kAdditionalSectionCount; ++section) {
    std::vector<uint8_t> section_plain(kLargeSectionPlainSize, 0);
    section_plain[0] = 'F';
    section_plain[1] = '2';
    section_plain[2] = 'S';
    section_plain[3] = 'E';
    section_plain[4] = static_cast<uint8_t>(section);
    section_plain[0x100] = static_cast<uint8_t>(0xC0 + section);
    std::vector<uint8_t> packed = PackContainer(section_plain, 0);
    payload.insert(payload.end(), packed.begin(), packed.end());
  }
  payload.insert(payload.end(), 16, 0);
  return payload;
}

std::vector<uint8_t> MakeEmptyMain() {
  return PackContainer(std::vector<uint8_t>(kMainPlainSize, 0), kMainContainerOffset);
}

std::vector<uint8_t> MakeSyntheticArena() {
  std::vector<uint8_t> plain(kArenaPlainSize, 0);
  plain[0x07] = 0x81;
  WriteBe32(&plain, 0x3C, 0x81);
  WriteFixedName(&plain, 0x48, 0x16, "F2 TEST ARENA");
  return PackContainer(plain, 0);
}

std::vector<uint8_t> MakeHeader() {
  std::vector<uint8_t> header(0x148, 0);
  WriteBe32(&header, 0x04, 1);
  WriteBe32(&header, 0x140, 0x545408B2);
  return header;
}

void WriteTestFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  assert(internal::EnsureDirectory(path.parent_path()));
  assert(internal::WriteBinaryFile(path, bytes));
}

void CheckOrderedPayload(const std::vector<uint8_t>& payload) {
  size_t offset = 0;
  for (size_t frame = 0; frame < 1 + kAdditionalSectionCount; ++frame) {
    assert(offset + 16 <= payload.size());
    assert(std::equal(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                      payload.begin() + static_cast<std::ptrdiff_t>(offset + 4), "ZLIB"));
    const uint32_t version = static_cast<uint32_t>(payload[offset + 4]) |
        (static_cast<uint32_t>(payload[offset + 5]) << 8) |
        (static_cast<uint32_t>(payload[offset + 6]) << 16) |
        (static_cast<uint32_t>(payload[offset + 7]) << 24);
    const uint32_t compressed_size = static_cast<uint32_t>(payload[offset + 8]) |
        (static_cast<uint32_t>(payload[offset + 9]) << 8) |
        (static_cast<uint32_t>(payload[offset + 10]) << 16) |
        (static_cast<uint32_t>(payload[offset + 11]) << 24);
    const uint32_t plain_size = static_cast<uint32_t>(payload[offset + 12]) |
        (static_cast<uint32_t>(payload[offset + 13]) << 8) |
        (static_cast<uint32_t>(payload[offset + 14]) << 16) |
        (static_cast<uint32_t>(payload[offset + 15]) << 24);
    assert(version == 0x3012);
    assert(plain_size == (frame == 0 ? kCawPlainSize : kLargeSectionPlainSize));
    assert(offset + 16ull + compressed_size <= payload.size());

    std::vector<uint8_t> plain(plain_size);
    mz_ulong actual_size = static_cast<mz_ulong>(plain.size());
    assert(mz_uncompress(plain.data(), &actual_size, payload.data() + offset + 16, compressed_size) == MZ_OK);
    assert(actual_size == plain_size);
    if (frame == 0) {
      for (size_t region = 0; region < kIndexedRegionCount; ++region) {
        assert(plain[kIndexedDataBase + region * kIndexedDataStride + 0x100] == 0xA0 + region);
        assert(plain[kAuxIndexedDataBase + region * kAuxIndexedDataStride + 0x100] == 0xB0 + region);
      }
    } else {
      const size_t section = frame - 1;
      assert(plain[0] == 'F' && plain[1] == '2' && plain[2] == 'S' && plain[3] == 'E');
      assert(plain[4] == section && plain[0x100] == 0xC0 + section);
    }
    offset += 16 + compressed_size;
  }
  assert(payload.size() - offset == 16);
  assert(std::all_of(payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.end(),
                     [](uint8_t byte) { return byte == 0; }));
}

}  // namespace

int main() {
  const fs::path root = fs::temp_directory_path() /
      ("wwe2k14-creation-export-attires-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  const Paths source_paths = MakePaths(root / "source");
  const Paths destination_paths = MakePaths(root / "destination");
  const std::string caw_folder = "00CustomSuperStar.cas";
  const fs::path source_main_path = source_paths.userdata_dir / std::string(kAccount) /
      std::string(kTitle) / "00000001" / "SaveData.dat" / "SaveData.Dat";
  const fs::path source_header_path = source_paths.userdata_dir / std::string(kAccount) /
      std::string(kTitle) / "Headers" / "00000001" / "SaveData.dat.header";
  const fs::path source_payload_path = source_paths.userdata_dir / std::string(kAccount) /
      std::string(kTitle) / "00000001" / caw_folder / "SaveData.Dat";

  const std::vector<uint8_t> payload = MakeSyntheticCawPayload();
  const auto source_main_with_caw = internal::AddKnownCreation(MakeEmptyMain(), payload, true, 0);
  assert(source_main_with_caw);
  const auto source_main = internal::AddKnownCreation(*source_main_with_caw,
                                                       MakeSyntheticArena(), false, 0);
  assert(source_main);
  WriteTestFile(source_main_path, *source_main);
  WriteTestFile(source_header_path, MakeHeader());
  WriteTestFile(source_payload_path, payload);

  std::vector<uint8_t> source_main_check;
  assert(internal::ReadBinaryFile(source_main_path, source_main_check));
  std::vector<uint8_t> source_plain(kMainPlainSize);
  const uint32_t main_compressed_size = static_cast<uint32_t>(source_main_check[kMainContainerOffset + 8]) |
      (static_cast<uint32_t>(source_main_check[kMainContainerOffset + 9]) << 8) |
      (static_cast<uint32_t>(source_main_check[kMainContainerOffset + 10]) << 16) |
      (static_cast<uint32_t>(source_main_check[kMainContainerOffset + 11]) << 24);
  mz_ulong main_actual = static_cast<mz_ulong>(source_plain.size());
  assert(mz_uncompress(source_plain.data(), &main_actual,
                       source_main_check.data() + kMainContainerOffset + 16,
                       main_compressed_size) == MZ_OK);
  assert(main_actual == kMainPlainSize);
  assert(internal::IsSupportedCreationPlain(source_plain));

  const fs::path export_path = root / "exports" / "f2-attire-test.cas";
  assert(internal::EnsureDirectory(export_path.parent_path()));
  CreationExportSummary export_summary;
  const Result exported = ExportCreation(source_paths, CreationSaveKind::kSuperstar, 0,
                                         export_path, &export_summary);
  if (!exported.ok) std::cerr << "creation_export_attires_test: export failed: " << exported.error << '\n';
  if (exported.ok && export_summary.name != "F2 ATTIRE TEST") {
    std::cerr << "creation_export_attires_test: unexpected exported name: " << export_summary.name << '\n';
  }
  assert(exported.ok && export_summary.name == "F2 ATTIRE TEST");
  std::vector<uint8_t> exported_payload, package_header;
  std::string package_error;
  assert(internal::ExtractCreationConPackage(export_path, true, &exported_payload,
                                             &package_header, &package_error));
  assert(exported_payload == payload);
  CheckOrderedPayload(exported_payload);

  const fs::path destination_main_path = destination_paths.userdata_dir / std::string(kAccount) /
      std::string(kTitle) / "00000001" / "SaveData.dat" / "SaveData.Dat";
  const fs::path destination_header_path = destination_paths.userdata_dir / std::string(kAccount) /
      std::string(kTitle) / "Headers" / "00000001" / "SaveData.dat.header";
  const auto destination_main = internal::AddKnownCreation(MakeEmptyMain(),
                                                           MakeSyntheticArena(), false, 0);
  assert(destination_main);
  WriteTestFile(destination_main_path, *destination_main);
  WriteTestFile(destination_header_path, MakeHeader());

  uint32_t imported_slot = (std::numeric_limits<uint32_t>::max)();
  const Result imported = ImportCreationSave(destination_paths, CreationSaveKind::kSuperstar,
                                              export_path, &imported_slot);
  assert(imported.ok && imported_slot == 0);
  const fs::path imported_payload_path = destination_paths.userdata_dir / std::string(kAccount) /
      std::string(kTitle) / "00000001" / caw_folder / "SaveData.Dat";
  std::vector<uint8_t> imported_payload;
  assert(internal::ReadBinaryFile(imported_payload_path, imported_payload));
  assert(imported_payload == payload);
  CheckOrderedPayload(imported_payload);

  std::vector<uint8_t> imported_main;
  assert(internal::ReadBinaryFile(destination_main_path, imported_main));
  assert(internal::CreationFileMatchesMainRecord(imported_main, imported_payload, true, imported_slot));
  const auto records = ListCreationSaveRecords(destination_paths);
  assert(records);
  const auto imported_record = std::find_if(records->begin(), records->end(), [](const CreationSaveRecord& record) {
    return record.kind == CreationSaveKind::kSuperstar;
  });
  assert(imported_record != records->end() && imported_record->slot_index == 0 &&
         imported_record->name == "F2 ATTIRE TEST");

  std::error_code error;
  fs::remove_all(root, error);
  assert(!error);
  std::cout << "creation_export_attires_test: PASS (four indexed CAS regions and twelve additional ZLIB sections "
               "survive .cas export/import in byte order)\n";
  return 0;
}
