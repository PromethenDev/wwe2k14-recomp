#ifdef NDEBUG
#undef NDEBUG
#endif

#include "../launcher_core.h"
#include "../internal/archive.h"
#include "../internal/creation_save.h"
#include "../internal/util.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
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
constexpr size_t kCawPlainSize = 0x1CB7AC;
constexpr size_t kArenaPlainSize = 626'764;
constexpr size_t kCawBase = 0x1E517;
constexpr size_t kCawStride = 0xB6C;
constexpr size_t kArenaBase = 0x7368B;
constexpr size_t kArenaStride = 0xCC;
constexpr uint32_t kSyntheticCasChecksum = 0x78;
constexpr uint32_t kSyntheticOldArenaChecksum = 0x2E7;
constexpr uint32_t kSyntheticNewArenaChecksum = 0x2F2;

Paths MakePaths(const fs::path& root) {
  Paths paths;
  paths.exe_dir = root;
  paths.userdata_dir = root / "userdata";
  paths.settings_file = root / "wwe2k14-enhanced.ini";
  paths.backups_dir = root / "backups";
  return paths;
}

void WriteBe32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value >> 24);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 16);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value);
}

uint32_t ReadBe32(const std::vector<uint8_t>& bytes, size_t offset) {
  assert(offset + 4 <= bytes.size());
  return (static_cast<uint32_t>(bytes[offset]) << 24) |
         (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<uint32_t>(bytes[offset + 3]);
}

void WriteLe32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 16);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value >> 24);
}

void WriteFixedName(std::vector<uint8_t>* bytes, size_t offset, size_t size, std::string_view name) {
  std::fill(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
            bytes->begin() + static_cast<std::ptrdiff_t>(offset + size), 0);
  std::copy(name.begin(), name.end(), bytes->begin() + static_cast<std::ptrdiff_t>(offset));
}

std::vector<uint8_t> PackContainer(const std::vector<uint8_t>& plain, size_t prefix_size,
                                  const std::vector<uint8_t>& tail = {}) {
  mz_ulong capacity = mz_compressBound(static_cast<mz_ulong>(plain.size()));
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
  file.insert(file.end(), tail.begin(), tail.end());
  return file;
}

std::vector<uint8_t> UnpackFirstContainer(const std::vector<uint8_t>& file, size_t expected_offset) {
  assert(expected_offset + 16 <= file.size());
  assert(std::string(reinterpret_cast<const char*>(file.data() + expected_offset), 4) == "ZLIB");
  const auto read_le = [&file](size_t offset) {
    return static_cast<uint32_t>(file[offset]) |
           (static_cast<uint32_t>(file[offset + 1]) << 8) |
           (static_cast<uint32_t>(file[offset + 2]) << 16) |
           (static_cast<uint32_t>(file[offset + 3]) << 24);
  };
  assert(read_le(expected_offset + 4) == 0x3012);
  const uint32_t compressed = read_le(expected_offset + 8);
  const uint32_t plain_size = read_le(expected_offset + 12);
  assert(expected_offset + 16ull + compressed <= file.size());
  std::vector<uint8_t> plain(plain_size);
  mz_ulong actual = static_cast<mz_ulong>(plain.size());
  assert(mz_uncompress(plain.data(), &actual, file.data() + expected_offset + 16, compressed) == MZ_OK);
  assert(actual == plain_size);
  return plain;
}

std::vector<uint8_t> MakeSyntheticCaw() {
  std::vector<uint8_t> plain(kCawPlainSize, 0);
  WriteBe32(&plain, 0, 55);
  WriteBe32(&plain, 4, 42);
  WriteBe32(&plain, 8, 23);
  WriteFixedName(&plain, 0x1CA40F + 0xD1, 0x2A, "OLD CAW");
  WriteFixedName(&plain, 0x1CA40F + 0x115, 0x2A, "OLD NICK");
  WriteFixedName(&plain, 0x1CA40F + 0x13D, 0x2A, "OLD CAW");
  WriteBe32(&plain, 0x0C, kSyntheticCasChecksum);
  WriteBe32(&plain, 0x1CB068, kSyntheticCasChecksum);
  WriteBe32(&plain, 0x1CB7A8, kSyntheticCasChecksum);
  return PackContainer(plain, 0, {0xFA, 0xCE, 0x01, 0x02});
}

std::vector<uint8_t> MakeSyntheticArena() {
  std::vector<uint8_t> plain(kArenaPlainSize, 0);
  plain[0x07] = 0x81;
  WriteBe32(&plain, 0x3C, 0x81);
  WriteFixedName(&plain, 0x48, 0x16, "OLD ARENA");
  WriteBe32(&plain, 0x44, kSyntheticOldArenaChecksum);
  return PackContainer(plain, 0);
}

std::vector<uint8_t> MakeEmptyMain() {
  std::vector<uint8_t> plain(kMainPlainSize, 0);
  return PackContainer(plain, kMainContainerOffset);
}

std::vector<uint8_t> MakeHeader() {
  std::vector<uint8_t> header(0x148, 0);
  WriteBe32(&header, 0x04, 1);
  WriteBe32(&header, 0x140, 0x545408B2);
  return header;
}

std::string ReadFixedName(const std::vector<uint8_t>& bytes, size_t offset, size_t size) {
  size_t length = 0;
  while (length < size && bytes[offset + length] != 0) ++length;
  return std::string(reinterpret_cast<const char*>(bytes.data() + offset), length);
}

bool MainChecksumMatches(const std::vector<uint8_t>& plain) {
  uint32_t sum = 0;
  for (size_t offset = 0x1C; offset < plain.size(); ++offset) sum += plain[offset];
  return ReadBe32(plain, 0x18) == sum;
}

bool BackupContains(const fs::path& archive_path, const std::vector<uint8_t>& expected_main,
                    const std::vector<uint8_t>& expected_main_header, std::string_view creation_name,
                    const std::vector<uint8_t>& expected_payload,
                    const std::vector<uint8_t>& expected_creation_header) {
  internal::ZipReader zip;
  if (!zip.Open(archive_path)) return false;
  const std::string root = "userdata/" + std::string(kAccount) + "/" + std::string(kTitle);
  const std::string main_name = root + "/00000001/SaveData.dat/SaveData.Dat";
  const std::string main_header_name = root + "/Headers/00000001/SaveData.dat.header";
  const std::string payload_name = root + "/00000001/" + std::string(creation_name) + "/SaveData.Dat";
  const std::string creation_header_name = root + "/Headers/00000001/" +
                                           std::string(creation_name) + ".header";
  bool found_main = false, found_main_header = false, found_payload = false, found_creation_header = false;
  for (size_t index = 0; index < zip.Count(); ++index) {
    if (zip.IsDirectory(index)) continue;
    mz_zip_archive_file_stat stat{};
    std::vector<uint8_t> contents;
    if (!zip.Stat(index, stat) || !zip.Extract(index, contents) || contents.size() != stat.m_uncomp_size) return false;
    const std::string name = stat.m_filename;
    if (name == main_name) {
      if (contents != expected_main) return false;
      found_main = true;
    } else if (name == main_header_name) {
      if (contents != expected_main_header) return false;
      found_main_header = true;
    } else if (name == payload_name) {
      if (contents != expected_payload) return false;
      found_payload = true;
    } else if (name == creation_header_name) {
      if (contents != expected_creation_header) return false;
      found_creation_header = true;
    }
  }
  return found_main && found_main_header && found_payload && found_creation_header;
}

void WriteTestFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  assert(internal::EnsureDirectory(path.parent_path()));
  assert(internal::WriteBinaryFile(path, bytes));
}

// #115/#75: a game-written save with records / side-table values outside the old whitelists must stay usable, and
// those bytes must come through an import untouched. The old whole-save gate refused every case below.
void RunTolerantSaveTest() {
  constexpr size_t kTable1 = 0x75ECB, kTable2 = 0x76387;
  std::vector<uint8_t> plain(kMainPlainSize, 0);
  const size_t odd_caw = kCawBase + 5 * kCawStride;
  plain[odd_caw + 0x6ED] = 2;  // non-empty, unrecognized shape
  plain[odd_caw + 0x100] = 0x41;
  const size_t odd_arena = kArenaBase + 3 * kArenaStride;
  plain[odd_arena] = 0x55;  // first byte not 0x81
  plain[kTable2 + 4 * 9] = 3;  // value outside 0/1/2
  plain[kTable1 + 4 * 4] = 5;  // value outside 0/1
  assert(!internal::IsSupportedCreationPlain(plain));  // strict (old) check refuses
  assert(internal::IsTolerableCreationPlain(plain));
  assert(internal::DescribeCreationTolerance(plain) ==
         "save has 1 unrecognized CAW records / 1 arena records / 2 side-table values; they are kept as they are");
  const std::vector<uint8_t> main_file = PackContainer(plain, kMainContainerOffset);
  const std::vector<uint8_t> caw = MakeSyntheticCaw();

  const auto free_slot = internal::FindKnownFreeCreationSlot(main_file, true);
  assert(free_slot && *free_slot == 0);
  const auto listed = internal::ListKnownCreationSlots(main_file, true);
  assert(listed && listed->size() == 1 && listed->at(0) == 5);  // the odd record counts as occupied
  assert(!internal::AddKnownCreation(main_file, caw, true, 5));  // never chosen
  assert(!internal::RemoveKnownCreation(main_file, true, 5));    // never removed
  assert(!internal::RemoveKnownCreation(main_file, false, 3));

  const auto added = internal::AddKnownCreation(main_file, caw, true, 0);
  assert(added);
  const auto after = UnpackFirstContainer(*added, kMainContainerOffset);
  assert(after.size() == kMainPlainSize && MainChecksumMatches(after));
  assert(std::equal(plain.begin() + odd_caw, plain.begin() + odd_caw + kCawStride, after.begin() + odd_caw));
  assert(std::equal(plain.begin() + odd_arena, plain.begin() + odd_arena + kArenaStride, after.begin() + odd_arena));
  assert(after[kTable2 + 4 * 9] == 3 && after[kTable1 + 4 * 4] == 5);
  assert(after[kCawBase + 0x6ED] == 1);  // slot 0 written
  // The odd record counts as another creation, so zero table entries are bumped but unexpected values stay.
  assert(after[kTable2 + 4 * 10] == 1 && after[kTable2 + 4 * 9] == 3);
  const auto removed = internal::RemoveKnownCreation(*added, true, 0);  // a recognized record can still go
  assert(removed);

  // Removing the LAST creation resets only plain table entries; odd values stay (Fable review 11-15).
  {
    std::vector<uint8_t> only_tables(kMainPlainSize, 0);
    only_tables[kTable2 + 4 * 9] = 3;
    only_tables[kTable1 + 4 * 4] = 5;
    only_tables[kTable1 + 4 * 6 + 1] = 1;  // bytes 0,1,0,0: not a plain value either
    const auto one = internal::AddKnownCreation(PackContainer(only_tables, kMainContainerOffset), caw, true, 0);
    assert(one);
    const auto none = internal::RemoveKnownCreation(*one, true, 0);
    assert(none);
    const auto last = UnpackFirstContainer(*none, kMainContainerOffset);
    assert(last[kTable2 + 4 * 9] == 3 && last[kTable1 + 4 * 4] == 5 && last[kTable1 + 4 * 6 + 1] == 1);
  }

  // Plain size is still a hard refusal.
  std::vector<uint8_t> shorter(kMainPlainSize - 4, 0);
  assert(!internal::IsTolerableCreationPlain(shorter));
  assert(!internal::AddKnownCreation(PackContainer(shorter, kMainContainerOffset), caw, true, 0));
  std::cout << "rename_creation_test: PASS (tolerant save: odd records kept occupied and byte-identical)\n";
}

}  // namespace

int main() {
  RunTolerantSaveTest();
  const fs::path root = fs::temp_directory_path() /
      ("wwe2k14-rename-creation-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  const Paths paths = MakePaths(root);

  const std::vector<uint8_t> caw_payload = MakeSyntheticCaw();
  const std::vector<uint8_t> arena_payload = MakeSyntheticArena();
  std::vector<uint8_t> main_file = MakeEmptyMain();
  const auto with_caw = internal::AddKnownCreation(main_file, caw_payload, true, 0);
  assert(with_caw);
  const auto with_both = internal::AddKnownCreation(*with_caw, arena_payload, false, 0);
  assert(with_both);
  main_file = *with_both;

  const std::string caw_folder = "00CustomSuperStar.cas";
  const std::string arena_folder = "00CustomArena.car";
  const fs::path main_path = paths.userdata_dir / std::string(kAccount) / std::string(kTitle) /
                             "00000001" / "SaveData.dat" / "SaveData.Dat";
  const fs::path main_header_path = paths.userdata_dir / std::string(kAccount) / std::string(kTitle) /
                                    "Headers" / "00000001" / "SaveData.dat.header";
  const std::vector<uint8_t> main_header = MakeHeader();
  const std::vector<uint8_t> creation_header = MakeHeader();
  const fs::path caw_payload_path = paths.userdata_dir / std::string(kAccount) / std::string(kTitle) /
                                    "00000001" / caw_folder / "SaveData.Dat";
  const fs::path arena_payload_path = paths.userdata_dir / std::string(kAccount) / std::string(kTitle) /
                                      "00000001" / arena_folder / "SaveData.Dat";
  const fs::path caw_header_path = paths.userdata_dir / std::string(kAccount) / std::string(kTitle) /
                                   "Headers" / "00000001" / (caw_folder + ".header");
  const fs::path arena_header_path = paths.userdata_dir / std::string(kAccount) / std::string(kTitle) /
                                     "Headers" / "00000001" / (arena_folder + ".header");
  WriteTestFile(main_path, main_file);
  WriteTestFile(main_header_path, main_header);
  WriteTestFile(caw_payload_path, caw_payload);
  WriteTestFile(arena_payload_path, arena_payload);
  WriteTestFile(caw_header_path, creation_header);
  WriteTestFile(arena_header_path, creation_header);

  const auto initial_records = ListCreationSaveRecords(paths);
  assert(initial_records && initial_records->size() == 2);
  assert(initial_records->at(0).name == "OLD CAW");
  assert(initial_records->at(1).name == "OLD ARENA");

  assert(!RenameCreation(paths, CreationSaveKind::kSuperstar, 0, "").ok);
  assert(!RenameCreation(paths, CreationSaveKind::kSuperstar, 0, std::string(43, 'X')).ok);
  assert(!RenameCreation(paths, CreationSaveKind::kArena, 0, "bad\nname").ok);
  assert(ListBackups(paths).empty());
  std::vector<uint8_t> unchanged_main, unchanged_caw;
  assert(internal::ReadBinaryFile(main_path, unchanged_main) && unchanged_main == main_file);
  assert(internal::ReadBinaryFile(caw_payload_path, unchanged_caw) && unchanged_caw == caw_payload);

  assert(RenameCreation(paths, CreationSaveKind::kSuperstar, 0, "NEW CHAMPION").ok);
  auto caw_renamed_records = ListCreationSaveRecords(paths);
  assert(caw_renamed_records && caw_renamed_records->size() == 2 &&
         caw_renamed_records->at(0).name == "NEW CHAMPION");
  std::vector<uint8_t> caw_renamed_main, caw_renamed_payload, caw_renamed_header;
  assert(internal::ReadBinaryFile(main_path, caw_renamed_main));
  assert(internal::ReadBinaryFile(caw_payload_path, caw_renamed_payload));
  assert(internal::ReadBinaryFile(caw_header_path, caw_renamed_header));
  const std::vector<uint8_t> caw_plain = UnpackFirstContainer(caw_renamed_payload, 0);
  const std::vector<uint8_t> main_plain = UnpackFirstContainer(caw_renamed_main, kMainContainerOffset);
  assert(ReadFixedName(caw_plain, 0x1CA40F + 0xD1, 0x2A) == "NEW CHAMPION");
  assert(ReadFixedName(caw_plain, 0x1CA40F + 0x13D, 0x2A) == "NEW CHAMPION");
  assert(ReadFixedName(caw_plain, 0x1CA40F + 0x115, 0x2A) == "OLD NICK");
  assert(ReadFixedName(main_plain, kCawBase + 0xD1, 0x2A) == "NEW CHAMPION");
  assert(ReadFixedName(main_plain, kCawBase + 0x13D, 0x2A) == "NEW CHAMPION");
  assert(ReadBe32(caw_plain, 0x0C) == kSyntheticCasChecksum &&
         ReadBe32(caw_plain, 0x1CB068) == kSyntheticCasChecksum &&
         ReadBe32(caw_plain, 0x1CB7A8) == kSyntheticCasChecksum);
  assert(internal::CreationFileMatchesMainRecord(caw_renamed_main, caw_renamed_payload, true, 0));
  assert(MainChecksumMatches(main_plain));
  assert(caw_renamed_payload.size() >= 4 &&
         std::equal(caw_renamed_payload.end() - 4, caw_renamed_payload.end(),
                    std::array<uint8_t, 4>{0xFA, 0xCE, 0x01, 0x02}.begin()));
  auto backups = ListBackups(paths);
  assert(backups.size() == 1);
  assert(BackupContains(backups.front().file, main_file, main_header, caw_folder,
                        caw_payload, creation_header));

  assert(RenameCreation(paths, CreationSaveKind::kArena, 0, "NEW ARENA").ok);
  auto arena_renamed_records = ListCreationSaveRecords(paths);
  assert(arena_renamed_records && arena_renamed_records->size() == 2 &&
         arena_renamed_records->at(1).name == "NEW ARENA");
  std::vector<uint8_t> arena_renamed_main, arena_renamed_payload;
  assert(internal::ReadBinaryFile(main_path, arena_renamed_main));
  assert(internal::ReadBinaryFile(arena_payload_path, arena_renamed_payload));
  const std::vector<uint8_t> arena_plain = UnpackFirstContainer(arena_renamed_payload, 0);
  const std::vector<uint8_t> final_main_plain = UnpackFirstContainer(arena_renamed_main, kMainContainerOffset);
  assert(ReadFixedName(arena_plain, 0x48, 0x16) == "NEW ARENA");
  assert(ReadFixedName(final_main_plain, kArenaBase + 0x39, 0x16) == "NEW ARENA");
  assert(ReadBe32(arena_plain, 0x44) == kSyntheticNewArenaChecksum);
  assert(internal::CreationFileMatchesMainRecord(arena_renamed_main, arena_renamed_payload, false, 0));
  assert(MainChecksumMatches(final_main_plain));
  backups = ListBackups(paths);
  assert(backups.size() == 2);
  assert(BackupContains(backups.front().file, caw_renamed_main, main_header, arena_folder,
                        arena_payload, creation_header));

  assert(RenameCreation(paths, CreationSaveKind::kArena, 0, "NEW ARENA").ok);
  assert(ListBackups(paths).size() == 2);

  SetCommitFaultInjectionForTest(1, 0);
  const Result interrupted = RenameCreation(paths, CreationSaveKind::kArena, 0, "ROLLBACK");
  SetCommitFaultInjectionForTest(-1, 0);
  assert(!interrupted.ok);
  std::vector<uint8_t> after_fault_main, after_fault_payload;
  assert(internal::ReadBinaryFile(main_path, after_fault_main) && after_fault_main == arena_renamed_main);
  assert(internal::ReadBinaryFile(arena_payload_path, after_fault_payload) &&
         after_fault_payload == arena_renamed_payload);
  assert(ListBackups(paths).size() == 3);

  std::error_code error;
  fs::remove_all(root, error);
  assert(!error);
  std::cout << "rename_creation_test: PASS (CAW/arena names, checksums, backups, no-op, validation and rollback)\n";
  return 0;
}
