#include "launcher_core.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <fstream>
using namespace wwe2k14::launcher;
// Not CHECK(): release builds compile that out, and these checks call the code under test.
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::fflush(stdout); std::abort(); } } while (0)
int main() {
  const fs::path dir = fs::path(WWE2K14_LAUNCHER_TESTDATA) / "mods-test";
  fs::remove_all(dir);
  fs::create_directories(dir / "mods" / "wwe13");
  Paths paths;
  paths.exe_dir = dir;
  CHECK(LoadModWrestlers(paths).empty());  // no mod installed: the page shows its empty state
  CHECK(!HasEnabledModWrestlers(paths));
  { std::ofstream(dir / "mods" / "wwe13" / "roster.tsv") << "# id name gender\n294\tBrie Bella\tF\n162\tHunico\tM\n99\tToo low\tM\nbad\tNot a number\tM\n500\tToo high\tM\n"; }
  { std::ofstream(dir / "own-slots.txt") << "# list\r\n162 # kept\r\n294\r\n400\r\n"; }
  CHECK(HasEnabledModWrestlers(paths));
  auto list = LoadModWrestlers(paths);
  CHECK(list.size() == 2 && list[0].name == "Brie Bella" && list[0].female && list[0].enabled);  // sorted by name, bad rows dropped
  CHECK(list[1].id == 162 && !list[1].female && list[1].enabled);
  CHECK(SetModWrestlerEnabled(paths, 294, false).ok);
  CHECK(HasEnabledModWrestlers(paths));
  list = LoadModWrestlers(paths);
  CHECK(!list[0].enabled && list[1].enabled);
  CHECK(SetModWrestlerEnabled(paths, 294, true).ok && LoadModWrestlers(paths)[0].enabled);
  { std::ifstream in(dir / "own-slots.txt"); std::string all((std::istreambuf_iterator<char>(in)), {}); CHECK(all.find("400\n") != std::string::npos); CHECK(all.rfind("# ", 0) == 0); }  // ids outside the roster stay
  CHECK(SetModWrestlersEnabled(paths, {162, 294}, false).ok);
  list = LoadModWrestlers(paths);
  CHECK(!list[0].enabled && !list[1].enabled && HasEnabledModWrestlers(paths));
  {
    std::ifstream in(dir / "own-slots.txt", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    CHECK(bytes.rfind("# Written", 0) == 0 && bytes.find("400\n") != std::string::npos);
    CHECK(bytes.find("162\n") == std::string::npos && bytes.find("294\n") == std::string::npos);
  }  // one plain-byte rewrite removes the selected ids and preserves unrelated ids
  CHECK(SetModWrestlersEnabled(paths, {162, 294}, true).ok);
  list = LoadModWrestlers(paths);
  CHECK(list[0].enabled && list[1].enabled);
  CHECK(SetModWrestlerEnabled(paths, 162, false).ok && SetModWrestlerEnabled(paths, 294, false).ok);
  list = LoadModWrestlers(paths);
  CHECK(!list[0].enabled && !list[1].enabled && HasEnabledModWrestlers(paths));  // unrelated valid id 400 stays enabled
  // Each mods/<game> folder is a source. Tag from '#game', or from the folder for rosters made before tags.
  CHECK(LoadModWrestlers(paths)[0].game == "WWE '13");
  fs::create_directories(dir / "mods" / "wwe15");
  { std::ofstream(dir / "mods" / "wwe15" / "roster.tsv") << "#game WWE 2K15\n175\tBray Wyatt\tM\n162\tSame id as a '13 row\tM\n"; }
  fs::create_directories(dir / "mods" / "svr07");
  { std::ofstream(dir / "mods" / "svr07" / "roster.tsv") << "180\tOld Guy\tM\n"; }
  fs::create_directories(dir / "mods" / "empty-folder");
  list = LoadModWrestlers(paths);
  CHECK(list.size() == 4);
  CHECK(list[0].name == "Bray Wyatt" && list[0].game == "WWE 2K15");
  CHECK(list[1].name == "Brie Bella" && list[1].game == "WWE '13");
  CHECK(list[2].id == 162 && list[2].name == "Hunico" && list[2].game == "WWE '13");  // sorted folders: wwe13 before wwe15
  CHECK(list[3].name == "Old Guy" && list[3].game == "SVR07");
  fs::remove_all(dir);
  // Real pack check, when given: every wrestler's offset and value against the tool's table (no game data in the repo).
  if (const char* pack = std::getenv("WWE2K14_TEST_MUSIC_PACK")) {
    std::ifstream table(std::string(pack) + ".offsets.tsv");
    std::string line;
    std::getline(table, line);
    std::vector<uint32_t> ids;
    std::map<uint32_t, std::pair<uint64_t, double>> want;
    while (std::getline(table, line)) {
      uint32_t target = 0, id13 = 0, source = 0;
      unsigned long long offset = 0;
      char sound[16];
      double db = 0;
      CHECK(std::sscanf(line.c_str(), "%u\t%u\t%llu\t%u\t%15s\t%lf", &target, &id13, &offset, &source, sound, &db) == 6);
      ids.push_back(target);
      want[target] = {offset, db};
    }
    const auto themes = ReadModThemes(pack, ids);
    std::printf("real pack: %zu table rows, %zu themes read\n", ids.size(), themes.size());
    CHECK(ids.size() == 106 && themes.size() == 106);
    for (const auto& [id, theme] : themes) {
      CHECK(theme.offset == want[id].first);
      CHECK(std::abs(theme.db - want[id].second) < 0.01);
    }
    fs::path copy = fs::path(WWE2K14_LAUNCHER_TESTDATA) / "mods-pack-copy.pck";
    fs::create_directories(copy.parent_path());
    fs::copy_file(pack, copy, fs::copy_options::overwrite_existing);
    const ModTheme nikki = themes.at(295);
    CHECK(SetModThemeVolume(copy, nikki.offset, 1.5f).ok && !SetModThemeVolume(copy, nikki.offset, 7.0f).ok);
    const auto after = ReadModThemes(copy, ids);
    CHECK(std::abs(after.at(295).db - 1.5f) < 0.001 && after.at(294).db == themes.at(294).db);
    CHECK(fs::file_size(copy) == fs::file_size(pack));
    fs::remove(copy);
  }
  CHECK(ReadModThemes(dir / "missing.pck", {295}).empty());
  std::printf("mods_test OK\n");
  return 0;
}
