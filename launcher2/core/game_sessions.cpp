// Bug Reports page and crash pop-up (owner 2026-11-09): recent game sessions, how each one ended, saved reports,
// and the crash watcher that waits for the game after the launcher closes.
#include "launcher_core.h"

#include "internal/util.h"
#include "update.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/types.h>
extern char** environ;
#endif

namespace wwe2k14::launcher {
namespace {

constexpr std::string_view kCrashMarker = "CRASH UNHANDLED_EXCEPTION";
// Written by the game when its window closes / the run ends. A crash after these is the known exit crash.
constexpr std::string_view kClosedMarkers[] = {"disarmed at window close", "Execution complete",
                                               "Shutdown still running"};
// Watcher notes live in logs/crash-watch/ so the bug report's newest-files window only sees real logs.
constexpr const char* kWatchFolder = "crash-watch";
constexpr const char* kSeenFile = "notice-seen.txt";
constexpr const char* kPendingFile = "pending.txt";

// Plain one-line notes (the atomic text writer stores UTF-16, which the log-text reader does not expect).
void WriteNote(const fs::path& file, const std::string& text) {
  if (!internal::EnsureDirectory(file.parent_path())) return;
  fs::path temporary = file;
  temporary += ".tmp";
  if (!internal::WriteBinaryFile(temporary, text.data(), text.size())) return;
  std::error_code error;
  fs::rename(temporary, file, error);
  if (error) fs::remove(temporary, error);
}

fs::path ExitNoteFor(const Paths& paths, const fs::path& log) {
  return paths.logs_dir / kWatchFolder / (internal::PathToUtf8(log.filename()) + ".exit");
}

bool IsDigits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// "wwe2k14-YYYYMMDD-HHMMSS.log" -> "YYYYMMDD-HHMMSS"; empty for any other name (timelines, freeze reports, ...).
std::string SessionStamp(const fs::path& log) {
  const std::string name = internal::PathToUtf8(log.filename());
  if (name.size() != 27 || !name.starts_with("wwe2k14-") || !name.ends_with(".log") || name[16] != '-') return {};
  const std::string stamp = name.substr(8, 15);
  if (!IsDigits(stamp.substr(0, 8)) || !IsDigits(stamp.substr(9, 6))) return {};
  return stamp;
}

// Seconds since 2000 for a "YYYYMMDD-HHMMSS" stamp (only used to compare stamps; no time zone needed).
int64_t StampSeconds(std::string_view stamp) {
  if (stamp.size() != 15) return -1;
  auto number = [&](size_t at, size_t count) { return std::atoi(std::string(stamp.substr(at, count)).c_str()); };
  const int year = number(0, 4), month = number(4, 2), day = number(6, 2);
  const int hour = number(9, 2), minute = number(11, 2), second = number(13, 2);
  // Days from a civil date (Howard Hinnant's algorithm), exact for comparisons across months and years.
  const int y = year - (month <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
  return days * 86400 + hour * 3600 + minute * 60 + second;
}

// Stamp of "<prefix>YYYYMMDD-HHMMSS.txt" files (timelines, freeze reports), or empty.
std::string PrefixedStamp(const std::string& name, std::string_view prefix) {
  if (!name.starts_with(prefix) || !name.ends_with(".txt") || name.size() < prefix.size() + 19) return {};
  const std::string stamp = name.substr(prefix.size(), 15);
  if (stamp[8] != '-' || !IsDigits(stamp.substr(0, 8)) || !IsDigits(stamp.substr(9, 6))) return {};
  return stamp;
}

std::vector<fs::path> LogsFolderFiles(const Paths& paths) {
  std::vector<fs::path> files;
  std::error_code error;
  fs::directory_iterator iterator(paths.logs_dir, fs::directory_options::skip_permission_denied, error);
  if (error) return files;
  for (const auto& entry : iterator) {
    std::error_code type_error;
    if (entry.is_regular_file(type_error) && !type_error) files.push_back(entry.path());
  }
  return files;
}

bool ReadWholeFile(const fs::path& file, std::string& text) {
  std::ifstream input(file, std::ios::binary);
  if (!input) return false;
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size < 0) return false;
  input.seekg(0, std::ios::beg);
  text.resize(static_cast<size_t>(size));
  if (size > 0) input.read(text.data(), size);
  return static_cast<bool>(input) || input.eof();
}

GameSession ReadSessionWithFiles(const Paths& paths, const fs::path& log, const std::vector<fs::path>& files) {
  GameSession session;
  session.log = log;
  const std::string stamp = SessionStamp(log);
  if (stamp.size() == 15) {
    session.started = stamp.substr(0, 4) + "-" + stamp.substr(4, 2) + "-" + stamp.substr(6, 2) + " " +
                      stamp.substr(9, 2) + ":" + stamp.substr(11, 2) + ":" + stamp.substr(13, 2);
  }
  std::error_code error;
  session.log_bytes = fs::file_size(log, error);
  if (error) session.log_bytes = 0;
  error.clear();
  const auto written = fs::last_write_time(log, error);
  if (!error) session.ended = internal::IsoLocalTimestamp(written);

  // The next session's start bounds this one, so its freeze reports are not counted here.
  const int64_t start = StampSeconds(stamp);
  int64_t next_start = INT64_MAX;
  for (const fs::path& file : files) {
    const std::string other = SessionStamp(file);
    const int64_t seconds = StampSeconds(other);
    if (seconds > start && seconds < next_start) next_start = seconds;
  }
  int64_t best_timeline = INT64_MAX;
  for (const fs::path& file : files) {
    const std::string name = internal::PathToUtf8(file.filename());
    if (const std::string timeline = PrefixedStamp(name, "wwe2k14-timeline-"); !timeline.empty()) {
      // The game opens its timeline a moment after its log (owner laptop: 06:15:52 log, 06:15:53 timeline).
      const int64_t delta = StampSeconds(timeline) - start;
      if (start >= 0 && delta >= 0 && delta <= 30 && StampSeconds(timeline) < next_start && delta < best_timeline) {
        best_timeline = delta;
        session.timeline = file;
      }
    } else if (const std::string freeze = PrefixedStamp(name, "wwe2k14-freeze-"); !freeze.empty()) {
      const int64_t at = StampSeconds(freeze);
      if (start >= 0 && at >= start && at < next_start) session.freeze_reports.push_back(file);
    }
  }
  std::sort(session.freeze_reports.begin(), session.freeze_reports.end());

  std::string text, exit_text;
  if (!ReadWholeFile(log, text)) text.clear();
  if (!ReadWholeFile(ExitNoteFor(paths, log), exit_text)) exit_text.clear();
  session.end = ClassifyGameLogText(text, exit_text, !session.freeze_reports.empty(), &session.crash_code);

  if (stamp.size() == 15) {
    const fs::path report = paths.exe_dir / ("wwe2k14-bug-report-game-" + stamp + ".zip");
    error.clear();
    if (fs::is_regular_file(report, error) && !error) session.report = report;
  }
  return session;
}

}  // namespace

SessionEnd ClassifyGameLogText(std::string_view log_text, std::string_view exit_code_text, bool has_freeze_report,
                               std::string* crash_code) {
  if (crash_code) crash_code->clear();
  size_t closed_at = std::string_view::npos;
  for (std::string_view marker : kClosedMarkers) closed_at = (std::min)(closed_at, log_text.find(marker));
  const size_t crash_at = log_text.find(kCrashMarker);
  if (crash_at != std::string_view::npos && (closed_at == std::string_view::npos || crash_at < closed_at)) {
    // "code=3221225477" (decimal NTSTATUS) -> "C0000005".
    const size_t code_at = log_text.find("code=", crash_at);
    const size_t line_end = log_text.find('\n', crash_at);
    if (crash_code && code_at != std::string_view::npos && code_at < line_end) {
      const unsigned long long value = std::strtoull(std::string(log_text.substr(code_at + 5, 12)).c_str(),
                                                     nullptr, 10);
      if (value > 0 && value <= 0xFFFFFFFFull) {
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%08llX", value);
        *crash_code = hex;
      }
    }
    return SessionEnd::kCrashed;
  }
  if (closed_at != std::string_view::npos) return SessionEnd::kClosed;
  // No close line and no crash line. The Windows watcher's exit code tells a crash the handler could not log
  // (stack overflow, fast fail: NTSTATUS error 0xC.......) from a forced close (Task Manager = 1).
  const size_t code_at = exit_code_text.find("exit_code=0x");
  if (code_at != std::string_view::npos) {
    const unsigned long value =
        std::strtoul(std::string(exit_code_text.substr(code_at + 12, 8)).c_str(), nullptr, 16);
    if ((value & 0xC0000000ul) == 0xC0000000ul) {
      if (crash_code) {
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%08lX", value);
        *crash_code = hex;
      }
      return SessionEnd::kCrashed;
    }
  }
  return has_freeze_report ? SessionEnd::kFroze : SessionEnd::kNoEnding;
}

GameSession ReadGameSession(const Paths& paths, const fs::path& log) {
  return ReadSessionWithFiles(paths, log, LogsFolderFiles(paths));
}

std::vector<GameSession> ListRecentGameSessions(const Paths& paths, size_t max_sessions) {
  const std::vector<fs::path> files = LogsFolderFiles(paths);
  std::vector<fs::path> logs;
  for (const fs::path& file : files) {
    if (!SessionStamp(file).empty()) logs.push_back(file);
  }
  // Names carry the start time, so name order is start order.
  std::sort(logs.begin(), logs.end(), [](const fs::path& left, const fs::path& right) {
    return SessionStamp(left) > SessionStamp(right);
  });
  if (logs.size() > max_sessions) logs.resize(max_sessions);
  std::vector<GameSession> sessions;
  for (const fs::path& log : logs) sessions.push_back(ReadSessionWithFiles(paths, log, files));
  return sessions;
}

std::vector<BugReportFile> ListBugReports(const Paths& paths, size_t max_reports) {
  std::vector<BugReportFile> reports;
  std::error_code error;
  fs::directory_iterator iterator(paths.exe_dir, fs::directory_options::skip_permission_denied, error);
  if (error) return reports;
  std::vector<std::pair<fs::file_time_type, BugReportFile>> found;
  for (const auto& entry : iterator) {
    std::error_code entry_error;
    if (!entry.is_regular_file(entry_error) || entry_error) continue;
    const std::string name = internal::PathToUtf8(entry.path().filename());
    if (!name.starts_with("wwe2k14-bug-report-") || !name.ends_with(".zip")) continue;
    BugReportFile report;
    report.file = entry.path();
    report.bytes = entry.file_size(entry_error);
    if (entry_error) report.bytes = 0;
    entry_error.clear();
    const auto when = entry.last_write_time(entry_error);
    if (!entry_error) report.when = internal::IsoLocalTimestamp(when);
    found.emplace_back(entry_error ? fs::file_time_type{} : when, std::move(report));
  }
  std::sort(found.begin(), found.end(), [](const auto& left, const auto& right) { return left.first > right.first; });
  for (auto& [when, report] : found) {
    if (reports.size() >= max_reports) break;
    reports.push_back(std::move(report));
  }
  return reports;
}

bool CrashNoticeSeen(const Paths& paths, const fs::path& log) {
  std::string seen;
  if (!internal::ReadTextFile(paths.logs_dir / kWatchFolder / kSeenFile, seen)) return false;
  const std::string stamp = SessionStamp(log);
  const std::string seen_stamp = internal::Trim(seen);
  // Stamps sort by time: a crash at or before the newest one shown has been seen.
  return !stamp.empty() && !seen_stamp.empty() && stamp <= seen_stamp;
}

void MarkCrashNoticeSeen(const Paths& paths, const fs::path& log) {
  const std::string stamp = SessionStamp(log);
  if (stamp.empty() || CrashNoticeSeen(paths, log)) return;
  WriteNote(paths.logs_dir / kWatchFolder / kSeenFile, stamp + "\n");
}

std::optional<GameSession> UnseenRecentCrash(const Paths& paths, double max_age_hours) {
  const std::vector<GameSession> newest = ListRecentGameSessions(paths, 1);
  if (newest.empty() || newest.front().end != SessionEnd::kCrashed) return std::nullopt;
  if (CrashNoticeSeen(paths, newest.front().log)) return std::nullopt;
  std::error_code error;
  const auto written = fs::last_write_time(newest.front().log, error);
  if (error) return std::nullopt;
  const auto age = fs::file_time_type::clock::now() - written;
  if (age > std::chrono::duration_cast<fs::file_time_type::duration>(
                std::chrono::duration<double, std::ratio<3600>>(max_age_hours))) {
    return std::nullopt;
  }
  return newest.front();
}

fs::path LaunchPlanLogFile(const LaunchPlan& plan) {
  constexpr std::string_view prefix = "--log_file=";
  for (const std::string& argument : plan.arguments) {
    if (argument.starts_with(prefix)) return internal::PathFromUtf8(argument.substr(prefix.size()));
  }
  return {};
}

Result StartLauncherWithArguments(const fs::path& launcher_executable, const std::vector<std::string>& arguments) {
  try {
    std::error_code error;
    if (!fs::is_regular_file(launcher_executable, error)) return Result::Fail("The launcher could not be started.");
#ifdef _WIN32
    auto quote = [](const std::wstring& text) {
      std::wstring quoted = L"\"";
      size_t backslashes = 0;
      for (wchar_t c : text) {
        if (c == L'\\') {
          ++backslashes;
          continue;
        }
        if (c == L'"') quoted.append(backslashes * 2 + 1, L'\\');
        else quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(c);
      }
      quoted.append(backslashes * 2, L'\\');
      quoted.push_back(L'"');
      return quoted;
    };
    std::wstring command = quote(launcher_executable.wstring());
    for (const std::string& argument : arguments) {
      command.push_back(L' ');
      command += quote(internal::PathFromUtf8(argument).wstring());
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring working_directory = launcher_executable.parent_path().wstring();
    if (!CreateProcessW(launcher_executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &process)) {
      return Result::Fail("The launcher could not be started.");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return Result::Ok();
#else
    std::vector<std::string> owned{internal::PathToUtf8(launcher_executable)};
    owned.insert(owned.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& argument : owned) argv.push_back(argument.data());
    argv.push_back(nullptr);
    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0) return Result::Fail("The launcher could not be started.");
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
    pid_t child = 0;
    const int status = posix_spawn(&child, launcher_executable.c_str(), nullptr, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    return status == 0 ? Result::Ok() : Result::Fail("The launcher could not be started.");
#endif
  } catch (...) {
    return Result::Fail("The launcher could not be started.");
  }
}

Result StartCrashWatcher(const fs::path& launcher_executable, uint64_t game_process_id, const fs::path& game_log) {
  if (game_process_id == 0 || game_log.empty()) return Result::Fail("Nothing to watch.");
  return StartLauncherWithArguments(launcher_executable, {"--watch-game", std::to_string(game_process_id),
                                                          internal::PathToUtf8(game_log)});
}

bool WaitForProcessExit(uint64_t process_id, std::optional<uint32_t>* exit_code) {
  if (exit_code) exit_code->reset();
#ifdef _WIN32
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               static_cast<DWORD>(process_id));
  if (process == nullptr) return false;
  // V152 review (11-09): the launcher closed its own handle before starting this watcher. If the game ended in that
  // moment and Windows reused the id, this is another program: only watch a process that is the game.
  wchar_t image[4096];
  DWORD image_size = static_cast<DWORD>(sizeof(image) / sizeof(image[0]));
  // v1.7.0: wwe2k14.exe (Xbox) or one of the new-renderer programs.
  auto is_game_name = [](const std::wstring& name) {
    for (const wchar_t* game : {L"wwe2k14.exe", L"wwe2k14-vulkan.exe", L"wwe2k14-d3d12.exe", L"wwe2k14-d3d11.exe"}) {
      if (CompareStringOrdinal(name.c_str(), -1, game, -1, TRUE) == CSTR_EQUAL) return true;
    }
    return false;
  };
  if (!QueryFullProcessImageNameW(process, 0, image, &image_size) ||
      !is_game_name(fs::path(std::wstring(image, image_size)).filename().wstring())) {
    CloseHandle(process);
    return false;
  }
  const DWORD waited = WaitForSingleObject(process, INFINITE);
  DWORD code = 0;
  if (waited == WAIT_OBJECT_0 && GetExitCodeProcess(process, &code) && code != STILL_ACTIVE && exit_code) {
    *exit_code = static_cast<uint32_t>(code);
  }
  CloseHandle(process);
  return waited == WAIT_OBJECT_0;
#else
  // The game is not this process's child, so poll. /proc/<pid>/stat field 22 (start time) guards against a new
  // process reusing the id; state Z (exited, not yet collected) counts as ended.
  auto read_stat = [&](std::string* start_time, char* state) {
    std::ifstream input("/proc/" + std::to_string(process_id) + "/stat");
    std::string text;
    if (!std::getline(input, text)) return false;
    const size_t close = text.rfind(')');
    if (close == std::string::npos || close + 2 >= text.size()) return false;
    *state = text[close + 2];
    size_t at = close + 2;
    for (int field = 3; field < 22 && at != std::string::npos; ++field) at = text.find(' ', at + 1);
    if (at == std::string::npos) return false;
    const size_t end = text.find(' ', at + 1);
    *start_time = text.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
    return true;
  };
  std::string first_start;
  char state = 0;
  if (!read_stat(&first_start, &state)) return false;
  for (;;) {
    std::string start;
    if (!read_stat(&start, &state) || start != first_start || state == 'Z' || state == 'X') return true;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
#endif
}

bool TakeCrashPending(const Paths& paths) {
  const fs::path pending = paths.logs_dir / kWatchFolder / kPendingFile;
  std::error_code error;
  if (!fs::exists(pending, error) || error) return false;
  fs::remove(pending, error);
  return true;
}

bool ShowFileInFolder(const fs::path& file) {
#ifdef _WIN32
  const std::wstring arguments = L"/select,\"" + file.wstring() + L"\"";
  const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr,
                                         SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(result) > 32;
#else
  (void)file;
  return false;  // the UI opens the folder instead
#endif
}

int RunCrashWatcher(const fs::path& launcher_executable, uint64_t game_process_id, const fs::path& game_log) {
  const Paths paths = ResolvePaths(launcher_executable);
  std::optional<uint32_t> exit_code;
  if (!WaitForProcessExit(game_process_id, &exit_code)) return 0;
  // Give the game's log writer a moment; its crash handler writes the CRASH line as the process ends.
  std::this_thread::sleep_for(std::chrono::seconds(2));
  if (internal::EnsureDirectory(paths.logs_dir / kWatchFolder)) {
    char note[64];
    if (exit_code) std::snprintf(note, sizeof(note), "exit_code=0x%08X\n", static_cast<unsigned>(*exit_code));
    else std::snprintf(note, sizeof(note), "exit_code=unknown\n");
    WriteNote(ExitNoteFor(paths, game_log), note);
  }
  const GameSession session = ReadGameSession(paths, game_log);
  if (session.end != SessionEnd::kCrashed) return 0;
  (void)SaveBugReport(paths, nullptr, game_log);
  if (internal::EnsureDirectory(paths.logs_dir / kWatchFolder)) {
    WriteNote(paths.logs_dir / kWatchFolder / kPendingFile, SessionStamp(game_log) + "\n");
  }
  // A launcher the player opened meanwhile picks the note up itself; otherwise open one for the pop-up.
  Paths launcher_only = paths;
  launcher_only.game_exe = launcher_executable;
  if (GameProcessRunning(launcher_only)) return 0;
  (void)StartLauncherWithArguments(launcher_executable, {"--after-crash"});
  return 0;
}

}  // namespace wwe2k14::launcher
