// Timeline tracker core (card LOG1): the parts of src/timeline.cpp that need no SDK, so the host test
// (src/native/tests/timeline-test.cpp) can build them with plain g++. Times are milliseconds (or microseconds where
// named _us) since the timeline started, i.e. session time.
//
// Everything a crash handler touches (Ring::Add, Ring::Snapshot, FormatTimeline, the Append* helpers) is lock-free and
// allocation-free and calls no stdio, so it can run in a POSIX signal handler or a Windows unhandled-exception filter.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace wwe2k14::timeline {

constexpr size_t kTextBytes = 112;   // one note's text (category column + message), NUL-terminated, truncated
constexpr size_t kNoteSlots = 512;   // notes kept in memory (and written to the file)
constexpr size_t kPathSlots = 8;     // last file paths kept for the STUCK? line
constexpr size_t kCategoryWidth = 7; // "perf   ", "STUCK? " ...

struct Entry {
  uint64_t sequence;  // n-th note ever added
  uint64_t ms;        // session time
  char text[kTextBytes];
};

// --- signal-safe text helpers (no stdio, no allocation) -------------------------------------------------------------

// Each appends at out[*pos], never past cap - 1, keeps out NUL-terminated and advances *pos.
void AppendText(char* out, size_t cap, size_t* pos, const char* text);
void AppendUnsigned(char* out, size_t cap, size_t* pos, uint64_t value, int min_digits = 1);
void AppendHex(char* out, size_t cap, size_t* pos, uint64_t value, int min_digits = 1);

// Fixed-size multi-writer ring of notes. Writers never block each other or the reader.
//
// Seqlock per slot: a writer takes a unique note number n from next_, marks slot n % N "being written" (odd sequence
// 2n+1), copies the entry, then publishes the even sequence 2n+2. A reader accepts a slot only if it reads 2n+2 both
// before and after copying the entry and the copied entry carries number n; otherwise the slot is torn (a writer was
// mid-copy, or a newer note lapped it) and is skipped. A skipped note is lost from that snapshot, never garbled.
template <size_t N>
class Ring {
 public:
  // category may be null (raw text). The text is truncated to kTextBytes - 1.
  void Add(uint64_t ms, const char* category, const char* text) {
    const uint64_t n = next_.fetch_add(1, std::memory_order_relaxed);
    Slot& slot = slots_[n % N];
    Entry entry;
    entry.sequence = n;
    entry.ms = ms;
    entry.text[0] = '\0';
    size_t pos = 0;
    if (category) {
      AppendText(entry.text, sizeof(entry.text), &pos, category);
      while (pos < kCategoryWidth) AppendText(entry.text, sizeof(entry.text), &pos, " ");
      AppendText(entry.text, sizeof(entry.text), &pos, " ");
    }
    AppendText(entry.text, sizeof(entry.text), &pos, text ? text : "");
    slot.sequence.store((n << 1) | 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);  // the odd mark is visible before any entry byte changes
    std::memcpy(&slot.entry, &entry, sizeof(entry));
    slot.sequence.store((n << 1) + 2, std::memory_order_release);
  }

  // Copies the newest notes (up to capacity) oldest -> newest into out; returns how many were copied.
  size_t Snapshot(Entry* out, size_t capacity) const {
    const uint64_t end = next_.load(std::memory_order_acquire);
    const uint64_t keep = capacity < N ? capacity : N;
    const uint64_t begin = end > keep ? end - keep : 0;
    size_t count = 0;
    for (uint64_t n = begin; n < end; ++n) {
      const Slot& slot = slots_[n % N];
      const uint64_t before = slot.sequence.load(std::memory_order_acquire);
      if (before != (n << 1) + 2) continue;  // not finished yet, or already overwritten by a newer note
      std::memcpy(&out[count], &slot.entry, sizeof(Entry));
      std::atomic_thread_fence(std::memory_order_acquire);  // the copy completes before the second check
      if (slot.sequence.load(std::memory_order_relaxed) != before || out[count].sequence != n) continue;
      ++count;
    }
    return count;
  }

  uint64_t Count() const { return next_.load(std::memory_order_relaxed); }

 private:
  struct Slot {
    std::atomic<uint64_t> sequence{0};
    Entry entry{};
  };
  std::atomic<uint64_t> next_{0};
  Slot slots_[N];
};

using NoteRing = Ring<kNoteSlots>;
using PathRing = Ring<kPathSlots>;

// Writes "<header><[hh:mm:ss] text\n per entry>" into out (NUL-terminated, truncated at cap). Returns the length.
size_t FormatTimeline(const char* header, const Entry* entries, size_t count, char* out, size_t cap);

// --- performance notes from presents ---------------------------------------------------------------------------------

constexpr uint64_t kPerfWindowUs = 10000000;  // one "perf" note per 10 s of frames
constexpr uint64_t kSlowFrameUs = 50000;      // "slow": over 50 ms (a 30 fps frame is 33 ms and does not count)
constexpr uint64_t kHitchUs = 250000;         // HITCH note per frame over 250 ms ...
constexpr uint64_t kPauseUs = 2000000;        // ... and a PAUSE note instead when no frame came for over 2 s
constexpr uint32_t kMaxHitchNotesPerWindow = 5;

struct PerfWindow {
  bool started = false;
  uint64_t start_us = 0;     // window start
  uint64_t previous_us = 0;  // last present
  uint64_t frames = 0;       // presents since start_us
  uint64_t worst_us = 0;
  uint64_t slow = 0;
  uint32_t hitch_notes = 0;
};

// One game present at session time now_us. Adds "perf", "HITCH" and "PAUSE" notes to ring.
void AddPresent(PerfWindow& window, uint64_t now_us, NoteRing& ring);

// --- file areas ------------------------------------------------------------------------------------------------------

// The area a guest path belongs to, for "files <area>" notes: the device prefix ("D:", "game:", "\Device\Cdrom0") is
// dropped and the first two folder levels kept ("PAC\MENU"); a .pac file keeps its full path ("PAC\CH\ch82.pac").
// A directory open counts all of its components as folders. Returns false for an empty path.
bool FileAreaOf(const char* guest_path, bool is_directory, char* out, size_t cap);

// Collapses repeated areas: Observe returns true (note it) only for an area that is not among the last 4 noted ones,
// so a loader alternating between two folders gives two notes, not hundreds. Thread-safe (short mutex: the observer
// runs on the game's file threads, a few hundred opens per second at most).
class FileAreaTracker {
 public:
  bool Observe(const char* area);

 private:
  static constexpr size_t kRecent = 4;
  std::mutex mutex_;
  char recent_[kRecent][kTextBytes] = {};
  size_t next_ = 0;
};

// --- stuck hint ------------------------------------------------------------------------------------------------------

constexpr uint64_t kStuckQuietMs = 60000;      // no file opened for 60 s ...
constexpr uint32_t kStuckMinInputs = 5;        // ... while buttons changed at least 5 times in those 60 s
constexpr uint64_t kStuckRepeatMs = 300000;    // at most one note per 5 minutes
constexpr uint64_t kPresentAliveMs = 2000;     // "screen kept running": a present within the last 2 s

// Called once per second by the writer thread. It is a hint, not a verdict: a player idling in a menu while pressing
// buttons that read no files (options screens, move lists) also matches. The note only says what was observed.
class StuckDetector {
 public:
  // input_changes: running count of button/trigger changes; last_file_ms: last successful open (0 = none yet).
  bool Update(uint64_t now_ms, uint64_t last_present_ms, uint64_t input_changes, uint64_t last_file_ms);

 private:
  struct Sample {
    uint64_t ms;
    uint64_t inputs;
  };
  static constexpr size_t kSamples = 64;  // one per second: a bit more than the 60 s window
  Sample samples_[kSamples] = {};
  size_t count_ = 0;
  size_t next_ = 0;
  uint64_t last_note_ms_ = 0;
  bool noted_ = false;
};

// --- Move-Set S&F load watch (card H1) -------------------------------------------------------------------------------
// The Signatures & Finishers screen starts its load with a guest open of ...MOT08.PAC. In 37 good laptop entries >= 11
// successful opens followed within 6 s; in the endless-loading entry only 4 followed and then none. LoadWatch arms on
// every MOT08.PAC open and Check() reports once when fewer than kLoadWatchMinOpens opens followed within
// kLoadWatchWindowMs, so the caller can request a freeze report (what every guest thread waits on). At most
// kLoadWatchMaxFires reports per session. OnOpen runs on the game's file threads, Check on the timeline writer: one
// packed atomic (armed | count | fires | armed_ms), no lock.
constexpr uint64_t kLoadWatchWindowMs = 15000;
constexpr uint32_t kLoadWatchMinOpens = 8;
constexpr uint32_t kLoadWatchMaxFires = 3;

class LoadWatch {
 public:
  void SetEnabled(bool enabled);
  void OnOpen(uint64_t now_ms, const char* path, bool ok);
  bool Check(uint64_t now_ms);  // true once per stalled load
  uint32_t LastCount() const;   // opens counted in the load that last fired

 private:
  static constexpr uint64_t kArmedMask = 1ull;
  static constexpr uint64_t kCountMask = 0xffull << 1;
  static constexpr uint64_t kFiresMask = 3ull << 9;
  static constexpr uint64_t kMsShift = 11;
  static constexpr uint64_t kMsMask = 0x0000FFFFFFFFFFFFull << kMsShift;

  static uint64_t Pack(bool armed, uint32_t count, uint32_t fires, uint64_t ms);
  static uint32_t Count(uint64_t state) { return uint32_t((state & kCountMask) >> 1); }
  static uint32_t Fires(uint64_t state) { return uint32_t((state & kFiresMask) >> 9); }
  static uint64_t Ms(uint64_t state) { return (state & kMsMask) >> kMsShift; }

  std::atomic<bool> enabled_{false};
  std::atomic<uint64_t> state_{0};
  std::atomic<uint32_t> last_count_{0};
};

// --- header and files ------------------------------------------------------------------------------------------------

// The player's launcher settings (wwe2k14-enhanced.ini in the exe folder, "key=value" lines under [Settings]).
struct LauncherSettings {
  bool found = false;
  std::string gpu = "Automatic";      // the gpu= value (adapter id such as 1002:15BF); empty means Automatic
  std::vector<std::string> settings;  // "key=value", file order; game_folder, empty values and path-like values
                                      // (':' '\' '/', may hold the player's user name) are left out
};

// Parses the raw file bytes: UTF-16LE with BOM (what the Windows launcher writes), UTF-8 with or without BOM.
LauncherSettings ParseLauncherSettings(const std::string& bytes);

// The session header.
std::string BuildHeader(const std::string& started, const std::string& build_id, const std::string& renderer,
                        const LauncherSettings& launcher);

// --- failed opens ---------------------------------------------------------------------------------------------------

// 0xC000000F (no such device): lookups on devices that are not mounted (DLC slots, \Device\Image) - dozens at every
// start without DLC. They are counted, not noted one by one: Summary() gives one line for the new lookups once 30 s
// of the session have passed (called at each file write). Count() is lock-free; Summary() has a single caller at a
// time (the file writer, under its mutex).
constexpr uint32_t kStatusNoSuchDevice = 0xC000000Fu;
constexpr uint64_t kNoDeviceSummaryMs = 30000;

class NoDeviceLookups {
 public:
  void Count() { count_.fetch_add(1, std::memory_order_relaxed); }
  // True (with the note text in out) when there are lookups not yet summarised and now_ms >= 30 s.
  bool Summary(uint64_t now_ms, char* out, size_t cap);

 private:
  std::atomic<uint64_t> count_{0};
  uint64_t summarised_ = 0;
};

// "missing" for not-found statuses (0xC0000034 name, 0xC000003A path), "failed" otherwise.
const char* FailureWord(uint32_t status);

// --- freeze report ---------------------------------------------------------------------------------------------------

// The SDK freeze logger sometimes reports while frames are still presenting (seen at ~210-215 s uptime, "no frame
// presented for 0.0 s"). The timeline notes a FREEZE only when its own last present is older than 5 s
// (last_present_ms 0 = no frame yet). Returns the whole seconds without frames to put in the note.
constexpr uint64_t kFreezeNoteMs = 5000;
bool FreezeWorthNoting(uint64_t now_ms, uint64_t last_present_ms, uint64_t* seconds_without_frames);

// Deletes the oldest wwe2k14-timeline-*.txt in dir so that at most keep remain (names sort by their time stamp).
// Never touches other files. Returns how many were deleted.
size_t KeepNewestTimelines(const std::filesystem::path& dir, size_t keep);

}  // namespace wwe2k14::timeline
