// Synthetic host test of the WWE '13 arena converter pieces (no game data). The real-data parity check against
// tools/port/arena13/make_import.py is wwe13_arena_probe + tools/port/arena13/compare_import.py.
#include "../wwe13_arena.h"
#include "../wwe13_pipeline.h"
#include "../wwe13_arena_install.h"
#include "../wwe13_install.h"

#include <filesystem>
#include <fstream>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wwe2k14::launcher;
using Bytes = WWE13ConvertBytes;

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void PutLe32(Bytes* b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) (*b)[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
void PutBe32(Bytes* b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) (*b)[at + i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
uint32_t Le32(const Bytes& b, size_t at) {
  return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
}
Bytes Text(const std::string& s) { return Bytes(s.begin(), s.end()); }

// One-group EPAC: group 'STG ' with one 12-byte entry named `name`, word carrying the WWE '13 0x10 flag bits.
Bytes ArenaPac(const std::string& name, bool wwe13_flags) {
  Bytes pac(0x4000 + 0x800 + 0x800, 0);
  std::memcpy(pac.data(), "EPAC", 4);
  PutLe32(&pac, 8, 0x800);
  std::memcpy(pac.data() + 0x800, "STG ", 4);
  PutLe32(&pac, 0x804, 3 | (wwe13_flags ? 0x100000u : 0u));
  std::memcpy(pac.data() + 0x80c, name.data(), 4);
  PutLe32(&pac, 0x810, 0);
  PutLe32(&pac, 0x814, 8);
  std::memcpy(pac.data() + 0x4000, "PACH", 4);
  return pac;
}

void TestRename() {
  Bytes plain;
  std::string error;
  Check(RenameWWE13ArenaPac(ArenaPac("0041", true), 41, 89, &plain, &error), "rename failed");
  Check(std::memcmp(plain.data() + 0x80c, "0089", 4) == 0, "STG entry not renamed");
  Check(!RenameWWE13ArenaPac(ArenaPac("0041", true), 40, 89, &plain, &error), "rename of a missing entry must fail");
}

void TestPacList() {
  Bytes def = Text(";PAC_LIST_ARC_EPAC_BIG\r\npac\\audience.pac\r\npac\\bg\\bg00.pac\r\npac\\bg\\bg09.pac\r\npac\\bg\\bg11.pac\r\npac\\x.pac\r\n");
  uint16_t line = 0;
  std::string error;
  Check(InsertWWE13ArenaPacListLine(&def, 10, &line, &error), "insert failed");
  Check(line == 4, "line after bg09 expected");
  Check(std::string(def.begin(), def.end()) ==
            ";PAC_LIST_ARC_EPAC_BIG\r\npac\\audience.pac\r\npac\\bg\\bg00.pac\r\npac\\bg\\bg09.pac\r\npac\\bg\\bg10.pac\r\npac\\bg\\bg11.pac\r\npac\\x.pac\r\n",
        "def text");
  Check(!InsertWWE13ArenaPacListLine(&def, 10, &line, &error), "a second bg10 line must be refused");

  // Arc: blocks for lines 1..5 then the sentinel; the new block goes between line 3 and the old line 4 (now 5).
  Bytes arc{0xff, 0, 0, 0};
  auto block = [&](const char* entry, uint16_t index) {
    const uint8_t header[12] = {'S', 'T', 'G', ' ', 0x00, 0x30, 0x00, 0x00, 0, 0, 0, 0};
    arc.insert(arc.end(), header, header + 12);
    arc.insert(arc.end(), entry, entry + 4);
    for (int i = 0; i < 8; ++i) arc.push_back(0);
    arc.push_back(0xff); arc.push_back(0xff); arc.push_back(uint8_t(index >> 8)); arc.push_back(uint8_t(index));
  };
  block("A001", 1); block("A002", 2); block("A003", 3); block("A004", 4); block("A005", 5); block("SENT", 0xffff);
  Check(InsertWWE13ArcBlock(&arc, 4, ArenaPac("0010", false), &error), "arc insert failed");
  // block 4 is now the new pac (entry 0010, size 8), old 4 -> 5, old 5 -> 6, sentinel kept last
  const size_t rec = 28;  // 12 header + 12 entry + 4 index
  Check(std::memcmp(arc.data() + 4 + 3 * rec + 12, "0010", 4) == 0, "new block position");
  Check(arc[4 + 3 * rec + 27] == 4, "new block index");
  Check(arc[4 + 4 * rec + 27] == 5 && arc[4 + 5 * rec + 27] == 6, "later blocks renumbered");
  Check(arc[4 + 6 * rec + 26] == 0xff && arc[4 + 6 * rec + 27] == 0xff, "sentinel block kept");
}

void TestTextureBank() {
  Bytes bank(16 + 32 + 4, 0);
  PutLe32(&bank, 0, 1); PutLe32(&bank, 4, 0x100); PutLe32(&bank, 12, 0x10);
  std::memcpy(bank.data() + 16, "RAW", 3); std::memcpy(bank.data() + 32, "dds", 3);
  PutLe32(&bank, 36, 4); PutLe32(&bank, 40, 48);
  std::memcpy(bank.data() + 48, "abcd", 4);
  Bytes out;
  std::string error;
  Check(AddWWE13TextureBankEntries(bank, {{"W13_89", Text("xyz")}}, &out, &error), "bank add failed");
  Check(Le32(out, 0) == 2 && Le32(out, 4) == 0x100, "bank header");
  Check(Le32(out, 16 + 24) == 80 && Le32(out, 48 + 24) == 84 && Le32(out, 48 + 20) == 3, "bank offsets");
  Check(std::memcmp(out.data() + 48, "W13_89", 6) == 0 && std::memcmp(out.data() + 84, "xyz", 3) == 0, "bank data");
  Check(!AddWWE13TextureBankEntries(out, {{"RAW", Text("q")}}, &out, &error), "duplicate picture must be refused");
}

void TestRefusals() {
  WWE13ArenaInput input;
  WWE13ArenaOutput output;
  std::string error;
  Check(!ConvertWWE13Arenas({}, input, &output, &error), "empty port list must fail");
  WWE13ArenaPort port{41, 120, "W13_X", "AEM14", "X"};
  Check(!ConvertWWE13Arenas({port}, input, &output, &error), "missing inputs must fail");
}


namespace fs = std::filesystem;
void WriteText(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}
std::string ReadText(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Own undo set, hard-link-safe apply, byte-identical undo, and the guards between wrestler and arena imports.
void TestInstallTransaction() {
#ifdef WWE2K14_LAUNCHER_TESTDATA
  const fs::path root = fs::path(WWE2K14_LAUNCHER_TESTDATA) / "wwe13-arena-install";
#else
  const fs::path root = fs::temp_directory_path() / "wwe13-arena-install-test";
#endif
  std::error_code ignored;
  fs::remove_all(root, ignored);
  Paths paths;
  paths.exe_dir = root / "launcher";
  paths.game_exe = root / "game" / "wwe2k14";
  paths.userdata_dir = paths.exe_dir / "userdata";
  paths.backups_dir = paths.exe_dir / "backups";
  const fs::path game = root / "game";
  fs::create_directories(paths.userdata_dir);
  WriteText(game / "pac" / "string.pac", "stock strings");
  fs::create_hard_link(game / "pac" / "string.pac", root / "observer.pac");   // another copy sharing the file
  WriteText(root / "stage" / "game" / "pac" / "string.pac", "arena strings");
  WriteText(root / "stage" / "game" / "pac" / "bg" / "bg10.pac", "new arena");
  WriteText(root / "stage" / "launcher" / "mods" / "wwe13" / "arenas.txt", "10 W13_10\n");
  CancelFlag cancel{false};
  Check(!HasWWE13ArenaImport(paths), "no arena import yet");
  Check(ApplyWWE13ArenaStagedFiles(paths, game, root / "stage", {}, cancel).ok, "arena apply failed");
  Check(HasWWE13ArenaImport(paths) && !HasWWE13Import(paths), "arena import uses its own undo set");
  Check(fs::exists(paths.backups_dir / "wwe13-arenas") && !fs::exists(paths.backups_dir / "wwe13-import"), "undo set folder");
  Check(ReadText(game / "pac" / "string.pac") == "arena strings", "applied");
  Check(ReadText(root / "observer.pac") == "stock strings", "hard-linked copy untouched");
  Check(ReadText(WWE13ArenaListFile(paths)) == "10 W13_10\n", "list file in place");
  Check(!ApplyWWE13ArenaStagedFiles(paths, game, root / "stage", {}, cancel).ok, "second arena import must ask for undo");
  Check(!UndoWWE13Import(paths, game, {}, cancel).ok, "wrestler undo must refuse while arenas are installed");
  Check(UndoWWE13ArenaImport(paths, game, {}, cancel).ok, "arena undo failed");
  Check(ReadText(game / "pac" / "string.pac") == "stock strings", "undo restored the stock bytes");
  Check(!fs::exists(game / "pac" / "bg" / "bg10.pac") && !fs::exists(WWE13ArenaListFile(paths)), "undo removed added files");
  Check(!HasWWE13ArenaImport(paths), "undo cleared the arena set");
  fs::remove_all(root, ignored);
}

void TestSymlinkInstallTransaction() {
#ifdef WWE2K14_LAUNCHER_TESTDATA
  const fs::path root = fs::path(WWE2K14_LAUNCHER_TESTDATA) / "wwe13-arena-symlink-install";
#else
  const fs::path root = fs::temp_directory_path() / "wwe13-arena-symlink-install-test";
#endif
  std::error_code ignored;
  fs::remove_all(root, ignored);
  Paths paths;
  paths.exe_dir = root / "launcher";
  paths.game_exe = root / "game" / "wwe2k14";
  paths.userdata_dir = paths.exe_dir / "userdata";
  paths.backups_dir = paths.exe_dir / "backups";
  const fs::path game = root / "game";
  const fs::path stock = root / "stock-string.pac";
  WriteText(stock, "stock bytes behind the symlink");
  fs::create_directories((game / "pac" / "string.pac").parent_path());
  fs::create_symlink(stock, game / "pac" / "string.pac");
  WriteText(root / "stage" / "game" / "pac" / "string.pac", "imported replacement");
  WriteText(root / "stage" / "launcher" / "mods" / "wwe13" / "arenas.txt", "10 W13_10\n");
  CancelFlag cancel{false};
  Check(ApplyWWE13ArenaStagedFiles(paths, game, root / "stage", {}, cancel).ok, "symlink arena apply failed");
  Check(!fs::is_symlink(game / "pac" / "string.pac"), "imported replacement should be a regular file");
  Check(ReadText(stock) == "stock bytes behind the symlink", "import changed the symlink target bytes");
  Check(UndoWWE13ArenaImport(paths, game, {}, cancel).ok, "symlink arena undo failed");
  Check(fs::is_symlink(game / "pac" / "string.pac"), "undo did not restore the original symlink");
  Check(fs::read_symlink(game / "pac" / "string.pac") == stock, "undo restored a different symlink target");
  Check(ReadText(game / "pac" / "string.pac") == "stock bytes behind the symlink", "undo did not restore identical bytes");
  Check(!fs::exists(game / "pac" / "bg" / "bg10.pac") && !fs::exists(WWE13ArenaListFile(paths)),
        "symlink undo left imported files behind");
  fs::remove_all(root, ignored);
}

void TestTables() {
  Check(WWE13ArenaPicture(10) == "arena13" && WWE13ArenaPicture(5) == "arena09" && WWE13ArenaPicture(15) == "arena07", "'13 menu pictures");
  Check(WWE13ArenaPicture(28) == "AEM01" && WWE13ArenaPicture(49) == "AEM22" && WWE13ArenaPicture(50).empty(), "'13 Attitude pictures");
  const std::vector<uint32_t> expected{10, 18, 19, 20, 24, 25, 26, 57, 58, 59, 60, 61, 62,
                                       63, 69, 84, 89, 90, 91, 92, 93, 94, 95, 96, 97};
  Check(WWE13ArenaImportTargets() == expected, "all 25 proven target numbers stay in their established order");
  const fs::path root = fs::temp_directory_path() / "wwe13-arena-target-test";
  std::error_code ignored;
  fs::remove_all(root, ignored);
  Check(FreeWWE13ArenaNumbers(root) == expected, "all 25 target numbers are free in an empty game copy");
  WriteText(root / "pac" / "bg" / "bg57.pac", "occupied");
  std::vector<uint32_t> without_57 = expected;
  without_57.erase(without_57.begin() + 7);
  Check(FreeWWE13ArenaNumbers(root) == without_57, "an occupied target is not offered");
  fs::remove_all(root, ignored);
}

// Synthetic fixed-layout misc02 PAC: exercise the verified nested path without game data.
Bytes Misc02WithLightRows() {
  constexpr size_t kMscSize = 0x42000;
  constexpr size_t kChild102At = 0x3B88C;
  constexpr size_t kTableAt = 0x41610;
  constexpr size_t kTableIn102 = 0x5D84;
  constexpr size_t kChild5At = 0x5BC0;
  constexpr size_t kChild5Size = 2168;
  constexpr size_t kChild102Size = kChild5At + kChild5Size;
  Bytes plain(0x4000 + kMscSize + 0x800, 0);
  std::memcpy(plain.data(), "EPAC", 4);
  PutLe32(&plain, 4, 28);  // one 16-byte group entry plus group header
  PutLe32(&plain, 8, kMscSize);
  std::memcpy(plain.data() + 0x800, "BATL", 4);
  PutLe32(&plain, 0x804, 4 | 0x10000);
  std::memcpy(plain.data() + 0x80C, "MSC2    ", 8);
  PutLe32(&plain, 0x814, 0);
  PutLe32(&plain, 0x818, kMscSize / 0x100);

  const size_t msc = 0x4000;
  std::memcpy(plain.data() + msc, "PACH", 4);
  PutLe32(&plain, msc + 4, 1);
  PutLe32(&plain, msc + 8, 102);
  PutLe32(&plain, msc + 12, kChild102At - 20);
  PutLe32(&plain, msc + 16, kChild102Size);

  const size_t child = msc + kChild102At;
  std::memcpy(plain.data() + child, "PACH", 4);
  PutLe32(&plain, child + 4, 8);
  for (uint32_t i = 0; i < 8; ++i) {
    const size_t at = child + 8 + size_t(i) * 12;
    PutLe32(&plain, at, i);
    PutLe32(&plain, at + 4, i == 5 ? kChild5At - (8 + 12 * 8) : 0);
    PutLe32(&plain, at + 8, i == 5 ? kChild5Size : 0);
  }
  const size_t table = child + kTableIn102;
  PutBe32(&plain, table + 27 * 16, 1);
  PutBe32(&plain, table + 27 * 16 + 4, 5600);
  PutBe32(&plain, table + 27 * 16 + 8, 5750);
  PutBe32(&plain, table + 27 * 16 + 12, 250);
  // Assert the values named by the format facts point to the same byte span.
  Check(kChild102At + kTableIn102 == kTableAt, "synthetic child/table offsets disagree");
  return plain;
}

void TestArenaLightRows() {
  Bytes input = Misc02WithLightRows(), output;
  bool changed = false;
  std::string error;
  Check(FillWWE13ArenaLightRows(input, {57, 63, 96, 27}, &output, &changed, &error), "stands-light patch failed");
  Check(changed, "zero rows were not reported as changed");
  Bytes expected = input;
  for (uint32_t id : {57u, 63u, 96u}) {
    const size_t at = 0x4000 + 0x41610 + size_t(id) * 16;
    PutBe32(&expected, at, 1); PutBe32(&expected, at + 4, 5600);
    PutBe32(&expected, at + 8, 5750); PutBe32(&expected, at + 12, 250);
  }
  Check(output == expected, "stands-light patch changed bytes outside the selected zero rows");
  Check(FillWWE13ArenaLightRows(input, {27}, &output, &changed, &error), "populated control row failed");
  Check(!changed && output == input, "the populated control row should remain byte-identical");
  Check(!FillWWE13ArenaLightRows(input, {100}, &output, &changed, &error), "out-of-range arena id must be refused");
}

}  // namespace

int main() {
  try {
    TestRename();
    TestPacList();
    TestTextureBank();
    TestRefusals();
    TestInstallTransaction();
    TestSymlinkInstallTransaction();
    TestTables();
    TestArenaLightRows();
  } catch (const std::exception& e) {
    std::cerr << "wwe13_arena_test FAILED: " << e.what() << "\n";
    return 1;
  }
  std::cout << "wwe13_arena_test passed\n";
  return 0;
}
