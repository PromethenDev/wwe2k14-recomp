// Timeline tracker core - see timeline_core.h. No SDK headers here (host-tested).
#include "timeline_core.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace wwe2k14::timeline {

void AppendText(char* out, size_t cap, size_t* pos, const char* text) {
  if (!cap) return;
  size_t p = *pos < cap ? *pos : cap - 1;
  while (text && *text && p + 1 < cap) out[p++] = *text++;
  out[p] = '\0';
  *pos = p;
}

void AppendUnsigned(char* out, size_t cap, size_t* pos, uint64_t value, int min_digits) {
  char digits[24];
  int n = 0;
  do {
    digits[n++] = char('0' + value % 10);
    value /= 10;
  } while (value && n < 20);
  while (n < min_digits && n < 20) digits[n++] = '0';
  char text[24];
  for (int i = 0; i < n; ++i) text[i] = digits[n - 1 - i];
  text[n] = '\0';
  AppendText(out, cap, pos, text);
}

void AppendHex(char* out, size_t cap, size_t* pos, uint64_t value, int min_digits) {
  char digits[20];
  int n = 0;
  do {
    digits[n++] = "0123456789ABCDEF"[value & 15];
    value >>= 4;
  } while (value && n < 16);
  while (n < min_digits && n < 16) digits[n++] = '0';
  char text[20];
  for (int i = 0; i < n; ++i) text[i] = digits[n - 1 - i];
  text[n] = '\0';
  AppendText(out, cap, pos, text);
}

size_t FormatTimeline(const char* header, const Entry* entries, size_t count, char* out, size_t cap) {
  size_t pos = 0;
  if (cap) out[0] = '\0';
  AppendText(out, cap, &pos, header);
  for (size_t i = 0; i < count; ++i) {
    const uint64_t s = entries[i].ms / 1000;
    AppendText(out, cap, &pos, "[");
    AppendUnsigned(out, cap, &pos, s / 3600, 2);
    AppendText(out, cap, &pos, ":");
    AppendUnsigned(out, cap, &pos, s / 60 % 60, 2);
    AppendText(out, cap, &pos, ":");
    AppendUnsigned(out, cap, &pos, s % 60, 2);
    AppendText(out, cap, &pos, "] ");
    AppendText(out, cap, &pos, entries[i].text);
    AppendText(out, cap, &pos, "\n");
  }
  return pos;
}

namespace {

// "3.4" from microseconds (one decimal), without stdio.
void AppendSeconds(char* out, size_t cap, size_t* pos, uint64_t us) {
  const uint64_t tenths = (us + 50000) / 100000;
  AppendUnsigned(out, cap, pos, tenths / 10);
  AppendText(out, cap, pos, ".");
  AppendUnsigned(out, cap, pos, tenths % 10);
}

void Restart(PerfWindow& w, uint64_t now_us) {
  w = PerfWindow{};
  w.started = true;
  w.start_us = now_us;
  w.previous_us = now_us;
}

}  // namespace

void AddPresent(PerfWindow& w, uint64_t now_us, NoteRing& ring) {
  if (!w.started) {
    Restart(w, now_us);
    return;
  }
  const uint64_t delta = now_us > w.previous_us ? now_us - w.previous_us : 0;
  char text[kTextBytes];
  size_t pos = 0;
  text[0] = '\0';
  // Pause rule: a gap over 2 s is not a slow frame but a stop (loading screen without presents, minimised window,
  // debugger). It is noted when frames resume and starts a fresh window, so it does not distort the fps/worst numbers.
  if (delta > kPauseUs) {
    AppendSeconds(text, sizeof(text), &pos, delta);
    AppendText(text, sizeof(text), &pos, " s (no new frames)");
    ring.Add(now_us / 1000, "PAUSE", text);
    Restart(w, now_us);
    return;
  }
  w.previous_us = now_us;
  ++w.frames;
  w.worst_us = std::max(w.worst_us, delta);
  if (delta > kSlowFrameUs) ++w.slow;
  if (delta > kHitchUs && w.hitch_notes < kMaxHitchNotesPerWindow) {
    ++w.hitch_notes;
    AppendUnsigned(text, sizeof(text), &pos, delta / 1000);
    AppendText(text, sizeof(text), &pos, " ms frame");
    ring.Add(now_us / 1000, "HITCH", text);
  }
  const uint64_t elapsed = now_us - w.start_us;
  if (elapsed < kPerfWindowUs) return;
  pos = 0;
  text[0] = '\0';
  AppendUnsigned(text, sizeof(text), &pos, (w.frames * 1000000 + elapsed / 2) / elapsed);
  AppendText(text, sizeof(text), &pos, " fps, worst ");
  AppendUnsigned(text, sizeof(text), &pos, w.worst_us / 1000);
  AppendText(text, sizeof(text), &pos, " ms, ");
  AppendUnsigned(text, sizeof(text), &pos, w.slow);
  AppendText(text, sizeof(text), &pos, w.slow == 1 ? " slow frame" : " slow frames");
  ring.Add(now_us / 1000, "perf", text);
  Restart(w, now_us);
}

bool FileAreaOf(const char* guest_path, bool is_directory, char* out, size_t cap) {
  if (!cap) return false;
  out[0] = '\0';
  if (!guest_path || !*guest_path) return false;
  const char* p = guest_path;
  if (const char* colon = std::strchr(p, ':')) {
    p = colon + 1;  // "D:\PAC\..." / "game:\PAC\..."
  } else if (std::strncmp(p, "\\Device\\", 8) == 0) {
    p += 8;  // "\Device\Cdrom0\PAC\...": drop the device name too
    while (*p && *p != '\\' && *p != '/') ++p;
  }
  // Split into components (at most 16; deeper paths keep their first 16).
  const char* begin[16];
  size_t length[16];
  size_t n = 0;
  while (*p && n < 16) {
    while (*p == '\\' || *p == '/') ++p;
    if (!*p) break;
    begin[n] = p;
    while (*p && *p != '\\' && *p != '/') ++p;
    length[n] = size_t(p - begin[n]);
    ++n;
  }
  if (!n) return false;
  size_t keep;
  const size_t leaf_len = length[n - 1];
  const char* leaf = begin[n - 1];
  const bool pac = !is_directory && leaf_len > 4 && std::tolower(leaf[leaf_len - 4]) == '.' &&
                   std::tolower(leaf[leaf_len - 3]) == 'p' && std::tolower(leaf[leaf_len - 2]) == 'a' &&
                   std::tolower(leaf[leaf_len - 1]) == 'c';
  if (pac) {
    keep = n;
  } else {
    const size_t folders = is_directory ? n : n - 1;
    keep = folders ? std::min<size_t>(folders, 2) : 1;  // a file at the root is its own area
  }
  size_t pos = 0;
  for (size_t i = 0; i < keep; ++i) {
    if (i) AppendText(out, cap, &pos, "\\");
    for (size_t c = 0; c < length[i] && pos + 1 < cap; ++c) out[pos++] = begin[i][c];
    out[pos] = '\0';
  }
  return pos > 0;
}

bool FileAreaTracker::Observe(const char* area) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& recent : recent_) {
    if (std::strncmp(recent, area, kTextBytes - 1) == 0) return false;
  }
  size_t pos = 0;
  recent_[next_][0] = '\0';
  AppendText(recent_[next_], kTextBytes, &pos, area);
  next_ = (next_ + 1) % kRecent;
  return true;
}

bool StuckDetector::Update(uint64_t now_ms, uint64_t last_present_ms, uint64_t input_changes, uint64_t last_file_ms) {
  samples_[next_] = Sample{now_ms, input_changes};
  next_ = (next_ + 1) % kSamples;
  count_ = std::min(count_ + 1, kSamples);
  // Button changes in the last 60 s: now minus the oldest sample still inside the window.
  uint64_t oldest_inputs = input_changes;
  uint64_t oldest_ms = now_ms;
  for (size_t i = 0; i < count_; ++i) {
    const Sample& s = samples_[i];
    if (s.ms + kStuckQuietMs >= now_ms && s.ms < oldest_ms) {
      oldest_ms = s.ms;
      oldest_inputs = s.inputs;
    }
  }
  const bool presenting = last_present_ms && now_ms <= last_present_ms + kPresentAliveMs;
  const bool pressing = input_changes - oldest_inputs >= kStuckMinInputs;
  const bool no_files = now_ms >= last_file_ms + kStuckQuietMs;
  const bool may_repeat = !noted_ || now_ms >= last_note_ms_ + kStuckRepeatMs;
  if (!(presenting && pressing && no_files && may_repeat)) return false;
  noted_ = true;
  last_note_ms_ = now_ms;
  return true;
}

namespace {
bool EndsWithNoCase(const char* s, const char* suffix) {
  const size_t len = std::strlen(s), suffix_len = std::strlen(suffix);
  if (len < suffix_len) return false;
  s += len - suffix_len;
  for (size_t i = 0; i < suffix_len; ++i) {
    char a = s[i], b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = char(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = char(b + ('a' - 'A'));
    if (a != b) return false;
  }
  return true;
}
}  // namespace

uint64_t LoadWatch::Pack(bool armed, uint32_t count, uint32_t fires, uint64_t ms) {
  return (armed ? kArmedMask : 0ull) | ((uint64_t(count & 0xffu) << 1) & kCountMask) |
         ((uint64_t(fires & 3u) << 9) & kFiresMask) | ((ms & 0x0000FFFFFFFFFFFFull) << kMsShift);
}

void LoadWatch::SetEnabled(bool enabled) {
  enabled_.store(enabled, std::memory_order_release);
  if (!enabled) state_.store(0, std::memory_order_release);
}

void LoadWatch::OnOpen(uint64_t now_ms, const char* path, bool ok) {
  if (!ok || !path || !enabled_.load(std::memory_order_acquire)) return;
  uint64_t old = state_.load(std::memory_order_relaxed);
  if (EndsWithNoCase(path, "MOT08.PAC")) {  // (re-)arm; keep the session fire count
    while (!state_.compare_exchange_weak(old, Pack(true, 0, Fires(old), now_ms), std::memory_order_relaxed)) {
    }
    return;
  }
  for (;;) {
    if (!(old & kArmedMask)) return;
    const uint32_t count = Count(old);
    if (count >= 0xffu) return;
    const uint64_t next = (old & ~kCountMask) | ((uint64_t(count + 1) << 1) & kCountMask);
    if (state_.compare_exchange_weak(old, next, std::memory_order_relaxed)) return;
  }
}

bool LoadWatch::Check(uint64_t now_ms) {
  if (!enabled_.load(std::memory_order_acquire)) return false;
  uint64_t old = state_.load(std::memory_order_relaxed);
  for (;;) {
    if (!(old & kArmedMask) || now_ms < Ms(old) + kLoadWatchWindowMs) return false;
    const uint32_t count = Count(old), fires = Fires(old);
    const bool fire = count < kLoadWatchMinOpens && fires < kLoadWatchMaxFires;
    if (state_.compare_exchange_weak(old, Pack(false, 0, fire ? fires + 1 : fires, 0), std::memory_order_relaxed)) {
      if (fire) last_count_.store(count, std::memory_order_relaxed);
      return fire;
    }
  }
}

uint32_t LoadWatch::LastCount() const { return last_count_.load(std::memory_order_relaxed); }

namespace {

void AppendUtf8(std::string& out, uint32_t c) {
  if (c < 0x80) {
    out += char(c);
  } else if (c < 0x800) {
    out += char(0xC0 | (c >> 6));
    out += char(0x80 | (c & 0x3F));
  } else {
    out += char(0xE0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  }
}

// The file as UTF-8 text. Surrogate pairs (never in the launcher's keys or plain values) become '?'.
std::string DecodeSettingsText(const std::string& bytes) {
  const auto byte = [&](size_t i) { return uint8_t(bytes[i]); };
  if (bytes.size() >= 2 && byte(0) == 0xFF && byte(1) == 0xFE) {
    std::string out;
    for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
      const uint32_t c = uint32_t(byte(i)) | uint32_t(byte(i + 1)) << 8;
      AppendUtf8(out, c >= 0xD800 && c <= 0xDFFF ? uint32_t('?') : c);
    }
    return out;
  }
  if (bytes.size() >= 3 && byte(0) == 0xEF && byte(1) == 0xBB && byte(2) == 0xBF) return bytes.substr(3);
  return bytes;
}

std::string Trim(const std::string& text) {
  const size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return std::string();
  const size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

}  // namespace

LauncherSettings ParseLauncherSettings(const std::string& bytes) {
  LauncherSettings result;
  result.found = true;
  const std::string text = DecodeSettingsText(bytes);
  size_t line_start = 0;
  while (line_start < text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string::npos) line_end = text.size();
    const std::string line = Trim(text.substr(line_start, line_end - line_start));
    line_start = line_end + 1;
    if (line.empty() || line[0] == '[' || line[0] == ';' || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    const std::string key = Trim(line.substr(0, eq));
    const std::string value = Trim(line.substr(eq + 1));
    if (key == "gpu") {
      if (!value.empty()) result.gpu = value;
      continue;
    }
    if (key == "game_folder" || value.empty() || value.find_first_of(":\\/") != std::string::npos) continue;
    result.settings.push_back(key + "=" + value);
  }
  return result;
}

std::string BuildHeader(const std::string& started, const std::string& build_id, const std::string& renderer,
                        const LauncherSettings& launcher) {
  std::string h;
  h += "WWE 2K14 timeline - times are hh:mm:ss since the game started\n";
  h += "started: " + started + "\n";
  h += "build: " + build_id + "\n";
  h += "renderer: " + renderer + "\n";
  h += "GPU (launcher choice): " + (launcher.found ? launcher.gpu : std::string("unknown")) + "\n";
  if (!launcher.found) {
    h += "settings: launcher settings file not found\n";
  } else {
    h += "settings:";
    if (launcher.settings.empty()) h += " none";
    h += "\n";
    for (const auto& e : launcher.settings) h += "  " + e + "\n";
  }
  h += "----\n";
  return h;
}

bool NoDeviceLookups::Summary(uint64_t now_ms, char* out, size_t cap) {
  if (now_ms < kNoDeviceSummaryMs) return false;
  const uint64_t count = count_.load(std::memory_order_relaxed);
  if (count == summarised_) return false;
  const uint64_t fresh = count - summarised_;
  summarised_ = count;
  size_t pos = 0;
  if (cap) out[0] = '\0';
  AppendUnsigned(out, cap, &pos, fresh);
  AppendText(out, cap, &pos, fresh == 1 ? " lookup on a device that is not present"
                                        : " lookups on devices that are not present");
  AppendText(out, cap, &pos, " (normal when DLC is not installed)");
  return true;
}

const char* FailureWord(uint32_t status) {
  return status == 0xC0000034u || status == 0xC000003Au ? "missing" : "failed";
}

bool FreezeWorthNoting(uint64_t now_ms, uint64_t last_present_ms, uint64_t* seconds_without_frames) {
  if (now_ms < last_present_ms + kFreezeNoteMs) return false;
  if (seconds_without_frames) *seconds_without_frames = (now_ms - last_present_ms) / 1000;
  return true;
}

size_t KeepNewestTimelines(const std::filesystem::path& dir, size_t keep) {
  std::error_code ec;
  std::vector<std::filesystem::path> files;
  for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
    const std::string name = item.path().filename().string();
    if (name.rfind("wwe2k14-timeline-", 0) == 0 && item.path().extension() == ".txt") files.push_back(item.path());
  }
  std::sort(files.begin(), files.end());
  size_t deleted = 0;
  while (files.size() > keep) {
    if (std::filesystem::remove(files.front(), ec)) ++deleted;
    files.erase(files.begin());
  }
  return deleted;
}

}  // namespace wwe2k14::timeline
