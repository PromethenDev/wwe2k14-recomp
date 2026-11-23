// Issue plan step 5 (11-18): split a detailed vmaBuildStatsString JSON into render targets / textures / buffers.
// Every live allocation prints "Type": "<name>", "Size": N, "Usage": U (VmaAllocation_T::PrintParameters); FREE ranges
// print Type + Size only and are skipped. Image usage 0x10 (colour) / 0x20 (depth) attachment = target.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace nb_vma {
struct Kinds {
  uint64_t rt = 0, tex = 0, buf = 0, other = 0, n_rt = 0, n_tex = 0, n_buf = 0, big_tex = 0;
};
inline const char* NumberAfter(const char* from, const char* key, const char* limit, uint64_t* v) {
  const char* k = std::strstr(from, key);
  if (!k || (limit && k > limit)) return nullptr;
  k += std::strlen(key);
  while (*k && (*k < '0' || *k > '9')) {
    if (*k == '"' || *k == '{' || *k == '}') return nullptr;  // the key's value is not a number
    ++k;
  }
  *v = std::strtoull(k, nullptr, 10);
  return k;
}
inline Kinds Parse(const char* json) {
  Kinds r;
  const char* q = json;
  while ((q = std::strstr(q, "\"Type\"")) != nullptr) {
    const char* open = std::strchr(q + 6, '"');  // opening quote of the value
    if (!open) break;
    const char* name = open + 1;
    const char* next_type = std::strstr(name, "\"Type\"");
    uint64_t size = 0, usage = 0;
    const char* after_size = NumberAfter(name, "\"Size\"", next_type, &size);
    if (!after_size) { q = name; continue; }
    q = after_size;
    if (std::strncmp(name, "FREE", 4) == 0 || !NumberAfter(after_size, "\"Usage\"", next_type, &usage)) continue;
    if (std::strncmp(name, "IMAGE", 5) == 0) {
      if (usage & 0x30) { r.rt += size; ++r.n_rt; }
      else { r.tex += size; ++r.n_tex; if (size > r.big_tex) r.big_tex = size; }
    } else if (std::strncmp(name, "BUFFER", 6) == 0) {
      r.buf += size; ++r.n_buf;
    } else {
      r.other += size;
    }
  }
  return r;
}
}  // namespace nb_vma
