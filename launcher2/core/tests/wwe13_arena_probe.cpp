// Real-data probe for the WWE '13 arena converter: reads a WWE 2K14 copy and a WWE '13 copy (read only), converts the
// given arenas and writes every output file under <out dir> (game-relative paths) plus arena-list.txt.
// tools/port/arena13/compare_import.py compares that folder with make_import.py's fixture.
// usage: wwe13_arena_probe <2K14 dir> <WWE13 dir> <out dir> <src>:<id>:<tag>:<picture>:<NAME> ...
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

#include "wwe13_arena.h"

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

static bool Read(const fs::path& p, WWE13ConvertBytes* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

int main(int argc, char** argv) {
  if (argc < 5) { std::cerr << "usage: wwe13_arena_probe <2K14 dir> <WWE13 dir> <out dir> <src:id:tag:picture:NAME>...\n"; return 2; }
  const fs::path game = argv[1], source = argv[2], out = argv[3];
  std::vector<WWE13ArenaPort> ports;
  for (int i = 4; i < argc; ++i) {
    std::string a = argv[i];
    WWE13ArenaPort p;
    size_t at = 0;
    auto next = [&](bool last) { size_t e = last ? std::string::npos : a.find(':', at); std::string v = a.substr(at, e == std::string::npos ? std::string::npos : e - at); at = e == std::string::npos ? a.size() : e + 1; return v; };
    p.source_id = std::stoul(next(false)); p.target_id = std::stoul(next(false));
    p.tag = next(false); p.picture = next(false); p.name = next(true);
    ports.push_back(p);
  }
  WWE13ArenaInput input;
  for (const auto& f : WWE13ArenaGameFiles())
    if (!Read(game / f, &input.game[f])) { std::cerr << "cannot read " << (game / f) << "\n"; return 1; }
  for (const auto& f : WWE13ArenaSourceFiles(ports))
    if (!Read(source / f, &input.source[f])) { std::cerr << "cannot read " << (source / f) << "\n"; return 1; }
  for (const auto& e : fs::directory_iterator(game / "pac" / "bg")) {
    const std::string n = e.path().filename().string();
    if (n.size() == 8 && n.rfind("bg", 0) == 0 && isdigit((unsigned char)n[2]) && isdigit((unsigned char)n[3]))
      input.existing_bg.push_back(std::stoul(n.substr(2, 2)));
  }
  WWE13ArenaOutput output;
  std::string error;
  if (!ConvertWWE13Arenas(ports, input, &output, &error)) { std::cerr << "FAIL: " << error << "\n"; return 1; }
  for (const auto& [rel, bytes] : output.files) {
    fs::create_directories((out / rel).parent_path());
    std::ofstream(out / rel, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::cout << "wrote " << rel << " " << bytes.size() << "\n";
  }
  std::ofstream(out / "arena-list.txt") << output.arena_list;
  return 0;
}
