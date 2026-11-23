// Small cached integer knob table used by the FIX_ALT A/B experiment.
#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace nr::backend {
namespace fix_alt_knobs_detail {

constexpr size_t kMaxOverrides = 16;
constexpr size_t kMaxKnobs = 16;
constexpr size_t kNameCapacity = 96;
constexpr size_t kValueCapacity = 64;

struct Override {
  char name[kNameCapacity]{};
  int64_t value = 0;
  bool valid = false;
};

struct CachedKnob {
  char name[kNameCapacity]{};
  int64_t normal = 0;
  int64_t phase_b = 0;
  bool has_normal = false;
  bool has_phase_b = false;
  std::atomic<bool> ready{false};
};

struct Table {
  std::array<Override, kMaxOverrides> overrides{};
  size_t override_count = 0;
  std::array<CachedKnob, kMaxKnobs> knobs{};
  std::mutex insert_mutex;
  std::atomic<bool> phase_b{false};
};

inline Table& GetTable() {
  static Table table;
  return table;
}

inline bool ParseInt64(const char* first, const char* last, int64_t* value) {
  while (first != last && std::isspace(static_cast<unsigned char>(*first))) ++first;
  while (first != last && std::isspace(static_cast<unsigned char>(last[-1]))) --last;
  const size_t size = size_t(last - first);
  if (size == 0 || size >= kValueCapacity) return false;

  char text[kValueCapacity];
  std::memcpy(text, first, size);
  text[size] = '\0';
  errno = 0;
  char* end = nullptr;
  const long long parsed = std::strtoll(text, &end, 10);
  if (errno == ERANGE || end == text || end != text + size) return false;
  *value = static_cast<int64_t>(parsed);
  return true;
}

inline void ParseOverrides(Table& table) {
  const char* config = std::getenv("WWE2K14_NR_FIX_ALT_B");
  if (!config) return;

  const char* segment = config;
  while (*segment) {
    const char* end = std::strchr(segment, ';');
    if (!end) end = segment + std::strlen(segment);
    const char* equals = static_cast<const char*>(std::memchr(segment, '=', size_t(end - segment)));
    if (equals) {
      const char* name_first = segment;
      const char* name_last = equals;
      while (name_first != name_last && std::isspace(static_cast<unsigned char>(*name_first))) ++name_first;
      while (name_first != name_last && std::isspace(static_cast<unsigned char>(name_last[-1]))) --name_last;
      int64_t value = 0;
      if (name_first != name_last && size_t(name_last - name_first) < kNameCapacity &&
          ParseInt64(equals + 1, end, &value)) {
        char name[kNameCapacity]{};
        std::memcpy(name, name_first, size_t(name_last - name_first));
        size_t existing = 0;
        while (existing < table.override_count && std::strcmp(table.overrides[existing].name, name) != 0) ++existing;
        if (existing < kMaxOverrides) {
          Override& entry = table.overrides[existing];
          std::memcpy(entry.name, name, std::strlen(name) + 1);
          entry.value = value;  // The last valid occurrence wins.
          entry.valid = true;
          if (existing == table.override_count) ++table.override_count;
        }
      }
    }
    if (!*end) break;
    segment = end + 1;
  }
}

inline Override* FindOverride(Table& table, const char* name) {
  for (size_t i = 0; i < table.override_count; ++i) {
    if (table.overrides[i].valid && std::strcmp(table.overrides[i].name, name) == 0) return &table.overrides[i];
  }
  return nullptr;
}

inline CachedKnob* FindCached(Table& table, const char* name) {
  for (CachedKnob& knob : table.knobs) {
    if (knob.ready.load(std::memory_order_acquire) && std::strcmp(knob.name, name) == 0) return &knob;
  }
  return nullptr;
}

inline CachedKnob* GetOrInsert(Table& table, const char* name) {
  if (CachedKnob* cached = FindCached(table, name)) return cached;
  std::lock_guard<std::mutex> lock(table.insert_mutex);
  if (CachedKnob* cached = FindCached(table, name)) return cached;

  for (CachedKnob& knob : table.knobs) {
    if (knob.ready.load(std::memory_order_relaxed)) continue;
    const size_t name_size = std::strlen(name);
    if (name_size >= kNameCapacity) return nullptr;
    std::memcpy(knob.name, name, name_size + 1);
    if (const char* value = std::getenv(name)) knob.has_normal = ParseInt64(value, value + std::strlen(value), &knob.normal);
    if (Override* override_value = FindOverride(table, name)) {
      knob.has_phase_b = true;
      knob.phase_b = override_value->value;
    }
    knob.ready.store(true, std::memory_order_release);
    return &knob;
  }
  return nullptr;
}

inline void EnsureOverridesParsed(Table& table) {
  // C++ function-local static initialization is a one-time, thread-safe environment snapshot.
  static const bool parsed = [&table] {
    ParseOverrides(table);
    return true;
  }();
  (void)parsed;
}

}  // namespace fix_alt_knobs_detail

struct FixAltKnob {
  const fix_alt_knobs_detail::CachedKnob* cached = nullptr;
  const std::atomic<bool>* phase_b = nullptr;
};

inline void SetFixAltPhaseB(bool phase_b) {
  fix_alt_knobs_detail::GetTable().phase_b.store(phase_b, std::memory_order_release);
}

inline FixAltKnob FixAltKnobFor(const char* env_name) {
  using namespace fix_alt_knobs_detail;
  if (!env_name || !*env_name) return {};
  Table& table = GetTable();
  EnsureOverridesParsed(table);
  CachedKnob* knob = GetOrInsert(table, env_name);
  return {knob, &table.phase_b};
}

inline int64_t FixAltInt(FixAltKnob knob, int64_t default_value) {
  if (!knob.cached || !knob.phase_b) return default_value;
  // The handle is resolved once by its call site. In phase A (including the
  // default, FIX_ALT-off configuration) this is the only per-call state test;
  // the normal environment value was captured when the handle was resolved.
  if (knob.phase_b->load(std::memory_order_acquire) && knob.cached->has_phase_b)
    return knob.cached->phase_b;
  return knob.cached->has_normal ? knob.cached->normal : default_value;
}

}  // namespace nr::backend
