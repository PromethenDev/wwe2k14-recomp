#include "../../d3d/const_used_copy.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

using wwe2k14::d3d::const_used::Copy;
using wwe2k14::d3d::const_used::DifferenceCount;
using wwe2k14::d3d::const_used::HasVerifiedMap;
using wwe2k14::d3d::const_used::IsAll;
using wwe2k14::d3d::const_used::kBlockBytes;
using wwe2k14::d3d::const_used::kWords;
using wwe2k14::d3d::const_used::SelectSafeUsage;
using wwe2k14::d3d::const_used::SetAll;
using wwe2k14::d3d::const_used::ShaderStage;
using wwe2k14::d3d::const_used::UsedRowsEqual;

namespace {
int failures = 0;

void Check(bool condition, const char* label) {
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s\n", label);
  ++failures;
}

void Fill(std::array<uint8_t, kBlockBytes>& block, uint8_t seed) {
  for (size_t i = 0; i < block.size(); ++i) block[i] = uint8_t(seed + i * 13u);
}

bool RowIs(const uint8_t* block, uint32_t row, const uint8_t* expected) {
  return std::memcmp(block + row * 16, expected + row * 16, 16) == 0;
}

bool RowZero(const uint8_t* block, uint32_t row) {
  static constexpr uint8_t zero[16] = {};
  return std::memcmp(block + row * 16, zero, sizeof(zero)) == 0;
}
}  // namespace

int main() {
  std::array<uint8_t, kBlockBytes> dst{}, first{}, second{}, full{};
  uint64_t retained[kWords]{};
  uint64_t verified_usage[kWords] = {UINT64_C(0xF), 0, 0, UINT64_C(1) << 63};  // 01E48 PS: c0-c3, c255
  uint64_t guarded_usage[kWords]{};
  Check(HasVerifiedMap(ShaderStage::kPixel, 0x01E48C29D57B6A7Bull),
        "audited packaged PS key is admitted to sparse copies");
  Check(!HasVerifiedMap(ShaderStage::kVertex, 0x01E48C29D57B6A7Bull),
        "stage is part of the audited key identity");
  constexpr uint64_t kSetBKeyXor = UINT64_C(0x5E7B000000000000);
  constexpr uint64_t kF24KeyXor = UINT64_C(0xF240000000000000);
  Check(!HasVerifiedMap(ShaderStage::kPixel, 0x01E48C29D57B6A7Bull ^ kSetBKeyXor),
        "shader-set B effective key does not inherit the guest map whitelist entry");
  Check(!HasVerifiedMap(ShaderStage::kPixel, 0x01E48C29D57B6A7Bull ^ kF24KeyXor),
        "float24 effective key does not inherit the base map whitelist entry");
  Check(SelectSafeUsage(ShaderStage::kPixel, 0x01E48C29D57B6A7Bull, verified_usage, false, false,
                        guarded_usage) &&
            std::memcmp(guarded_usage, verified_usage, sizeof(verified_usage)) == 0,
        "audited static shader retains its exact sparse bitmap");
  Check(!SelectSafeUsage(ShaderStage::kPixel, 0x01E48C29D57B6A7Bull ^ kSetBKeyXor, verified_usage,
                         false, false, guarded_usage) && IsAll(guarded_usage),
        "shader-set B effective key fails closed to all rows");
  Check(!SelectSafeUsage(ShaderStage::kPixel, 0x01E48C29D57B6A7Bull ^ kF24KeyXor, verified_usage,
                         false, false, guarded_usage) && IsAll(guarded_usage),
        "float24 effective key fails closed to all rows");
  Check(!SelectSafeUsage(ShaderStage::kVertex, 0x089821F952D2D4AAull, verified_usage, false, false,
                         guarded_usage) && IsAll(guarded_usage),
        "unarchived dynamic VS key fails closed to all rows");
  Check(!SelectSafeUsage(ShaderStage::kPixel, 0x01E48C29D57B6A7Bull, verified_usage, true, false,
                         guarded_usage) && IsAll(guarded_usage),
        "dynamic addressing fails closed even for an audited key");
  Check(!SelectSafeUsage(ShaderStage::kPixel, 0xEFA4A1431609E60Dull, verified_usage, false, true,
                         guarded_usage) && IsAll(guarded_usage),
        "native PS replacement fails closed even when its guest key is audited");
  Check(!SelectSafeUsage(ShaderStage::kPixel, 0x01E48C29D57B6A7Bull, nullptr, false, false,
                         guarded_usage) && IsAll(guarded_usage),
        "missing usage bitmap fails closed to all rows");

  uint64_t first_usage[kWords] = {UINT64_C(1) | (UINT64_C(1) << 3), UINT64_C(1), 0,
                                  UINT64_C(1) << 63};  // c0, c3, c64, c255
  uint64_t second_usage[kWords] = {(UINT64_C(1) << 3) | (UINT64_C(1) << 5), 0, 0, 0};
  Fill(first, 17);
  Fill(second, 91);

  auto result = Copy(dst.data(), retained, first.data(), first_usage, true);
  Check(result.copied_registers == 4 && result.cleared_registers == 0 && !result.full_copy,
        "first sparse copy reports only used rows");
  Check(RowIs(dst.data(), 0, first.data()) && RowIs(dst.data(), 3, first.data()) &&
            RowIs(dst.data(), 64, first.data()) && RowIs(dst.data(), 255, first.data()),
        "first sparse copy preserves used rows byte-for-byte");
  Check(RowZero(dst.data(), 1) && RowZero(dst.data(), 63) && RowZero(dst.data(), 254),
        "never-used rows remain zero");

  result = Copy(dst.data(), retained, second.data(), second_usage, true);
  Check(result.copied_registers == 2 && result.cleared_registers == 3 && !result.full_copy,
        "pool reuse copies new rows and clears stale used rows");
  Check(RowIs(dst.data(), 3, second.data()) && RowIs(dst.data(), 5, second.data()),
        "pool reuse preserves current rows byte-for-byte");
  Check(RowZero(dst.data(), 0) && RowZero(dst.data(), 64) && RowZero(dst.data(), 255),
        "pool reuse clears prior-only rows");

  result = Copy(dst.data(), retained, full.data(), nullptr, true);
  Check(result.full_copy && result.copied_registers == 256 && UsedRowsEqual(dst.data(), full.data(), retained),
        "missing usage map safely falls back to full copy");
  Fill(first, 125);
  (void)SelectSafeUsage(ShaderStage::kVertex, 0x089821F952D2D4AAull, verified_usage, false, false,
                        guarded_usage);
  result = Copy(dst.data(), retained, first.data(), guarded_usage, true);
  Check(result.full_copy && std::memcmp(dst.data(), first.data(), kBlockBytes) == 0,
        "unverified-key fallback is byte-identical to the original full block");
  Fill(first, 29);
  uint64_t one_usage[kWords] = {UINT64_C(1) << 7, 0, 0, 0};
  result = Copy(dst.data(), retained, first.data(), one_usage, true);
  Check(result.copied_registers == 1 && result.cleared_registers == 255,
        "sparse copy after fallback clears every stale full-copy row");
  Check(RowIs(dst.data(), 7, first.data()) && RowZero(dst.data(), 6) && RowZero(dst.data(), 255),
        "fallback-to-sparse transition leaves canonical unused zeroes");

  SetAll(retained);
  Fill(second, 43);
  result = Copy(dst.data(), retained, second.data(), one_usage, false);
  Check(result.full_copy && result.copied_registers == 256 && RowIs(dst.data(), 255, second.data()),
        "legacy mode preserves the entire captured block");
  Fill(first, 201);
  result = Copy(dst.data(), retained, first.data(), one_usage, true);
  Check(result.cleared_registers == 255 && RowIs(dst.data(), 7, first.data()) && RowZero(dst.data(), 255),
        "sparse mode after legacy mode clears formerly captured unused rows");

  uint64_t all[kWords];
  SetAll(all);
  result = Copy(dst.data(), retained, second.data(), all, true);
  Check(result.full_copy && result.copied_registers == 256 && DifferenceCount(dst.data(), second.data(), all) == 0,
        "all-register dynamic map remains byte-identical through full copy");

  uint64_t check_usage[kWords] = {UINT64_C(1) << 9, 0, 0, 0};
  Fill(first, 73);
  result = Copy(dst.data(), retained, first.data(), check_usage, true);
  std::memcpy(full.data(), first.data(), kBlockBytes);
  Check(DifferenceCount(dst.data(), full.data(), check_usage) == 0,
        "CHECK sees byte-identical used rows in full and sparse blocks");
  dst[0] ^= 1;
  Check(DifferenceCount(dst.data(), full.data(), check_usage) == 0,
        "CHECK ignores unused rows when comparing full and sparse blocks");
  dst[9 * 16 + 4] ^= 1;
  Check(DifferenceCount(dst.data(), full.data(), check_usage) == 1 &&
            !UsedRowsEqual(dst.data(), full.data(), check_usage),
        "CHECK reports a changed used row");

  if (failures) return 1;
  std::puts("const-used-copy-test: PASS");
  return 0;
}
