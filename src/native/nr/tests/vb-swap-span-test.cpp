// Host test for the upload-ring swap helper. g++ -std=c++20 -O2 -Wall -Wextra vb-swap-span-test.cpp -o vb-swap-span-test
#include "../nr_vb_swap.h"

#include <cassert>
#include <cstring>
#include <cstdint>

int main() {
  uint8_t input[96], legacy[96], span[96];
  for (size_t i = 0; i < sizeof(input); ++i) input[i] = uint8_t(i * 73 + 19);
  for (uint32_t endian = 0; endian <= 4; ++endian) {
    for (size_t bytes = 0; bytes <= 33; ++bytes) {
      for (size_t src_off = 0; src_off <= 3; ++src_off) {
        for (size_t dst_off = 0; dst_off <= 5; ++dst_off) {
          std::memset(legacy, 0xA5, sizeof(legacy));
          std::memset(span, 0xA5, sizeof(span));
          nr::vbswap::CopySwapDwords(legacy + dst_off, input + src_off, bytes, endian);
          nr::vbswap::CopySwapDwordsToSpan(span + dst_off, input + src_off, bytes, endian);
          assert(std::memcmp(legacy, span, sizeof(legacy)) == 0);
          assert(dst_off == 0 || (span[dst_off - 1] == 0xA5 && legacy[dst_off - 1] == 0xA5));
          assert(dst_off + bytes == sizeof(span) ||
                 (span[dst_off + bytes] == 0xA5 && legacy[dst_off + bytes] == 0xA5));
        }
      }
    }
  }
}
