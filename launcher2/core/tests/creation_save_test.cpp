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
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {

constexpr std::string_view kAccount = "B13EBABEBABEBABE";
constexpr std::string_view kTitle = "545408B2";

Paths MakePaths(const fs::path& root) {
  Paths paths;
  paths.exe_dir = root;
  paths.userdata_dir = root / "userdata";
  paths.settings_file = root / "wwe2k14-enhanced.ini";
  paths.backups_dir = root / "backups";
  return paths;
}

fs::path EnvPathOr(const char* name, const fs::path& fallback) {
  const char* value = std::getenv(name);
  return value && *value ? fs::path(value) : fallback;
}

fs::path OwnerTestRoot() {
#ifdef WWE2K14_OWNER_CREATION_TESTDATA
  return EnvPathOr("WWE2K14_TEST_OWNER_ROOT", fs::path(WWE2K14_OWNER_CREATION_TESTDATA));
#else
  return {};
#endif
}

fs::path R101TestRoot() {
#ifdef WWE2K14_R101_CAW_TESTDATA
  return EnvPathOr("WWE2K14_TEST_R101_ROOT", fs::path(WWE2K14_R101_CAW_TESTDATA));
#else
  return {};
#endif
}

fs::path OwnerMainFile(const fs::path& root, std::string_view sample) {
  return root / std::string(sample) / std::string(kAccount) / std::string(kTitle) /
         "00000001" / "SaveData.dat" / "SaveData.Dat";
}

fs::path OwnerHeaderFile(const fs::path& root, std::string_view sample) {
  return root / std::string(sample) / std::string(kAccount) / std::string(kTitle) /
         "Headers" / "00000001" / "SaveData.dat.header";
}

fs::path OwnerCreationFile(const fs::path& root, std::string_view sample, std::string_view folder) {
  return root / std::string(sample) / std::string(kAccount) / std::string(kTitle) /
         "00000001" / std::string(folder) / "SaveData.Dat";
}

fs::path OwnerCreationHeader(const fs::path& root, std::string_view sample, std::string_view name) {
  return root / std::string(sample) / std::string(kAccount) / std::string(kTitle) /
         "Headers" / "00000001" / (std::string(name) + ".header");
}

bool Read(const fs::path& path, std::vector<uint8_t>* bytes) {
  return internal::ReadBinaryFile(path, *bytes);
}

void AssertExact(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected,
                 std::string_view label) {
  if (actual == expected) return;
  size_t differences = 0;
  std::vector<size_t> first;
  const size_t common = std::min(actual.size(), expected.size());
  for (size_t i = 0; i < common; ++i) {
    if (actual[i] != expected[i]) {
      ++differences;
      if (first.size() < 12) first.push_back(i);
    }
  }
  differences += actual.size() > common ? actual.size() - common : expected.size() - common;
  std::cerr << "creation_save_test: " << label << " differs: actual=" << actual.size()
            << " expected=" << expected.size() << " differing_bytes=" << differences << " first_offsets=";
  for (size_t offset : first) std::cerr << " 0x" << std::hex << offset << std::dec;
  std::cerr << '\n';
  assert(false && "byte-exact owner sample comparison failed");
}

uint32_t Le32(const std::vector<uint8_t>& bytes, size_t offset) {
  assert(offset + 4 <= bytes.size());
  return static_cast<uint32_t>(bytes[offset]) |
         (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

std::vector<uint8_t> UnpackMain(const std::vector<uint8_t>& file) {
  constexpr size_t kChunk = 0x2CA000;
  assert(file.size() >= kChunk + 16);
  assert(std::string(reinterpret_cast<const char*>(file.data() + kChunk), 4) == "ZLIB");
  assert(Le32(file, kChunk + 4) == 0x3012);
  const uint32_t compressed = Le32(file, kChunk + 8);
  const uint32_t plain_size = Le32(file, kChunk + 12);
  assert(kChunk + 16ull + compressed == file.size());
  std::vector<uint8_t> plain(plain_size);
  mz_ulong actual = static_cast<mz_ulong>(plain.size());
  assert(mz_uncompress(plain.data(), &actual, file.data() + kChunk + 16, compressed) == MZ_OK);
  assert(actual == plain_size);
  return plain;
}

std::vector<uint8_t> UnpackFirstZlibChunk(const std::vector<uint8_t>& file) {
  constexpr std::array<uint8_t, 4> magic{'Z', 'L', 'I', 'B'};
  const auto marker = std::search(file.begin(), file.end(), magic.begin(), magic.end());
  assert(marker != file.end());
  const size_t offset = static_cast<size_t>(marker - file.begin());
  assert(offset + 16 <= file.size() && Le32(file, offset + 4) == 0x3012);
  const uint32_t compressed = Le32(file, offset + 8);
  const uint32_t plain_size = Le32(file, offset + 12);
  assert(offset + 16ull + compressed <= file.size());
  std::vector<uint8_t> plain(plain_size);
  mz_ulong actual = static_cast<mz_ulong>(plain.size());
  assert(mz_uncompress(plain.data(), &actual, file.data() + offset + 16, compressed) == MZ_OK);
  assert(actual == plain_size);
  return plain;
}

bool ReadLe32Safe(const std::vector<uint8_t>& bytes, size_t offset, uint32_t* value) {
  if (!value || offset > bytes.size() || bytes.size() - offset < 4) return false;
  *value = static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
  return true;
}

uint32_t ReadBe32Safe(const std::vector<uint8_t>& bytes, size_t offset) {
  assert(offset <= bytes.size() && bytes.size() - offset >= 4);
  return (static_cast<uint32_t>(bytes[offset]) << 24) |
         (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<uint32_t>(bytes[offset + 3]);
}

bool AllZeroRange(const std::vector<uint8_t>& bytes, size_t offset, size_t size) {
  return offset <= bytes.size() && size <= bytes.size() - offset &&
         std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                     bytes.begin() + static_cast<std::ptrdiff_t>(offset + size),
                     [](uint8_t byte) { return byte == 0; });
}

struct SaveInspection {
  std::vector<uint8_t> main_file;
  std::vector<uint8_t> header;
  std::vector<uint8_t> plain;
  size_t first_marker = std::string::npos;
  size_t marker_count = 0;
  uint32_t version = 0;
  uint32_t compressed_size = 0;
  uint32_t plain_size = 0;
  bool container_valid = false;
  bool decompressed = false;
};

bool CheckCreationTable(const std::vector<uint8_t>& plain, size_t base, size_t count,
                        uint32_t maximum, size_t* bad_index, uint32_t* bad_value) {
  for (size_t index = 0; index < count; ++index) {
    const size_t offset = base + index * 4;
    uint32_t value = 0;
    if (!ReadLe32Safe(plain, offset, &value) || value > maximum) {
      if (bad_index) *bad_index = index;
      if (bad_value) *bad_value = value;
      return false;
    }
  }
  return true;
}

bool CheckCawRecord(const std::vector<uint8_t>& plain, size_t offset) {
  constexpr size_t size = 0xB6C;
  if (AllZeroRange(plain, offset, size)) return true;
  return offset <= plain.size() && size <= plain.size() - offset &&
         plain[offset + 0x6ED] == 1 && plain[offset + 0x7FC] == 0 &&
         (plain[offset + 0x813] == 2 || plain[offset + 0x813] == 0) && !AllZeroRange(plain, offset + 0x7FD, 4);
}

bool CheckArenaRecord(const std::vector<uint8_t>& plain, size_t offset) {
  constexpr size_t size = 0xCC;
  if (AllZeroRange(plain, offset, size)) return true;
  if (offset > plain.size() || size > plain.size() - offset ||
      plain[offset] != 0x81 || plain[offset + 0x7C] != 0x46) return false;
  for (size_t byte = 0; byte < size; ++byte) {
    const bool mapped = byte == 0 || (byte >= 0x21 && byte < 0x26) ||
                        (byte >= 0x36 && byte < 0x45) || byte == 0x7C;
    if (!mapped && plain[offset + byte] != 0) return false;
  }
  return true;
}

bool InspectRealCreationSave(std::string_view label, const fs::path& userdata,
                             SaveInspection* inspection) {
  constexpr size_t main_offset = 0x2CA000;
  constexpr size_t main_plain_size = 1'790'648;
  constexpr size_t caw_base = 0x1E517, caw_stride = 0xB6C, caw_count = 100;
  constexpr size_t arena_base = 0x7368B, arena_stride = 0xCC;
  constexpr size_t table1_base = 0x75ECB, table1_count = 159;
  constexpr size_t table2_base = 0x76387, table2_count = 256;
  const fs::path title_root = userdata / std::string(kAccount) / std::string(kTitle);
  const fs::path main_path = title_root / "00000001" / "SaveData.dat" / "SaveData.Dat";
  const fs::path header_path = title_root / "Headers" / "00000001" / "SaveData.dat.header";
  if (!Read(main_path, &inspection->main_file) || !Read(header_path, &inspection->header)) {
    std::cerr << "creation_save_test: real-save " << label << " file read failed\n";
    return false;
  }
  const auto& file = inspection->main_file;
  const auto& header = inspection->header;
  std::cout << "creation_save_test: real-save " << label << " header_bytes=" << header.size()
            << " expected=328";
  if (header.size() >= 0x148) {
    std::cout << " device_id=0x" << std::hex << ReadBe32Safe(header, 0)
              << " content_type=0x" << ReadBe32Safe(header, 4) << " expected=0x1"
              << " title_id=0x" << ReadBe32Safe(header, 0x140) << " expected=0x545408b2"
              << " pair_byte=0x" << static_cast<unsigned>(header[0x144]) << std::dec;
  }
  std::cout << '\n';

  const std::array<uint8_t, 4> marker{'Z', 'L', 'I', 'B'};
  auto cursor = file.begin();
  while (cursor != file.end()) {
    const auto found = std::search(cursor, file.end(), marker.begin(), marker.end());
    if (found == file.end()) break;
    if (inspection->first_marker == std::string::npos) {
      inspection->first_marker = static_cast<size_t>(found - file.begin());
    }
    ++inspection->marker_count;
    cursor = found + 1;
  }
  std::cout << "creation_save_test: real-save " << label << " main_bytes=" << file.size()
            << " first_ZLIB=";
  if (inspection->first_marker == std::string::npos) {
    std::cout << "none";
  } else {
    std::cout << "0x" << std::hex << inspection->first_marker << std::dec
              << " expected=0x2ca000 markers=" << inspection->marker_count;
    const size_t offset = inspection->first_marker;
    if (ReadLe32Safe(file, offset + 4, &inspection->version) &&
        ReadLe32Safe(file, offset + 8, &inspection->compressed_size) &&
        ReadLe32Safe(file, offset + 12, &inspection->plain_size)) {
      const uint64_t chunk_end = offset + 16ull + inspection->compressed_size;
      inspection->container_valid = offset == main_offset && inspection->version == 0x3012 &&
          chunk_end == file.size() && inspection->plain_size != 0 &&
          inspection->plain_size <= static_cast<uint32_t>((std::numeric_limits<int>::max)());
      std::cout << " version=0x" << std::hex << inspection->version << " expected=0x3012"
                << " compressed=" << std::dec << inspection->compressed_size
                << " plain=" << inspection->plain_size << " expected=" << main_plain_size
                << " chunk_end=" << chunk_end << " terminal=" << (chunk_end == file.size());
      if (inspection->container_valid) {
        inspection->plain.resize(inspection->plain_size);
        mz_ulong actual_size = static_cast<mz_ulong>(inspection->plain.size());
        inspection->decompressed = mz_uncompress(
            inspection->plain.data(), &actual_size, file.data() + offset + 16,
            static_cast<mz_ulong>(inspection->compressed_size)) == MZ_OK &&
            actual_size == inspection->plain_size;
        std::cout << " decompress=" << (inspection->decompressed ? "ok" : "failed");
      }
    } else {
      std::cout << " short_header=yes";
    }
  }
  std::cout << '\n';

  if (inspection->decompressed) {
    const auto& plain = inspection->plain;
    size_t bad_index = 0;
    uint32_t bad_value = 0;
    const bool table1_ok = CheckCreationTable(plain, table1_base, table1_count, 1, &bad_index, &bad_value);
    std::cout << "creation_save_test: real-save " << label << " plain_bytes=" << plain.size()
              << " expected=" << main_plain_size << " table1=" << (table1_ok ? "PASS" : "FAIL");
    if (!table1_ok) std::cout << " first_bad_index=" << bad_index << " word=0x" << std::hex << bad_value << std::dec;
    const bool table2_ok = CheckCreationTable(plain, table2_base, table2_count, 2, &bad_index, &bad_value);
    std::cout << " table2=" << (table2_ok ? "PASS" : "FAIL");
    if (!table2_ok) std::cout << " first_bad_index=" << bad_index << " word=0x" << std::hex << bad_value << std::dec;
    size_t valid_caws = 0, valid_arenas = 0, occupied_caws = 0, occupied_arenas = 0;
    int bad_caw = -1, bad_arena = -1;
    for (size_t slot = 0; slot < caw_count; ++slot) {
      const size_t offset = caw_base + slot * caw_stride;
      const bool valid = CheckCawRecord(plain, offset);
      if (valid) ++valid_caws;
      else if (bad_caw < 0) bad_caw = static_cast<int>(slot);
      if (!AllZeroRange(plain, offset, caw_stride)) ++occupied_caws;
    }
    const size_t arena_count = (table1_base - arena_base) / arena_stride;
    for (size_t slot = 0; slot < arena_count; ++slot) {
      const size_t offset = arena_base + slot * arena_stride;
      const bool valid = CheckArenaRecord(plain, offset);
      if (valid) ++valid_arenas;
      else if (bad_arena < 0) bad_arena = static_cast<int>(slot);
      if (!AllZeroRange(plain, offset, arena_stride)) ++occupied_arenas;
    }
    std::cout << " caw_records=" << valid_caws << "/" << caw_count
              << " occupied_caws=" << occupied_caws << " arena_records=" << valid_arenas << "/" << arena_count
              << " occupied_arenas=" << occupied_arenas;
    if (bad_caw >= 0) {
      const size_t offset = caw_base + static_cast<size_t>(bad_caw) * caw_stride;
      std::cout << " first_bad_caw=" << bad_caw << " @0x" << std::hex << offset
                << " +6ed=0x" << static_cast<unsigned>(plain[offset + 0x6ED])
                << " expected=0x1 +7fc=0x" << static_cast<unsigned>(plain[offset + 0x7FC])
                << " expected=0 +7fd..800_all_zero=" << AllZeroRange(plain, offset + 0x7FD, 4)
                << " +813=0x" << static_cast<unsigned>(plain[offset + 0x813]) << " expected=0x2" << std::dec;
    }
    if (bad_arena >= 0) {
      const size_t offset = arena_base + static_cast<size_t>(bad_arena) * arena_stride;
      std::cout << " first_bad_arena=" << bad_arena << " @0x" << std::hex << offset
                << " +00=0x" << static_cast<unsigned>(plain[offset]) << " expected=0x81"
                << " +7c=0x" << static_cast<unsigned>(plain[offset + 0x7C]) << " expected=0x46" << std::dec;
      for (size_t byte = 0; byte < arena_stride; ++byte) {
        const bool mapped = byte == 0 || (byte >= 0x21 && byte < 0x26) ||
                            (byte >= 0x36 && byte < 0x45) || byte == 0x7C;
        if (!mapped && plain[offset + byte] != 0) {
          std::cout << " first_unmapped_nonzero=+0x" << std::hex << byte << " value=0x"
                    << static_cast<unsigned>(plain[offset + byte]) << std::dec;
          break;
        }
      }
    }
    std::cout << " validator=" << (internal::IsSupportedCreationPlain(plain) ? "PASS" : "FAIL") << '\n';
  } else {
    std::cout << "creation_save_test: real-save " << label << " plain/tables/records=not available\n";
  }

  Paths paths = MakePaths(userdata.parent_path());
  paths.userdata_dir = userdata;
  const auto records = ListCreationSaveRecords(paths);
  if (!records) {
    std::cout << "creation_save_test: real-save " << label << " ListCreationSaveRecords=nullopt\n";
  } else {
    const size_t caws = static_cast<size_t>(std::count_if(records->begin(), records->end(), [](const auto& record) {
      return record.kind == CreationSaveKind::kSuperstar;
    }));
    const size_t arenas = static_cast<size_t>(std::count_if(records->begin(), records->end(), [](const auto& record) {
      return record.kind == CreationSaveKind::kArena;
    }));
    std::cout << "creation_save_test: real-save " << label << " ListCreationSaveRecords=ok records="
              << records->size() << " caws=" << caws << " arenas=" << arenas << '\n';
  }
  return inspection->header.size() == 0x148 && !file.empty();
}

bool RunInspectRealCreationSavePair(const fs::path& player_userdata, const fs::path& owner_userdata) {
  SaveInspection player, owner;
  const bool player_read = InspectRealCreationSave("player", player_userdata, &player);
  const bool owner_read = InspectRealCreationSave("owner", owner_userdata, &owner);
  if (!player_read || !owner_read) return false;
  const auto same_range = [](const std::vector<uint8_t>& left, const std::vector<uint8_t>& right,
                             size_t offset, size_t size) {
    return offset <= left.size() && size <= left.size() - offset && offset <= right.size() &&
           size <= right.size() - offset && std::equal(left.begin() + static_cast<std::ptrdiff_t>(offset),
               left.begin() + static_cast<std::ptrdiff_t>(offset + size),
               right.begin() + static_cast<std::ptrdiff_t>(offset));
  };
  std::cout << "creation_save_test: real-save comparison header device_id="
            << (ReadBe32Safe(player.header, 0) == ReadBe32Safe(owner.header, 0) ? "same" : "different")
            << " content_type=" << (ReadBe32Safe(player.header, 4) == ReadBe32Safe(owner.header, 4) ? "same" : "different")
            << " display_name=" << (same_range(player.header, owner.header, 0x08, 0x100) ? "same" : "different")
            << " package_name=" << (same_range(player.header, owner.header, 0x108, 0x30) ? "same" : "different")
            << " title_id=" << (ReadBe32Safe(player.header, 0x140) == ReadBe32Safe(owner.header, 0x140) ? "same" : "different")
            << " pair_byte=" << (player.header[0x144] == owner.header[0x144] ? "same" : "different")
            << " whole_header=" << (player.header == owner.header ? "same" : "different") << '\n';
  std::cout << "creation_save_test: real-save comparison header differences=";
  bool any_header_difference = false;
  for (size_t offset = 0; offset < player.header.size(); ++offset) {
    if (player.header[offset] == owner.header[offset]) continue;
    any_header_difference = true;
    std::cout << " 0x" << std::hex << offset << ":0x" << static_cast<unsigned>(player.header[offset])
              << "/0x" << static_cast<unsigned>(owner.header[offset]) << std::dec;
  }
  if (!any_header_difference) std::cout << "none";
  std::cout << '\n';
  std::cout << "creation_save_test: real-save comparison main file_bytes player=" << player.main_file.size()
            << " owner=" << owner.main_file.size() << " first_marker="
            << (player.first_marker == owner.first_marker ? "same" : "different")
            << " version=" << (player.version == owner.version ? "same" : "different")
            << " compressed_size=" << (player.compressed_size == owner.compressed_size ? "same" : "different")
            << " plain_size=" << (player.plain_size == owner.plain_size ? "same" : "different")
            << " raw_prefix=" << (player.main_file.size() >= 0x2CA000 && owner.main_file.size() >= 0x2CA000 &&
                same_range(player.main_file, owner.main_file, 0, 0x2CA000) ? "same" : "different");
  if (player.decompressed && owner.decompressed && player.plain.size() == owner.plain.size()) {
    size_t differing = 0;
    std::vector<size_t> first_offsets;
    for (size_t offset = 0; offset < player.plain.size(); ++offset) {
      if (player.plain[offset] == owner.plain[offset]) continue;
      ++differing;
      if (first_offsets.size() < 12) first_offsets.push_back(offset);
    }
    std::cout << " plain_differing_bytes=" << differing << " first_plain_offsets=";
    for (size_t offset : first_offsets) std::cout << " 0x" << std::hex << offset << std::dec;
  }
  std::cout << '\n';
  return player_read && owner_read;
}

void PrintDdsOffsets(std::string_view label, const std::vector<uint8_t>& bytes) {
  constexpr std::array<uint8_t, 4> signature{'D', 'D', 'S', ' '};
  std::vector<size_t> offsets;
  auto search_from = bytes.begin();
  while (search_from != bytes.end()) {
    const auto found = std::search(search_from, bytes.end(), signature.begin(), signature.end());
    if (found == bytes.end()) break;
    offsets.push_back(static_cast<size_t>(found - bytes.begin()));
    search_from = found + 1;
  }
  std::cout << "creation_save_test: " << label << " DDS-magic-count=" << offsets.size() << " offsets=";
  if (offsets.empty()) std::cout << "none";
  for (size_t index = 0; index < std::min<size_t>(8, offsets.size()); ++index) {
    std::cout << " 0x" << std::hex << offsets[index] << std::dec;
  }
  std::cout << '\n';
}

bool BackupContains(const fs::path& archive_path, const std::vector<uint8_t>& expected_main,
                    const std::vector<uint8_t>& expected_header, std::string_view creation_name = {},
                    const std::vector<uint8_t>* expected_payload = nullptr,
                    const std::vector<uint8_t>* expected_creation_header = nullptr) {
  internal::ZipReader zip;
  if (!zip.Open(archive_path)) return false;
  const std::string main_name = "userdata/" + std::string(kAccount) + "/" + std::string(kTitle) +
                                "/00000001/SaveData.dat/SaveData.Dat";
  const std::string header_name = "userdata/" + std::string(kAccount) + "/" + std::string(kTitle) +
                                  "/Headers/00000001/SaveData.dat.header";
  const std::string payload_name = "userdata/" + std::string(kAccount) + "/" + std::string(kTitle) +
                                   "/00000001/" + std::string(creation_name) + "/SaveData.Dat";
  const std::string creation_header_name = "userdata/" + std::string(kAccount) + "/" + std::string(kTitle) +
                                           "/Headers/00000001/" + std::string(creation_name) + ".header";
  bool found_main = false, found_header = false;
  bool found_payload = expected_payload == nullptr, found_creation_header = expected_creation_header == nullptr;
  for (size_t index = 0; index < zip.Count(); ++index) {
    if (zip.IsDirectory(index)) continue;
    mz_zip_archive_file_stat stat{};
    if (!zip.Stat(index, stat)) return false;
    std::vector<uint8_t> extracted;
    if (!zip.Extract(index, extracted)) return false;
    if (extracted.size() != stat.m_uncomp_size) return false;
    static constexpr uint8_t kEmpty = 0;
    const void* data = extracted.empty() ? static_cast<const void*>(&kEmpty)
                                         : static_cast<const void*>(extracted.data());
    if (mz_crc32(0, static_cast<const mz_uint8*>(data), extracted.size()) != stat.m_crc32) return false;
    const std::string name = stat.m_filename;
    if (name == main_name) {
      if (extracted != expected_main) return false;
      found_main = true;
    } else if (name == header_name) {
      if (extracted != expected_header) return false;
      found_header = true;
    } else if (name == payload_name && expected_payload) {
      if (extracted != *expected_payload) return false;
      found_payload = true;
    } else if (name == creation_header_name && expected_creation_header) {
      if (extracted != *expected_creation_header) return false;
      found_creation_header = true;
    }
  }
  return found_main && found_header && found_payload && found_creation_header;
}

void AssertOnlyCawSlotsDiffer(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected,
                              const std::vector<uint32_t>& slots);

void CopySampleToTemp(const fs::path& owner_root, std::string_view sample, const fs::path& temp_root) {
  const fs::path userdata = temp_root / "userdata";
  fs::create_directories(userdata);
  fs::copy(owner_root / std::string(sample), userdata, fs::copy_options::recursive);
}

void CopyUserdataToTemp(const fs::path& source_userdata, const fs::path& temp_root) {
  const fs::path userdata = temp_root / "userdata";
  fs::create_directories(userdata);
  fs::copy(source_userdata, userdata, fs::copy_options::recursive);
}

std::map<std::string, fs::path> SaveTreeFiles(const fs::path& root) {
  std::map<std::string, fs::path> result;
  for (fs::recursive_directory_iterator iterator(root), end; iterator != end; ++iterator) {
    if (iterator->is_regular_file()) {
      result.emplace(iterator->path().lexically_relative(root).generic_string(), iterator->path());
    }
  }
  return result;
}

bool CompareSaveTrees(const fs::path& actual_root, const fs::path& expected_root) {
  const auto actual_files = SaveTreeFiles(actual_root);
  const auto expected_files = SaveTreeFiles(expected_root);
  std::cout << "creation_save_test: save-tree compare actual_files=" << actual_files.size()
            << " expected_files=" << expected_files.size() << '\n';
  bool exact = actual_files.size() == expected_files.size();
  for (const auto& [name, expected_path] : expected_files) {
    const auto actual = actual_files.find(name);
    if (actual == actual_files.end()) {
      std::cout << "creation_save_test: MISSING " << name << '\n';
      exact = false;
      continue;
    }
    std::vector<uint8_t> expected, current;
    assert(Read(expected_path, &expected) && Read(actual->second, &current));
    if (current == expected) {
      std::cout << "creation_save_test: EXACT " << name << " bytes=" << current.size() << '\n';
      continue;
    }
    exact = false;
    size_t differing_bytes = 0;
    std::vector<size_t> first_offsets;
    const size_t common = std::min(current.size(), expected.size());
    for (size_t offset = 0; offset < common; ++offset) {
      if (current[offset] != expected[offset]) {
        ++differing_bytes;
        if (first_offsets.size() < 16) first_offsets.push_back(offset);
      }
    }
    differing_bytes += current.size() > common ? current.size() - common : expected.size() - common;
    std::cout << "creation_save_test: DIFF " << name << " actual_bytes=" << current.size()
              << " expected_bytes=" << expected.size() << " differing_bytes=" << differing_bytes
              << " first_offsets=";
    for (const size_t offset : first_offsets) std::cout << " 0x" << std::hex << offset << std::dec;
    std::cout << '\n';
  }
  for (const auto& [name, actual_path] : actual_files) {
    (void)actual_path;
    if (!expected_files.contains(name)) {
      std::cout << "creation_save_test: EXTRA " << name << '\n';
      exact = false;
    }
  }
  return exact;
}

bool CompareSaveTreesWithGeneralCreationRules(const fs::path& actual_root, const fs::path& expected_root) {
  constexpr size_t chunk_offset = 0x2CA000;
  constexpr size_t checksum_offset = 0x18;  // u32 BE sum of plain[0x1C..]; compared like every other byte
  const std::string main_name = std::string(kTitle) + "/00000001/SaveData.dat/SaveData.Dat";
  const auto actual_files = SaveTreeFiles(actual_root);
  const auto expected_files = SaveTreeFiles(expected_root);
  std::cout << "creation_save_test: general save-tree compare actual_files=" << actual_files.size()
            << " expected_files=" << expected_files.size() << '\n';
  bool exact = actual_files.size() == expected_files.size();
  for (const auto& [name, expected_path] : expected_files) {
    const auto actual = actual_files.find(name);
    if (actual == actual_files.end()) {
      std::cout << "creation_save_test: GENERAL MISSING " << name << '\n';
      exact = false;
      continue;
    }
    std::vector<uint8_t> expected, current;
    assert(Read(expected_path, &expected) && Read(actual->second, &current));
    if (name != main_name) {
      if (current != expected) {
        std::cout << "creation_save_test: GENERAL DIFF " << name << '\n';
        exact = false;
      }
      continue;
    }
    if (current.size() < chunk_offset || expected.size() < chunk_offset ||
        !std::equal(current.begin(), current.begin() + static_cast<std::ptrdiff_t>(chunk_offset),
                    expected.begin())) {
      std::cout << "creation_save_test: GENERAL main raw-prefix differs\n";
      exact = false;
      continue;
    }
    const auto current_plain = UnpackMain(current);
    const auto expected_plain = UnpackMain(expected);
    if (current_plain.size() != expected_plain.size()) {
      std::cout << "creation_save_test: GENERAL main plain sizes differ\n";
      exact = false;
      continue;
    }
    size_t unmodeled_differences = 0;
    std::vector<size_t> first_unmodeled;
    for (size_t offset = 0; offset < expected_plain.size(); ++offset) {
      if (current_plain[offset] == expected_plain[offset]) continue;
      ++unmodeled_differences;
      if (first_unmodeled.size() < 12) first_unmodeled.push_back(offset);
    }
    std::cout << "creation_save_test: GENERAL main plain checksum@0x18 actual=" << std::hex;
    for (size_t i = 0; i < 4; ++i) std::cout << ' ' << static_cast<unsigned>(current_plain[checksum_offset + i]);
    std::cout << " expected=";
    for (size_t i = 0; i < 4; ++i) std::cout << ' ' << static_cast<unsigned>(expected_plain[checksum_offset + i]);
    std::cout << std::dec
              << " other_differing_bytes=" << unmodeled_differences << " first_offsets=";
    for (size_t offset : first_unmodeled) std::cout << " 0x" << std::hex << offset << std::dec;
    std::cout << '\n';
    if (unmodeled_differences != 0) exact = false;
  }
  for (const auto& [name, path] : actual_files) {
    (void)path;
    if (!expected_files.contains(name)) {
      std::cout << "creation_save_test: GENERAL EXTRA " << name << '\n';
      exact = false;
    }
  }
  return exact;
}

void PrintByteDiffRuns(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected,
                       size_t begin, size_t end, std::string_view label,
                       size_t max_reported_runs = 80) {
  assert(begin <= actual.size() && begin <= expected.size() && end <= actual.size() && end <= expected.size());
  size_t offset = begin;
  size_t runs = 0;
  size_t bytes = 0;
  while (offset < end) {
    while (offset < end && actual[offset] == expected[offset]) ++offset;
    if (offset == end) break;
    const size_t first = offset;
    while (offset < end && actual[offset] != expected[offset]) {
      ++offset;
      ++bytes;
    }
    ++runs;
    if (runs <= max_reported_runs) {
      std::cout << "creation_save_test: " << label << " run [0x" << std::hex << first << ",0x" << offset
                << ") actual=";
      for (size_t index = first; index < offset; ++index) std::cout << std::setw(2) << std::setfill('0')
                                                                  << static_cast<unsigned>(actual[index]);
      std::cout << " expected=";
      for (size_t index = first; index < offset; ++index) std::cout << std::setw(2) << std::setfill('0')
                                                                  << static_cast<unsigned>(expected[index]);
      std::cout << std::dec << '\n';
    }
  }
  std::cout << "creation_save_test: " << label << " differing_bytes=" << bytes << " runs=" << runs << '\n';
}

void ReportMainSaveDetails(const fs::path& actual_root, const fs::path& expected_root,
                           const fs::path& caw_payload_path, const fs::path& actual_header_path,
                           const fs::path& expected_header_path) {
  constexpr size_t chunk_offset = 0x2CA000;
  std::vector<uint8_t> actual, expected, caw_payload, actual_header, expected_header;
  const fs::path relative_main = fs::path(std::string(kTitle)) / "00000001" / "SaveData.dat" / "SaveData.Dat";
  assert(Read(actual_root / relative_main, &actual) && Read(expected_root / relative_main, &expected));
  assert(Read(caw_payload_path, &caw_payload));
  assert(Read(actual_header_path, &actual_header) && Read(expected_header_path, &expected_header));
  assert(actual.size() >= chunk_offset + 16 && expected.size() >= chunk_offset + 16);
  const uint32_t actual_compressed = Le32(actual, chunk_offset + 8);
  const uint32_t expected_compressed = Le32(expected, chunk_offset + 8);
  const auto caw_plain = UnpackFirstZlibChunk(caw_payload);
  std::cout << "creation_save_test: main-container actual=" << actual.size() << " expected=" << expected.size()
            << " chunk=0x" << std::hex << chunk_offset << std::dec
            << " version=" << Le32(actual, chunk_offset + 4) << '/' << Le32(expected, chunk_offset + 4)
            << " compressed=" << actual_compressed << '/' << expected_compressed
            << " plain=" << Le32(actual, chunk_offset + 12) << '/' << Le32(expected, chunk_offset + 12)
            << " tail=" << (actual.size() - chunk_offset - 16 - actual_compressed) << '/'
            << (expected.size() - chunk_offset - 16 - expected_compressed)
            << " zlib=" << std::hex << static_cast<unsigned>(actual[chunk_offset + 16]) << ' '
            << static_cast<unsigned>(actual[chunk_offset + 17]) << "/" << static_cast<unsigned>(expected[chunk_offset + 16])
            << ' ' << static_cast<unsigned>(expected[chunk_offset + 17]) << std::dec << '\n';
  PrintByteDiffRuns(actual, expected, 0, chunk_offset, "main raw prefix");
  const auto actual_plain = UnpackMain(actual);
  const auto expected_plain = UnpackMain(expected);
  PrintByteDiffRuns(actual_plain, expected_plain, 0, actual_plain.size(), "main plain");
  PrintDdsOffsets("owner CAW raw payload", caw_payload);
  PrintDdsOffsets("owner CAW plain payload", caw_plain);
  std::cout << "creation_save_test: caw-thumbnail-equals-main-after="
            << (caw_payload.size() >= 0x2980 &&
                std::equal(caw_payload.begin() + 0x1980, caw_payload.begin() + 0x2980,
                           expected.begin() + 0x1980) ? "yes" : "no") << '\n';
  PrintByteDiffRuns(actual_header, expected_header, 0, std::min(actual_header.size(), expected_header.size()),
                    "main header");
}

bool RunGameMadeSaveAfterReproduction(const fs::path& owner_root) {
  const fs::path caw_folder = owner_root / "save-after" / std::string(kAccount) / std::string(kTitle) /
                              "00000001" / "00CustomSuperStar.cas";
  const fs::path arena_folder = owner_root / "save-after" / std::string(kAccount) / std::string(kTitle) /
                                "00000001" / "00CustomArena.car";
  if (!fs::is_regular_file(caw_folder / "SaveData.Dat") || !fs::is_regular_file(arena_folder / "SaveData.Dat")) {
    std::cout << "creation_save_test: SKIP (game-made slot-00 CAW/arena samples are absent)\n";
    return true;
  }

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("wwe2k14-game-made-after-test-" + std::to_string(stamp));
  CopySampleToTemp(owner_root, "save-before", root);
  const Paths paths = MakePaths(root);
  uint32_t caw_slot = UINT32_MAX, arena_slot = UINT32_MAX;
  const Result caw = ImportCreationSave(paths, CreationSaveKind::kSuperstar, caw_folder, &caw_slot);
  if (!caw.ok) std::cerr << "game-made CAW import failed: " << caw.error << '\n';
  assert(caw.ok && caw_slot == 0);
  const Result arena = ImportCreationSave(paths, CreationSaveKind::kArena, arena_folder, &arena_slot);
  if (!arena.ok) std::cerr << "game-made arena import failed: " << arena.error << '\n';
  assert(arena.ok && arena_slot == 0);

  const fs::path actual_account = root / "userdata" / std::string(kAccount);
  const fs::path expected_account = owner_root / "save-after" / std::string(kAccount);
  const bool exact = CompareSaveTrees(actual_account, expected_account);
  ReportMainSaveDetails(actual_account, expected_account, caw_folder / "SaveData.Dat",
                        root / "userdata" / std::string(kAccount) / std::string(kTitle) /
                            "Headers" / "00000001" / "SaveData.dat.header",
                        owner_root / "save-after" / std::string(kAccount) / std::string(kTitle) /
                            "Headers" / "00000001" / "SaveData.dat.header");
  std::error_code error;
  fs::remove_all(root, error);
  assert(!error);
  std::cout << "creation_save_test: " << (exact ? "PASS" : "FAIL")
            << " (save-before + game-made CAW 00 + arena 00 compared with the entire save-after tree)\n";
  return exact;
}

bool RunGeneralCreationRulesReproduction(const fs::path& owner_root) {
  const fs::path after_caw = owner_root / "save-after" / std::string(kAccount) / std::string(kTitle) /
                             "00000001" / "00CustomSuperStar.cas";
  const fs::path after_arena = owner_root / "save-after" / std::string(kAccount) / std::string(kTitle) /
                               "00000001" / "00CustomArena.car";
  const fs::path after2_caw = owner_root / "save-after2" / std::string(kAccount) / std::string(kTitle) /
                              "00000001" / "01CustomSuperStar.cas";
  const fs::path after2_arena = owner_root / "save-after2" / std::string(kAccount) / std::string(kTitle) /
                                "00000001" / "01CustomArena.car";
  const std::array<fs::path, 4> required{after_caw / "SaveData.Dat", after_arena / "SaveData.Dat",
                                         after2_caw / "SaveData.Dat", after2_arena / "SaveData.Dat"};
  for (const fs::path& path : required) {
    if (!fs::is_regular_file(path)) {
      std::cout << "creation_save_test: SKIP (general-rule sample is absent: " << path << ")\n";
      return false;
    }
  }

  bool first_exact = false;
  {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("wwe2k14-general-after-test-" + std::to_string(stamp));
    CopySampleToTemp(owner_root, "save-before", root);
    const Paths paths = MakePaths(root);
    uint32_t caw_slot = UINT32_MAX, arena_slot = UINT32_MAX;
    const Result caw = ImportCreationSave(paths, CreationSaveKind::kSuperstar, after_caw, &caw_slot);
    if (!caw.ok) std::cerr << "general-rule CAW slot-00 import failed: " << caw.error << '\n';
    assert(caw.ok && caw_slot == 0);
    const Result arena = ImportCreationSave(paths, CreationSaveKind::kArena, after_arena, &arena_slot);
    if (!arena.ok) std::cerr << "general-rule arena slot-00 import failed: " << arena.error << '\n';
    assert(arena.ok && arena_slot == 0);
    first_exact = CompareSaveTreesWithGeneralCreationRules(
        root / "userdata" / std::string(kAccount), owner_root / "save-after" / std::string(kAccount));
    std::error_code error;
    fs::remove_all(root, error);
    assert(!error);
  }

  bool second_exact = false;
  {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("wwe2k14-general-after2-test-" + std::to_string(stamp));
    CopySampleToTemp(owner_root, "save-after", root);
    const Paths paths = MakePaths(root);
    uint32_t caw_slot = UINT32_MAX, arena_slot = UINT32_MAX;
    const Result caw = ImportCreationSave(paths, CreationSaveKind::kSuperstar, after2_caw, &caw_slot);
    if (!caw.ok) std::cerr << "general-rule CAW slot-01 import failed: " << caw.error << '\n';
    assert(caw.ok && caw_slot == 1);
    const Result arena = ImportCreationSave(paths, CreationSaveKind::kArena, after2_arena, &arena_slot);
    if (!arena.ok) std::cerr << "general-rule arena slot-01 import failed: " << arena.error << '\n';
    assert(arena.ok && arena_slot == 1);
    second_exact = CompareSaveTreesWithGeneralCreationRules(
        root / "userdata" / std::string(kAccount), owner_root / "save-after2" / std::string(kAccount));
    std::error_code error;
    fs::remove_all(root, error);
    assert(!error);
  }
  const bool reproduced = first_exact && second_exact;
  std::cout << "creation_save_test: " << (reproduced ? "PASS" : "FAIL")
            << " (general rules reproduced after and after2 including the main plain checksum at 0x18; "
               "no sample switch required)\n";
  return reproduced;
}

bool RunOwnerRoundTrip(const fs::path& owner_root) {
  const fs::path source_folder = owner_root / "save-after" / std::string(kAccount) / std::string(kTitle) /
                                 "00000001" / "00CustomSuperStar.cas";
  const fs::path source_payload_path = source_folder / "SaveData.Dat";
  const fs::path source_creation_header_path = OwnerCreationHeader(owner_root, "save-after", "00CustomSuperStar.cas");
  const fs::path expected_before = owner_root / "main-before.plain";
  const fs::path source_main = OwnerMainFile(owner_root, "save-before");
  const fs::path source_header = OwnerHeaderFile(owner_root, "save-before");
  const std::array<fs::path, 5> required{source_payload_path, source_creation_header_path, source_main,
                                         source_header, expected_before};
  for (const fs::path& path : required) {
    if (!fs::is_regular_file(path)) {
      std::cout << "creation_save_test: SKIP (owner content sample is absent: " << path << ")\n";
      return false;
    }
  }

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("wwe2k14-save-before-creation-test-" + std::to_string(stamp));
  CopySampleToTemp(owner_root, "save-before", root);

  std::vector<uint8_t> expected_before_plain, original_file, original_header, source_payload, source_creation_header;
  assert(Read(expected_before, &expected_before_plain));
  assert(Read(source_main, &original_file));
  assert(Read(source_header, &original_header));
  assert(Read(source_payload_path, &source_payload));
  assert(Read(source_creation_header_path, &source_creation_header));

  const Paths paths = MakePaths(root);
  uint32_t imported_slot = 999;
  const auto supported_before = ListCreationSaveRecords(paths);
  assert(supported_before);
  const auto first_free = FindFreeCreationSaveSlot(paths, CreationSaveKind::kSuperstar);
  assert(first_free && *first_free == 0);
  const Result imported = ImportCreationSave(paths, CreationSaveKind::kSuperstar, source_folder, &imported_slot);
  if (!imported.ok) std::cerr << "save-before CAW import failed: " << imported.error << '\n';
  assert(imported.ok && imported_slot == 0);
  const fs::path current_main = OwnerMainFile(root, "userdata");
  const fs::path current_header = OwnerHeaderFile(root, "userdata");
  std::vector<uint8_t> imported_file, imported_header;
  assert(Read(current_main, &imported_file));
  assert(Read(current_header, &imported_header));
  AssertOnlyCawSlotsDiffer(UnpackMain(imported_file), expected_before_plain, {0});
  {
    // F1n: a single CAW import must write what the game writes for a new creation. Against the game-made
    // save-after (CAW 0 + arena 0 added together) only the arena record and the transient 0x76C9F flag may differ.
    std::vector<uint8_t> game_after_plain;
    assert(Read(owner_root / "main-after.plain", &game_after_plain));
    const std::vector<uint8_t> mine = UnpackMain(imported_file);
    assert(mine.size() == game_after_plain.size());
    size_t unexplained = 0;
    for (size_t offset = 0; offset < mine.size(); ++offset) {
      if (mine[offset] == game_after_plain[offset]) continue;
      const bool arena0 = offset >= 0x7368B && offset < 0x7368B + 0xCC;
      const bool checksum = offset >= 0x18 && offset < 0x1C;
      if (!arena0 && !checksum && offset != 0x76C9F) ++unexplained;
    }
    assert(unexplained == 0);
    assert(mine[0x76CC0] == 0x01 && mine[0x76CC1] == 0x90 && mine[0x1A6251] == 0x9C && mine[0xFE83F] == 0);
    // Thumbnail DDS header in the file prefix, like the game.
    assert(imported_file[0x1980] == 0x44 && imported_file[0x1981] == 0x44 && imported_file[0x1983] == 0x20);
    const auto named = ListCreationSaveRecords(paths);
    assert(named && !named->empty() && !named->front().name.empty());
    std::cout << "creation_save_test: PASS (single CAW import equals game-made save-after except arena 0 + 0x76C9F; list name '"
              << named->front().name << "')\n";
  }
  AssertExact(imported_header, original_header, "main header remains unchanged for single-record import");
  assert(imported_header[0x144] == 0x02);
  const fs::path installed_payload = OwnerCreationFile(root, "userdata", "00CustomSuperStar.cas");
  const fs::path installed_header = OwnerCreationHeader(root, "userdata", "00CustomSuperStar.cas");
  std::vector<uint8_t> installed_payload_bytes, installed_header_bytes;
  assert(Read(installed_payload, &installed_payload_bytes) && installed_payload_bytes == source_payload);
  assert(Read(installed_header, &installed_header_bytes) && installed_header_bytes == source_creation_header);
  const auto imported_records = ListCreationSaveRecords(paths);
  assert(imported_records && imported_records->size() == 1 && imported_records->front().slot_index == 0);

  const auto after_add_backups = ListBackups(paths);
  assert(after_add_backups.size() == 1 && !after_add_backups.front().automatic);
  assert(BackupContains(after_add_backups.front().file, original_file, original_header));

  const Result removed = RemoveCreationSave(paths, CreationSaveKind::kSuperstar, imported_slot);
  assert(removed.ok);
  std::vector<uint8_t> removed_file, removed_header;
  assert(Read(current_main, &removed_file));
  assert(Read(current_header, &removed_header));
  AssertExact(UnpackMain(removed_file), expected_before_plain, "save-before CAW add/remove");
  assert(std::equal(removed_file.begin(), removed_file.begin() + 0x2CA000, original_file.begin()));
  AssertExact(removed_header, original_header, "main header after remove");
  assert(!fs::exists(installed_payload.parent_path()) && !fs::exists(installed_header));
  const auto after_remove_backups = ListBackups(paths);
  assert(after_remove_backups.size() == 2);
  assert(std::any_of(after_remove_backups.begin(), after_remove_backups.end(), [&](const BackupInfo& backup) {
    return BackupContains(backup.file, imported_file, imported_header, "00CustomSuperStar.cas",
                          &source_payload, &source_creation_header);
  }));

  std::error_code error;
  fs::remove_all(root, error);
  assert(!error);
  std::cout << "creation_save_test: PASS (save-before accepted; CAW record, payload and renamed header installed; "
               "remove restored plain bytes and deleted both slot files)\n";
  return true;
}

fs::path CommunityCawFolder(const fs::path& root, std::string_view package_slot) {
  return root / (std::string(package_slot) + "CustomSuperStar.cas");
}

fs::path CommunityCawHeader(const fs::path& root, std::string_view package_slot) {
  return root.parent_path() / "Headers" / "00000001" /
         (std::string(package_slot) + "CustomSuperStar.cas.header");
}

std::string SlotDigits(uint32_t slot) {
  std::string value = std::to_string(slot);
  if (value.size() < 2) value.insert(value.begin(), '0');
  return value;
}

std::vector<uint8_t> ExpectedRenamedHeader(const std::vector<uint8_t>& source,
                                           std::string_view target_name, uint32_t target_slot) {
  std::vector<uint8_t> expected = source;
  std::fill(expected.begin() + 0x108, expected.begin() + 0x138, 0);
  std::copy(target_name.begin(), target_name.end(), expected.begin() + 0x108);
  std::string display_prefix = std::to_string(target_slot + 1);
  if (display_prefix.size() < 2) display_prefix.insert(display_prefix.begin(), '0');
  std::vector<uint16_t> units;
  for (size_t offset = 0; offset < 0x100; offset += 2) {
    const uint16_t unit = static_cast<uint16_t>((source[0x08 + offset] << 8) | source[0x09 + offset]);
    if (unit == 0) break;
    units.push_back(unit);
  }
  size_t prefix_end = 0;
  while (prefix_end < units.size() && units[prefix_end] >= '0' && units[prefix_end] <= '9') ++prefix_end;
  if (prefix_end != 0 && prefix_end < units.size() && units[prefix_end] == '.') {
    std::vector<uint16_t> renamed;
    for (unsigned char character : display_prefix) renamed.push_back(character);
    renamed.push_back('.');
    renamed.insert(renamed.end(), units.begin() + static_cast<std::ptrdiff_t>(prefix_end + 1), units.end());
    std::fill(expected.begin() + 0x08, expected.begin() + 0x108, 0);
    for (size_t index = 0; index < renamed.size(); ++index) {
      expected[0x08 + index * 2] = static_cast<uint8_t>(renamed[index] >> 8);
      expected[0x09 + index * 2] = static_cast<uint8_t>(renamed[index]);
    }
  }
  return expected;
}

void RunCommunityCawAgainstGameMadeAfter(const fs::path& owner_root, const fs::path& community_root) {
  const fs::path community_folder = CommunityCawFolder(community_root, "03");
  const fs::path arena_folder = owner_root / "save-after" / std::string(kAccount) / std::string(kTitle) /
                                "00000001" / "00CustomArena.car";
  const fs::path community_header_path = CommunityCawHeader(community_root, "03");
  if (!fs::is_regular_file(community_folder / "SaveData.Dat") ||
      !fs::is_regular_file(community_header_path) || !fs::is_regular_file(arena_folder / "SaveData.Dat")) {
    std::cout << "creation_save_test: SKIP (community CAW 03 or owner arena 00 sample is absent)\n";
    return;
  }

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("wwe2k14-community-vs-save-after-test-" + std::to_string(stamp));
  CopySampleToTemp(owner_root, "save-before", root);
  const Paths paths = MakePaths(root);
  uint32_t caw_slot = UINT32_MAX, arena_slot = UINT32_MAX;
  const Result caw = ImportCreationSave(paths, CreationSaveKind::kSuperstar, community_folder, &caw_slot);
  if (!caw.ok) std::cerr << "community CAW 03 import failed: " << caw.error << '\n';
  assert(caw.ok && caw_slot == 0);
  const Result arena = ImportCreationSave(paths, CreationSaveKind::kArena, arena_folder, &arena_slot);
  if (!arena.ok) std::cerr << "community-case arena 00 import failed: " << arena.error << '\n';
  assert(arena.ok && arena_slot == 0);

  const fs::path actual_account = root / "userdata" / std::string(kAccount);
  const fs::path expected_account = owner_root / "save-after" / std::string(kAccount);
  const bool entire_tree_exact = CompareSaveTrees(actual_account, expected_account);
  assert(!entire_tree_exact);  // This is the intentionally different community-CAW control.

  const fs::path current_caw_header = OwnerCreationHeader(root, "userdata", "00CustomSuperStar.cas");
  const fs::path owner_caw_header = OwnerCreationHeader(owner_root, "save-after", "00CustomSuperStar.cas");
  std::vector<uint8_t> current_header, owner_header, current_main_file, owner_main_file;
  assert(Read(current_caw_header, &current_header) && Read(owner_caw_header, &owner_header));
  assert(Read(OwnerMainFile(root, "userdata"), &current_main_file));
  assert(Read(OwnerMainFile(owner_root, "save-after"), &owner_main_file));
  PrintByteDiffRuns(current_header, owner_header, 0, std::min(current_header.size(), owner_header.size()),
                    "community CAW header vs game-made CAW 00 header");
  std::cout << "creation_save_test: community CAW header +0x144=" << std::hex
            << Le32(current_header, 0x144) << " game-made CAW header +0x144="
            << Le32(owner_header, 0x144) << std::dec << '\n';
  PrintByteDiffRuns(current_main_file, owner_main_file, 0, 0x2CA000,
                    "community-case main raw prefix vs game-made");
  constexpr size_t main_chunk = 0x2CA000;
  const uint32_t current_compressed = Le32(current_main_file, main_chunk + 8);
  const uint32_t owner_compressed = Le32(owner_main_file, main_chunk + 8);
  std::cout << "creation_save_test: community-case container actual_bytes=" << current_main_file.size()
            << " expected_bytes=" << owner_main_file.size() << " actual_compressed=" << current_compressed
            << " expected_compressed=" << owner_compressed << " actual_zlib=" << std::hex
            << static_cast<unsigned>(current_main_file[main_chunk + 16]) << ' '
            << static_cast<unsigned>(current_main_file[main_chunk + 17]) << " expected_zlib="
            << static_cast<unsigned>(owner_main_file[main_chunk + 16]) << ' '
            << static_cast<unsigned>(owner_main_file[main_chunk + 17]) << std::dec << '\n';
  const auto current_plain = UnpackMain(current_main_file);
  const auto owner_plain = UnpackMain(owner_main_file);
  constexpr size_t caw_base = 0x1E517;
  constexpr size_t caw_stride = 0xB6C;
  std::vector<uint8_t> current_non_caw = current_plain;
  std::vector<uint8_t> owner_non_caw = owner_plain;
  std::fill(current_non_caw.begin() + caw_base, current_non_caw.begin() + caw_base + caw_stride, 0);
  std::fill(owner_non_caw.begin() + caw_base, owner_non_caw.begin() + caw_base + caw_stride, 0);
  PrintByteDiffRuns(current_non_caw, owner_non_caw, 0, current_non_caw.size(),
                    "community-case non-CAW main plain vs game-made");
  PrintByteDiffRuns(current_plain, owner_plain, caw_base, caw_base + caw_stride,
                    "community CAW record vs game-made CAW record", 0);
  std::vector<uint8_t> current_main_header, owner_main_header;
  assert(Read(OwnerHeaderFile(root, "userdata"), &current_main_header));
  assert(Read(OwnerHeaderFile(owner_root, "save-after"), &owner_main_header));
  PrintByteDiffRuns(current_main_header, owner_main_header, 0,
                    std::min(current_main_header.size(), owner_main_header.size()),
                    "community-case main header vs game-made");
  std::error_code error;
  fs::remove_all(root, error);
  assert(!error);
  std::cout << "creation_save_test: INFO (community CAW case remains different; unmatched fields above are not assigned semantics)\n";
}

void CopyPostSampleToTemp(const fs::path& owner_root, const fs::path& temp_root) {
  const fs::path userdata = temp_root / "userdata";
  fs::create_directories(userdata);
  fs::copy(owner_root / "save-after2", userdata, fs::copy_options::recursive);
}

void AssertOnlyCawSlotsDiffer(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected,
                              const std::vector<uint32_t>& slots) {
  constexpr size_t caw_base = 0x1E517;
  constexpr size_t caw_stride = 0xB6C;
  constexpr size_t caw_size = 0xB6C;
  assert(actual.size() == expected.size());
  std::vector<bool> allowed(expected.size(), false);
  for (uint32_t slot : slots) {
    const size_t offset = caw_base + static_cast<size_t>(slot) * caw_stride;
    assert(offset + caw_size <= allowed.size());
    std::fill(allowed.begin() + static_cast<std::ptrdiff_t>(offset),
              allowed.begin() + static_cast<std::ptrdiff_t>(offset + caw_size), true);
  }
  // The main plain checksum (u32 BE at 0x18 = sum of bytes 0x1C..end) follows the record change.
  std::fill(allowed.begin() + 0x18, allowed.begin() + 0x1C, true);
  // CAW bookkeeping the game also writes (F1n): per-slot entry/marker, last-created index, first-creation flags,
  // and the second-creation arrays.
  for (uint32_t slot : slots) {
    const size_t entry = 0x76CC0 + static_cast<size_t>(slot) * 0x168;
    std::fill(allowed.begin() + static_cast<std::ptrdiff_t>(entry),
              allowed.begin() + static_cast<std::ptrdiff_t>(entry + 6), true);
    allowed[0x1A6251 + static_cast<size_t>(slot) * 0x40] = true;
  }
  allowed[0xFE83F] = true;
  allowed[0x1A4298] = true;
  allowed[0x11283C] = true;
  std::fill(allowed.begin() + 0xB6D24, allowed.begin() + 0xB6D2D, true);
  for (const size_t flag : {0x1A3655u, 0x1A3C14u, 0x1A3C2Bu, 0x1A3C8Bu, 0x1A3CFEu, 0x1A3D25u, 0x1A3D39u,
                            0x1A3D50u, 0x1A3D8Du}) {
    allowed[flag] = true;
  }
  std::fill(allowed.begin() + 0x75ECB, allowed.begin() + 0x75ECB + 159 * 4, true);
  std::fill(allowed.begin() + 0x76387, allowed.begin() + 0x76387 + 256 * 4, true);
  uint32_t sum = 0;
  for (size_t offset = 0x1C; offset < actual.size(); ++offset) sum += actual[offset];
  assert((uint32_t(actual[0x18]) << 24 | uint32_t(actual[0x19]) << 16 | uint32_t(actual[0x1A]) << 8 |
          uint32_t(actual[0x1B])) == sum);
  for (size_t offset = 0; offset < expected.size(); ++offset) {
    if (!allowed[offset]) assert(actual[offset] == expected[offset]);
  }
}

bool RunCommunityCawRoundTrip(const fs::path& owner_root, const fs::path& community_root) {
  const std::array<std::string_view, 6> slots{"03", "11", "12", "16", "22", "62"};
  for (const std::string_view slot : slots) {
    if (!fs::is_regular_file(CommunityCawFolder(community_root, slot) / "SaveData.Dat")) {
      std::cout << "creation_save_test: SKIP (community CAW input is absent: "
                << CommunityCawFolder(community_root, slot) << ")\n";
      return false;
    }
  }
  const fs::path expected_plain_path = owner_root / "main-after2.plain";
  if (!fs::is_regular_file(expected_plain_path)) {
    std::cout << "creation_save_test: SKIP (save-after2 plain sample is absent)\n";
    return false;
  }

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("wwe2k14-community-creation-test-" + std::to_string(stamp));
  CopyPostSampleToTemp(owner_root, root);
  const Paths paths = MakePaths(root);
  const fs::path current_main = OwnerMainFile(root, "userdata");
  const fs::path current_header = OwnerHeaderFile(root, "userdata");
  std::vector<uint8_t> expected_plain, original_file, original_header;
  assert(Read(expected_plain_path, &expected_plain));
  assert(Read(current_main, &original_file));
  assert(Read(current_header, &original_header));

  const auto initial_records = ListCreationSaveRecords(paths);
  assert(initial_records && initial_records->size() == 4);
  const std::vector<uint32_t> expected_slots{2, 4, 5, 6, 7, 8};
  std::vector<uint32_t> imported_slots;
  for (size_t index = 0; index < slots.size(); ++index) {
    const auto free_slot = FindFreeCreationSaveSlot(paths, CreationSaveKind::kSuperstar);
    assert(free_slot && *free_slot == expected_slots[index]);
    const std::string target_name = SlotDigits(expected_slots[index]) + "CustomSuperStar.cas";
    std::vector<uint8_t> source_payload, source_header;
    assert(Read(CommunityCawFolder(community_root, slots[index]) / "SaveData.Dat", &source_payload));
    assert(Read(CommunityCawHeader(community_root, slots[index]), &source_header));
    const std::vector<uint8_t> expected_creation_header =
        ExpectedRenamedHeader(source_header, target_name, expected_slots[index]);
    uint32_t selected_slot = UINT32_MAX;
    const Result imported = ImportCreationSave(paths, CreationSaveKind::kSuperstar,
                                                CommunityCawFolder(community_root, slots[index]),
                                                &selected_slot);
    if (!imported.ok) std::cerr << "community import " << slots[index] << " failed: " << imported.error << '\n';
    assert(imported.ok && selected_slot == expected_slots[index]);
    imported_slots.push_back(selected_slot);
    std::vector<uint8_t> installed_payload, installed_header;
    assert(Read(OwnerCreationFile(root, "userdata", target_name), &installed_payload));
    assert(Read(OwnerCreationHeader(root, "userdata", target_name), &installed_header));
    AssertExact(installed_payload, source_payload, "installed community CAW payload");
    AssertExact(installed_header, expected_creation_header, "renamed community CAW header");
    if (index == 0) {
      const auto backups = ListBackups(paths);
      assert(backups.size() == 1 && BackupContains(backups.front().file, original_file, original_header));
    }
  }

  std::vector<uint8_t> fully_imported_file, fully_imported_header;
  assert(Read(current_main, &fully_imported_file));
  assert(Read(current_header, &fully_imported_header));
  const auto imported_records = ListCreationSaveRecords(paths);
  assert(imported_records && imported_records->size() == 10);
  AssertOnlyCawSlotsDiffer(UnpackMain(fully_imported_file), expected_plain, imported_slots);
  const auto backups_before_remove = ListBackups(paths);
  assert(backups_before_remove.size() == slots.size());
  const std::string last_name = "08CustomSuperStar.cas";
  std::vector<uint8_t> last_payload, last_source_header;
  assert(Read(OwnerCreationFile(root, "userdata", last_name), &last_payload));
  assert(Read(OwnerCreationHeader(root, "userdata", last_name), &last_source_header));

  for (auto slot = imported_slots.rbegin(); slot != imported_slots.rend(); ++slot) {
    const Result removed = RemoveCreationSave(paths, CreationSaveKind::kSuperstar, *slot);
    if (!removed.ok) std::cerr << "community removal slot " << *slot << " failed: " << removed.error << '\n';
    assert(removed.ok);
  }

  std::vector<uint8_t> removed_file, removed_header;
  assert(Read(current_main, &removed_file));
  assert(Read(current_header, &removed_header));
  AssertExact(UnpackMain(removed_file), expected_plain, "six community CAWs add/remove/save-after2");
  assert(std::equal(removed_file.begin(), removed_file.begin() + 0x2CA000, original_file.begin()));
  AssertExact(removed_header, original_header, "save-after2 sidecar after community CAW round trip");
  const auto final_records = ListCreationSaveRecords(paths);
  assert(final_records && final_records->size() == 4);
  const auto all_backups = ListBackups(paths);
  assert(all_backups.size() == 12);
  assert(std::any_of(all_backups.begin(), all_backups.end(), [&](const BackupInfo& backup) {
    return BackupContains(backup.file, fully_imported_file, fully_imported_header, last_name,
                          &last_payload, &last_source_header);
  }));

  for (uint32_t slot : imported_slots) {
    const std::string name = SlotDigits(slot) + "CustomSuperStar.cas";
    assert(!fs::exists(OwnerCreationFile(root, "userdata", name).parent_path()));
    assert(!fs::exists(OwnerCreationHeader(root, "userdata", name)));
  }

  std::vector<uint8_t> sample_cas;
  assert(Read(CommunityCawFolder(community_root, "03") / "SaveData.Dat", &sample_cas));
  const auto high_caw = internal::AddKnownCreation(original_file, sample_cas, true, 99);
  assert(high_caw);
  const std::vector<uint8_t> high_caw_plain = UnpackMain(*high_caw);
  assert(!std::all_of(high_caw_plain.begin() + 0x1E517 + 99 * 0xB6C,
                      high_caw_plain.begin() + 0x1E517 + 100 * 0xB6C,
                      [](uint8_t byte) { return byte == 0; }));
  // Two-digit package names: there is no CAW slot 100.
  assert(!internal::AddKnownCreation(original_file, sample_cas, true, 100));
  // Other game data between CAW record 99 and the arena table (owner save 2026-10-26: non-zero bytes from 0x65B51)
  // must not make the save unsupported.
  std::vector<uint8_t> tail_plain = expected_plain;
  tail_plain[0x65B51] = 0x0C;
  tail_plain[0x73684] = 0x03;
  assert(internal::IsSupportedCreationPlain(tail_plain));
  const auto high_caw_removed = internal::RemoveKnownCreation(*high_caw, true, 99);
  AssertExact(UnpackMain(*high_caw_removed), expected_plain, "highest mapped CAW slot add/remove");

  std::error_code error;
  fs::remove_all(root, error);
  assert(!error);
  std::cout << "creation_save_test: PASS (six community CAWs imported into free two-digit slots, skipping existing payload folders, and removed; "
               "save-after2 plain restored byte-for-byte, 12 backups created with pre-change snapshots verified)\n";
  return true;
}

bool RunRealArenaImportMatrix(const fs::path& owner_root, const fs::path& full_userdata_root) {
  const fs::path source_caw = OwnerCreationFile(owner_root, "save-after2", "00CustomSuperStar.cas").parent_path();
  const fs::path source_arena = OwnerCreationFile(owner_root, "save-after2", "00CustomArena.car").parent_path();
  if (!fs::is_regular_file(source_caw / "SaveData.Dat") ||
      !fs::is_regular_file(source_arena / "SaveData.Dat")) {
    std::cerr << "creation_save_test: FAIL (real arena matrix source CAW/arena sample is absent)\n";
    return false;
  }

  struct TargetSave {
    const char* label;
    const char* sample;
  };
  constexpr std::array<TargetSave, 3> samples{{
      {"owner-save-before (0 created arenas)", "save-before"},
      {"owner-save-after (1 created arena)", "save-after"},
      {"owner-save-after2 (2 created arenas)", "save-after2"},
  }};
  const auto run_one = [&](const std::string& label, const fs::path& sample,
                           bool copy_full_userdata) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("wwe2k14-real-arena-matrix-" +
                                                        std::to_string(stamp));
    fs::create_directories(root);
    if (copy_full_userdata) {
      if (!fs::is_directory(sample)) return false;
      fs::copy(sample, root / "userdata", fs::copy_options::recursive);
    } else {
      CopySampleToTemp(owner_root, internal::PathToUtf8(sample), root);
    }
    const Paths paths = MakePaths(root);
    const auto before = ListCreationSaveRecords(paths);
    if (!before) {
      std::cerr << "creation_save_test: real-arena " << label << " unsupported before import\n";
      std::error_code ignored;
      fs::remove_all(root, ignored);
      return false;
    }
    const auto count_kind = [](const std::vector<CreationSaveRecord>& records, CreationSaveKind kind) {
      return static_cast<size_t>(std::count_if(records.begin(), records.end(),
          [kind](const CreationSaveRecord& record) { return record.kind == kind; }));
    };
    const size_t caws_before = count_kind(*before, CreationSaveKind::kSuperstar);
    const size_t arenas_before = count_kind(*before, CreationSaveKind::kArena);
    uint32_t caw_slot = UINT32_MAX, arena_slot = UINT32_MAX;
    const Result caw = ImportCreationSave(paths, CreationSaveKind::kSuperstar, source_caw, &caw_slot);
    if (!caw.ok) {
      std::cerr << "creation_save_test: real-arena " << label << " CAW import failed: " << caw.error << '\n';
      std::error_code ignored;
      fs::remove_all(root, ignored);
      return false;
    }
    const Result arena = ImportCreationSave(paths, CreationSaveKind::kArena, source_arena, &arena_slot);
    if (!arena.ok) {
      std::cerr << "creation_save_test: real-arena " << label << " arena import failed: " << arena.error << '\n';
      std::error_code ignored;
      fs::remove_all(root, ignored);
      return false;
    }
    const auto after = ListCreationSaveRecords(paths);
    const bool correct = after && caw_slot == arena_slot &&
                         count_kind(*after, CreationSaveKind::kSuperstar) == caws_before + 1 &&
                         count_kind(*after, CreationSaveKind::kArena) == arenas_before + 1;
    std::cout << "creation_save_test: real-arena " << label << " supported=yes caws=" << caws_before
              << " arenas=" << arenas_before << " caw_import=ok arena_import=ok paired_slot=" << caw_slot
              << " after_caws=" << (after ? count_kind(*after, CreationSaveKind::kSuperstar) : 0)
              << " after_arenas=" << (after ? count_kind(*after, CreationSaveKind::kArena) : 0)
              << " result=" << (correct ? "PASS" : "FAIL") << '\n';
    std::error_code error;
    fs::remove_all(root, error);
    return correct && !error;
  };

  bool passed = true;
  for (const TargetSave& item : samples) {
    passed &= run_one(item.label, item.sample, false);
  }
  passed &= run_one("owner-userdata fixture copy", full_userdata_root, true);
  return passed;
}

bool RunPlayerSaveImportModes(const fs::path& player_userdata, const fs::path& owner_root) {
  constexpr size_t table1_base = 0x75ECB, table1_bytes = 159 * 4;
  constexpr size_t table2_base = 0x76387, table2_bytes = 256 * 4;
  const fs::path first_caw = OwnerCreationFile(owner_root, "save-after", "00CustomSuperStar.cas").parent_path();
  const fs::path first_arena = OwnerCreationFile(owner_root, "save-after", "00CustomArena.car").parent_path();
  const fs::path second_caw = OwnerCreationFile(owner_root, "save-after2", "01CustomSuperStar.cas").parent_path();
  const fs::path second_arena = OwnerCreationFile(owner_root, "save-after2", "01CustomArena.car").parent_path();
  const fs::path source_caw = first_caw;
  const fs::path source_arena = first_arena;
  if (!fs::is_regular_file(first_caw / "SaveData.Dat") ||
      !fs::is_regular_file(first_arena / "SaveData.Dat") ||
      !fs::is_regular_file(second_caw / "SaveData.Dat") ||
      !fs::is_regular_file(second_arena / "SaveData.Dat")) {
    std::cerr << "creation_save_test: FAIL (owner CAW/arena sequence samples are absent)\n";
    return false;
  }

  const auto import_one = [&](std::string_view kind_label, CreationSaveKind kind,
                              const fs::path& source) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = player_userdata.parent_path().parent_path() /
        ("player-import-" + std::string(kind_label) + "-" + std::to_string(stamp));
    fs::create_directories(root);
    fs::copy(player_userdata, root / "userdata", fs::copy_options::recursive);
    const Paths paths = MakePaths(root);
    const auto before = ListCreationSaveRecords(paths);
    const fs::path main_path = OwnerMainFile(root, "userdata");
    std::vector<uint8_t> original_main;
    if (!before || !Read(main_path, &original_main)) {
      std::cerr << "creation_save_test: player-save " << kind_label
                << " copy is unsupported before import\n";
      return false;
    }
    const std::vector<uint8_t> original_plain = UnpackMain(original_main);
    uint32_t selected_slot = UINT32_MAX;
    const Result imported = ImportCreationSave(paths, kind, source, &selected_slot);
    if (!imported.ok) {
      std::cerr << "creation_save_test: player-save " << kind_label << " import failed: "
                << imported.error << '\n';
      return false;
    }
    const auto after = ListCreationSaveRecords(paths);
    std::vector<uint8_t> updated_main;
    if (!after || !Read(main_path, &updated_main)) return false;
    const std::vector<uint8_t> updated_plain = UnpackMain(updated_main);
    const auto count_kind = [](const std::vector<CreationSaveRecord>& records, CreationSaveKind selected) {
      return static_cast<size_t>(std::count_if(records.begin(), records.end(), [selected](const auto& record) {
        return record.kind == selected;
      }));
    };
    uint32_t checksum = 0;
    for (size_t offset = 0x1C; offset < updated_plain.size(); ++offset) checksum += updated_plain[offset];
    const bool checksum_ok = ReadBe32Safe(updated_plain, 0x18) == checksum;
    const bool tables_preserved = original_plain.size() == updated_plain.size() &&
        std::equal(original_plain.begin() + table1_base, original_plain.begin() + table1_base + table1_bytes,
                   updated_plain.begin() + table1_base) &&
        std::equal(original_plain.begin() + table2_base, original_plain.begin() + table2_base + table2_bytes,
                   updated_plain.begin() + table2_base);
    std::vector<uint8_t> excessive_value = original_plain;
    excessive_value[table2_base] = 3;
    const bool value_three_rejected = !internal::IsSupportedCreationPlain(excessive_value);
    const size_t imported_count = count_kind(*after, kind);
    const size_t other_count = count_kind(*after,
        kind == CreationSaveKind::kSuperstar ? CreationSaveKind::kArena : CreationSaveKind::kSuperstar);
    const bool valid = selected_slot == 0 && imported_count == 1 && other_count == 0 &&
        internal::IsSupportedCreationPlain(updated_plain) && checksum_ok && tables_preserved &&
        value_three_rejected;
    std::cout << "creation_save_test: player-save-copy " << kind_label
              << " supported_before=yes import=ok slot=" << selected_slot
              << " records_after=" << after->size() << " checksum=" << (checksum_ok ? "PASS" : "FAIL")
              << " validator=" << (internal::IsSupportedCreationPlain(updated_plain) ? "PASS" : "FAIL")
              << " observed_table_values_preserved=" << (tables_preserved ? "yes" : "no")
              << " reject_table_value_3=" << (value_three_rejected ? "PASS" : "FAIL")
              << " result=" << (valid ? "PASS" : "FAIL") << " copy=" << root << '\n';
    return valid;
  };

  const bool caw_ok = import_one("caw", CreationSaveKind::kSuperstar, source_caw);
  const bool arena_ok = import_one("arena", CreationSaveKind::kArena, source_arena);
  const auto run_sequence = [&](std::string_view label, bool use_owner_save,
                                uint8_t expected_initial_pair) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("wwe2k14-sequential-import-" + std::string(label) + "-" + std::to_string(stamp));
    fs::create_directories(root / "userdata");
    if (use_owner_save) {
      fs::copy(owner_root / "save-before", root / "userdata", fs::copy_options::recursive);
    } else {
      fs::copy(player_userdata, root / "userdata", fs::copy_options::recursive);
    }
    const Paths paths = MakePaths(root);
    std::vector<uint8_t> initial_main, initial_header;
    if (!Read(OwnerMainFile(root, "userdata"), &initial_main) ||
        !Read(OwnerHeaderFile(root, "userdata"), &initial_header) || initial_header.size() <= 0x144 ||
        initial_header[0x144] != expected_initial_pair) {
      std::cerr << "creation_save_test: sequential-import " << label << " initial save read/pair check failed\n";
      return false;
    }
    const std::vector<uint8_t> initial_plain = UnpackMain(initial_main);
    const auto initial_records = ListCreationSaveRecords(paths);
    if (!initial_records || !initial_records->empty()) {
      std::cerr << "creation_save_test: sequential-import " << label << " initial record list was not empty\n";
      return false;
    }

    const auto expected_table_value = [&](size_t base, size_t index, size_t step) {
      const uint32_t original = Le32(initial_plain, base + 4 * index);
      return step >= 3 && original == 0 ? 1u : original;
    };
    const auto check_state = [&](std::string_view step_name, size_t step, uint32_t selected_slot) {
      std::vector<uint8_t> current_main, current_header;
      if (!Read(OwnerMainFile(root, "userdata"), &current_main) ||
          !Read(OwnerHeaderFile(root, "userdata"), &current_header) || current_header.size() <= 0x144) return false;
      const std::vector<uint8_t> plain = UnpackMain(current_main);
      const auto records = ListCreationSaveRecords(paths);
      if (!records) return false;
      const size_t caws = static_cast<size_t>(std::count_if(records->begin(), records->end(), [](const auto& record) {
        return record.kind == CreationSaveKind::kSuperstar;
      }));
      const size_t arenas = static_cast<size_t>(std::count_if(records->begin(), records->end(), [](const auto& record) {
        return record.kind == CreationSaveKind::kArena;
      }));
      uint32_t checksum = 0;
      for (size_t offset = 0x1C; offset < plain.size(); ++offset) checksum += plain[offset];
      bool tables_match = true;
      for (size_t index = 0; index < 159; ++index) {
        tables_match = tables_match && Le32(plain, table1_base + 4 * index) ==
                                           expected_table_value(table1_base, index, step);
      }
      for (size_t index = 0; index < 256; ++index) {
        tables_match = tables_match && Le32(plain, table2_base + 4 * index) ==
                                           expected_table_value(table2_base, index, step);
      }
      const uint8_t expected_pair = use_owner_save
          ? (step == 1 ? 0x02 : (step < 4 ? 0x16 : 0x18))
          : expected_initial_pair;
      const bool checksum_ok = ReadBe32Safe(plain, 0x18) == checksum;
      const bool validator_ok = internal::IsSupportedCreationPlain(plain);
      const size_t expected_caws = (step + 1) / 2;
      const size_t expected_arenas = step / 2;
      const bool valid = selected_slot == (step <= 2 ? 0u : 1u) &&
          caws == expected_caws && arenas == expected_arenas && records->size() == step &&
          checksum_ok && validator_ok && tables_match && current_header[0x144] == expected_pair;
      std::cout << "creation_save_test: sequential-import " << label << " step=" << step_name
                << " slot=" << selected_slot << " records=" << records->size()
                << " caws=" << caws << " arenas=" << arenas
                << " checksum=" << (checksum_ok ? "PASS" : "FAIL")
                << " validator=" << (validator_ok ? "PASS" : "FAIL")
                << " tables=" << (tables_match ? "PASS" : "FAIL")
                << " pair_byte=0x" << std::hex << static_cast<unsigned>(current_header[0x144]) << std::dec
                << " listed=";
      for (size_t index = 0; index < records->size(); ++index) {
        if (index != 0) std::cout << ',';
        std::cout << (records->at(index).kind == CreationSaveKind::kSuperstar ? "CAW" : "arena")
                  << '@' << records->at(index).slot_index;
      }
      std::cout << " result=" << (valid ? "PASS" : "FAIL") << '\n';
      return valid;
    };

    const std::array<std::pair<std::string_view, const fs::path*>, 4> steps{{
        {"CAW-1", &first_caw}, {"arena-1", &first_arena},
        {"CAW-2", &second_caw}, {"arena-2", &second_arena}}};
    bool passed = true;
    for (size_t index = 0; index < steps.size(); ++index) {
      uint32_t slot = UINT32_MAX;
      const CreationSaveKind kind = index % 2 == 0
          ? CreationSaveKind::kSuperstar : CreationSaveKind::kArena;
      const Result imported = ImportCreationSave(paths, kind, *steps[index].second, &slot);
      if (!imported.ok) {
        std::cout << "creation_save_test: sequential-import " << label << " step=" << steps[index].first
                  << " import=failed reason=" << imported.error << " copy=" << root << '\n';
        return false;
      }
      passed = check_state(steps[index].first, index + 1, slot) && passed;
    }
    std::cout << "creation_save_test: sequential-import " << label << " result="
              << (passed ? "PASS" : "FAIL") << " copy=" << root << '\n';
    return passed;
  };

  const bool player_sequence = run_sequence("player", false, 0x7E);
  const bool owner_sequence = run_sequence("owner", true, 0x02);
  return caw_ok && arena_ok && player_sequence && owner_sequence;
}

bool RunArenaSlotRoundTrip(const fs::path& owner_root) {
  const fs::path arena_folder = owner_root / "save-after2" / std::string(kAccount) / std::string(kTitle) /
                                "00000001" / "00CustomArena.car";
  const fs::path arena = arena_folder / "SaveData.Dat";
  if (!fs::is_regular_file(arena)) {
    std::cout << "creation_save_test: SKIP (arena source is absent: " << arena << ")\n";
    return false;
  }
  const fs::path arena_header_path = OwnerCreationHeader(owner_root, "save-after2", "00CustomArena.car");
  std::vector<uint8_t> arena_bytes, source_header;
  assert(Read(arena, &arena_bytes) && Read(arena_header_path, &source_header));
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("wwe2k14-arena-slot-test-" + std::to_string(stamp));
  CopyPostSampleToTemp(owner_root, root);
  const Paths paths = MakePaths(root);
  const fs::path current_main = OwnerMainFile(root, "userdata");
  std::vector<uint8_t> expected_plain;
  assert(Read(owner_root / "main-after2.plain", &expected_plain));
  const std::string target_name = "02CustomArena.car";
  const std::vector<uint8_t> expected_header = ExpectedRenamedHeader(source_header, target_name, 2);
  uint32_t selected_slot = UINT32_MAX;
  const Result imported = ImportCreationSave(paths, CreationSaveKind::kArena, arena_folder, &selected_slot);
  if (!imported.ok) std::cerr << "arena import failed: " << imported.error << '\n';
  assert(imported.ok && selected_slot == 2);
  std::vector<uint8_t> installed_payload, installed_header;
  assert(Read(OwnerCreationFile(root, "userdata", target_name), &installed_payload));
  assert(Read(OwnerCreationHeader(root, "userdata", target_name), &installed_header));
  AssertExact(installed_payload, arena_bytes, "installed arena payload");
  AssertExact(installed_header, expected_header, "renamed arena header");
  const auto records = ListCreationSaveRecords(paths);
  assert(records && std::count_if(records->begin(), records->end(), [](const CreationSaveRecord& record) {
    return record.kind == CreationSaveKind::kArena;
  }) == 3);
  assert(RemoveCreationSave(paths, CreationSaveKind::kArena, selected_slot).ok);
  assert(!fs::exists(OwnerCreationFile(root, "userdata", target_name).parent_path()));
  assert(!fs::exists(OwnerCreationHeader(root, "userdata", target_name)));
  std::vector<uint8_t> removed_file;
  assert(Read(current_main, &removed_file));
  AssertExact(UnpackMain(removed_file), expected_plain, "arena slot-02 add/remove");
  const auto high_arena = internal::AddKnownCreation(removed_file, arena_bytes, false, 49);
  assert(high_arena);
  const std::vector<uint8_t> high_arena_plain = UnpackMain(*high_arena);
  assert(!std::all_of(high_arena_plain.begin() + 0x7368B + 49 * 0xCC,
                      high_arena_plain.begin() + 0x7368B + 50 * 0xCC,
                      [](uint8_t byte) { return byte == 0; }));
  const auto high_arena_removed = internal::RemoveKnownCreation(*high_arena, false, 49);
  assert(high_arena_removed);
  AssertExact(UnpackMain(*high_arena_removed), expected_plain, "highest mapped arena slot add/remove");
  std::error_code error;
  fs::remove_all(root, error);
  assert(!error);
  std::cout << "creation_save_test: PASS (arena import selected slot index 2; highest mapped slot 49 round-tripped)\n";
  return true;
}

bool RunUnknownLayoutNoWrite(const fs::path& owner_root, const fs::path& community_root) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("wwe2k14-unknown-creation-layout-test-" + std::to_string(stamp));
  CopyPostSampleToTemp(owner_root, root);
  const Paths paths = MakePaths(root);
  const fs::path main_path = OwnerMainFile(root, "userdata");
  const fs::path header_path = OwnerHeaderFile(root, "userdata");
  std::vector<uint8_t> unsupported_main, original_header;
  assert(Read(main_path, &unsupported_main));
  assert(Read(header_path, &original_header));
  unsupported_main[0x2CA004] ^= 0x01;  // An unrecognized terminal ZLIB chunk version.
  assert(internal::WriteBinaryFile(main_path, unsupported_main));
  const Result rejected = ImportCreationSave(paths, CreationSaveKind::kSuperstar,
                                              CommunityCawFolder(community_root, "03"));
  assert(!rejected.ok && rejected.error.find("Unsupported save layout") != std::string::npos);
  std::vector<uint8_t> after_main, after_header;
  assert(Read(main_path, &after_main) && Read(header_path, &after_header));
  AssertExact(after_main, unsupported_main, "unknown-layout main save unchanged");
  AssertExact(after_header, original_header, "unknown-layout sidecar unchanged");
  assert(ListBackups(paths).empty());
  std::error_code error;
  fs::remove_all(root, error);
  assert(!error);
  std::cout << "creation_save_test: PASS (unknown layout rejected with clear message and no write/backup)\n";
  return true;
}

bool RunInterruptedTransactionRecovery(const fs::path& owner_root) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("wwe2k14-creation-recovery-test-" + std::to_string(stamp));
  CopySampleToTemp(owner_root, "save-after2", root);
  const Paths paths = MakePaths(root);
  const fs::path main_path = OwnerMainFile(root, "userdata");
  const fs::path header_path = OwnerHeaderFile(root, "userdata");
  std::vector<uint8_t> original_main, original_header;
  assert(Read(main_path, &original_main) && Read(header_path, &original_header));

  const std::string id = "round3-test";
  const fs::path old_main = main_path.string() + ".creation-old-" + id;
  const fs::path old_header = header_path.string() + ".creation-old-" + id;
  assert(internal::WriteBinaryFile(old_main, original_main));
  assert(internal::WriteBinaryFile(old_header, original_header));
  std::vector<uint8_t> interrupted_main = original_main;
  std::vector<uint8_t> interrupted_header = original_header;
  interrupted_main[0] ^= 0x01;
  interrupted_header[0] ^= 0x01;
  assert(internal::WriteBinaryFile(main_path, interrupted_main));
  assert(internal::WriteBinaryFile(header_path, interrupted_header));

  const std::string new_name = "02CustomSuperStar.cas";
  const fs::path new_payload = OwnerCreationFile(root, "userdata", new_name);
  const fs::path new_creation_header = OwnerCreationHeader(root, "userdata", new_name);
  fs::create_directories(new_payload.parent_path());
  std::vector<uint8_t> marker_payload{1, 2, 3};
  assert(internal::WriteBinaryFile(new_payload, marker_payload));
  assert(internal::WriteBinaryFile(new_creation_header, marker_payload));

  const fs::path journal = paths.userdata_dir / ".creation-journal-round3-test";
  const auto relative = [&](const fs::path& path) {
    return internal::PathToUtf8(path.lexically_relative(paths.userdata_dir));
  };
  const std::string manifest = "WWE2K14-CREATION-TXN-1\nE\t" + relative(main_path) +
                               "\nE\t" + relative(header_path) + "\nN\t" + relative(new_payload) +
                               "\nN\t" + relative(new_creation_header) + "\n";
  assert(internal::WriteBinaryFile(journal, manifest.data(), manifest.size()));

  const Result recovered = RecoverCreationSaveFiles(paths);
  assert(recovered.ok);
  std::vector<uint8_t> restored_main, restored_header;
  assert(Read(main_path, &restored_main) && Read(header_path, &restored_header));
  AssertExact(restored_main, original_main, "recovered interrupted main save");
  AssertExact(restored_header, original_header, "recovered interrupted main header");
  assert(!fs::exists(new_payload) && !fs::exists(new_creation_header));
  assert(!fs::exists(journal) && !fs::exists(old_main) && !fs::exists(old_header));

  const fs::path stray_old_main = main_path.string() + ".creation-old-orphan";
  assert(internal::WriteBinaryFile(stray_old_main, original_main));
  interrupted_main = original_main;
  interrupted_main[1] ^= 0x01;
  assert(internal::WriteBinaryFile(main_path, interrupted_main));
  assert(RecoverCreationSaveFiles(paths).ok);
  assert(Read(main_path, &restored_main));
  AssertExact(restored_main, original_main, "recovered stray .creation-old main save");
  assert(!fs::exists(stray_old_main));

  std::error_code error;
  fs::remove_all(root, error);
  assert(!error);
  std::cout << "creation_save_test: PASS (journal and stray .creation-old recovery restore saves and remove uncommitted new slot files)\n";
  return true;
}

bool ExtractOwnerSaveZipToTemp(const fs::path& archive_path, const fs::path& temp_root) {
  internal::ZipReader zip;
  if (!zip.Open(archive_path) || zip.Count() == 0) return false;
  const fs::path userdata = temp_root / "userdata";
  std::error_code error;
  fs::create_directories(userdata, error);
  if (error) return false;
  std::set<std::string> written;
  bool found_main = false, found_header = false;
  for (size_t index = 0; index < zip.Count(); ++index) {
    if (zip.IsDirectory(index)) continue;
    mz_zip_archive_file_stat stat{};
    if (!zip.Stat(index, stat)) return false;
    const std::string name = stat.m_filename;
    constexpr std::string_view kPrefix = "userdata/";
    if (!name.starts_with(kPrefix)) continue;
    std::string relative_text = name.substr(kPrefix.size());
    std::replace(relative_text.begin(), relative_text.end(), '\\', '/');
    const fs::path relative = internal::PathFromUtf8(relative_text);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory()) return false;
    for (const fs::path& component : relative) {
      if (component.empty() || component == "." || component == ".." ||
          component.generic_string().find(':') != std::string::npos) return false;
    }
    const std::string normalized = internal::LowerAscii(relative.lexically_normal().generic_string());
    if (!written.insert(normalized).second) return false;
    std::vector<uint8_t> bytes;
    if (!zip.Extract(index, bytes) || bytes.size() != stat.m_uncomp_size) return false;
    static constexpr uint8_t kEmpty = 0;
    const void* data = bytes.empty() ? static_cast<const void*>(&kEmpty)
                                     : static_cast<const void*>(bytes.data());
    if (mz_crc32(0, static_cast<const mz_uint8*>(data), bytes.size()) != stat.m_crc32) return false;
    const fs::path target = userdata / relative.lexically_normal();
    fs::create_directories(target.parent_path(), error);
    if (error || !internal::WriteBinaryFile(target, bytes)) return false;
    const std::string target_name = internal::PathToUtf8(relative);
    found_main = found_main || target_name ==
        std::string(kAccount) + "/" + std::string(kTitle) + "/00000001/SaveData.dat/SaveData.Dat";
    found_header = found_header || target_name ==
        std::string(kAccount) + "/" + std::string(kTitle) + "/Headers/00000001/SaveData.dat.header";
  }
  return found_main && found_header;
}

void StoreBe32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value >> 24);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 16);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value);
}

void StoreLe16(std::vector<uint8_t>* bytes, size_t offset, uint16_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void StoreLe24(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 16);
}

uint64_t ReadOnlyStfsDataOffset(uint32_t block) {
  constexpr uint64_t kBlocksPerHashTable = 0xAA;
  constexpr uint64_t kBlockSize = 0x1000;
  constexpr uint64_t kDataBase = 0x1000;
  uint64_t base = kBlocksPerHashTable;
  uint64_t physical = block;
  for (unsigned level = 0; level < 3; ++level) {
    physical += (static_cast<uint64_t>(block) + base) / base;
    if (block < base) break;
    base *= kBlocksPerHashTable;
  }
  return kDataBase + (physical << 12);
}

uint64_t ReadOnlyStfsHashBlockNumber(uint32_t block) {
  constexpr uint32_t kBlocksPerHashTable = 0xAA;
  constexpr uint32_t kBlocksPerSecondHashLevel = 0x70E4;
  constexpr uint32_t kHashStep0 = kBlocksPerHashTable + 1;
  if (block < kBlocksPerHashTable) return 0;
  uint64_t number = (block / kBlocksPerHashTable) * kHashStep0;
  number += (block / kBlocksPerSecondHashLevel) + 1;
  if (block >= kBlocksPerSecondHashLevel) ++number;
  return number;
}

bool WriteReadOnlyConPackage(const fs::path& package_path,
                             const std::vector<uint8_t>& save_data,
                             const std::vector<uint8_t>& source_header,
                             uint32_t extra_chain_blocks = 0) {
  constexpr uint32_t kBlockSize = 0x1000;
  constexpr uint32_t kFileHeaderSize = 0x1000;
  constexpr uint32_t kVolumeDescriptor = 0x379;
  constexpr uint32_t kFileTableEntryOffset = 0;
  const uint32_t payload_blocks = static_cast<uint32_t>(
      save_data.size() / kBlockSize + (save_data.size() % kBlockSize != 0));
  const uint32_t file_chain_blocks = payload_blocks + extra_chain_blocks;
  const uint32_t total_blocks = file_chain_blocks + 1;  // block 0 is the single file-table block.
  const uint64_t last_data_offset = ReadOnlyStfsDataOffset(total_blocks - 1);
  if (source_header.size() < 0x148 || save_data.empty() ||
      last_data_offset > (std::numeric_limits<size_t>::max)() - kBlockSize) return false;
  std::vector<uint8_t> package(static_cast<size_t>(last_data_offset + kBlockSize), 0);
  std::copy_n("CON ", 4, package.begin());
  StoreBe32(&package, 0x340, kFileHeaderSize);
  StoreBe32(&package, 0x344, 1);  // Save content type.
  StoreBe32(&package, 0x360, 0x545408B2);
  package[kVolumeDescriptor] = 0x24;
  package[kVolumeDescriptor + 2] = 1;  // Read-only STFS hash layout.
  StoreLe16(&package, kVolumeDescriptor + 3, 1);
  StoreLe24(&package, kVolumeDescriptor + 5, 0);
  StoreBe32(&package, kVolumeDescriptor + 0x1C, total_blocks);
  std::copy_n(source_header.begin() + 8, 0x80, package.begin() + 0x411);

  const uint64_t table_offset = ReadOnlyStfsDataOffset(0);
  const std::string_view entry_name = "SaveData.Dat";
  std::copy(entry_name.begin(), entry_name.end(),
            package.begin() + static_cast<std::ptrdiff_t>(table_offset + kFileTableEntryOffset));
  package[table_offset + 0x28] = static_cast<uint8_t>(entry_name.size());
  StoreLe24(&package, static_cast<size_t>(table_offset + 0x2C), file_chain_blocks);
  StoreLe24(&package, static_cast<size_t>(table_offset + 0x2F), 1);
  package[table_offset + 0x32] = 0xFF;
  package[table_offset + 0x33] = 0xFF;
  StoreBe32(&package, static_cast<size_t>(table_offset + 0x34),
            static_cast<uint32_t>(save_data.size()));

  for (uint32_t block = 0; block < total_blocks; ++block) {
    const uint64_t hash_page = 0x1000 + (ReadOnlyStfsHashBlockNumber(block) << 12);
    const size_t info_offset = static_cast<size_t>(hash_page + (block % 0xAA) * 0x18 + 0x14);
    const uint32_t next = block + 1 < total_blocks ? block + 1 : 0xFFFFFF;
    StoreBe32(&package, info_offset, 0x80000000 | next);
  }
  for (uint32_t index = 0; index < payload_blocks; ++index) {
    const size_t source_offset = static_cast<size_t>(index) * kBlockSize;
    const size_t count = std::min<size_t>(kBlockSize, save_data.size() - source_offset);
    const uint64_t destination_offset = ReadOnlyStfsDataOffset(index + 1);
    std::copy_n(save_data.begin() + static_cast<std::ptrdiff_t>(source_offset), count,
                package.begin() + static_cast<std::ptrdiff_t>(destination_offset));
  }
  return internal::WriteBinaryFile(package_path, package);
}

bool RunCommunityConPackageEqualityAndImport(const fs::path& owner_root,
                                             const fs::path& community_root,
                                             const fs::path& packages_root) {
  const std::array<std::string_view, 6> slots{"03", "11", "12", "16", "22", "62"};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path synthetic_root = fs::temp_directory_path() /
      ("wwe2k14-synthetic-con-test-" + std::to_string(stamp));
  std::error_code temp_error;
  fs::create_directory(synthetic_root, temp_error);
  assert(!temp_error);
  std::vector<uint8_t> slot62_payload, slot62_header;
  assert(Read(CommunityCawFolder(community_root, "62") / "SaveData.Dat", &slot62_payload));
  assert(Read(CommunityCawHeader(community_root, "62"), &slot62_header));
  const fs::path synthetic_slot62 = synthetic_root / "62CustomSuperStar.cas";
  assert(WriteReadOnlyConPackage(synthetic_slot62, slot62_payload, slot62_header, 2));
  for (const std::string_view slot : slots) {
    const fs::path package = slot == "62"
                                 ? synthetic_slot62
                                 : packages_root / (std::string(slot) + "CustomSuperStar.cas");
    std::vector<uint8_t> extracted_payload, extracted_header, expected_payload, expected_header;
    std::string error;
    if (!internal::ExtractCreationConPackage(package, true, &extracted_payload, &extracted_header, &error)) {
      std::cerr << "CON extraction failed for " << package << ": " << error << '\n';
      return false;
    }
    assert(Read(CommunityCawFolder(community_root, slot) / "SaveData.Dat", &expected_payload));
    assert(Read(CommunityCawHeader(community_root, slot), &expected_header));
    AssertExact(extracted_payload, expected_payload, "CON SaveData.Dat equals r101 unpack");
    AssertExact(extracted_header, expected_header, "CON sidecar header equals r101 unpack");
  }
  std::error_code temp_remove_error;
  fs::remove_all(synthetic_root, temp_remove_error);
  assert(!temp_remove_error);
  std::cout << "creation_save_test: PASS (five RAR-extracted CON .cas files plus one large CON-chain fixture yielded byte-identical SaveData.Dat and headers to r101 up)\n";

  const fs::path source_package = packages_root / "03CustomSuperStar.cas";
  const fs::path root = fs::temp_directory_path() / ("wwe2k14-direct-con-import-" + std::to_string(stamp));
  CopySampleToTemp(owner_root, "save-before", root);
  const Paths paths = MakePaths(root);
  uint32_t selected_slot = UINT32_MAX;
  const Result imported = ImportCreationSave(paths, CreationSaveKind::kSuperstar,
                                              source_package, &selected_slot);
  if (!imported.ok) std::cerr << "direct CON import failed: " << imported.error << '\n';
  assert(imported.ok && selected_slot == 0);
  std::vector<uint8_t> installed_payload, source_payload, source_header, installed_header;
  assert(Read(OwnerCreationFile(root, "userdata", "00CustomSuperStar.cas"), &installed_payload));
  assert(Read(CommunityCawFolder(community_root, "03") / "SaveData.Dat", &source_payload));
  AssertExact(installed_payload, source_payload, "direct CON import payload");
  assert(Read(CommunityCawHeader(community_root, "03"), &source_header));
  const auto expected_header = ExpectedRenamedHeader(source_header, "00CustomSuperStar.cas", 0);
  assert(Read(OwnerCreationHeader(root, "userdata", "00CustomSuperStar.cas"), &installed_header));
  AssertExact(installed_header, expected_header, "direct CON import target header");
  const auto backups = ListBackups(paths);
  assert(backups.size() == 1);
  std::error_code error;
  fs::remove_all(root, error);
  assert(!error);
  std::cout << "creation_save_test: PASS (direct CON .cas import installed the exact payload and made a verified backup)\n";
  return true;
}

bool RunCreationZipImports(const fs::path& owner_root, const fs::path& community_root,
                           const fs::path& packages_root, const fs::path& rar_path) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("wwe2k14-creation-zip-import-" + std::to_string(stamp));
  fs::create_directories(root);
  const fs::path package_zip = root / "creation-package.zip";
  std::vector<uint8_t> package_bytes;
  assert(Read(packages_root / "03CustomSuperStar.cas", &package_bytes));
  assert(internal::CreateZip(package_zip, {{"download/03CustomSuperStar.cas", package_bytes}}));
  const fs::path first_save = root / "package-save";
  CopySampleToTemp(owner_root, "save-before", first_save);
  const Paths first_paths = MakePaths(first_save);
  uint32_t package_slot = UINT32_MAX;
  const Result package_import = ImportCreationSave(first_paths, CreationSaveKind::kSuperstar,
                                                   package_zip, &package_slot);
  if (!package_import.ok) std::cerr << "ZIP package import failed: " << package_import.error << '\n';
  assert(package_import.ok && package_slot == 0);
  std::vector<uint8_t> installed_payload, source_payload;
  assert(Read(OwnerCreationFile(first_save, "userdata", "00CustomSuperStar.cas"), &installed_payload));
  assert(Read(CommunityCawFolder(community_root, "03") / "SaveData.Dat", &source_payload));
  AssertExact(installed_payload, source_payload, "ZIP-wrapped CON import payload");

  const fs::path folder_zip = root / "creation-folder.zip";
  std::vector<uint8_t> source_header, folder_payload;
  assert(Read(CommunityCawFolder(community_root, "03") / "SaveData.Dat", &folder_payload));
  assert(Read(CommunityCawHeader(community_root, "03"), &source_header));
  assert(internal::CreateZip(folder_zip, {
      {"unpacked/03CustomSuperStar.cas/SaveData.Dat", folder_payload},
      {"unpacked/Headers/00000001/03CustomSuperStar.cas.header", source_header}}));
  const fs::path second_save = root / "folder-save";
  CopySampleToTemp(owner_root, "save-before", second_save);
  const Paths second_paths = MakePaths(second_save);
  uint32_t folder_slot = UINT32_MAX;
  const Result folder_import = ImportCreationSave(second_paths, CreationSaveKind::kSuperstar,
                                                  folder_zip, &folder_slot);
  if (!folder_import.ok) std::cerr << "ZIP folder import failed: " << folder_import.error << '\n';
  assert(folder_import.ok && folder_slot == 0);
  assert(Read(OwnerCreationFile(second_save, "userdata", "00CustomSuperStar.cas"), &installed_payload));
  AssertExact(installed_payload, folder_payload, "ZIP-wrapped unpacked folder payload");

  const fs::path arena_source = owner_root / "save-after2" / std::string(kAccount) /
      std::string(kTitle) / "00000001" / "00CustomArena.car";
  const fs::path arena_package = root / "00CustomArena.car";
  std::vector<uint8_t> arena_payload, arena_header;
  assert(Read(arena_source / "SaveData.Dat", &arena_payload));
  assert(Read(OwnerCreationHeader(owner_root, "save-after2", "00CustomArena.car"), &arena_header));
  std::vector<uint8_t> arena_package_header = arena_header;
  std::fill(arena_package_header.begin() + 0x144, arena_package_header.end(), 0xFF);
  assert(WriteReadOnlyConPackage(arena_package, arena_payload, arena_package_header));
  const fs::path arena_save = root / "arena-save";
  CopySampleToTemp(owner_root, "save-before", arena_save);
  const Paths arena_paths = MakePaths(arena_save);
  uint32_t arena_slot = UINT32_MAX;
  const Result arena_import = ImportCreationSave(arena_paths, CreationSaveKind::kArena,
                                                 arena_package, &arena_slot);
  if (!arena_import.ok) std::cerr << "direct CON .car import failed: " << arena_import.error << '\n';
  assert(arena_import.ok && arena_slot == 0);
  std::vector<uint8_t> installed_arena, installed_arena_header;
  assert(Read(OwnerCreationFile(arena_save, "userdata", "00CustomArena.car"), &installed_arena));
  AssertExact(installed_arena, arena_payload, "direct CON .car import payload");
  assert(Read(OwnerCreationHeader(arena_save, "userdata", "00CustomArena.car"), &installed_arena_header));
  AssertExact(installed_arena_header,
              ExpectedRenamedHeader(arena_package_header, "00CustomArena.car", 0),
              "direct CON .car import sidecar");

  const fs::path traversal_zip = root / "unsafe.zip";
  assert(internal::CreateZip(traversal_zip, {{"../outside/03CustomSuperStar.cas/SaveData.Dat", folder_payload}}));
  const fs::path unsafe_save = root / "unsafe-save";
  CopySampleToTemp(owner_root, "save-before", unsafe_save);
  const Paths unsafe_paths = MakePaths(unsafe_save);
  const Result rejected_zip = ImportCreationSave(unsafe_paths, CreationSaveKind::kSuperstar,
                                                  traversal_zip);
  assert(!rejected_zip.ok && ListBackups(unsafe_paths).empty());
  assert(!fs::exists(root / "outside" / "03CustomSuperStar.cas" / "SaveData.Dat"));

  const fs::path rar_save = root / "rar-save";
  CopySampleToTemp(owner_root, "save-before", rar_save);
  const Paths rar_paths = MakePaths(rar_save);
  const Result rejected_rar = ImportCreationSave(rar_paths, CreationSaveKind::kSuperstar, rar_path);
  assert(!rejected_rar.ok && rejected_rar.error.find("RAR4 archives are not supported") != std::string::npos);
  assert(ListBackups(rar_paths).empty());

  std::error_code error;
  fs::remove_all(root, error);
  assert(!error);
  std::cout << "creation_save_test: PASS (ZIP package/folder, direct .car CON package, traversal rejection, and explicit RAR4 status)\n";
  return true;
}

// F1: import/remove register + repair in the same operation (one backup); removed creations never come back.
bool RunAutoRegisterOwnerSave(const fs::path& owner_save_zip, const fs::path& community_root) {
  SetCreationAutoRegisterEnabled(true);
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("wwe2k14-autoreg-owner-" + std::to_string(stamp));
  assert(ExtractOwnerSaveZipToTemp(owner_save_zip, root));
  const Paths paths = MakePaths(root);
  const fs::path main_path = OwnerMainFile(root, "userdata");
  const size_t backups_before = ListBackups(paths).size();
  uint32_t slot = 999;
  CreationChangeSummary summary;
  const Result imported = ImportCreationSave(paths, CreationSaveKind::kSuperstar,
                                              CommunityCawFolder(community_root, "22"), &slot, &summary);
  if (!imported.ok) std::cerr << "auto-register import failed: " << imported.error << '\n';
  assert(imported.ok);
  assert(ListBackups(paths).size() == backups_before + 1);  // one backup for the whole operation
  assert(summary.registered == 6 && summary.repaired == 1 && summary.name == "KURT ANGLE");
  std::vector<uint8_t> main_file;
  assert(Read(main_path, &main_file));
  const auto plain = UnpackMain(main_file);
  assert(internal::IsSupportedCreationPlain(plain));
  {  // A CAW saved with a WWE Superstar's head stores +0x813 = 0 (+0x814 = 0xA0); it must stay supported. Other values
     // remain unrecognized.
    std::vector<uint8_t> superstar_head = plain;
    const size_t record = 0x1FBEF;  // slot 2, as in the owner's "BATISTA TEST" save
    assert(superstar_head[record + 0x6ED] == 1);
    superstar_head[record + 0x813] = 0x00;
    superstar_head[record + 0x814] = 0xA0;
    assert(internal::IsSupportedCreationPlain(superstar_head));
    superstar_head[record + 0x813] = 0x07;
    assert(!internal::IsSupportedCreationPlain(superstar_head));
  }
  const std::array<uint8_t, 6> entry{0x01, 0x90, 0x02, 0x8A, 0x02, 0xBC};
  for (const uint32_t s : {0u, 1u, 2u, 3u, 11u, 12u, 16u, 22u, 62u, slot}) {
    assert(std::equal(entry.begin(), entry.end(), plain.begin() + 0x76CC0 + s * 0x168));
    assert(plain[0x1A6251 + s * 0x40] == 0x9C);
  }
  // Remove a registered creation (12) and the new import: neither comes back, no extra backup beyond one each.
  for (const uint32_t victim : {12u, slot}) {
    CreationChangeSummary removed_summary;
    const size_t before = ListBackups(paths).size();
    const Result removed = RemoveCreationSave(paths, CreationSaveKind::kSuperstar, victim, &removed_summary);
    assert(removed.ok && !removed_summary.name.empty());
    assert(ListBackups(paths).size() == before + 1);
    const std::string name = SlotDigits(victim) + "CustomSuperStar.cas";
    assert(!fs::exists(OwnerCreationFile(root, "userdata", name).parent_path()));
    assert(!fs::exists(OwnerCreationHeader(root, "userdata", name)));
  }
  uint32_t registered = 9, repaired = 9;
  assert(RegisterExistingCreationSaves(paths, &registered, &repaired).ok);
  assert(registered == 0 && repaired == 0);
  const auto records = ListCreationSaveRecords(paths);
  assert(records);
  for (const CreationSaveRecord& record : *records) {
    assert(!(record.kind == CreationSaveKind::kSuperstar && (record.slot_index == 12 || record.slot_index == slot)));
  }
  std::cout << "creation_save_test: PASS (auto-register on import: registered " << summary.registered << ", repaired "
            << summary.repaired << ", name " << summary.name << ", one backup per operation; removed slots 12 and "
            << slot << " stay empty after Register)\n";
  std::error_code error;
  fs::remove_all(root, error);
  SetCreationAutoRegisterEnabled(false);
  return true;
}

// F1 follow-up: the owner's save made by the OLD launcher has Kurt (slot index 2) with a record but no entry/marker.
bool RunRepairOwnerSave(const fs::path& owner_save_zip) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("wwe2k14-repair-owner-" + std::to_string(stamp));
  assert(ExtractOwnerSaveZipToTemp(owner_save_zip, root));
  const Paths paths = MakePaths(root);
  const fs::path main_path = OwnerMainFile(root, "userdata");
  std::vector<uint8_t> before;
  assert(Read(main_path, &before));
  const auto before_plain = UnpackMain(before);
  assert(before_plain[0x76CC0 + 2 * 0x168] == 0 && before_plain[0x1A6251 + 2 * 0x40] == 0);
  uint32_t registered = 0, repaired = 0;
  const Result first = RegisterExistingCreationSaves(paths, &registered, &repaired);
  if (!first.ok) std::cerr << "repair register failed: " << first.error << '\n';
  assert(first.ok);
  std::vector<uint8_t> after;
  assert(Read(main_path, &after));
  const auto after_plain = UnpackMain(after);
  assert(internal::IsSupportedCreationPlain(after_plain));
  const std::array<uint8_t, 6> entry{0x01, 0x90, 0x02, 0x8A, 0x02, 0xBC};
  for (const uint32_t slot : {2u, 3u, 11u, 12u, 16u, 22u, 62u}) {
    assert(std::equal(entry.begin(), entry.end(), after_plain.begin() + 0x76CC0 + slot * 0x168));
    assert(after_plain[0x1A6251 + slot * 0x40] == 0x9C);
    assert(after[0x1980 + slot * 0x2200] == 0x44 && after[0x1980 + slot * 0x2200 + 3] == 0x20);
  }
  const auto records = ListCreationSaveRecords(paths);
  assert(records);
  assert(std::any_of(records->begin(), records->end(), [](const CreationSaveRecord& r) {
    return r.kind == CreationSaveKind::kSuperstar && r.slot_index == 2 && r.name == "KURT ANGLE";
  }));
  std::cout << "creation_save_test: PASS (owner 1026g save: registered " << registered << ", repaired " << repaired
            << "; slot 2 entry+marker+thumbnail written, save supported)\n";
  assert(repaired >= 1);
  // Second run changes nothing.
  uint32_t registered2 = 9, repaired2 = 9;
  assert(RegisterExistingCreationSaves(paths, &registered2, &repaired2).ok);
  std::vector<uint8_t> again;
  assert(Read(main_path, &again));
  assert(registered2 == 0 && repaired2 == 0 && again == after);
  std::error_code error;
  fs::remove_all(root, error);
  return true;
}

bool RunRegisterExistingOwnerSave(const fs::path& owner_save_zip) {
  const std::array<std::string_view, 6> slots{"03", "11", "12", "16", "22", "62"};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("wwe2k14-register-existing-" + std::to_string(stamp));
  assert(ExtractOwnerSaveZipToTemp(owner_save_zip, root));
  const Paths paths = MakePaths(root);
  const fs::path main_path = OwnerMainFile(root, "userdata");
  const fs::path main_header_path = OwnerHeaderFile(root, "userdata");
  std::vector<uint8_t> original_main, original_header;
  assert(Read(main_path, &original_main) && Read(main_header_path, &original_header));
  const auto original_plain = UnpackMain(original_main);
  assert(internal::IsSupportedCreationPlain(original_plain));
  const auto original_records = ListCreationSaveRecords(paths);
  assert(original_records);
  for (const std::string_view slot : slots) {
    assert(fs::is_regular_file(OwnerCreationFile(root, "userdata", std::string(slot) + "CustomSuperStar.cas")));
    const uint32_t numeric_slot = static_cast<uint32_t>((slot[0] - '0') * 10 + slot[1] - '0');
    assert(std::none_of(original_records->begin(), original_records->end(), [&](const CreationSaveRecord& record) {
      return record.kind == CreationSaveKind::kSuperstar && record.slot_index == numeric_slot;
    }));
  }
  std::vector<std::vector<uint8_t>> original_payloads, original_creation_headers;
  for (const std::string_view slot : slots) {
    const std::string name = std::string(slot) + "CustomSuperStar.cas";
    original_payloads.emplace_back();
    original_creation_headers.emplace_back();
    assert(Read(OwnerCreationFile(root, "userdata", name), &original_payloads.back()));
    assert(Read(OwnerCreationHeader(root, "userdata", name), &original_creation_headers.back()));
  }

  uint32_t registered = 0;
  const Result result = RegisterExistingCreationSaves(paths, &registered);
  if (!result.ok) std::cerr << "owner-save registration failed: " << result.error << '\n';
  assert(result.ok && registered == slots.size());
  std::vector<uint8_t> registered_main;
  assert(Read(main_path, &registered_main));
  const auto registered_plain = UnpackMain(registered_main);
  assert(internal::IsSupportedCreationPlain(registered_plain));
  const auto registered_records = ListCreationSaveRecords(paths);
  assert(registered_records && registered_records->size() == original_records->size() + slots.size());
  for (size_t index = 0; index < slots.size(); ++index) {
    const uint32_t numeric_slot = static_cast<uint32_t>((slots[index][0] - '0') * 10 + slots[index][1] - '0');
    assert(std::any_of(registered_records->begin(), registered_records->end(), [&](const CreationSaveRecord& record) {
      return record.kind == CreationSaveKind::kSuperstar && record.slot_index == numeric_slot;
    }));
    const std::string name = std::string(slots[index]) + "CustomSuperStar.cas";
    std::vector<uint8_t> payload, creation_header;
    assert(Read(OwnerCreationFile(root, "userdata", name), &payload));
    assert(Read(OwnerCreationHeader(root, "userdata", name), &creation_header));
    AssertExact(payload, original_payloads[index], "registered payload left byte-identical");
    AssertExact(creation_header, original_creation_headers[index], "registered header left byte-identical");
  }
  const auto backups = ListBackups(paths);
  assert(backups.size() == 1 && BackupContains(backups.front().file, original_main, original_header));
  uint32_t repeated_count = UINT32_MAX;
  assert(RegisterExistingCreationSaves(paths, &repeated_count).ok && repeated_count == 0);
  std::vector<uint8_t> repeated_main;
  assert(Read(main_path, &repeated_main));
  AssertExact(repeated_main, registered_main, "second register run is a no-op");
  assert(ListBackups(paths).size() == 1);

  std::error_code error;
  fs::remove_all(root, error);
  assert(!error);
  std::cout << "creation_save_test: PASS (owner save: six records registered at 3/11/12/16/22/62, supported state preserved, second run no-op)\n";

  const fs::path removal_root = fs::temp_directory_path() /
      ("wwe2k14-register-remove-" + std::to_string(stamp));
  assert(ExtractOwnerSaveZipToTemp(owner_save_zip, removal_root));
  const Paths removal_paths = MakePaths(removal_root);
  const std::string retained_name = "03CustomSuperStar.cas";
  for (const std::string_view slot : slots) {
    if (slot == "03") continue;
    const std::string name = std::string(slot) + "CustomSuperStar.cas";
    fs::remove_all(OwnerCreationFile(removal_root, "userdata", name).parent_path(), error);
    assert(!error);
    fs::remove(OwnerCreationHeader(removal_root, "userdata", name), error);
    assert(!error);
  }
  std::vector<uint8_t> before_single_main, before_single_header;
  assert(Read(OwnerMainFile(removal_root, "userdata"), &before_single_main));
  assert(Read(OwnerHeaderFile(removal_root, "userdata"), &before_single_header));
  const auto before_single_plain = UnpackMain(before_single_main);
  uint32_t single_count = 0;
  assert(RegisterExistingCreationSaves(removal_paths, &single_count).ok && single_count == 1);
  assert(RemoveCreationSave(removal_paths, CreationSaveKind::kSuperstar, 3).ok);
  std::vector<uint8_t> after_single_main, after_single_header;
  assert(Read(OwnerMainFile(removal_root, "userdata"), &after_single_main));
  assert(Read(OwnerHeaderFile(removal_root, "userdata"), &after_single_header));
  AssertExact(UnpackMain(after_single_main), before_single_plain, "register/remove restores prior main plain");
  AssertExact(after_single_header, before_single_header, "register/remove preserves main header");
  assert(!fs::exists(OwnerCreationFile(removal_root, "userdata", retained_name).parent_path()));
  assert(!fs::exists(OwnerCreationHeader(removal_root, "userdata", retained_name)));
  error.clear();
  fs::remove_all(removal_root, error);
  assert(!error);
  std::cout << "creation_save_test: PASS (register one existing slot then remove restores the exact prior plain)\n";
  return true;
}

std::array<uint8_t, 20> HashTree(const fs::path& root) {
  std::vector<fs::path> files;
  for (fs::recursive_directory_iterator iterator(root), end; iterator != end; ++iterator) {
    if (iterator->is_regular_file()) files.push_back(iterator->path());
  }
  std::sort(files.begin(), files.end(), [&](const fs::path& left, const fs::path& right) {
    return left.lexically_relative(root).generic_string() < right.lexically_relative(root).generic_string();
  });
  std::vector<uint8_t> manifest;
  for (const fs::path& file : files) {
    const std::string relative = file.lexically_relative(root).generic_string();
    manifest.insert(manifest.end(), relative.begin(), relative.end());
    manifest.push_back(0);
    std::vector<uint8_t> bytes;
    assert(Read(file, &bytes));
    const auto digest = internal::Sha1(bytes.data(), bytes.size());
    manifest.insert(manifest.end(), digest.begin(), digest.end());
  }
  return internal::Sha1(manifest.data(), manifest.size());
}

std::string Sha1Text(const std::array<uint8_t, 20>& digest) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (uint8_t byte : digest) output << std::setw(2) << static_cast<unsigned>(byte);
  return output.str();
}

bool MainChecksumMatches(const std::vector<uint8_t>& plain) {
  if (plain.size() < 0x1C) return false;
  uint32_t sum = 0;
  for (size_t offset = 0x1C; offset < plain.size(); ++offset) sum += plain[offset];
  return ReadBe32Safe(plain, 0x18) == sum;
}

std::vector<uint8_t> CreationRecordBytes(const std::vector<uint8_t>& plain,
                                        CreationSaveKind kind, uint32_t slot) {
  const size_t offset = kind == CreationSaveKind::kSuperstar
                            ? 0x1E517 + static_cast<size_t>(slot) * 0xB6C
                            : 0x7368B + static_cast<size_t>(slot) * 0xCC;
  const size_t size = kind == CreationSaveKind::kSuperstar ? 0xB6C : 0xCC;
  assert(offset <= plain.size() && size <= plain.size() - offset);
  return std::vector<uint8_t>(plain.begin() + static_cast<std::ptrdiff_t>(offset),
                              plain.begin() + static_cast<std::ptrdiff_t>(offset + size));
}

bool VerifyExportedStfs(const fs::path& package, std::string_view display_name, size_t payload_size,
                        const fs::path& payload = {}) {
  std::string command = "python3 -I \"" +
      (fs::path(__FILE__).parent_path() / "verify_stfs.py").string() + "\" \"" +
      package.string() + "\" 545408B2 \"" + std::string(display_name) + "\" " +
      std::to_string(payload_size);
  if (!payload.empty()) command += " \"" + payload.string() + "\"";
  return std::system(command.c_str()) == 0;
}

// X1 F2 (owner laptop save, 2026-10-31): game-edited CAW records differ from the CAS rebuild in a few bytes but carry
// the payload's checksum; every payload must validate against its own slot and no payload against another slot.
bool RunEditedCawExportPairing(const fs::path& userdata_root) {
  const fs::path content = userdata_root / "B13EBABEBABEBABE" / "545408B2" / "00000001";
  std::vector<uint8_t> main_file;
  if (!Read(content / "SaveData.dat" / "SaveData.Dat", &main_file)) {
    std::cerr << "creation_export_test: edited-CAW fixture main save missing under " << content << '\n';
    return false;
  }
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> superstars;
  size_t arenas = 0;
  for (const auto& entry : fs::directory_iterator(content)) {
    const std::string name = entry.path().filename().string();
    const bool superstar = name.size() == 21 && name.substr(2) == "CustomSuperStar.cas";
    const bool arena = name.size() == 17 && name.substr(2) == "CustomArena.car";
    if (!superstar && !arena) continue;
    const uint32_t slot = static_cast<uint32_t>(std::stoul(name.substr(0, 2)));
    std::vector<uint8_t> payload;
    if (!Read(entry.path() / "SaveData.Dat", &payload) ||
        !internal::CreationFileMatchesMainRecord(main_file, payload, superstar, slot)) {
      std::cerr << "creation_export_test: edited-CAW fixture " << name << " does not validate against its slot\n";
      return false;
    }
    // Export it for real (arenas with their own images pass 170 STFS blocks and need the level-1 hash table) and
    // read the package back with the launcher's reader and the independent verifier.
    const fs::path out_dir = fs::temp_directory_path() / "wwe2k14-edited-caw-export";
    fs::create_directories(out_dir);
    const fs::path exported = out_dir / (name.substr(0, name.size() - 4) + (superstar ? ".cas" : ".car"));
    CreationExportSummary summary;
    const Result result = ExportCreation(MakePaths(userdata_root.parent_path()),
                                         superstar ? CreationSaveKind::kSuperstar : CreationSaveKind::kArena, slot,
                                         exported, &summary);
    std::vector<uint8_t> extracted, header;
    std::string extract_error;
    const std::string display = SlotDigits(slot + 1) + "." +
        (summary.name.empty() ? (superstar ? "CUSTOM SUPERSTAR" : "CUSTOM ARENA") : summary.name);
    if (!result.ok ||
        !internal::ExtractCreationConPackage(exported, superstar, &extracted, &header, &extract_error) ||
        extracted != payload ||
        !VerifyExportedStfs(exported, display, payload.size(), entry.path() / "SaveData.Dat")) {
      std::cerr << "creation_export_test: edited-CAW fixture " << name << " export/read-back failed: " << result.error
                << extract_error << " stage=" << result.stage << '\n';
      return false;
    }
    std::cout << "creation_export_test: edited-CAW export " << name << " payload_bytes=" << payload.size()
              << " package_bytes=" << fs::file_size(exported) << " read_back=identical verifier=OK\n";
    fs::remove(exported);
    if (superstar) superstars.emplace_back(slot, std::move(payload));
    else ++arenas;
  }
  for (const auto& [slot, payload] : superstars) {
    for (const auto& other : superstars) {
      if (other.first != slot && internal::CreationFileMatchesMainRecord(main_file, payload, true, other.first)) {
        std::cerr << "creation_export_test: CAW payload of slot " << slot << " validated against slot " << other.first
                  << '\n';
        return false;
      }
    }
  }
  std::cout << "creation_export_test: edited-CAW pairing PASS superstars=" << superstars.size() << " arenas=" << arenas
            << " (each validates only against its own slot)\n";
  return !superstars.empty();
}

bool RunCreationExportContainmentRegressions(const Paths& source_paths, const fs::path& source_root,
                                             uint32_t slot) {
  const auto refuses_inside = [&](const Paths& paths, const fs::path& output, std::string_view label) {
    const Result result = ExportCreation(paths, CreationSaveKind::kSuperstar, slot, output);
    if (result.ok || result.stage != "destination-in-userdata" || fs::exists(output)) {
      std::cerr << "creation_export_test: containment " << label << " was not refused safely"
                << " stage=" << result.stage << " output_exists=" << fs::exists(output) << '\n';
      return false;
    }
    return true;
  };

  const fs::path deep_inside = source_paths.userdata_dir / "deep" / "nested" / "blocked.cas";
  if (!refuses_inside(source_paths, deep_inside, "deep-inside")) return false;

  Paths root_equal_paths = source_paths;
  root_equal_paths.userdata_dir = source_root / "userdata-root.cas";
  if (!refuses_inside(root_equal_paths, root_equal_paths.userdata_dir, "root-equality")) return false;

  const fs::path escape_dir = source_root / "outside-userdata";
  fs::create_directories(escape_dir);
  const fs::path escaped_output = source_paths.userdata_dir / ".." / "outside-userdata" / "escaped.cas";
  const Result escaped = ExportCreation(source_paths, CreationSaveKind::kSuperstar, slot, escaped_output);
  if (!escaped.ok || !fs::is_regular_file(escaped_output)) {
    std::cerr << "creation_export_test: containment '..' escape was not allowed: " << escaped.error
              << " stage=" << escaped.stage << '\n';
    return false;
  }

  const fs::path common_prefix_dir = source_root / "userdata2";
  fs::create_directories(common_prefix_dir);
  const fs::path common_prefix_output = common_prefix_dir / "sibling.cas";
  const Result common_prefix = ExportCreation(source_paths, CreationSaveKind::kSuperstar, slot,
                                               common_prefix_output);
  if (!common_prefix.ok || !fs::is_regular_file(common_prefix_output)) {
    std::cerr << "creation_export_test: containment userdata2 sibling was not allowed: "
              << common_prefix.error << " stage=" << common_prefix.stage << '\n';
    return false;
  }

#ifdef _WIN32
  Paths cross_drive_paths = source_paths;
  cross_drive_paths.userdata_dir = fs::path(L"D:\\wwe2k14-x1f-userdata");
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path cross_drive_output = fs::path(
      L"C:\\wwe2k14-x1f-cross-drive-") += std::to_wstring(stamp) + L".cas";
  const Result cross_drive = ExportCreation(cross_drive_paths, CreationSaveKind::kSuperstar, slot,
                                             cross_drive_output);
  if (cross_drive.stage == "destination-in-userdata" || fs::exists(cross_drive_output)) {
    std::cerr << "creation_export_test: containment cross-drive C: candidate was treated as inside D: userdata"
              << " stage=" << cross_drive.stage << '\n';
    return false;
  }
#endif

  std::cout << "creation_export_test: containment PASS (deep inside and userdata root refused; '..' escape and "
               "userdata2 sibling allowed"
#ifdef _WIN32
            << "; C: candidate vs D: userdata allowed"
#endif
            << ")\n";
  return true;
}

bool RunCreationExportFailureStageRegressions(const fs::path& owner_root, uint32_t slot) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string package_name = SlotDigits(slot) + "CustomSuperStar.cas";
  const auto make_copy = [&](std::string_view label) {
    const fs::path root = fs::temp_directory_path() /
        ("wwe2k14-export-stage-" + std::string(label) + "-" + std::to_string(stamp));
    fs::create_directories(root);
    CopySampleToTemp(owner_root, "save-after", root);
    fs::create_directories(root / "exports");
    return root;
  };
  const auto check_failure = [&](const fs::path& root, const fs::path& output,
                                 const Result& result, std::string_view expected_stage) {
    const bool okay = !result.ok && result.stage == expected_stage && !fs::exists(output);
    if (!okay) {
      std::cerr << "creation_export_test: expected stage=" << expected_stage << " got=" << result.stage
                << " ok=" << result.ok << " output_exists=" << fs::exists(output) << '\n';
    }
    std::error_code error;
    fs::remove_all(root, error);
    if (error) {
      std::cerr << "creation_export_test: could not remove stage-test copy " << root << ": "
                << error.message() << '\n';
      return false;
    }
    return okay;
  };

  const fs::path missing_root = make_copy("missing");
  const fs::path missing_payload = OwnerCreationFile(missing_root, "userdata", package_name);
  std::error_code error;
  if (!fs::remove(missing_payload, error) || error) {
    std::cerr << "creation_export_test: could not remove payload for missing-stage case\n";
    fs::remove_all(missing_root, error);
    return false;
  }
  const fs::path missing_output = missing_root / "exports" / "missing.cas";
  const Result missing = ExportCreation(MakePaths(missing_root), CreationSaveKind::kSuperstar,
                                         slot, missing_output);
  if (!check_failure(missing_root, missing_output, missing, "payload-missing")) return false;

  const fs::path unreadable_root = make_copy("unreadable");
  const fs::path unreadable_payload = OwnerCreationFile(unreadable_root, "userdata", package_name);
  const fs::path unreadable_output = unreadable_root / "exports" / "unreadable.cas";
  Result unreadable;
#ifdef _WIN32
  const HANDLE locked_payload = CreateFileW(unreadable_payload.c_str(), GENERIC_READ, 0, nullptr,
                                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (locked_payload == INVALID_HANDLE_VALUE) {
    std::cerr << "creation_export_test: could not lock payload for unreadable-stage case\n";
    fs::remove_all(unreadable_root, error);
    return false;
  }
  unreadable = ExportCreation(MakePaths(unreadable_root), CreationSaveKind::kSuperstar,
                              slot, unreadable_output);
  CloseHandle(locked_payload);
#else
  const fs::perms original_permissions = fs::status(unreadable_payload, error).permissions();
  if (error) {
    std::cerr << "creation_export_test: could not inspect payload permissions for unreadable-stage case\n";
    fs::remove_all(unreadable_root, error);
    return false;
  }
  fs::permissions(unreadable_payload, fs::perms::none, fs::perm_options::replace, error);
  if (error) {
    std::cerr << "creation_export_test: could not restrict payload for unreadable-stage case\n";
    fs::remove_all(unreadable_root, error);
    return false;
  }
  std::vector<uint8_t> permission_probe;
  if (Read(unreadable_payload, &permission_probe)) {
    fs::permissions(unreadable_payload, original_permissions, fs::perm_options::replace, error);
    std::cerr << "creation_export_test: current user can bypass unreadable payload permissions\n";
    fs::remove_all(unreadable_root, error);
    return false;
  }
  unreadable = ExportCreation(MakePaths(unreadable_root), CreationSaveKind::kSuperstar,
                              slot, unreadable_output);
  fs::permissions(unreadable_payload, original_permissions, fs::perm_options::replace, error);
  if (error) {
    std::cerr << "creation_export_test: could not restore payload permissions after unreadable-stage case\n";
    fs::remove_all(unreadable_root, error);
    return false;
  }
#endif
  if (!check_failure(unreadable_root, unreadable_output, unreadable, "payload-unreadable")) return false;

  const fs::path mismatch_root = make_copy("mismatch");
  const fs::path mismatch_payload = OwnerCreationFile(mismatch_root, "userdata", package_name);
  const std::array<uint8_t, 4> wrong_payload{0x58, 0x31, 0x46, 0x03};
  if (!internal::WriteBinaryFile(mismatch_payload, wrong_payload.data(), wrong_payload.size())) {
    std::cerr << "creation_export_test: could not replace payload for record-mismatch case\n";
    fs::remove_all(mismatch_root, error);
    return false;
  }
  const fs::path mismatch_output = mismatch_root / "exports" / "mismatch.cas";
  const Result mismatch = ExportCreation(MakePaths(mismatch_root), CreationSaveKind::kSuperstar,
                                         slot, mismatch_output);
  if (!check_failure(mismatch_root, mismatch_output, mismatch, "record-mismatch")) return false;

  if (missing.stage == unreadable.stage || missing.stage == mismatch.stage || unreadable.stage == mismatch.stage) {
    std::cerr << "creation_export_test: payload failure stages are not distinct\n";
    return false;
  }
  std::cout << "creation_export_test: stages PASS missing=" << missing.stage
            << " unreadable=" << unreadable.stage << " mismatch=" << mismatch.stage
            << " no_destination_files=yes\n";
  return true;
}

bool RunCreationExportRoundTrips(const fs::path& owner_root, const fs::path& player_userdata_root) {
  struct Sample {
    const char* name;
  };
  constexpr std::array<Sample, 2> samples{{{"save-after"}, {"save-after2"}}};
  size_t round_trips = 0;
  std::optional<std::array<uint8_t, 20>> player_fixture_hash;
  if (!player_userdata_root.empty()) player_fixture_hash = HashTree(player_userdata_root);
  for (const Sample& sample : samples) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path source_root = fs::temp_directory_path() /
        (std::string("wwe2k14-creation-export-source-") + sample.name + "-" + std::to_string(stamp));
    fs::create_directories(source_root);
    CopySampleToTemp(owner_root, sample.name, source_root);
    Paths source_paths = MakePaths(source_root);
    const auto source_records = ListCreationSaveRecords(source_paths);
    if (!source_records) {
      std::cerr << "creation_export_test: copied " << sample.name << " is not a supported save\n";
      return false;
    }
    const auto source_hash = HashTree(source_paths.userdata_dir);
    const auto run_one = [&](CreationSaveKind kind, const CreationSaveRecord& source_record) {
      const bool superstar = kind == CreationSaveKind::kSuperstar;
      const std::string package_name = SlotDigits(source_record.slot_index) +
          (superstar ? "CustomSuperStar.cas" : "CustomArena.car");
      const fs::path source_payload_path = OwnerCreationFile(source_root, "userdata", package_name);
      std::vector<uint8_t> source_payload, source_main_file;
      if (!Read(source_payload_path, &source_payload) ||
          !Read(OwnerMainFile(source_root, "userdata"), &source_main_file)) {
        std::cerr << "creation_export_test: source files missing for " << package_name << '\n';
        return false;
      }
      const std::vector<uint8_t> source_plain = UnpackMain(source_main_file);
      if (!MainChecksumMatches(source_plain) ||
          !internal::IsSupportedCreationPlain(source_plain) ||
          !internal::CreationFileMatchesMainRecord(source_main_file, source_payload, superstar,
                                                   source_record.slot_index)) {
        std::cerr << "creation_export_test: source validator/checksum failed for " << package_name << '\n';
        return false;
      }

      const fs::path export_dir = source_root / "exports";
      fs::create_directories(export_dir);
      const std::string base = source_record.name.empty() ? (superstar ? "CAW" : "Arena") : source_record.name;
      const fs::path exported = export_dir / (base + (superstar ? ".cas" : ".car"));
      CreationExportSummary export_summary;
      const Result export_result = ExportCreation(source_paths, kind, source_record.slot_index,
                                                  exported, &export_summary);
      if (!export_result.ok) {
        std::cerr << "creation_export_test: export failed for " << package_name << ": "
                  << export_result.error << '\n';
        return false;
      }
      if (export_summary.name != source_record.name || export_summary.slot_index != source_record.slot_index) {
        std::cerr << "creation_export_test: export summary mismatch for " << package_name << '\n';
        return false;
      }
      std::vector<uint8_t> package_payload, package_header;
      std::string package_error;
      if (!internal::ExtractCreationConPackage(exported, superstar, &package_payload,
                                               &package_header, &package_error) ||
          package_payload != source_payload) {
        std::cerr << "creation_export_test: package extraction mismatch for " << package_name
                  << ": " << package_error << '\n';
        return false;
      }
      const std::string display_name = SlotDigits(source_record.slot_index + 1) + "." +
          (source_record.name.empty() ? (superstar ? "CUSTOM SUPERSTAR" : "CUSTOM ARENA")
                                      : source_record.name);
      if (!VerifyExportedStfs(exported, display_name, source_payload.size())) {
        std::cerr << "creation_export_test: independent STFS checksum verifier failed for "
                  << exported << '\n';
        return false;
      }

      if (HashTree(source_paths.userdata_dir) != source_hash) {
        std::cerr << "creation_export_test: export mutated copied source userdata for " << package_name << '\n';
        return false;
      }

      const fs::path imported_root = source_root / ("import-" +
          std::string(superstar ? "caw-" : "arena-") + std::to_string(source_record.slot_index));
      fs::create_directories(imported_root);
      if (player_userdata_root.empty()) {
        CopySampleToTemp(owner_root, "save-before", imported_root);
      } else {
        CopyUserdataToTemp(player_userdata_root, imported_root);
      }
      Paths imported_paths = MakePaths(imported_root);
      uint32_t imported_slot = UINT32_MAX;
      CreationChangeSummary import_summary;
      const Result import_result = ImportCreationSave(imported_paths, kind, exported,
                                                      &imported_slot, &import_summary);
      if (!import_result.ok) {
        std::cerr << "creation_export_test: reimport failed for " << package_name << ": "
                  << import_result.error << '\n';
        return false;
      }
      std::vector<uint8_t> imported_payload, imported_main_file;
      const std::string imported_name = SlotDigits(imported_slot) +
          (superstar ? "CustomSuperStar.cas" : "CustomArena.car");
      if (!Read(OwnerCreationFile(imported_root, "userdata", imported_name), &imported_payload) ||
          !Read(OwnerMainFile(imported_root, "userdata"), &imported_main_file)) {
        std::cerr << "creation_export_test: reimported save files missing for " << package_name << '\n';
        return false;
      }
      const std::vector<uint8_t> imported_plain = UnpackMain(imported_main_file);
      const auto imported_records = ListCreationSaveRecords(imported_paths);
      const bool listed = imported_records && std::any_of(
          imported_records->begin(), imported_records->end(), [&](const CreationSaveRecord& record) {
            return record.kind == kind && record.slot_index == imported_slot &&
                   record.name == source_record.name;
          });
      if (imported_slot != 0 || imported_payload != source_payload ||
          CreationRecordBytes(imported_plain, kind, imported_slot) !=
              CreationRecordBytes(source_plain, kind, source_record.slot_index) ||
          !MainChecksumMatches(imported_plain) || !internal::IsSupportedCreationPlain(imported_plain) ||
          !listed || import_summary.name != source_record.name) {
        std::cerr << "creation_export_test: byte, checksum, validator, or listing mismatch for "
                  << package_name << '\n';
        return false;
      }

      std::cout << "creation_export_test: ROUNDTRIP sample=" << sample.name
                << " kind=" << (superstar ? "CAW" : "Arena")
                << " source_slot=" << source_record.slot_index
                << " import_slot=" << imported_slot
                << " name=\"" << source_record.name << "\""
                << " payload_bytes=" << source_payload.size()
                << " payload_identical=yes record_bytes=" << CreationRecordBytes(source_plain, kind,
                                                                                 source_record.slot_index).size()
                << " record_identical=yes main_checksum=PASS validator=PASS listed=PASS"
                << " stfs_sha1=PASS source_tree_unchanged=yes\n";
      ++round_trips;
      return true;
    };

    for (CreationSaveKind kind : {CreationSaveKind::kSuperstar, CreationSaveKind::kArena}) {
      for (const CreationSaveRecord& record : *source_records) {
        if (record.kind == kind && !run_one(kind, record)) return false;
      }
    }
    const auto valid_caw = std::find_if(source_records->begin(), source_records->end(),
                                        [](const CreationSaveRecord& record) {
                                          return record.kind == CreationSaveKind::kSuperstar;
                                        });
    const fs::path forbidden_output = source_paths.userdata_dir / "export-probe.cas";
    if (valid_caw == source_records->end()) return false;
    const Result forbidden_export = ExportCreation(source_paths, CreationSaveKind::kSuperstar,
                                                    valid_caw->slot_index, forbidden_output);
    if (forbidden_export.ok || forbidden_export.error.find("outside the player's userdata") == std::string::npos ||
        fs::exists(forbidden_output) || HashTree(source_paths.userdata_dir) != source_hash) {
      std::cerr << "creation_export_test: export into userdata was not refused safely\n";
      return false;
    }
    if (!RunCreationExportContainmentRegressions(source_paths, source_root, valid_caw->slot_index)) return false;
    if (sample.name == std::string_view("save-after") &&
        !RunCreationExportFailureStageRegressions(owner_root, valid_caw->slot_index)) return false;
    const auto empty_caw_slot = FindFreeCreationSaveSlot(source_paths, CreationSaveKind::kSuperstar);
    const auto empty_arena_slot = FindFreeCreationSaveSlot(source_paths, CreationSaveKind::kArena);
    const fs::path empty_caw = source_root / "exports" / "empty-slot.cas";
    const fs::path empty_arena = source_root / "exports" / "empty-slot.car";
    if (!empty_caw_slot || !empty_arena_slot ||
        ExportCreation(source_paths, CreationSaveKind::kSuperstar, *empty_caw_slot, empty_caw).ok ||
        ExportCreation(source_paths, CreationSaveKind::kArena, *empty_arena_slot, empty_arena).ok ||
        fs::exists(empty_caw) || fs::exists(empty_arena) || HashTree(source_paths.userdata_dir) != source_hash) {
      std::cerr << "creation_export_test: empty-slot rejection or read-only check failed\n";
      return false;
    }
    const auto source_hash_after = HashTree(source_paths.userdata_dir);
    std::cout << "creation_export_test: SOURCE-HASH sample=" << sample.name
              << " before=" << Sha1Text(source_hash) << " after=" << Sha1Text(source_hash_after)
              << " unchanged=" << (source_hash == source_hash_after ? "yes" : "no") << '\n';
    std::error_code error;
    fs::remove_all(source_root, error);
    if (error) return false;
  }
  if (player_fixture_hash && HashTree(player_userdata_root) != *player_fixture_hash) {
    std::cerr << "creation_export_test: owner-userdata fixture copy changed during round trips\n";
    return false;
  }
  std::cout << "creation_export_test: PASS (" << round_trips
            << " real creation export/import round trips; empty CAW/arena slots refused; source copies unchanged; "
            << (player_fixture_hash ? "owner-userdata target copies used" : "save-before target copies used")
            << ")\n";
  return round_trips == 6;
}

bool RunCawHeadIdRangeRegression() {
  constexpr size_t kMainPlainSize = 1'790'648;
  constexpr size_t kFirstCawRecord = 0x1E517;
  constexpr std::array<std::pair<uint32_t, bool>, 5> kCases{{
      {160, true}, {324, true}, {512, true}, {50, false}, {600, false}}};
  std::vector<uint8_t> plain(kMainPlainSize, 0);
  plain[kFirstCawRecord + 0x6ED] = 1;
  plain[kFirstCawRecord + 0x7FD] = 1;
  for (const auto& [head_id, expected] : kCases) {
    plain[kFirstCawRecord + 0x811] = static_cast<uint8_t>(head_id >> 24);
    plain[kFirstCawRecord + 0x812] = static_cast<uint8_t>(head_id >> 16);
    plain[kFirstCawRecord + 0x813] = static_cast<uint8_t>(head_id >> 8);
    plain[kFirstCawRecord + 0x814] = static_cast<uint8_t>(head_id);
    const bool actual = internal::IsSupportedCreationPlain(plain);
    if (actual != expected) {
      std::cerr << "creation_save_test: synthetic head id " << head_id << " expected "
                << (expected ? "accepted" : "rejected") << " but was "
                << (actual ? "accepted" : "rejected") << '\n';
      return false;
    }
  }
  std::cout << "creation_save_test: PASS (synthetic BE head IDs 160/324/512 accepted; 50/600 rejected)\n";
  return true;
}

// #115: the launcher log must name WHICH part of a refused save failed (old code only said "unsupported save layout").
bool RunRefusalReasonRegression() {
  constexpr size_t kMainPlainSize = 1'790'648;
  constexpr size_t kFirstCawRecord = 0x1E517;
  constexpr size_t kSecondTable = 0x76387;
  std::vector<uint8_t> plain(kMainPlainSize, 0);
  if (!internal::DescribeUnsupportedCreationPlain(plain).empty() || !internal::IsSupportedCreationPlain(plain)) return false;
  auto bad = plain;
  bad[kSecondTable + 4 * 7] = 3;
  auto reason = internal::DescribeUnsupportedCreationPlain(bad);
  if (reason.find("second table entry 7") == std::string::npos) { std::cerr << reason << '\n'; return false; }
  bad = plain;
  bad[kFirstCawRecord + 0xB6C * 11 + 0x6ED] = 1;  // slot 11 with no checksum / head id
  reason = internal::DescribeUnsupportedCreationPlain(bad);
  if (reason.find("CAW slot 11 rejected") == std::string::npos || internal::IsSupportedCreationPlain(bad)) {
    std::cerr << reason << '\n';
    return false;
  }
  std::vector<uint8_t> shortened(100, 0);
  if (internal::DescribeUnsupportedCreationPlain(shortened).find("plain size") == std::string::npos) return false;
  std::cout << "creation_save_test: PASS (refusal reason names table entry / CAW slot / size)\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (!RunCawHeadIdRangeRegression()) return 1;
  if (!RunRefusalReasonRegression()) return 1;
#ifndef WWE2K14_OWNER_CREATION_TESTDATA
  std::cout << "creation_save_test: SKIP (owner sample path was not configured)\n";
  return 0;
#else
  try {
    SetCreationAutoRegisterEnabled(false);  // the older fixtures expect un-registered payload folders to stay so
    const fs::path owner_root = OwnerTestRoot();
    const fs::path community_root = R101TestRoot();
    const fs::path player_userdata_root = EnvPathOr("WWE2K14_TEST_FULL_USERDATA_ROOT", {});
    if (argc == 4 && std::string_view(argv[1]) == "--player-save-imports") {
      return RunPlayerSaveImportModes(argv[2], argv[3]) ? 0 : 1;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--inspect-real-save-pair") {
      return RunInspectRealCreationSavePair(argv[2], argv[3]) ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--real-arena-import-matrix") {
      const fs::path full_userdata_root = EnvPathOr("WWE2K14_TEST_FULL_USERDATA_ROOT", {});
      return RunRealArenaImportMatrix(owner_root, full_userdata_root) ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--game-made-save-after") {
#if defined(WWE2K14_CREATION_SAMPLE_REPRODUCTION)
      return RunGameMadeSaveAfterReproduction(owner_root) ? 0 : 1;
#else
      std::cout << "creation_save_test: SKIP (WWE2K14_CREATION_SAMPLE_REPRODUCTION is disabled)\n";
      return 0;
#endif
    }
    if (argc == 2 && std::string_view(argv[1]) == "--general-rules") {
      return RunGeneralCreationRulesReproduction(owner_root) ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--community-vs-game-made") {
      RunCommunityCawAgainstGameMadeAfter(owner_root, community_root);
      return 0;
    }
    RunOwnerRoundTrip(owner_root);
    assert(RunCreationExportRoundTrips(owner_root, player_userdata_root));
    if (const fs::path edited = EnvPathOr("WWE2K14_TEST_EDITED_CAW_USERDATA", {}); !edited.empty()) {
      assert(RunEditedCawExportPairing(edited));
    } else {
      std::cout << "creation_save_test: SKIP (edited-CAW fixture WWE2K14_TEST_EDITED_CAW_USERDATA not configured)\n";
    }
    assert(RunGeneralCreationRulesReproduction(owner_root));
#if defined(WWE2K14_CREATION_SAMPLE_REPRODUCTION)
    assert(RunGameMadeSaveAfterReproduction(owner_root));
#else
    std::cout << "creation_save_test: SKIP (WWE2K14_CREATION_SAMPLE_REPRODUCTION is disabled)\n";
#endif
    RunCommunityCawAgainstGameMadeAfter(owner_root, community_root);
    RunCommunityCawRoundTrip(owner_root, community_root);
    RunArenaSlotRoundTrip(owner_root);
    RunUnknownLayoutNoWrite(owner_root, community_root);
    RunInterruptedTransactionRecovery(owner_root);
    const char* package_data = std::getenv("WWE2K14_CON_PACKAGE_TESTDATA");
    if (package_data && *package_data) {
      assert(RunCommunityConPackageEqualityAndImport(owner_root, community_root, fs::path(package_data)));
      const fs::path rar_path = EnvPathOr(
          "WWE2K14_RAR_TEST_FILE",
          owner_root / "WWE 2K14 Community Creations" / "AJ Styles TNA by SMK.rar");
      assert(RunCreationZipImports(owner_root, community_root, fs::path(package_data), rar_path));
    } else {
      std::cout << "creation_save_test: SKIP (CON-package fixtures were not configured)\n";
    }
    const fs::path owner_save_zip = EnvPathOr(
        "WWE2K14_OWNER_SAVE_TEST_ZIP", owner_root / "owner-save.zip");
    if (fs::is_regular_file(owner_save_zip)) {
      assert(RunRegisterExistingOwnerSave(owner_save_zip));
    } else {
      std::cout << "creation_save_test: SKIP (owner save ZIP fixture was not configured)\n";
    }
    const fs::path repair_zip = EnvPathOr("WWE2K14_OWNER_SAVE_1026G_ZIP",
        owner_root.parent_path() / "owner-1026g" / "wwe2k14-saves-20261025-162930-auto.zip");
    if (fs::is_regular_file(repair_zip)) {
      assert(RunRepairOwnerSave(repair_zip));
      assert(RunAutoRegisterOwnerSave(repair_zip, community_root));
    } else {
      std::cout << "creation_save_test: SKIP (owner 1026g save ZIP is absent)\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "creation_save_test: FAIL: " << error.what() << '\n';
    return 1;
  }
#endif
}
