// Pure decision of the "multi-man shadows" option (card G70, docs/tasks/G70-option-report.md). No game or logging includes, so
// src/native/tests/multi-shadows-test.cpp can check it on the host.
//
// The game creates its plane shadow pass (the wrestler + rope caster map the mat reads) only when the wrestler count is under 3.
// Two call sites decide that, and nothing else reads their result: sub_822893F0 (return address 0x822894B8) and sub_827F8040
// (0x827F8058); both then also test G[24] and G[19], which stay in force. Every other user of the count keeps the real number.
#pragma once
#include <cstdint>

namespace wwe2k14::multishadows {
constexpr uint32_t kCreatorReturnA = 0x822894B8u;  // sub_822893F0
constexpr uint32_t kCreatorReturnB = 0x827F8058u;  // sub_827F8040
constexpr int32_t kGameLimit = 2;                  // the game's test is "count < 3"
// sub_827E5228 counts 6 slots outside online play. Larger values only come from its online branches: left as the game wants them.
constexpr int32_t kMaxForced = 6;

constexpr bool IsCreatorCall(uint32_t return_address) {
  return return_address == kCreatorReturnA || return_address == kCreatorReturnB;
}
// The count the caller at return_address should see.
constexpr int32_t ReportedCount(uint32_t return_address, int32_t real_count, bool enabled) {
  return enabled && IsCreatorCall(return_address) && real_count > kGameLimit && real_count <= kMaxForced ? kGameLimit : real_count;
}
// Ladder and TLC (card G70, docs/tasks/G70-audit-report.md): the game's flags G[19] and G[24] also stop the pass. With the option on, a match of 3 to 6
// wrestlers gets it anyway. Two-man ladder matches stay as the game draws them.
constexpr bool ForceWithFlags(int32_t real_count, bool g19, bool g24, bool enabled) {
  return enabled && real_count > kGameLimit && real_count <= kMaxForced && (g19 || g24);
}
}  // namespace wwe2k14::multishadows
