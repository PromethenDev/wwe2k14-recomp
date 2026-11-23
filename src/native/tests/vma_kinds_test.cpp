// Host test for src/native/nr/vma_kinds.h (issue plan step 5). Build: c++ -std=c++17 -I src/native/nr this.cpp
#include "vma_kinds.h"
#include <cstdio>
int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s\n", #c); ++fails; } } while (0)
int main() {
  // Shape of VMA 3.x detailed JSON (VmaJsonWriter: "key": value, entries inside Suballocations / DedicatedAllocations).
  const char* j =
      "{\"DefaultPools\": {\"Type 0\": {\"Blocks\": {\"0\": {\"MapRefs\": 0, \"TotalBytes\": 1000, \"Suballocations\": ["
      "{\"Offset\": 0, \"Type\": \"IMAGE_OPTIMAL\", \"Size\": 1048576, \"Usage\": 6},"
      "{\"Offset\": 1048576, \"Type\": \"FREE\", \"Size\": 4096},"
      "{\"Offset\": 1052672, \"Type\": \"IMAGE_OPTIMAL\", \"Size\": 2097152, \"Usage\": 22},"
      "{\"Offset\": 3149824, \"Type\": \"BUFFER\", \"Size\": 65536, \"Usage\": 130, \"Name\": \"x\"}]}}}},"
      "\"DedicatedAllocations\": [{\"Type\": \"IMAGE_OPTIMAL\", \"Size\": 8388608, \"Usage\": 36},"
      "{\"Type\": \"IMAGE_LINEAR\", \"Size\": 512, \"Usage\": 4}, {\"Type\": \"UNKNOWN\", \"Size\": 7, \"Usage\": 0}]}";
  nb_vma::Kinds k = nb_vma::Parse(j);
  CHECK(k.tex == 1048576 + 512 && k.n_tex == 2);
  CHECK(k.rt == 2097152 + 8388608 && k.n_rt == 2);
  CHECK(k.buf == 65536 && k.n_buf == 1);
  CHECK(k.other == 7);
  CHECK(k.big_tex == 1048576);
  // "Type 0" (pool name keys) must not confuse the scanner; a lone FREE at the end is skipped.
  nb_vma::Kinds e = nb_vma::Parse("{\"Type\": \"FREE\", \"Size\": 99}");
  CHECK(e.rt + e.tex + e.buf + e.other == 0);
  std::printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
  return fails ? 1 : 0;
}
