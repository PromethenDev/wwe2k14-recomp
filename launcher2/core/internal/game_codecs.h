#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wwe2k14::launcher::internal {

// Read-only wrappers around the game archive decoders shared with the WWE 2K14 exporter.
bool DecodeWweXCompress(const std::vector<uint8_t>& input, std::vector<uint8_t>& output,
                        std::string& error);
bool DecodeWweBpe(const std::vector<uint8_t>& input, std::vector<uint8_t>& output,
                  std::string& error);

}  // namespace wwe2k14::launcher::internal
