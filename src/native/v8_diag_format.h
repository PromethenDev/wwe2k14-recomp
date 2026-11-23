#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wwe2k14::v8diag {

struct ShaderKey {
  uint64_t ps = 0;
  uint64_t vs = 0;
  bool operator<(const ShaderKey& other) const { return ps < other.ps || (ps == other.ps && vs < other.vs); }
};

struct ShaderRow {
  ShaderKey key;
  uint64_t gpu_ns = 0;
  uint64_t draws = 0;
};

inline std::string FormatShaderTop(uint64_t first, uint64_t last, uint32_t frames, std::size_t shader_count,
                                   std::vector<ShaderRow> rows) {
  std::sort(rows.begin(), rows.end(), [](const ShaderRow& a, const ShaderRow& b) {
    if (a.gpu_ns != b.gpu_ns) return a.gpu_ns > b.gpu_ns;
    return a.key < b.key;
  });
  if (rows.size() > 30) rows.resize(30);
  const double divisor = frames ? double(frames) : 1.0;
  char buf[256];
  std::snprintf(buf, sizeof(buf), "[nr-gpu-shader-top] frames=%llu-%llu n=%u shaders=%zu\n",
                static_cast<unsigned long long>(first), static_cast<unsigned long long>(last), frames, shader_count);
  std::string result = buf;
  for (size_t i = 0; i < rows.size(); ++i) {
    const ShaderRow& row = rows[i];
    std::snprintf(buf, sizeof(buf),
                  "[nr-gpu-shader-top] #%zu ps=%016llx vs=%016llx ms/f=%.4f draws/f=%.2f\n", i + 1,
                  static_cast<unsigned long long>(row.key.ps), static_cast<unsigned long long>(row.key.vs),
                  double(row.gpu_ns) * 1.0e-6 / divisor, double(row.draws) / divisor);
    result += buf;
  }
  return result;
}

}  // namespace wwe2k14::v8diag
