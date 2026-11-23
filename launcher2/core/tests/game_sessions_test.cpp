#ifdef NDEBUG
#undef NDEBUG
#endif

#include "launcher_core.h"

#include "internal/archive.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {

// Bug Reports page (owner 2026-11-09). Log shapes copied from real game logs: the owner laptop Ladder crash (#110,
// crash while playing), the known exit crash (CRASH after "disarmed at window close"), a clean close, and the #107
// Linux start failure (log stops, no close or crash line).
void TestGameSessions(const fs::path& root) {
  const std::string crash_playing =
      "[2026-11-09 06:22:42.413] [info] [core] [t4028] [wwe13-flip] seq=24170\n"
      "[2026-11-09 06:22:42.417] [error] [core] [t11696] [d3d-shadow] CRASH UNHANDLED_EXCEPTION code=3221225477 "
      "fault_addr=0x1000000bc ip=0x7ff77cb6dabf\n"
      "[2026-11-09 06:22:50.255] [info] [core] [t28636] [rate-census-swap] t_ms=153658659\n";
  const std::string crash_at_exit =
      "[2026-10-27 06:19:23.632] [info] [core] [t23496] [freeze-logger] disarmed at window close\n"
      "[2026-10-27 06:19:23.636] [info] [core] [t3228] Execution complete\n"
      "[2026-10-27 06:19:23.649] [error] [core] [t7100] [d3d-shadow] CRASH UNHANDLED_EXCEPTION code=3221225477\n"
      "[2026-10-27 06:19:32.580] [warning] [core] [t25428] Shutdown still running 3 s after the window closed\n";
  const std::string clean =
      "[2026-11-09 06:12:18.735] [info] [core] [t10172] [freeze-logger] disarmed at window close\n"
      "[2026-11-09 06:12:18.737] [info] [core] [t38340] Execution complete\n";
  const std::string stopped = "[2026-11-09 01:30:09.100] [info] [core] [t1] starting\n";
  const std::string purecall =
      "[2026-11-08 16:59:46.504] [error] [core] [t18904] [d3d-shadow] CRASH UNHANDLED_EXCEPTION code=2147483651 "
      "fault_addr=0x0\n";
  std::string code;
  assert(ClassifyGameLogText(crash_playing, "", false, &code) == SessionEnd::kCrashed && code == "C0000005");
  assert(ClassifyGameLogText(crash_at_exit, "", false, &code) == SessionEnd::kClosed && code.empty());
  assert(ClassifyGameLogText(clean, "", false, &code) == SessionEnd::kClosed);
  assert(ClassifyGameLogText(stopped, "", false, &code) == SessionEnd::kNoEnding);
  assert(ClassifyGameLogText(stopped, "", true, &code) == SessionEnd::kFroze);
  assert(ClassifyGameLogText(purecall, "", false, &code) == SessionEnd::kCrashed && code == "80000003");
  // Windows watcher exit codes: an NTSTATUS error the crash handler could not log is a crash; Task Manager (1) and
  // a normal 0 are not.
  assert(ClassifyGameLogText(stopped, "exit_code=0xC00000FD\n", false, &code) == SessionEnd::kCrashed &&
         code == "C00000FD");
  assert(ClassifyGameLogText(stopped, "exit_code=0x00000001\n", false, &code) == SessionEnd::kNoEnding);
  assert(ClassifyGameLogText(clean, "exit_code=0xC0000005\n", false, &code) == SessionEnd::kClosed);

  const Paths paths = ResolvePaths(root / "sessions" / "wwe2k14-launcher");
  fs::create_directories(paths.logs_dir);
  auto write = [&](const std::string& name, const std::string& text) {
    std::ofstream output(paths.logs_dir / name, std::ios::binary);
    output << text;
  };
  for (int i = 0; i < 12; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "wwe2k14-20261101-1000%02d.log", i);
    write(name, clean);
  }
  write("wwe2k14-20261109-060919.log", clean);
  write("wwe2k14-20261109-061552.log", crash_playing);
  write("wwe2k14-timeline-20261109-060920.txt", "timeline A");
  write("wwe2k14-timeline-20261109-061553.txt", "timeline B");
  write("wwe2k14-freeze-20261109-062000.txt", "freeze in session B");
  write("launcher.log", "launcher");
  write("wwe2k14-20261109.log", "not a session name");

  const std::vector<GameSession> sessions = ListRecentGameSessions(paths, 10);
  assert(sessions.size() == 10);
  assert(sessions[0].log.filename() == "wwe2k14-20261109-061552.log");
  assert(sessions[0].end == SessionEnd::kCrashed && sessions[0].crash_code == "C0000005");
  assert(sessions[0].started == "2026-11-09 06:15:52");
  assert(sessions[0].timeline.filename() == "wwe2k14-timeline-20261109-061553.txt");
  assert(sessions[0].freeze_reports.size() == 1);
  assert(sessions[1].end == SessionEnd::kClosed && sessions[1].freeze_reports.empty());
  assert(sessions[1].timeline.filename() == "wwe2k14-timeline-20261109-060920.txt");

  // Crash pop-up once per crash.
  std::optional<GameSession> unseen = UnseenRecentCrash(paths);
  assert(unseen && unseen->log == sessions[0].log && unseen->report.empty());
  MarkCrashNoticeSeen(paths, unseen->log);
  assert(CrashNoticeSeen(paths, unseen->log) && !UnseenRecentCrash(paths));
  assert(CrashNoticeSeen(paths, sessions[1].log));  // older sessions count as seen

  // The session's report: that log, its timeline + freeze report, both launcher logs; named after the session and
  // replaced when saved again. Watcher notes never enter the normal newest-three report.
  fs::create_directories(paths.exe_dir);
  write("launcher-previous.log", "previous launcher");
  fs::path report;
  assert(SaveBugReport(paths, &report, sessions[0].log).ok);
  assert(report.filename() == "wwe2k14-bug-report-game-20261109-061552.zip");
  wwe2k14::launcher::internal::ZipReader zip;
  assert(zip.Open(report));
  std::vector<std::string> names;
  for (size_t index = 0; index < zip.Count(); ++index) {
    mz_zip_archive_file_stat stat{};
    assert(zip.Stat(index, stat));
    names.push_back(stat.m_filename);
  }
  auto has = [&](const std::string& name) { return std::find(names.begin(), names.end(), name) != names.end(); };
  assert(has("logs/wwe2k14-20261109-061552.log") && has("logs/wwe2k14-timeline-20261109-061553.txt") &&
         has("logs/wwe2k14-freeze-20261109-062000.txt") && has("logs/launcher.log") &&
         has("logs/launcher-previous.log") && has("system.txt") && has("gpu-list.txt"));
  assert(!has("logs/wwe2k14-20261109-060919.log") && !has("logs/wwe2k14-timeline-20261109-060920.txt"));
  fs::path again;
  assert(SaveBugReport(paths, &again, sessions[0].log).ok && again == report);
  assert(ReadGameSession(paths, sessions[0].log).report == report);
  assert(!SaveBugReport(paths, &again, paths.logs_dir / "wwe2k14-20991231-000000.log").ok);
  const std::vector<BugReportFile> reports = ListBugReports(paths, 10);
  assert(reports.size() == 1 && reports[0].file == report);
  fs::path newest;
  assert(SaveBugReport(paths, &newest).ok);
  wwe2k14::launcher::internal::ZipReader plain;
  assert(plain.Open(newest));
  for (size_t index = 0; index < plain.Count(); ++index) {
    mz_zip_archive_file_stat stat{};
    assert(plain.Stat(index, stat));
    assert(std::string(stat.m_filename).find("crash-watch") == std::string::npos);
    assert(std::string(stat.m_filename).find("notice-seen") == std::string::npos);
  }
  assert(ListBugReports(paths, 10).size() == 2);
  assert(!TakeCrashPending(paths));

  // A Windows watcher note (plain text in logs/crash-watch/) turns a log without a crash line into a crash.
  write("wwe2k14-20261109-070000.log", stopped);
  fs::create_directories(paths.logs_dir / "crash-watch");
  {
    std::ofstream note(paths.logs_dir / "crash-watch" / "wwe2k14-20261109-070000.log.exit", std::ios::binary);
    note << "exit_code=0xC0000409\n";
  }
  const GameSession fast_fail = ReadGameSession(paths, paths.logs_dir / "wwe2k14-20261109-070000.log");
  assert(fast_fail.end == SessionEnd::kCrashed && fast_fail.crash_code == "C0000409");
  assert(UnseenRecentCrash(paths) && UnseenRecentCrash(paths)->log == fast_fail.log);
  // The seen note written by the launcher is plain text.
  MarkCrashNoticeSeen(paths, fast_fail.log);
  {
    std::ifstream seen(paths.logs_dir / "crash-watch" / "notice-seen.txt", std::ios::binary);
    std::string line;
    std::getline(seen, line);
    assert(line == "20261109-070000");
  }

  LaunchPlan plan;
  plan.arguments = {"game", "--log_file=/x/logs/wwe2k14-20261109-061552.log", "--debug_ui=false"};
  assert(LaunchPlanLogFile(plan) == fs::path("/x/logs/wwe2k14-20261109-061552.log"));
  std::cout << "PASS game sessions: crash vs exit crash vs clean close vs no ending, 10 newest, timeline/freeze "
               "matching, one pop-up per crash, per-session report contents and name, watcher notes kept out\n";
}


}  // namespace

int main() {
  const fs::path root = fs::path(WWE2K14_LAUNCHER_TESTDATA).parent_path() / "game-sessions-run";
  std::error_code error;
  fs::remove_all(root, error);
  fs::create_directories(root);
  TestGameSessions(root);
  return 0;
}
