// Session timeline (card LOG1) - see timeline.h. The SDK-free parts (ring, perf maths, file areas, stuck rule, file
// text) live in timeline_core.cpp and are host-tested by src/native/tests/timeline-test.cpp.
//
// Crash path: a crash handler may not allocate or lock (POSIX: it runs in a signal handler; Windows: the heap or a
// lock may be held by the crashed thread). So Start() opens the timeline file once and keeps the descriptor/handle,
// copies the header into a static buffer, and reserves static entry/text buffers just for the crash writer. The
// handler adds "CRASH <summary>" to the lock-free ring, snapshots it, formats with the stdio-free helpers and rewrites
// the file from offset 0 with pwrite+ftruncate (POSIX) or WriteFile+SetEndOfFile (Windows). The normal writer uses the
// same handle (rewrite in place, no temp file + rename, which would leave the crash handle on an unlinked file).
// A normal write racing the crash write can mix the two texts; the crash write normally finishes last.
#include "timeline.h"

#include "hitch_catcher.h"
#include "timeline_core.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/diagnostics/observers.h>

#if defined(_WIN32)
#include <windows.h>
extern "C" IMAGE_DOS_HEADER __ImageBase;
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace wwe2k14::timeline {
namespace {

namespace observers = rex::diagnostics::observers;

constexpr size_t kHeaderBytes = 8192;
constexpr size_t kFileBytes = kHeaderBytes + kNoteSlots * (kTextBytes + 16);
constexpr uint32_t kMaxFailedOpenNotes = 50;  // per session; the game may probe optional files repeatedly
constexpr uint8_t kTriggerPressed = 30;       // XInput's trigger threshold

std::atomic<bool> g_enabled{false};  // Note/OnPresent/observers do nothing until Start() succeeded
std::atomic<bool> g_crash_ready{false};
std::chrono::steady_clock::time_point g_start;

NoteRing g_ring;
PathRing g_paths;  // last file opens, for the STUCK? lines
FileAreaTracker g_areas;
FileAreaTracker g_failed_paths;
NoDeviceLookups g_no_device;  // 0xC000000F lookups, summarised (timeline_core.h)
PerfWindow g_perf;  // present thread only
StuckDetector g_stuck;  // writer thread only
LoadWatch g_sf_watch;  // card H1: OnOpen on file threads, Check on the writer thread

std::atomic<uint64_t> g_last_present_ms{0};
std::atomic<uint64_t> g_last_file_ms{0};
std::atomic<uint64_t> g_input_presses{0};
std::atomic<uint32_t> g_input_state[4];
std::atomic<uint32_t> g_failed_notes{0};

char g_header[kHeaderBytes];
Entry g_write_entries[kNoteSlots];  // normal writer (under g_write_mutex)
char g_write_text[kFileBytes];
Entry g_crash_entries[kNoteSlots];  // crash writer only
char g_crash_text[kFileBytes];
std::mutex g_write_mutex;

std::mutex g_wake_mutex;
std::condition_variable g_wake;
bool g_running = false;
// Never destroyed: a std::thread still joinable at static destruction calls std::terminate, and an exit path that
// skips OnShutdown (exit() from elsewhere) must not turn into an abort.
std::thread* g_writer = nullptr;

#if defined(_WIN32)
HANDLE g_file = INVALID_HANDLE_VALUE;
uintptr_t g_exe_base = 0;
uintptr_t g_exe_end = 0;
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_previous_filter{nullptr};
#else
int g_fd = -1;
#endif

uint64_t NowUs() {
  const auto d = std::chrono::steady_clock::now() - g_start;
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(d).count());
}

uint64_t NowMs() { return NowUs() / 1000; }

// Rewrites the whole file from offset 0. Async-signal-safe on POSIX; no allocation on Windows.
void WriteWhole(const char* text, size_t length) {
#if defined(_WIN32)
  if (g_file == INVALID_HANDLE_VALUE) return;
  OVERLAPPED at{};  // offset 0 (positional write on a synchronous handle)
  DWORD written = 0;
  WriteFile(g_file, text, DWORD(length), &written, &at);
  LARGE_INTEGER end;
  end.QuadPart = LONGLONG(length);
  SetFilePointerEx(g_file, end, nullptr, FILE_BEGIN);
  SetEndOfFile(g_file);
#else
  if (g_fd < 0) return;
  size_t done = 0;
  while (done < length) {
    const ssize_t n = pwrite(g_fd, text + done, length - done, off_t(done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    done += size_t(n);
  }
  if (ftruncate(g_fd, off_t(length)) != 0) {
    // nothing to do: a longer old file keeps a stale tail
  }
#endif
}

void Write() {
  std::lock_guard<std::mutex> lock(g_write_mutex);
  char summary[kTextBytes];
  if (g_no_device.Summary(NowMs(), summary, sizeof(summary))) g_ring.Add(NowMs(), "files", summary);
  const size_t count = g_ring.Snapshot(g_write_entries, kNoteSlots);
  const size_t length = FormatTimeline(g_header, g_write_entries, count, g_write_text, sizeof(g_write_text));
  WriteWhole(g_write_text, length);
}

// Lock-free, allocation-free: runs in the crash handler.
void CrashWrite(const char* summary) {
  if (!g_crash_ready.load(std::memory_order_acquire)) return;
  g_ring.Add(NowMs(), "CRASH", summary);
  const size_t count = g_ring.Snapshot(g_crash_entries, kNoteSlots);
  const size_t length = FormatTimeline(g_header, g_crash_entries, count, g_crash_text, sizeof(g_crash_text));
  WriteWhole(g_crash_text, length);
}

#if defined(_WIN32)
LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
  // Guard against a filter chain loop (another filter installed after ours that chains back to us).
  static std::atomic<int> depth{0};
  if (depth.fetch_add(1) > 0) {
    depth.fetch_sub(1);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  static std::atomic_flag written = ATOMIC_FLAG_INIT;
  if (ep && ep->ExceptionRecord && !written.test_and_set()) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    const uintptr_t at = reinterpret_cast<uintptr_t>(r->ExceptionAddress);
    char summary[96];
    size_t pos = 0;
    summary[0] = '\0';
    AppendText(summary, sizeof(summary), &pos, "exception 0x");
    AppendHex(summary, sizeof(summary), &pos, r->ExceptionCode, 8);
    if (at >= g_exe_base && at < g_exe_end) {
      AppendText(summary, sizeof(summary), &pos, " at exe+0x");  // RVA: matches the stored pdb (tools/diag)
      AppendHex(summary, sizeof(summary), &pos, at - g_exe_base);
    } else {
      AppendText(summary, sizeof(summary), &pos, " at 0x");
      AppendHex(summary, sizeof(summary), &pos, at);
    }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
      AppendText(summary, sizeof(summary), &pos, " address 0x");
      AppendHex(summary, sizeof(summary), &pos, r->ExceptionInformation[1]);
    }
    CrashWrite(summary);
  }
  const LPTOP_LEVEL_EXCEPTION_FILTER previous = g_previous_filter.load();
  const LONG result = previous ? previous(ep) : EXCEPTION_CONTINUE_SEARCH;
  depth.fetch_sub(1);
  return result;
}

// The native D3D layer installs its own filter later (without chaining), so the writer re-asserts ours once a
// second and chains to whatever was installed in between.
void InstallCrashFilter() {
  const LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(&UnhandledFilter);
  if (previous != &UnhandledFilter) g_previous_filter.store(previous);
}
#else
void OnCrash(const char* summary) {
  const int saved_errno = errno;
  CrashWrite(summary);
  errno = saved_errno;
}
#endif

// SDK file-open observer: runs on the game's file threads, so it stays short.
void OnFileOpen(const char* guest_path, uint32_t status, bool is_directory) {
  if (!g_enabled.load(std::memory_order_acquire) || !guest_path) return;
  const uint64_t ms = NowMs();
  const bool ok = int32_t(status) >= 0;  // NT status: success and informational codes are >= 0
  // Last paths for the STUCK? lines: the tail of the path is the informative part.
  char recent[kTextBytes];
  size_t pos = 0;
  recent[0] = '\0';
  if (!ok) AppendText(recent, sizeof(recent), &pos, "(failed) ");
  const size_t length = std::strlen(guest_path);
  const size_t room = sizeof(recent) - 1 - pos;
  AppendText(recent, sizeof(recent), &pos, length > room ? guest_path + (length - room) : guest_path);
  g_paths.Add(ms, nullptr, recent);
  if (ok) {
    g_last_file_ms.store(ms ? ms : 1, std::memory_order_relaxed);
    g_sf_watch.OnOpen(ms, guest_path, !is_directory);
    char area[kTextBytes];
    if (FileAreaOf(guest_path, is_directory, area, sizeof(area)) && g_areas.Observe(area)) {
      g_ring.Add(ms, "files", area);
    }
    return;
  }
  if (status == kStatusNoSuchDevice) {
    g_no_device.Count();  // DLC slots / devices not mounted: one summary line later, not one line each
    return;
  }
  if (!g_failed_paths.Observe(guest_path)) return;  // the same path failed recently
  const uint32_t n = g_failed_notes.fetch_add(1, std::memory_order_relaxed);
  if (n > kMaxFailedOpenNotes) return;
  if (n == kMaxFailedOpenNotes) {
    g_ring.Add(ms, "file", "more failed opens not listed");
    return;
  }
  char text[kTextBytes];
  std::snprintf(text, sizeof(text), "%s %s status 0x%08X", FailureWord(status), guest_path, status);
  g_ring.Add(ms, "file", text);
}

// SDK input observer: counts presses (a button or trigger going down), not polls or releases.
void OnInput(uint32_t user, uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger) {
  if (user >= 4) return;
  const uint32_t state = uint32_t(buttons) | (left_trigger > kTriggerPressed ? 1u << 16 : 0u) |
                         (right_trigger > kTriggerPressed ? 1u << 17 : 0u);
  const uint32_t old = g_input_state[user].exchange(state, std::memory_order_relaxed);
  if (state & ~old) g_input_presses.fetch_add(1, std::memory_order_relaxed);
}

void NoteStuck(uint64_t now_ms) {
  g_ring.Add(now_ms, "STUCK?", "screen kept running 60 s with buttons pressed and no files read (a hint only)");
  Entry recent[kPathSlots];
  const size_t count = g_paths.Snapshot(recent, kPathSlots);
  const size_t first = count > 3 ? count - 3 : 0;
  if (count == 0) g_ring.Add(now_ms, "STUCK?", "last files: none");
  for (size_t i = first; i < count; ++i) {
    char text[kTextBytes];
    std::snprintf(text, sizeof(text), "last file %s", recent[i].text);
    g_ring.Add(now_ms, "STUCK?", text);
  }
}

// Ask the freeze logger for a full report (all guest threads: waits, CPU, stacks) by creating its request file
// (WWE2K14_FREEZE_DUMP_NOW, always set by wwe2k14_app.h). Returns false when unavailable.
bool RequestFreezeReport() {
  const char* path = std::getenv("WWE2K14_FREEZE_DUMP_NOW");
  if (!path || !*path) return false;
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

// Once a second: stuck check (+ crash filter re-assert); every 10 s or when woken for a flush: write the file.
// 10 s, not 30 s: a frozen game is usually force-closed, which loses everything since the last write (2026-10-31).
void WriterLoop() {
  uint64_t last_write_ms = NowMs();
  std::unique_lock<std::mutex> lock(g_wake_mutex);
  while (g_running) {
    g_wake.wait_for(lock, std::chrono::seconds(1), [] { return !g_running; });
    if (!g_running) break;
    lock.unlock();
#if defined(_WIN32)
    InstallCrashFilter();
#endif
    const uint64_t now = NowMs();
    const bool stuck = g_stuck.Update(now, g_last_present_ms.load(std::memory_order_relaxed),
                                      g_input_presses.load(std::memory_order_relaxed),
                                      g_last_file_ms.load(std::memory_order_relaxed));
    if (stuck) {
      NoteStuck(now);
      // 11-15 (owner: better debugging): a stuck loading screen keeps presenting, so the freeze logger's no-frame trigger
      // never fires and player bug reports had nothing. The first STUCK? of a session also asks for a full freeze report
      // (one per session; costs nothing until it fires).
      static bool stuck_report_sent = false;
      if (!stuck_report_sent) {
        stuck_report_sent = true;
        g_ring.Add(now, "STUCK?", RequestFreezeReport() ? "freeze report requested (logs folder)" : "freeze report unavailable");
      }
    }
    bool write_now = stuck;
    if (g_sf_watch.Check(now)) {  // card H1: Move-Set load stopped -> ask the freeze logger for a wait report
      const bool requested = RequestFreezeReport();
      char text[kTextBytes];
      std::snprintf(text, sizeof(text), "Move-Set load stopped after %u files; freeze report %s",
                    g_sf_watch.LastCount(), requested ? "requested" : "unavailable");
      g_ring.Add(now, "LOAD?", text);
      write_now = true;
    }
    if (write_now || now >= last_write_ms + 10000) {
      Write();
      last_write_ms = now;
    }
    lock.lock();
  }
}

bool Disabled() {
  const char* value = std::getenv("WWE2K14_TIMELINE");
  return value && std::strcmp(value, "0") == 0;
}

LauncherSettings ReadLauncherSettings(const char* path) {
  if (!path || !*path) return LauncherSettings{};
  std::ifstream file(std::filesystem::path(path), std::ios::binary);
  if (!file) return LauncherSettings{};
  const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return ParseLauncherSettings(bytes);
}

std::tm LocalNow() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  return tm;
}

bool OpenFile(const std::filesystem::path& path) {
#if defined(_WIN32)
#if defined(REX_UWP)
  g_file = CreateFile2(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       CREATE_ALWAYS, nullptr);
#else
  g_file = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
  return g_file != INVALID_HANDLE_VALUE;
#else
  g_fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  return g_fd >= 0;
#endif
}

}  // namespace

// One line of hardware facts for the header (owner 2026-10-31), read once at start: CPU model, memory, OS version,
// power source. No computer name, account name or serial numbers.
static std::string PcSummary() {
  std::string cpu, os, power;
  uint64_t memory_mb = 0;
#if defined(_WIN32)
  auto reg = [](const wchar_t* key, const wchar_t* value) {
    wchar_t buffer[256]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
      return std::string();
    }
    char utf8[512]{};
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    return std::string(utf8);
  };
  cpu = reg(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
  const wchar_t* version_key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  const std::string build = reg(version_key, L"CurrentBuildNumber");
  os = reg(version_key, L"ProductName");
  // Windows 11 still reports "Windows 10 ..." as ProductName; the build number tells them apart.
  if (std::strtoul(build.c_str(), nullptr, 10) >= 22000 && os.rfind("Windows 10", 0) == 0) os.replace(8, 2, "11");
  const std::string display_version = reg(version_key, L"DisplayVersion");
  if (!display_version.empty()) os += " " + display_version;
  if (!build.empty()) os += " (build " + build + ")";
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) memory_mb = memory.ullTotalPhys >> 20;
  SYSTEM_POWER_STATUS status{};
  if (GetSystemPowerStatus(&status)) {
    power = status.ACLineStatus == 1 ? "plugged in" : status.ACLineStatus == 0 ? "on battery" : "";
    if (status.SystemStatusFlag == 1) power += ", battery saver on";
  }
#else
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (cpu.empty() && std::getline(cpuinfo, line)) {
    if (line.rfind("model name\t: ", 0) == 0) cpu = line.substr(13);
  }
  std::ifstream os_release("/etc/os-release");
  while (std::getline(os_release, line)) {
    if (line.rfind("PRETTY_NAME=", 0) == 0) {
      os = line.substr(12);
      os.erase(std::remove(os.begin(), os.end(), '"'), os.end());
    }
  }
  std::ifstream meminfo("/proc/meminfo");
  while (std::getline(meminfo, line)) {
    if (line.rfind("MemTotal:", 0) == 0) memory_mb = std::strtoull(line.c_str() + 9, nullptr, 10) >> 10;
  }
#endif
  while (!cpu.empty() && cpu.back() == ' ') cpu.pop_back();  // Windows pads ProcessorNameString with spaces
  std::string summary = cpu.empty() ? "CPU unknown" : cpu;
  if (memory_mb) summary += ", " + std::to_string((memory_mb + 512) / 1024) + " GB RAM";
  if (!os.empty()) summary += ", " + os;
  if (!power.empty()) summary += ", " + power;
  return summary;
}

void Start(const char* directory, const char* settings_file) {
  static std::once_flag once;
  std::call_once(once, [directory, settings_file] {
    if (Disabled() || !directory || !*directory) return;
    g_start = std::chrono::steady_clock::now();
    const std::filesystem::path dir(directory);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::tm tm = LocalNow();
    char stamp[32];
    char started[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", &tm);
    if (!OpenFile(dir / ("wwe2k14-timeline-" + std::string(stamp) + ".txt"))) return;
    KeepNewestTimelines(dir, 5);

    const char* nr = std::getenv("WWE2K14_NR");
    const bool native = nr && (std::strcmp(nr, "replace") == 0 || std::strcmp(nr, "shadow") == 0);
    std::string header =
        BuildHeader(started, hitch::BuildId(), native ? "Native" : "Xbox", ReadLauncherSettings(settings_file));
    const size_t after_renderer = header.find('\n', header.find("renderer: "));
    if (after_renderer != std::string::npos) header.insert(after_renderer + 1, "PC: " + PcSummary() + "\n");
    size_t pos = 0;
    g_header[0] = '\0';
    AppendText(g_header, sizeof(g_header), &pos, header.c_str());
#if defined(_WIN32)
    const auto* dos = &__ImageBase;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
    g_exe_base = reinterpret_cast<uintptr_t>(dos);
    g_exe_end = g_exe_base + nt->OptionalHeader.SizeOfImage;
#endif

    g_enabled.store(true, std::memory_order_release);
    {
      const char* watch = std::getenv("WWE2K14_SF_WATCH");  // card H1 load watch; "0" disables it
      g_sf_watch.SetEnabled(!(watch && std::strcmp(watch, "0") == 0));
    }
    g_ring.Add(0, "event", "game started");
    g_crash_ready.store(true, std::memory_order_release);
    observers::SetFileOpenObserver(&OnFileOpen);
    observers::SetInputObserver(&OnInput);
#if defined(_WIN32)
    InstallCrashFilter();
#else
    observers::SetCrashObserver(&OnCrash);
#endif
    Write();
    {
      std::lock_guard<std::mutex> lock(g_wake_mutex);
      g_running = true;
    }
    g_writer = new std::thread(WriterLoop);
  });
}

void Note(const char* category, const char* fmt, ...) {
  if (!g_enabled.load(std::memory_order_acquire)) return;
  char text[kTextBytes];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  g_ring.Add(NowMs(), category, text);
}

void OnPresent() {
  if (!g_enabled.load(std::memory_order_acquire)) return;
  const uint64_t now_us = NowUs();
  const uint64_t now_ms = now_us / 1000;
  if (g_last_present_ms.exchange(now_ms ? now_ms : 1, std::memory_order_relaxed) == 0) {
    g_ring.Add(now_ms, "event", "first frame");
  }
  AddPresent(g_perf, now_us, g_ring);
}

void OnFreezeReport() {
  if (!g_enabled.load(std::memory_order_acquire)) return;
  uint64_t seconds = 0;
  if (!FreezeWorthNoting(NowMs(), g_last_present_ms.load(std::memory_order_relaxed), &seconds)) return;
  Note("FREEZE", "no new frames for %llu s (freeze report written to the logs folder)", (unsigned long long)seconds);
  Write();
}

void Flush() {
  if (g_enabled.load(std::memory_order_acquire)) Write();
}

void Stop() {
  if (!g_enabled.load(std::memory_order_acquire)) return;
  {
    std::lock_guard<std::mutex> lock(g_wake_mutex);
    if (!g_running) return;
    g_running = false;
  }
  g_wake.notify_all();
  if (g_writer && g_writer->joinable()) g_writer->join();
  observers::SetFileOpenObserver(nullptr);
  observers::SetInputObserver(nullptr);
  Write();  // the file handle stays open: a crash during the rest of shutdown can still write
}

}  // namespace wwe2k14::timeline
