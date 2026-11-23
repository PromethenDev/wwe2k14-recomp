// Victory EVP donor fallback (#142): when the WWE '13 source has no victory, BuildWinningPac
// reuses the 2K14 donor's EVP but must retarget its own-number word (Be16 at expanded +32,
// 6000+donor) to 6000+target, or the game plays the per-gender default victory. The test
// exercises the exported shared form of that logic, WWE13SharedRetargetEvp.
#include "../wwe13_convert.h"
#include "../wwe13_shared.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wwe2k14::launcher;

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

uint16_t Be16(const std::vector<uint8_t>& bytes, size_t at) {
  return static_cast<uint16_t>((uint16_t(bytes[at]) << 8) | bytes[at + 1]);
}

void PutBe16(std::vector<uint8_t>* bytes, size_t at, uint16_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value >> 8);
  (*bytes)[at + 1] = static_cast<uint8_t>(value);
}

}  // namespace

int main() {
  try {
    constexpr uint32_t kDonorId = 152;
    constexpr uint32_t kTargetId = 400;
    constexpr uint16_t kOldKey = static_cast<uint16_t>(6000 + kDonorId);   // 6152
    constexpr uint16_t kNewKey = static_cast<uint16_t>(6000 + kTargetId);  // 6400

    std::vector<uint8_t> block(40, 0);  // an expanded victory EVP's first block, own number at +32
    PutBe16(&block, 32, kOldKey);

    {  // The fix: the donor fallback retargets 6000+donor to 6000+target and changes nothing else.
      const WWE13ConvertBytes raw = StoreWWE13BpeLiteral(block);
      WWE13ConvertBytes out;
      std::string error;
      Check(WWE13SharedRetargetEvp(raw, kNewKey, kOldKey, &out, &error),
            "retarget with the matching old key must succeed: " + error);
      Check(error.empty(), "a successful retarget must not set an error");
      WWE13ConvertBytes decoded;
      Check(DecodeWWE13Bpe(out, &decoded, &error), "the retargeted EVP must still decode: " + error);
      Check(decoded.size() == block.size(), "the expanded block keeps its size");
      Check(Be16(decoded, 32) == kNewKey, "the own-number field is now 6000+target");
      Check(Be16(decoded, 32) != kOldKey, "the donor's own number is gone");
      for (size_t i = 0; i < decoded.size(); ++i) {
        if (i == 32 || i == 33) continue;
        Check(decoded[i] == block[i], "only the own-number word changes (byte " + std::to_string(i) + ")");
      }
    }
    {  // A wrong old key is refused.
      const WWE13ConvertBytes raw = StoreWWE13BpeLiteral(block);
      WWE13ConvertBytes out;
      std::string error;
      Check(!WWE13SharedRetargetEvp(raw, kNewKey, 9999, &out, &error), "a mismatched old key must be refused");
      Check(!error.empty(), "the mismatch refusal explains itself");
    }
    {  // Bytes that are not a BPE file are refused.
      const WWE13ConvertBytes raw = {'n', 'o', 't', 'b', 'p', 'e'};
      WWE13ConvertBytes out;
      std::string error;
      Check(!WWE13SharedRetargetEvp(raw, kNewKey, kOldKey, &out, &error), "non-BPE input must be refused");
      Check(!error.empty(), "the non-BPE refusal explains itself");
    }
    std::cout << "wwe13_victory_retarget_test: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "wwe13_victory_retarget_test: FAIL: " << error.what() << "\n";
    return 1;
  }
}
