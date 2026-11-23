#include "launcher_core.h"
#include "internal/miniz_api.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace wwe2k14::launcher;
using Bytes = std::vector<uint8_t>;

#define CHECK(condition) do { if (!(condition)) { \
  std::printf("FAIL line %d: %s\n", __LINE__, #condition); std::fflush(stdout); std::abort(); \
} } while (0)

namespace {
uint32_t Le32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
         (uint32_t(bytes[3]) << 24);
}
void SetLe16(Bytes& bytes, size_t at, uint16_t value) {
  bytes[at] = static_cast<uint8_t>(value);
  bytes[at + 1] = static_cast<uint8_t>(value >> 8);
}
void SetLe32(Bytes& bytes, size_t at, uint32_t value) {
  bytes[at] = static_cast<uint8_t>(value);
  bytes[at + 1] = static_cast<uint8_t>(value >> 8);
  bytes[at + 2] = static_cast<uint8_t>(value >> 16);
  bytes[at + 3] = static_cast<uint8_t>(value >> 24);
}
void SetBe32(Bytes& bytes, size_t at, uint32_t value) {
  bytes[at] = static_cast<uint8_t>(value >> 24);
  bytes[at + 1] = static_cast<uint8_t>(value >> 16);
  bytes[at + 2] = static_cast<uint8_t>(value >> 8);
  bytes[at + 3] = static_cast<uint8_t>(value);
}

Bytes BpeEncodeLiterals(const Bytes& raw) {
  CHECK(raw.size() <= 65535);
  Bytes payload;
  payload.reserve(262 + raw.size());
  payload.push_back(127);
  for (unsigned i = 0; i < 128; ++i) payload.push_back(static_cast<uint8_t>(i));
  payload.push_back(127);
  for (unsigned i = 128; i < 256; ++i) payload.push_back(static_cast<uint8_t>(i));
  payload.push_back(static_cast<uint8_t>(raw.size()));
  payload.push_back(static_cast<uint8_t>(raw.size() >> 8));
  payload.insert(payload.end(), raw.begin(), raw.end());
  Bytes out(16, 0);
  std::memcpy(out.data(), "BPE ", 4);
  SetLe32(out, 8, static_cast<uint32_t>(payload.size()));
  SetLe32(out, 12, static_cast<uint32_t>(raw.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

Bytes BuildPach(const std::vector<std::pair<uint32_t, Bytes>>& leaves) {
  const size_t base = 8 + 12 * leaves.size();
  Bytes out(base, 0);
  std::memcpy(out.data(), "PACH", 4);
  SetLe32(out, 4, static_cast<uint32_t>(leaves.size()));
  Bytes body;
  for (size_t i = 0; i < leaves.size(); ++i) {
    const size_t record = 8 + i * 12;
    SetLe32(out, record, leaves[i].first);
    SetLe32(out, record + 4, static_cast<uint32_t>(body.size()));
    SetLe32(out, record + 8, static_cast<uint32_t>(leaves[i].second.size()));
    body.insert(body.end(), leaves[i].second.begin(), leaves[i].second.end());
  }
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

using PacGroup = std::pair<std::string, std::vector<std::pair<std::string, Bytes>>>;
Bytes BuildEpac(const std::vector<PacGroup>& groups) {
  Bytes out(0x4000, 0);
  std::memcpy(out.data(), "EPAC", 4);
  SetLe32(out, 4, 0x1C);
  SetLe32(out, 12, 7);
  size_t directory = 0x800;
  Bytes body;
  for (const PacGroup& group : groups) {
    CHECK(group.first.size() == 4);
    CHECK(directory + 12 + group.second.size() * 16 <= 0x4000);
    std::memcpy(out.data() + directory, group.first.data(), 4);
    SetLe32(out, directory + 4, static_cast<uint32_t>(group.second.size() * 4) | 0x10000);
    SetLe32(out, directory + 8, 0);
    directory += 12;
    for (const auto& [name, data] : group.second) {
      CHECK(name.size() <= 8);
      uint8_t* record = out.data() + directory;
      std::memcpy(record, name.data(), name.size());
      SetLe32(out, directory + 8, static_cast<uint32_t>(body.size() / 0x800));
      SetLe32(out, directory + 12, static_cast<uint32_t>((data.size() + 0xFF) / 0x100));
      body.insert(body.end(), data.begin(), data.end());
      body.resize((body.size() + 0x7FF) & ~size_t(0x7FF), 0);
      directory += 16;
    }
  }
  out.insert(out.end(), body.begin(), body.end());
  out.resize(out.size() + 0x800, 0);
  return out;
}

Bytes WrapStoredXcompress(const Bytes& raw) {
  CHECK(raw.size() <= 0x8000);
  Bytes out(20 + raw.size(), 0);
  out[0] = 0x0F; out[1] = 0xF5; out[2] = 0x12; out[3] = 0xED;
  SetBe32(out, 12, 0x40);  // one 20-bit output-size entry, no LZX window shift
  SetBe32(out, 16, static_cast<uint32_t>(raw.size()) << 12);
  std::copy(raw.begin(), raw.end(), out.begin() + 20);
  return out;
}

Bytes BuildDds() {
  Bytes dds(132, 0);
  std::memcpy(dds.data(), "DDS ", 4);
  SetLe32(dds, 4, 124);
  SetLe32(dds, 12, 1);  // one-pixel synthetic height
  SetLe32(dds, 16, 1);  // one-pixel synthetic width
  SetLe32(dds, 20, 4);
  SetLe32(dds, 76, 32);
  dds[128] = 0x11; dds[129] = 0x22; dds[130] = 0x33; dds[131] = 0x44;
  return dds;
}

Bytes BuildTextureBundle(const Bytes& dds) {
  Bytes bundle(48 + dds.size(), 0);
  SetLe32(bundle, 0, 1);
  SetLe32(bundle, 4, 0x100);
  SetLe32(bundle, 12, 0x10);
  std::memcpy(bundle.data() + 16, "color", 5);
  std::memcpy(bundle.data() + 32, "dds", 3);
  SetLe32(bundle, 36, static_cast<uint32_t>(dds.size()));
  SetLe32(bundle, 40, 48);
  std::copy(dds.begin(), dds.end(), bundle.begin() + 48);
  return bundle;
}

Bytes BuildSdbZlib(uint32_t id, const std::string& name) {
  Bytes plain(8 + 12 + name.size() + 1, 0);
  SetLe32(plain, 0, 0);
  SetLe32(plain, 4, 1);
  SetLe32(plain, 8, 20);
  SetLe32(plain, 12, static_cast<uint32_t>(name.size() + 1));
  SetLe32(plain, 16, 60000 + id);
  std::memcpy(plain.data() + 20, name.c_str(), name.size() + 1);
  wwe_mz_ulong compressed_size = mz_compressBound(static_cast<wwe_mz_ulong>(plain.size()));
  Bytes compressed(compressed_size);
  CHECK(mz_compress2(compressed.data(), &compressed_size, plain.data(),
                     static_cast<wwe_mz_ulong>(plain.size()), kWweMzBestCompression) == kWweMzOk);
  compressed.resize(compressed_size);
  Bytes zlib(16, 0);
  std::memcpy(zlib.data(), "ZLIB", 4);
  SetLe32(zlib, 8, static_cast<uint32_t>(compressed.size()));
  SetLe32(zlib, 12, static_cast<uint32_t>(plain.size()));
  zlib.insert(zlib.end(), compressed.begin(), compressed.end());
  return zlib;
}

Bytes BuildEmptySdbPartialZlib() {
  // Matches the incomplete but harmless 8-byte zero table present in retail string PACs.
  Bytes zlib(24, 0);
  std::memcpy(zlib.data(), "ZLIB", 4);
  SetLe32(zlib, 8, 8);
  SetLe32(zlib, 12, 8);
  const uint8_t stream[] = {0x78, 0xda, 0x63, 0x60, 0x80, 0x00, 0x00, 0x00};
  std::copy(std::begin(stream), std::end(stream), zlib.begin() + 16);
  return zlib;
}

Bytes BuildStringPac(uint32_t id, const std::string& name) {
  const Bytes string_leaf = BuildSdbZlib(id, name);
  const Bytes pach = BuildPach({{0, BuildEmptySdbPartialZlib()}, {1, string_leaf}});
  return BuildEpac({{"SDB ", {{"0000", pach}}}});
}

Bytes BuildCharacterPac(uint32_t id, const std::string& entry_name, Bytes& want_leaf, Bytes& want_dds) {
  want_leaf = Bytes{'J', 'B', 'O', 'Y', '-', 's', 'y', 'n', 't', 'h', 'e', 't', 'i', 'c'};
  want_dds = BuildDds();
  const Bytes textures = BuildTextureBundle(want_dds);
  const Bytes pach = BuildPach({{0, BpeEncodeLiterals(want_leaf)},
                                {10, BpeEncodeLiterals(textures)},
                                {50, Bytes{'s', 'p', 'r', 'i', 'n', 'g'}}});
  const Bytes epac = BuildEpac({{"EMD ", {{entry_name, pach}}}});
  return WrapStoredXcompress(epac);
}

void Write(const fs::path& path, const Bytes& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  CHECK(static_cast<bool>(file));
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  CHECK(static_cast<bool>(file));
}

Bytes Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  CHECK(static_cast<bool>(file));
  return Bytes(std::istreambuf_iterator<char>(file), {});
}

std::string ReadText(const fs::path& path) {
  const Bytes bytes = Read(path);
  return std::string(bytes.begin(), bytes.end());
}
}  // namespace

int main() {
  const fs::path root = fs::path(WWE2K14_LAUNCHER_TESTDATA) / "wwe14-export-test";
  std::error_code error;
  fs::remove_all(root, error);
  const fs::path base = root / "base";
  const fs::path dlc = root / "dlc04";
  fs::create_directories(base / "pac" / "ch");
  fs::create_directories(dlc / "pac" / "ch");

  Bytes base_leaf, base_dds, dlc_leaf, dlc_dds;
  Write(base / "pac" / "string.pac", BuildStringPac(100, "Synthetic Base \xC3\x89"));
  Write(base / "pac" / "ch" / "ch100.pac", BuildCharacterPac(100, "00010002", base_leaf, base_dds));
  Write(dlc / "pac" / "string_dlc04.pac", BuildStringPac(390, "Synthetic DLC"));
  Write(dlc / "pac" / "ch" / "ch390_dlc04.pac", BuildCharacterPac(390, "00039002", dlc_leaf, dlc_dds));

  const std::vector<fs::path> roots{base, dlc};
  const auto roster = LoadStockWrestlers(roots);
  CHECK(roster.size() == 2);
  CHECK(roster[0].id == 100 && roster[0].name == "Synthetic Base \xC3\x89" && roster[0].game == "WWE 2K14");
  CHECK(roster[1].id == 390 && roster[1].name == "Synthetic DLC" && roster[1].game == "WWE 2K14");

  const fs::path output = root / "output";
  CancelFlag cancel{false};
  std::vector<std::string> progress;
  const Result result = ExportStockWrestlers(roots, {100, 390}, output,
      [&](uint64_t, uint64_t, const std::string& label) { progress.push_back(label); }, cancel);
  if (!result.ok) std::printf("export error: %s\n", result.error.c_str());
  CHECK(result.ok);
  CHECK(!progress.empty());
  const fs::path base_output = output / "Synthetic_Base [100]";
  const fs::path dlc_output = output / "Synthetic_DLC [390]";
  CHECK(ReadText(base_output / "name.txt").find("Name: Synthetic Base \xC3\x89\nID: 100\n") != std::string::npos);
  CHECK(ReadText(dlc_output / "name.txt").find("Name: Synthetic DLC\nID: 390\n") != std::string::npos);
  CHECK(Read(base_output / "leaves" / "EMD_00010002_leaf-0.bin") == base_leaf);
  CHECK(Read(dlc_output / "leaves" / "EMD_00039002_leaf-0.bin") == dlc_leaf);
  CHECK(Read(base_output / "textures" / "EMD_00010002_leaf-10-color.dds") == base_dds);
  CHECK(Read(dlc_output / "textures" / "EMD_00039002_leaf-10-color.dds") == dlc_dds);
  CHECK(ReadText(base_output / "manifest.tsv").find("leaf-50.bin") != std::string::npos);
  CHECK(fs::exists(base_output / "leaves" / "EMD_00010002_leaf-50.bin"));

  const Result duplicate = ExportStockWrestlers(roots, {100}, output, {}, cancel);
  CHECK(!duplicate.ok && duplicate.error.find("already contains") != std::string::npos);
  CancelFlag cancelled{true};
  const Result cancelled_result = ExportStockWrestlers(roots, {100}, root / "cancelled", {}, cancelled);
  CHECK(!cancelled_result.ok && !fs::exists(root / "cancelled"));
  fs::remove_all(root, error);
  std::printf("wwe14_export_test OK: synthetic XCompress, EPAC, PACH, BPE, SDB/ZLIB and DDS round-trip\n");
  return 0;
}
