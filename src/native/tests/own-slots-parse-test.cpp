#include "../../own_slots_parse.h"
#include <cstdio>
#include <filesystem>
#include <sstream>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
using V = std::vector<uint32_t>;
int main(int argc, char** argv) {
  int fails = 0;
  { std::istringstream s("# header\n152\n\n  338  # booker\n\t401\r\n"); CHECK((ownslots::Parse(s) == V{152, 338, 401})); }
  { std::istringstream s("99\n100\n499\n500\n0\n-5\n1e3\nabc\n15 2\n1234567\n"); CHECK((ownslots::Parse(s) == V{100, 499})); }
  { std::istringstream s("152\n152\n338\n152 # again\n"); CHECK((ownslots::Parse(s) == V{152, 338})); }
  { std::istringstream s("   \n# only comments\n"); CHECK(ownslots::Parse(s).empty()); }
  { std::istringstream s("0150\n+160\n"); CHECK((ownslots::Parse(s) == V{150})); }
  CHECK(ownslots::LoadFile("/nonexistent/own-slots.txt").empty());  // no file: unmodded game, no ids
  const std::string dir = std::filesystem::temp_directory_path().string();
  { FILE* f = std::fopen((dir + "/own_slots_test.txt").c_str(), "w"); std::fputs("200\n# x\n201\n", f); std::fclose(f);
    CHECK((ownslots::LoadFile((dir + "/own_slots_test.txt").c_str()) == V{200, 201})); }
  { FILE* f = std::fopen((dir + "/own_slots_empty.txt").c_str(), "w"); std::fclose(f); CHECK(ownslots::LoadFile((dir + "/own_slots_empty.txt").c_str()).empty()); }
  // Added for own10: CRLF file as a Windows editor would save it, last line without newline, and the real kit list when given.
  { std::istringstream s("120\r\n162\r\n350"); CHECK((ownslots::Parse(s) == V{120, 162, 350})); }
  if (argc > 1) {
    const V ids = ownslots::LoadFile(argv[1]);
    std::printf("kit list: %zu ids, first %u, last %u\n", ids.size(), ids.empty() ? 0 : ids.front(), ids.empty() ? 0 : ids.back());
    CHECK(ids.size() == 106);
    bool has162 = false, has309 = false, has152 = false, has338 = false;
    for (uint32_t x : ids) { has162 |= x == 162; has309 |= x == 309; has152 |= x == 152; has338 |= x == 338; }
    CHECK(has162 && !has309 && has152 && has338);  // Hunico moved 309 -> 162 (11-06); Shane and Booker stay
  }
  std::printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails);
  return fails != 0;
}
