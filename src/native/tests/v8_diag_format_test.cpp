#include "../v8_diag_format.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
  std::vector<wwe2k14::v8diag::ShaderRow> rows;
  for (uint64_t i = 0; i < 35; ++i) rows.push_back({{i + 1, i + 101}, (i + 1) * 1000000, i + 1});
  const std::string shader = wwe2k14::v8diag::FormatShaderTop(1, 120, 120, rows.size(), rows);
  assert(shader.find("frames=1-120 n=120 shaders=35") != std::string::npos);
  assert(shader.find("#1 ps=0000000000000023 vs=0000000000000087 ms/f=0.2917 draws/f=0.29") != std::string::npos);
  assert(shader.find("#30 ") != std::string::npos);
  assert(shader.find("#31 ") == std::string::npos);
  return 0;
}
