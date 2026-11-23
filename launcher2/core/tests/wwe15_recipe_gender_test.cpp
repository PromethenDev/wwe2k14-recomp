// GitHub #178: the WWE 2K15 import now writes launcher/mods/wwe15/roster.tsv (id, name, gender) so the Mods page counts
// the imported wrestlers instead of "Nothing imported yet". The gender comes from the new 10th recipe column; this test
// reads the shipped data/wwe2k15-ports.tsv and checks every row parses with M or F, and that the women are marked F.
#include "../wwe15_pipeline.h"

#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wwe2k14::launcher;

namespace {
void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() {
  try {
    const std::filesystem::path recipe =
        std::filesystem::path(WWE2K14_WWE13_RECIPE).parent_path() / "wwe2k15-ports.tsv";
    std::vector<WWE15ImportPort> ports;
    std::string error;
    Check(ReadWWE15ImportRecipes(recipe, &ports, &error), "recipe read failed: " + error);
    Check(ports.size() >= 70, "expected the full 2K15 recipe table, got " + std::to_string(ports.size()) + " rows");
    std::set<std::string> women;
    for (const WWE15ImportPort& p : ports) {
      Check(p.gender == "M" || p.gender == "F", p.name + ": gender " + p.gender);
      if (p.gender == "F") women.insert(p.name);
    }
    for (const char* name : {"TAMINA", "LANA", "AJ LEE '15", "NAOMI '15"})
      Check(women.count(name) == 1, std::string(name) + " should be F");
    Check(women.count("BRAY WYATT") == 0, "BRAY WYATT should be M");
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
  }
  std::cout << "wwe15_recipe_gender_test: PASS\n";
  return 0;
}
