// ShaderToolsPresent (GitHub #105): false without the translator file, true with an empty file.
#include "../launcher_core.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

int main() {
  try {
    const fs::path root = fs::temp_directory_path() /
                          ("shader_tools_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    Paths paths;
    paths.exe_dir = root;
    if (ShaderToolsPresent(paths)) throw std::runtime_error("present without file");
    fs::create_directories(root / "shadertools");
    if (ShaderToolsPresent(paths)) throw std::runtime_error("present with empty folder");
    std::ofstream(ShaderToolsPath(paths)).put('\0');
    if (!ShaderToolsPresent(paths)) throw std::runtime_error("missing with file");
    fs::remove_all(root);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
