#include "../wwe13_pipeline.h"

#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

int main() {
  try {
#ifndef WWE2K14_WWE13_RECIPE
    throw std::runtime_error("recipe file path was not configured");
#else
    std::vector<WWE13ImportPort> ports;
    std::string error;
    if (!ReadWWE13ImportRecipes(WWE2K14_WWE13_RECIPE, &ports, &error))
      throw std::runtime_error(error);
    if (ports.size() != 106) throw std::runtime_error("the supported recipe table must contain 106 rows");
    std::set<uint32_t> sources, targets;
    bool saw_female = false;
    for (const WWE13ImportPort& port : ports) {
      if (!sources.insert(port.source_id).second || !targets.insert(port.target_id).second)
        throw std::runtime_error("recipe source and target IDs must be unique");
      if (port.source_id == 68 && port.gender == "F") saw_female = true;
      if (port.name.empty() || port.handle.empty() || port.donor_id == 0 || port.tron.empty())
        throw std::runtime_error("recipe row is missing a supported conversion choice");
    }
    if (!saw_female) throw std::runtime_error("recipe gender metadata did not load");
    std::cout << "wwe13_recipe_test: PASS (106 supported rows)\n";
    return 0;
#endif
  } catch (const std::exception& error) {
    std::cerr << "wwe13_recipe_test: FAIL: " << error.what() << '\n';
    return 1;
  }
}
