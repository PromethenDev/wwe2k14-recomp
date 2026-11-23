// Host-only test for the session timeline core (card LOG1, src/timeline_core.{h,cpp}). No SDK.
// Build + run (from the workspace root):
//   g++ -std=c++20 -O1 -pthread -I out/lanes/log2/src out/lanes/log2/src/native/tests/timeline-test.cpp
//       out/lanes/log2/src/timeline_core.cpp -o /tmp/claude-1000/timeline-test && /tmp/claude-1000/timeline-test
// Writes an example timeline to /tmp/claude-1000/timeline-example.txt.
#include "timeline_core.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace wwe2k14::timeline;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++g_failures;
  }
}

std::string Text(const NoteRing& ring) {
  static Entry entries[kNoteSlots];
  static char out[kNoteSlots * (kTextBytes + 16)];
  const size_t n = ring.Snapshot(entries, kNoteSlots);
  FormatTimeline("", entries, n, out, sizeof(out));
  return out;
}

void TestRingWrap() {
  auto ring = std::make_unique<NoteRing>();
  for (int i = 0; i < 600; ++i) {
    char text[32];
    std::snprintf(text, sizeof(text), "note %d", i);
    ring->Add(uint64_t(i), "test", text);
  }
  static Entry e[kNoteSlots];
  const size_t n = ring->Snapshot(e, kNoteSlots);
  bool ordered = n == 512;
  for (size_t i = 0; ordered && i < n; ++i) {
    char want[48];
    std::snprintf(want, sizeof(want), "test    note %zu", i + 88);
    ordered = e[i].sequence == i + 88 && e[i].ms == i + 88 && std::strcmp(e[i].text, want) == 0;
  }
  Check(ordered, "ring wrap: 600 notes -> newest 512 (88..599) in order");
  Entry few[3];
  Check(ring->Snapshot(few, 3) == 3 && few[0].sequence == 597 && few[2].sequence == 599, "snapshot capacity 3");
  std::printf("ring wrap: %zu notes kept, first #%llu last #%llu\n", n, (unsigned long long)e[0].sequence,
              (unsigned long long)e[n - 1].sequence);
}

// Each writer fills its text with its own letter; a torn copy would mix letters or break the "T:I:" prefix.
bool WellFormed(const Entry& e) {
  int thread = -1;
  int index = -1;
  if (std::sscanf(e.text, "thread  %d:%d:", &thread, &index) != 2 || thread < 0 || thread > 3) return false;
  const char* fill = std::strrchr(e.text, ':') + 1;
  const size_t length = std::strlen(fill);
  if (length < 40) return false;
  for (size_t i = 0; i < length; ++i) {
    if (fill[i] != char('a' + thread)) return false;
  }
  return true;
}

void TestConcurrent() {
  auto ring = std::make_unique<NoteRing>();
  std::atomic<bool> done{false};
  std::atomic<uint64_t> checked{0};
  std::atomic<uint64_t> bad{0};
  std::thread reader([&] {
    auto entries = std::make_unique<Entry[]>(kNoteSlots);
    while (!done.load()) {
      const size_t n = ring->Snapshot(entries.get(), kNoteSlots);
      for (size_t i = 0; i < n; ++i) {
        if (!WellFormed(entries[i])) ++bad;
        if (i && entries[i].sequence <= entries[i - 1].sequence) ++bad;
      }
      checked += n;
    }
  });
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&, t] {
      for (int i = 0; i < 20000; ++i) {
        char text[kTextBytes];
        int pos = std::snprintf(text, sizeof(text), "%d:%d:", t, i);
        while (pos < 90) text[pos++] = char('a' + t);
        text[pos] = '\0';
        ring->Add(uint64_t(i), "thread", text);
      }
    });
  }
  for (auto& w : writers) w.join();
  done = true;
  reader.join();
  auto entries = std::make_unique<Entry[]>(kNoteSlots);
  const size_t n = ring->Snapshot(entries.get(), kNoteSlots);
  bool final_ok = n == 512;
  for (size_t i = 0; i < n; ++i) final_ok = final_ok && WellFormed(entries[i]);
  Check(bad.load() == 0, "concurrent: no torn or out-of-order entries seen by the live reader");
  Check(final_ok, "concurrent: final snapshot 512 well-formed entries");
  Check(ring->Count() == 80000, "concurrent: 80000 notes counted");
  std::printf("concurrent: 4 writers x 20000 notes, live reader checked %llu entries, %llu bad\n",
              (unsigned long long)checked.load(), (unsigned long long)bad.load());
}

std::vector<std::string> Lines(const NoteRing& ring) {
  static Entry e[kNoteSlots];
  const size_t n = ring.Snapshot(e, kNoteSlots);
  std::vector<std::string> out;
  for (size_t i = 0; i < n; ++i) out.emplace_back(e[i].text);
  return out;
}

void TestPerf() {
  auto ring = std::make_unique<NoteRing>();
  PerfWindow w;
  // 60 fps from 1 s for a bit over 10 s; the first present only starts the window.
  for (uint64_t t = 1000000; t <= 11100000; t += 16667) AddPresent(w, t, *ring);
  auto lines = Lines(*ring);
  Check(lines.size() == 1 && lines[0] == "perf    60 fps, worst 16 ms, 0 slow frames", "perf: 60 fps window");
  std::printf("perf 60 fps: %s\n", lines.empty() ? "(none)" : lines[0].c_str());

  // 30 fps with two 60 ms frames and one 412 ms frame: 33 ms frames are not slow; 3 slow, 1 HITCH. The long frames
  // cost 12 frames' time, so the window shows 29 fps.
  ring = std::make_unique<NoteRing>();
  w = PerfWindow{};
  uint64_t t = 0;
  AddPresent(w, t, *ring);
  int frame = 0;
  while (t < 10000000) {
    ++frame;
    t += frame == 50 || frame == 100 ? 60000 : frame == 150 ? 412000 : 33333;
    AddPresent(w, t, *ring);
  }
  lines = Lines(*ring);
  Check(lines.size() == 2 && lines[0] == "HITCH   412 ms frame", "perf: HITCH note for a 412 ms frame");
  Check(lines.size() == 2 && lines[1] == "perf    29 fps, worst 412 ms, 3 slow frames", "perf: 30 fps window");
  for (const auto& l : lines) std::printf("perf 30 fps: %s\n", l.c_str());
}

void TestPause() {
  auto ring = std::make_unique<NoteRing>();
  PerfWindow w;
  uint64_t t = 0;
  AddPresent(w, t, *ring);
  for (int i = 0; i < 60; ++i) AddPresent(w, t += 16667, *ring);
  t += 1900000;  // 1.9 s: a long frame, not a pause
  AddPresent(w, t, *ring);
  t += 3400000;  // 3.4 s without frames: PAUSE, then a fresh window
  AddPresent(w, t, *ring);
  for (int i = 0; i < 601; ++i) AddPresent(w, t += 16667, *ring);
  const auto lines = Lines(*ring);
  Check(lines.size() == 3, "pause: three notes");
  Check(lines.size() > 0 && lines[0] == "HITCH   1900 ms frame", "pause: 1.9 s gap is a HITCH");
  Check(lines.size() > 1 && lines[1] == "PAUSE   3.4 s (no new frames)", "pause: 3.4 s gap is a PAUSE");
  Check(lines.size() > 2 && lines[2] == "perf    60 fps, worst 16 ms, 0 slow frames",
        "pause: the window after the pause is not polluted by it");
  Check(lines.size() > 1 && std::string(Text(*ring)).find("[00:00:06] PAUSE") != std::string::npos,
        "pause: noted at resume time");
  for (const auto& l : lines) std::printf("pause: %s\n", l.c_str());
}

void TestRotation() {
  const auto dir = std::filesystem::path("/tmp/claude-1000/timeline-rotation");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  for (int i = 1; i <= 7; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "wwe2k14-timeline-20261031-1200%02d.txt", i);
    std::ofstream(dir / name) << i;
  }
  std::ofstream(dir / "wwe2k14-20261031.log") << "main log";
  std::ofstream(dir / "wwe2k14-timeline-notes.md") << "other";
  const size_t deleted = KeepNewestTimelines(dir, 5);
  size_t kept = 0;
  bool oldest_gone = !std::filesystem::exists(dir / "wwe2k14-timeline-20261031-120001.txt") &&
                     !std::filesystem::exists(dir / "wwe2k14-timeline-20261031-120002.txt");
  for (int i = 3; i <= 7; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "wwe2k14-timeline-20261031-1200%02d.txt", i);
    kept += std::filesystem::exists(dir / name);
  }
  Check(deleted == 2 && kept == 5 && oldest_gone, "rotation: 7 files -> newest 5 kept");
  const bool others_kept = std::filesystem::exists(dir / "wwe2k14-20261031.log") &&
                           std::filesystem::exists(dir / "wwe2k14-timeline-notes.md");
  Check(others_kept, "rotation: other files untouched");
  std::printf("rotation: 7 timeline files, %zu deleted, %zu newest kept, other files kept\n", deleted, kept);
  std::filesystem::remove_all(dir);
}

void TestFileAreas() {
  struct Case {
    const char* path;
    bool directory;
    const char* area;
  };
  const Case cases[] = {
      {"game:\\PAC\\MENU\\menu_main.bin", false, "PAC\\MENU"},
      {"D:\\PAC\\CH\\ch82.pac", false, "PAC\\CH\\ch82.pac"},
      {"\\Device\\Cdrom0\\PAC\\ARENA\\sub\\a.bin", false, "PAC\\ARENA"},
      {"game:\\PAC\\CH", true, "PAC\\CH"},
      {"game:\\default.xex", false, "default.xex"},
      {"game:\\SOUND\\x.XWB", false, "SOUND"},
      {"cache:\\", false, ""},
  };
  bool ok = true;
  for (const auto& c : cases) {
    char area[kTextBytes];
    const bool got = FileAreaOf(c.path, c.directory, area, sizeof(area));
    const bool match = got ? std::strcmp(area, c.area) == 0 : c.area[0] == '\0';
    if (!match) std::printf("  area mismatch: %s -> '%s' (want '%s')\n", c.path, got ? area : "", c.area);
    ok = ok && match;
  }
  Check(ok, "file areas: prefixes dropped, two folder levels, .pac keeps its path");

  // Collapsing: a loader alternating between two areas, many times, gives one note per area.
  FileAreaTracker tracker;
  const char* opens[] = {"PAC\\MENU", "PAC\\MENU", "PAC\\MENU", "SOUND", "PAC\\MENU", "SOUND", "PAC\\CH\\ch82.pac",
                         "PAC\\CH\\ch82.pac", "PAC\\MENU", "PAC\\ARENA", "PAC\\CH\\ch90.pac", "PAC\\MENU"};
  std::string noted;
  for (const char* a : opens) {
    if (tracker.Observe(a)) noted += std::string(noted.empty() ? "" : ", ") + a;
  }
  // PAC\MENU comes back at the end once four newer areas pushed it out of the recent list.
  const std::string want = "PAC\\MENU, SOUND, PAC\\CH\\ch82.pac, PAC\\ARENA, PAC\\CH\\ch90.pac, PAC\\MENU";
  Check(noted == want, "file areas: repeats collapsed");
  std::printf("file areas: 12 opens -> noted: %s\n", noted.c_str());
}

// Card H1: Move-Set S&F load watch.
void TestLoadWatch() {
  {  // good load: MOT08 then 12 opens within 6 s -> no report
    LoadWatch w;
    w.SetEnabled(true);
    w.OnOpen(0, "D:\\PAC\\MOT08.PAC", true);
    for (int i = 0; i < 12; ++i) w.OnOpen(1000 + uint64_t(i) * 400, "D:\\PAC\\file.pac", true);
    Check(!w.Check(16000), "loadwatch: good load does not fire");
    Check(!w.Check(17000), "loadwatch: good load disarmed");
  }
  {  // bad load: 4 opens; not before 15 s, once at 16 s, not again at 17 s
    LoadWatch w;
    w.SetEnabled(true);
    w.OnOpen(0, "D:\\PAC\\MOT08.PAC", true);
    for (int i = 0; i < 4; ++i) w.OnOpen(1000 + uint64_t(i) * 1000, "D:\\PAC\\file.pac", true);
    Check(!w.Check(14000), "loadwatch: no report before 15 s");
    Check(w.Check(16000), "loadwatch: bad load fires at 16 s");
    Check(w.LastCount() == 4, "loadwatch: bad load reports 4 files");
    Check(!w.Check(17000), "loadwatch: fires once");
  }
  {  // re-entry re-arms; at most 3 reports per session
    LoadWatch w;
    w.SetEnabled(true);
    auto stall = [&](uint64_t t) {
      w.OnOpen(t, "D:\\PAC\\MOT08.PAC", true);
      for (int i = 0; i < 4; ++i) w.OnOpen(t + 1000 + uint64_t(i) * 1000, "D:\\PAC\\file.pac", true);
    };
    stall(0);
    Check(w.Check(16000), "loadwatch: first fire");
    stall(20000);
    Check(w.Check(36000), "loadwatch: re-entry re-arms");
    stall(40000);
    Check(w.Check(56000), "loadwatch: third fire");
    stall(60000);
    Check(!w.Check(76000), "loadwatch: cap of 3 reports");
  }
  {  // re-open of MOT08 during a load restarts the window and the count
    LoadWatch w;
    w.SetEnabled(true);
    w.OnOpen(0, "D:\\PAC\\MOT08.PAC", true);
    for (int i = 0; i < 10; ++i) w.OnOpen(100 + uint64_t(i), "D:\\PAC\\file.pac", true);
    w.OnOpen(5000, "D:\\PAC\\MOT08.PAC", true);
    Check(!w.Check(16000), "loadwatch: re-arm moves the window");
    Check(w.Check(20001), "loadwatch: re-arm resets the count");
  }
  {  // case-insensitive; other files never arm; failed opens ignored; disabled never fires
    LoadWatch w;
    w.SetEnabled(true);
    w.OnOpen(0, "d:\\pac\\mot08.pac", true);
    Check(w.Check(16000), "loadwatch: lower-case path arms");
    LoadWatch o;
    o.SetEnabled(true);
    for (int i = 0; i < 20; ++i) o.OnOpen(uint64_t(i) * 1000, "D:\\PAC\\MOT07.PAC", true);
    Check(!o.Check(21000), "loadwatch: other files never arm");
    o.OnOpen(30000, "D:\\PAC\\MOT08.PAC", false);
    Check(!o.Check(46000), "loadwatch: failed open does not arm");
    LoadWatch d;
    d.SetEnabled(false);
    d.OnOpen(0, "D:\\PAC\\MOT08.PAC", true);
    Check(!d.Check(16000), "loadwatch: disabled does not fire");
  }
}

void TestStuck() {
  StuckDetector stuck;
  uint64_t inputs = 0;
  uint64_t last_file = 5000;
  std::vector<uint64_t> fired;
  // 0-10 min of a simulated session, one Update per second, presents always running.
  for (uint64_t s = 1; s <= 600; ++s) {
    const uint64_t now = s * 1000;
    if (s < 100) last_file = now;           // loading files for the first 100 s
    if (s >= 130 && s % 10 == 0) ++inputs;  // from 130 s the player presses a button every 10 s
    if (s >= 400 && s <= 401) last_file = now;  // a file read at 400-401 s resets the quiet timer
    if (stuck.Update(now, now - 16, inputs, last_file)) fired.push_back(s);
  }
  // First at 170 s: no files since 99 s (quiet from 159 s), and the 5th press (130, 140, ... 170) lands at 170 s.
  // Next allowed 5 minutes later (470 s); the file read at 401 s is 69 s old by then.
  std::string list;
  for (auto s : fired) list += std::to_string(s) + " ";
  std::printf("stuck: fired at s = %s\n", list.c_str());
  Check(fired.size() == 2 && fired[0] == 170 && fired[1] == 470, "stuck: fires at 170 s and again at 470 s");

  // No presents (a real freeze, the freeze report's job) -> no STUCK?; no buttons -> no STUCK?.
  StuckDetector frozen;
  StuckDetector idle;
  bool any = false;
  for (uint64_t s = 1; s <= 300; ++s) {
    any = any || frozen.Update(s * 1000, 1000, s, 0);
    any = any || idle.Update(s * 1000, s * 1000, 3, 0);
  }
  Check(!any, "stuck: needs presents and button presses");
}

void TestTimestampsAndHeader() {
  Entry e[3] = {};
  e[0].ms = 0;
  std::strcpy(e[0].text, "event   game started");
  e[1].ms = 4 * 60000 + 12999;
  std::strcpy(e[1].text, "perf    60 fps");
  e[2].ms = 3 * 3600000ull + 5 * 60000 + 7000;
  std::strcpy(e[2].text, "event   EXIT");
  char out[512];
  FormatTimeline("H\n", e, 3, out, sizeof(out));
  Check(std::string(out) == "H\n[00:00:00] event   game started\n[00:04:12] perf    60 fps\n[03:05:07] event   EXIT\n",
        "timestamps: session-relative hh:mm:ss");
  char small[20];
  const size_t n = FormatTimeline("H\n", e, 3, small, sizeof(small));
  Check(n == 19 && std::strlen(small) == 19, "format: truncates at the buffer end");

}

// The launcher's ini as the Windows launcher writes it (shortened from a real player file; user name replaced).
const char* kSampleIni =
    "[Settings]\r\nschema=7\r\nrenderer=native\r\nresolution=720p\r\ndisplay=fullscreen\r\nlower_cpu=1\r\n"
    "fast_loading=1\r\nlanguage=1\r\ngpu=1002:15BF\r\n"
    "game_folder=C:\\Users\\player\\Downloads\\WWE2K14\\game\r\nauto_start=0\r\nskipped_update_version=\r\n"
    "export_dir=/home/player/x\r\nname=Jos\xC3\xA9\r\n";

std::string Utf16LeWithBom(const std::string& utf8) {
  // Test input is ASCII except "\xC3\xA9" (U+00E9), encoded here by hand.
  std::string out("\xFF\xFE", 2);
  for (size_t i = 0; i < utf8.size(); ++i) {
    uint32_t c = uint8_t(utf8[i]);
    if (c == 0xC3) c = 0xC0 | (uint8_t(utf8[++i]) & 0x3F);
    out += char(c & 0xFF);
    out += char(c >> 8);
  }
  return out;
}

void TestSettings() {
  const std::vector<std::string> want = {"schema=7",      "renderer=native", "resolution=720p", "display=fullscreen",
                                         "lower_cpu=1",   "fast_loading=1",  "language=1",      "auto_start=0",
                                         "name=Jos\xC3\xA9"};
  const LauncherSettings utf16 = ParseLauncherSettings(Utf16LeWithBom(kSampleIni));
  Check(utf16.found && utf16.settings == want && utf16.gpu == "1002:15BF", "settings: UTF-16LE with BOM");
  const LauncherSettings utf8_bom = ParseLauncherSettings("\xEF\xBB\xBF" + std::string(kSampleIni));
  Check(utf8_bom.settings == want && utf8_bom.gpu == "1002:15BF", "settings: UTF-8 with BOM");
  const LauncherSettings plain = ParseLauncherSettings("[Settings]\nresolution=480p\ngpu=\n");
  Check(plain.settings == std::vector<std::string>{"resolution=480p"} && plain.gpu == "Automatic",
        "settings: UTF-8, LF lines, empty gpu = Automatic");
  const std::string missing = BuildHeader("s", "b", "Xbox", LauncherSettings{});
  Check(missing.find("settings: launcher settings file not found\n") != std::string::npos &&
            missing.find("GPU (launcher choice): unknown\n") != std::string::npos,
        "settings: missing file line");
  std::printf("settings: UTF-16LE file -> %zu settings, gpu %s (game_folder, empty and path values left out)\n",
              utf16.settings.size(), utf16.gpu.c_str());
}

void TestNoDeviceSummary() {
  NoDeviceLookups lookups;
  char text[kTextBytes];
  for (int i = 0; i < 46; ++i) lookups.Count();
  Check(!lookups.Summary(29999, text, sizeof(text)), "no-device: nothing before 30 s");
  Check(lookups.Summary(30000, text, sizeof(text)) &&
            std::string(text) == "46 lookups on devices that are not present (normal when DLC is not installed)",
        "no-device: one summary at 30 s");
  std::printf("no-device: files   %s\n", text);
  Check(!lookups.Summary(60000, text, sizeof(text)), "no-device: no repeat without new lookups");
  lookups.Count();
  Check(lookups.Summary(90000, text, sizeof(text)) &&
            std::string(text) == "1 lookup on a device that is not present (normal when DLC is not installed)",
        "no-device: later lookups summarised as new ones");
  Check(std::string(FailureWord(0xC0000034u)) == "missing" && std::string(FailureWord(0xC000003Au)) == "missing" &&
            std::string(FailureWord(0xC0000022u)) == "failed",
        "failure words");
}

void TestFreezeRule() {
  uint64_t seconds = 0;
  // The SDK's false report at ~215 s: the timeline saw a frame 16 ms ago -> nothing.
  Check(!FreezeWorthNoting(215100, 215084, &seconds), "freeze: frames still presenting -> no note");
  Check(!FreezeWorthNoting(215100, 210200, &seconds), "freeze: 4.9 s without frames -> no note");
  Check(FreezeWorthNoting(215100, 203100, &seconds) && seconds == 12, "freeze: 12 s without frames -> note");
  Check(FreezeWorthNoting(9000, 0, &seconds) && seconds == 9, "freeze: no frame yet after 9 s -> note");
}

void WriteExample() {
  auto ring = std::make_unique<NoteRing>();
  ring->Add(0, "event", "game started");
  ring->Add(212, "files", "default.xex");
  ring->Add(1830, "files", "PAC\\MENU");
  PerfWindow w;
  uint64_t t = 2400000;
  ring->Add(t / 1000, "event", "first frame");
  for (int i = 0; i < 1300; ++i) AddPresent(w, t += 16667, *ring);
  ring->Add(t / 1000 + 300, "files", "PAC\\CH\\ch82.pac");
  ring->Add(t / 1000 + 340, "file", "missing game:\\PAC\\CH\\ch82_alt.pac status 0xC0000034");
  AddPresent(w, t += 3400000, *ring);  // loading screen without presents
  ring->Add(t / 1000 + 10, "files", "PAC\\ARENA");
  {
    NoDeviceLookups lookups;
    for (int i = 0; i < 46; ++i) lookups.Count();
    char text[kTextBytes];
    if (lookups.Summary(30000, text, sizeof(text))) ring->Add(t / 1000 + 2500, "files", text);
  }
  for (int i = 0; i < 700; ++i) AddPresent(w, t += i == 300 ? 412000 : 16667, *ring);
  const uint64_t stuck_ms = t / 1000 + 61000;
  ring->Add(stuck_ms, "STUCK?", "screen kept running 60 s with buttons pressed and no files read (a hint only)");
  ring->Add(stuck_ms, "STUCK?", "last file game:\\PAC\\CH\\ch82.pac");
  ring->Add(stuck_ms, "STUCK?", "last file (failed) game:\\PAC\\CH\\ch82_alt.pac");
  ring->Add(stuck_ms, "STUCK?", "last file game:\\PAC\\ARENA\\arena07.bin");
  ring->Add(stuck_ms + 30000, "event", "window closed");
  const std::string header = BuildHeader("2026-10-31 20:14:03", "be7fdbf6d2adfccdaca1b7a5b2d25c18080d8b99", "Native",
                                         ParseLauncherSettings(Utf16LeWithBom(kSampleIni)));
  static Entry e[kNoteSlots];
  static char out[kNoteSlots * (kTextBytes + 16)];
  const size_t n = ring->Snapshot(e, kNoteSlots);
  const size_t length = FormatTimeline(header.c_str(), e, n, out, sizeof(out));
  std::ofstream("/tmp/claude-1000/timeline-example.txt", std::ios::binary).write(out, std::streamsize(length));
  std::printf("example: /tmp/claude-1000/timeline-example.txt (%zu bytes)\n", length);
}

}  // namespace

int main() {
  std::filesystem::create_directories("/tmp/claude-1000");
  TestRingWrap();
  TestConcurrent();
  TestPerf();
  TestPause();
  TestRotation();
  TestFileAreas();
  TestStuck();
  TestLoadWatch();
  TestTimestampsAndHeader();
  TestSettings();
  TestNoDeviceSummary();
  TestFreezeRule();
  WriteExample();
  std::printf(g_failures ? "FAILED: %d check(s)\n" : "PASS (all timeline checks)\n", g_failures);
  return g_failures ? 1 : 0;
}
