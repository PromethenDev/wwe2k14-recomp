// Host-only checks for the pure sampled-detector classifiers.
#include "../glitch_detectors_core.h"

#include <cstdio>

using namespace wwe2k14::d3d::glitch::core;

int main() {
  uint32_t bad = 0;
  auto check = [&](bool value, const char* name) {
    if (!value) {
      ++bad;
      std::printf("FAIL %s\n", name);
    }
  };

  check(ParseFrameInterval(nullptr) == 0, "unset interval disables sampling");
  check(ParseFrameInterval("") == 0, "empty interval disables sampling");
  check(ParseFrameInterval("0") == 0, "zero interval disables sampling");
  check(ParseFrameInterval("60") == 60, "positive interval accepted");
  check(ParseFrameInterval("60x") == 0, "trailing junk rejected");
  check(ParseFrameInterval("4294967296") == 0, "overflow rejected");
  check(!IsSampleFrame(59, 60) && IsSampleFrame(60, 60) && IsSampleFrame(0, 60), "sample frame cadence");
  check(!IsSampleFrame(7, 0), "zero interval never samples");

  check(!IsNonFiniteFloatBits(0x3F800000u), "one is finite");
  check(IsNonFiniteFloatBits(0x7F800000u), "positive infinity recognized");
  check(IsNonFiniteFloatBits(0xFF800000u), "negative infinity recognized");
  check(IsNonFiniteFloatBits(0x7FC00000u), "NaN recognized");
  const uint32_t words[] = {0x3F800000u, 0x7F800000u, 0xFF800000u};
  uint32_t bits = 0;
  check(FirstNonFiniteFloat(words, sizeof(words), &bits) == 1 && bits == 0x7F800000u,
        "first non-finite word and bits returned");
  check(FirstNonFiniteFloat(words, sizeof(uint32_t)) == 1, "finite bank has no non-finite word");

  const uint32_t constants[6 * 4] = {
      0x7FC00000u, 0, 0, 0,  // unused c0: ignored
      0, 0x7F800000u, 0, 0,  // used c1: one report for the register
      0, 0, 0xFF800000u, 0,  // unused c2: ignored
      0, 0, 0, 0,             // c3: finite
      0, 0, 0, 0x7FC00000u,  // used c4: one report for the register
      0, 0, 0, 0};
  const uint64_t usage[4] = {(uint64_t(1) << 1) | (uint64_t(1) << 4), 0, 0, 0};
  uint32_t found[2] = {}, components[2] = {}, found_bits[2] = {};
  size_t found_count = 0;
  ForEachNonFiniteUsedFloatConstant(constants, sizeof(constants), usage,
                                    [&](uint32_t reg, uint32_t component, uint32_t value) {
    if (found_count < 2) {
      found[found_count] = reg;
      components[found_count] = component;
      found_bits[found_count] = value;
    }
    ++found_count;
  });
  check(found_count == 2 && found[0] == 1 && components[0] == 1 && found_bits[0] == 0x7F800000u && found[1] == 4 &&
            components[1] == 3 && found_bits[1] == 0x7FC00000u,
        "non-finite scan visits only used registers once each");

  UniqueKeys nonfinite_once(8);
  const UniqueKey vs_c4 = NonFiniteConstantKey(2, 101, 202, 0, 4);
  const UniqueKey ps_c4 = NonFiniteConstantKey(2, 101, 202, 1, 4);
  check(nonfinite_once.Insert(vs_c4) && !nonfinite_once.Insert(vs_c4), "non-finite pair/register key deduplicates repeats");
  check(nonfinite_once.Insert(ps_c4), "stage-qualified register remains distinct in shader pair");
  check(nonfinite_once.Insert(NonFiniteConstantKey(2, 101, 203, 0, 4)), "different shader pair remains distinct");

  UniqueKeys once(2);
  const UniqueKey a{1, 10, 20, 30, 40}, b{2, 10, 20, 30, 40}, c{3, 10, 20, 30, 40};
  check(once.Insert(a), "first unique key claimed");
  check(!once.Insert(a), "duplicate key is rate limited");
  check(once.Insert(b), "second unique key claimed");
  check(!once.Insert(c) && once.size() == 2, "key cap bounds unique log set");

  std::printf("glitch detector core: %s (%u failures)\n", bad ? "FAIL" : "PASS", bad);
  return bad ? 1 : 0;
}
