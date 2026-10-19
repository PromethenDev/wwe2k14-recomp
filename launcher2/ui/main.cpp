#include "launcher_core.h"
#include "launcher_version.h"
#include "native_keys.h"
#include "display_scale.h"
#include "update.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "launcher_embedded_fonts.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <exception>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using namespace wwe2k14::launcher;

constexpr ImVec4 kWindow{0.0588f, 0.0627f, 0.0784f, 1.0f};
constexpr ImVec4 kSidebar{0.0314f, 0.0353f, 0.0431f, 1.0f};
constexpr ImVec4 kPanel{0.0863f, 0.0941f, 0.1137f, 1.0f};
constexpr ImVec4 kRaised{0.1059f, 0.1137f, 0.1373f, 1.0f};
constexpr ImVec4 kBorder{0.1725f, 0.1843f, 0.2157f, 1.0f};
constexpr ImVec4 kText{0.9255f, 0.9294f, 0.9412f, 1.0f};
constexpr ImVec4 kSecondary{0.7255f, 0.7373f, 0.7686f, 1.0f};
constexpr ImVec4 kMuted{0.5412f, 0.5569f, 0.5961f, 1.0f};
constexpr ImVec4 kAccent{0.8431f, 0.1490f, 0.2392f, 1.0f};
constexpr ImVec4 kSuccess{0.2902f, 0.8706f, 0.5020f, 1.0f};
constexpr ImVec4 kDanger{0.9608f, 0.3608f, 0.3725f, 1.0f};
constexpr char kDropWaitMessage[] = "Please wait until the current task finishes, then drop the file again.";
constexpr char kReadOnlyFolderMessage[] =
    "This folder is read-only. Move the WWE2K14-Recomp folder somewhere like Documents or Desktop and start it again.";

enum class Tab { Play, Settings, Controls, Music, GameFiles, Saves };
enum class DialogKind { None, Music, DiscImage, Package, GameFolder, Backup };

struct JobOutcome {
  Result result = Result::Ok();
  std::function<void()> apply;
};

using JobTask = std::function<JobOutcome(const ProgressFn&, CancelFlag&)>;

struct JobState {
  std::thread thread;
  std::mutex mutex;
  CancelFlag cancel{false};
  std::atomic<bool> active{false};
  bool finished = false;
  uint64_t done = 0;
  uint64_t total = 0;
  std::string label;
  Result result = Result::Ok();
  std::function<void()> apply;
  std::chrono::steady_clock::time_point started;
};

struct DialogState {
  std::mutex mutex;
  DialogKind kind = DialogKind::None;
  bool ready = false;
  bool failed = false;
  std::vector<std::string> paths;
};

// Result of the background "is there a newer version?" check.
struct UpdateCheck {
  std::mutex mutex;
  bool running = false;
  bool checked = false;
  UpdateInfo info;
};

struct Fonts {
  ImFont* oswald_medium = nullptr;
  ImFont* oswald_semibold = nullptr;
  ImFont* oswald_bold = nullptr;
  ImFont* barlow_regular = nullptr;
  ImFont* barlow_small = nullptr;
  ImFont* barlow_medium = nullptr;
  ImFont* barlow_semibold = nullptr;
};

struct AppState {
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  Fonts fonts;
  JobState job;
  DialogState dialog;
  std::mutex drop_mutex;
  std::vector<std::string> dropped;

  Paths paths;
  Settings settings;
  std::vector<GpuInfo> gpus;
  Recommendation recommendation;
  GameFilesStatus game_files;
  std::vector<FoundGame> candidates;
  std::vector<Song> songs;
  std::vector<BackupInfo> backups;
  KeyboardControls controls = DefaultKeyboardControls();
  bool scroll_to_candidates = false;
  FolderInstallables folder_installables;  // ISO / TU / DLC files dropped into the "WWE 2K14" folder  // Game files: bring the "folders found" list into view once
  int capture_index = -1;  // Controls row waiting for a key press, -1 = none
  std::string controls_status;
  bool controls_status_ok = true;
  Tab tab = Tab::Play;
  bool initialized = false;
  bool running = true;
  bool drop_wait_notice_pending = false;
  bool shift_at_start = false;
  bool test_shift_held = false;
  bool bypass_auto_start = false;
  bool screenshot_mode = false;
  std::filesystem::path screenshot_dir;
  float test_pixel_density = 0.0f;  // Screenshot-only simulated high-density framebuffer.
  size_t screenshot_index = 0;
  std::vector<fs::path> test_drop_files;
  Tab test_drop_tab = Tab::Music;
  bool imgui_platform_ready = false;
  bool imgui_renderer_ready = false;
  float display_scale = 1.0f;
  float fit_scale = 1.0f;
  unsigned int last_fitted_input_event = 0;
  bool test_window_size = false;
  fs::path restore_file;
  std::string banner;
  bool banner_success = false;
  std::atomic<bool> launch_process_started{false};
  std::string launch_warning;
  bool launch_warning_pending = false;
  bool gamepad_connected = false;
  int startup_width = 1280;
  int startup_height = 760;

  // Auto-updater (ported from WWE '13 v1.4). The check runs on its own thread; the UI only reads the result.
  UpdateCheck update;
  std::thread update_thread;
  bool update_hidden_this_session = false;  // "Not now"
  bool update_job = false;                  // the running job is an update download/install
  bool update_auto_attempted = false;       // the test-only auto-accept ran once
  bool update_notes_open = false;
  bool update_screenshot_mode = false;      // test-only capture sequence
  size_t update_screenshot_index = 0;
};

float S(const AppState& app, float pixels) {
  return pixels * app.display_scale;
}

float GetWindowPixelDensity(const AppState& app) {
  if (app.screenshot_mode && app.test_pixel_density > 0.0f) return app.test_pixel_density;
  return SDL_GetWindowPixelDensity(app.window);
}

ImVec2 S(const AppState& app, float x, float y) {
  return ImVec2(S(app, x), S(app, y));
}

// Usable desktop area of the window's display (excludes the taskbar), minus room for the title bar
// and a small margin, so the launcher never opens partly off-screen.
SDL_Rect UsableWindowArea(const AppState& app) {
  SDL_Rect usable{0, 0, 0, 0};
  if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(app.window), &usable)) return usable;
  const int margin = static_cast<int>(std::lround(48.0f * app.display_scale));
  usable.w = std::max(0, usable.w - margin);
  usable.h = std::max(0, usable.h - margin);
  return usable;
}

void SetMinimumWindowSize(const AppState& app) {
  const float scale = app.test_window_size ? 1.0f : app.display_scale;
  int width = static_cast<int>(std::lround(1024.0f * scale));
  int height = static_cast<int>(std::lround(640.0f * scale));
  // The layout scales down to fit (fit-to-window), so a small screen may go below the design size.
  const SDL_Rect usable = UsableWindowArea(app);
  if (usable.w > 0 && usable.h > 0) {
    width = std::min(width, usable.w);
    height = std::min(height, usable.h);
  }
  SDL_SetWindowMinimumSize(app.window, width, height);
}

const std::array<std::pair<Tab, const char*>, 6> kTabs{{
    {Tab::Play, "Play"}, {Tab::Settings, "Settings"}, {Tab::Controls, "Controls"}, {Tab::Music, "Music"},
    {Tab::GameFiles, "Game files"}, {Tab::Saves, "Saves"},
}};

std::string PathUtf8(const fs::path& path) {
  const auto encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

fs::path Utf8Path(const std::string& text) {
  const auto* begin = reinterpret_cast<const char8_t*>(text.data());
  return fs::path(std::u8string(begin, begin + text.size()));
}

fs::path LauncherExecutablePath() {
  // Ask the OS for the real path: the packaged Windows launcher is "WWE 2K14 Launcher.exe" while the build
  // target and the Linux binary are "wwe2k14-launcher". Guessing the name breaks relaunching after an update.
#ifdef _WIN32
  wchar_t buffer[4096];
  const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
  if (length > 0 && length < std::size(buffer)) return fs::path(buffer);
#else
  std::error_code error;
  fs::path self = fs::read_symlink("/proc/self/exe", error);
  if (!error && !self.empty()) {
    // After an in-place update the running inode has been unlinked, so /proc/self/exe reads
    // "<path> (deleted)". The path still names the location and now holds the new binary.
    const std::string text = self.string();
    constexpr std::string_view deleted = " (deleted)";
    if (text.ends_with(deleted)) self = fs::path(text.substr(0, text.size() - deleted.size()));
    return self;
  }
#endif
  const char* base = SDL_GetBasePath();
  const fs::path directory = base ? Utf8Path(base) : fs::current_path();
#ifdef _WIN32
  return directory / "wwe2k14-launcher.exe";
#else
  return directory / "wwe2k14-launcher";
#endif
}

fs::path ActiveGameFolder(const AppState& app) {
  return app.settings.game_folder.empty() ? app.paths.default_game_folder : app.settings.game_folder;
}

Settings ActiveSettings(const AppState& app) {
  if (app.settings.explicit_choice) return app.settings;
  Settings settings = app.recommendation.settings;
  settings.gpu_id = app.settings.gpu_id;
  settings.game_folder = app.settings.game_folder;
  settings.auto_start = app.settings.auto_start;
  settings.auto_backup = app.settings.auto_backup;
  settings.check_updates = app.settings.check_updates;
  settings.sync_frames_to_monitor = app.settings.sync_frames_to_monitor;
  settings.stretch_to_fill_wide_screens = app.settings.stretch_to_fill_wide_screens;
  settings.picture_scale = app.settings.picture_scale;
  settings.skipped_update_version = app.settings.skipped_update_version;
  settings.explicit_choice = false;
  return settings;
}

void KeepManualSettings(AppState& app) {
  if (!app.settings.explicit_choice) app.settings = ActiveSettings(app);
  app.settings.explicit_choice = true;
}

void SetBanner(AppState& app, std::string text, bool success = false) {
  app.banner = std::move(text);
  app.banner_success = success;
}

bool CanWriteExeDirectory(const fs::path& directory) {
  for (uint64_t attempt = 0; attempt < 8; ++attempt) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path probe = directory / ("wwe2k14-launcher-write-test-" + std::to_string(stamp) + "-" +
                                        std::to_string(attempt) + ".tmp");
#ifdef _WIN32
    const int descriptor = _wopen(probe.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                                  _S_IREAD | _S_IWRITE);
#else
    const int descriptor = ::open(probe.c_str(), O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
#endif
    if (descriptor < 0) {
      if (errno == EEXIST) continue;
      return false;
    }
    const char marker = 'x';
#ifdef _WIN32
    const bool wrote = _write(descriptor, &marker, 1) == 1;
    const bool closed = _close(descriptor) == 0;
#else
    const bool wrote = ::write(descriptor, &marker, 1) == 1;
    const bool closed = ::close(descriptor) == 0;
#endif
    std::error_code error;
    const bool removed = fs::remove(probe, error);
    return wrote && closed && removed && !error;
  }
  return false;
}

void BeginJob(AppState& app, std::string label, JobTask task) {
  if (app.job.active.load()) return;
  if (app.job.thread.joinable()) app.job.thread.join();
  {
    std::lock_guard lock(app.job.mutex);
    app.job.finished = false;
    app.job.done = 0;
    app.job.total = 0;
    app.job.label = std::move(label);
    app.job.result = Result::Ok();
    app.job.apply = {};
    app.job.started = std::chrono::steady_clock::now();
  }
  app.job.cancel.store(false);
  app.job.active.store(true);
  app.job.thread = std::thread([&app, task = std::move(task)]() mutable {
    ProgressFn progress = [&app](uint64_t done, uint64_t total, const std::string& label) {
      std::lock_guard lock(app.job.mutex);
      app.job.done = done;
      app.job.total = total;
      app.job.label = label;
    };
    JobOutcome outcome;
    try {
      outcome = task(progress, app.job.cancel);
    } catch (const std::exception& error) {
      outcome.result = Result::Fail(std::string("Something went wrong: ") + error.what());
    } catch (...) {
      outcome.result = Result::Fail("Something went wrong while completing that action.");
    }
    std::lock_guard lock(app.job.mutex);
    app.job.result = std::move(outcome.result);
    app.job.apply = std::move(outcome.apply);
    app.job.finished = true;
  });
}

void RefreshFolderInstallables(AppState& app);
void DrawProgress(AppState& app);  // defined with the Game files page; reused by the updater
void DrawUpdateBanner(AppState& app);
void DrawUpdateNotes(AppState& app);
void StartUpdate(AppState& app);
void StartUpdateCheck(AppState& app);
void PollUpdateCheck(AppState& app);

void PollJob(AppState& app) {
  // StartGame reports process creation separately because it may keep watching for an early crash.
  // Keep SDL window changes on the UI thread while the worker waits.
  if (app.launch_process_started.exchange(false, std::memory_order_acquire)) {
    SDL_MinimizeWindow(app.window);
  }
  std::function<void()> apply;
  Result result;
  bool finished = false;
  {
    std::lock_guard lock(app.job.mutex);
    if (app.job.finished) {
      finished = true;
      result = app.job.result;
      apply = std::move(app.job.apply);
      app.job.finished = false;
    }
  }
  if (!finished) return;
  if (app.job.thread.joinable()) app.job.thread.join();
  app.job.active.store(false);
  app.update_job = false;
  if (result.unknown) {
    // A wait API error cannot prove that the game exited; keep the launcher open without a false warning.
    SDL_RestoreWindow(app.window);
    SDL_RaiseWindow(app.window);
  } else if (!result.ok) {
    if (result.error == kGameClosedEarlyMessage) {
      SDL_RestoreWindow(app.window);
      SDL_RaiseWindow(app.window);
      app.launch_warning = result.error;
      app.launch_warning_pending = true;
    } else {
      SetBanner(app, result.error.empty() ? "That action could not be completed." : result.error);
    }
  } else {
    if (apply) apply();
  }
  if (app.initialized) RefreshFolderInstallables(app);
  if (app.drop_wait_notice_pending) {
    app.drop_wait_notice_pending = false;
    SetBanner(app, kDropWaitMessage);
  }
}

// logs/launcher.log: renderer facts at start plus every slow frame / settings save, so a "clicking is
// slow" report from a player's PC names the stage. Small and capped; rewritten each start.
std::FILE* g_perf_log = nullptr;
int g_perf_log_lines = 0;
constexpr int kPerfLogMaxLines = 400;

void PerfLog(const char* format, ...) {
  if (!g_perf_log || g_perf_log_lines >= kPerfLogMaxLines) return;
  char line[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  // The update check logs from its own thread; serialise so lines never interleave.
  static std::mutex log_mutex;
  std::lock_guard lock(log_mutex);
  if (g_perf_log_lines >= kPerfLogMaxLines) return;
  std::fprintf(g_perf_log, "[%8.3f s] %s\n", SDL_GetTicksNS() / 1e9, line);
  std::fflush(g_perf_log);
  ++g_perf_log_lines;
}

void SaveSettings(AppState& app) {
  const Uint64 start = SDL_GetTicksNS();
  const Result result = wwe2k14::launcher::SaveSettings(app.paths, app.settings);
  const double ms = (SDL_GetTicksNS() - start) / 1e6;
  if (ms > 10.0) PerfLog("settings save took %.1f ms", ms);
  if (!result.ok) SetBanner(app, result.error);
}

void StartRefreshGameFiles(AppState& app) {
  const Paths paths = app.paths;
  const fs::path folder = ActiveGameFolder(app);
  BeginJob(app, "Checking game files…", [&app, paths, folder](const ProgressFn&, CancelFlag&) {
    const GameFilesStatus status = CheckGameFolder(folder, paths);
    const auto songs = ListSongs(paths);
    return JobOutcome{Result::Ok(), [&app, status, songs] {
                        app.game_files = status;
                        app.songs = songs;
                      }};
  });
}

void StartLaunch(AppState& app);

void LaunchNow(AppState& app) {
  if (!app.game_files.ready()) {
    SetBanner(app, "Game files need attention before you can play.");
    app.tab = Tab::GameFiles;
    return;
  }
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, "WWE 2K14 is already running. Close the game window before pressing Play again.");
    return;
  }
  const Paths paths = app.paths;
  const Settings settings = ActiveSettings(app);
  const std::vector<GpuInfo> gpus = app.gpus;
  app.launch_process_started.store(false, std::memory_order_relaxed);
  BeginJob(app, "Starting WWE 2K14…", [&app, paths, settings, gpus](const ProgressFn&, CancelFlag& cancel) {
    const LaunchPlan plan = BuildLaunchPlan(paths, settings, gpus);
    const Result result = StartGame(plan, 90.0, &app.launch_process_started, &cancel);
    return JobOutcome{result, [&app] {
                        SDL_MinimizeWindow(app.window);
                        app.running = false;
                      }};
  });
}

void StartLaunch(AppState& app) {
  if (app.job.active.load()) return;
  if (!app.game_files.ready()) {
    SetBanner(app, "Add the base game and Title Update 1.0.3 to continue.");
    app.tab = Tab::GameFiles;
    return;
  }
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, "WWE 2K14 is already running. Close the game window before pressing Play again.");
    return;
  }
  if (!app.settings.auto_backup) {
    LaunchNow(app);
    return;
  }
  const Paths paths = app.paths;
  BeginJob(app, "Backing up saves before launch…", [&app, paths](const ProgressFn&, CancelFlag&) {
    BackupInfo created;
    const Result result = BackupSaves(paths, true, &created);
    std::vector<BackupInfo> backups;
    if (result.ok) backups = ListBackups(paths);
    return JobOutcome{result, [&app, backups = std::move(backups)]() mutable {
                        app.backups = std::move(backups);
                        LaunchNow(app);
                      }};
  });
}

void StartInitialization(AppState& app) {
  const fs::path executable = LauncherExecutablePath();
  BeginJob(app, "Getting things ready…", [&app, executable](const ProgressFn&, CancelFlag&) {
    const Paths paths = ResolvePaths(executable);
    const bool exe_dir_writable = CanWriteExeDirectory(paths.exe_dir);
    Settings settings = LoadSettings(paths);
    const std::vector<GpuInfo> gpus = ListGpus();
    const Recommendation recommendation = RecommendedSettings(gpus);
    const GameFilesStatus game_files = CheckGameFolder(
        settings.game_folder.empty() ? paths.default_game_folder : settings.game_folder, paths);
    const std::vector<Song> songs = ListSongs(paths);
    const std::vector<BackupInfo> backups = ListBackups(paths);
    if (exe_dir_writable) EnsureKeyboardDefaults(paths);  // first start: write the modern default keys
    if (exe_dir_writable) CleanupUpdateLeftovers(paths);  // remove *.old / *.part from a previous update
    const KeyboardControls controls = LoadKeyboardControls(paths);
    return JobOutcome{Result::Ok(), [&app, paths, settings,
                                      exe_dir_writable, controls,
                                      gpus, recommendation, game_files, songs, backups] {
                         app.paths = paths;
                         app.settings = settings;
                        app.gpus = gpus;
                        app.recommendation = recommendation;
                        app.game_files = game_files;
                         app.songs = songs;
                         app.backups = backups;
                         app.controls = controls;
                         app.initialized = true;
                         RefreshFolderInstallables(app);
                         if (app.settings.check_updates && (!app.screenshot_mode || app.update_screenshot_mode)) {
                           StartUpdateCheck(app);
                         }
                         if (!app.game_files.ready() && (!app.folder_installables.disc_images.empty() ||
                                                         !app.folder_installables.packages.empty())) {
                           app.tab = Tab::GameFiles;
                         }
                         if (app.settings.auto_start && !app.shift_at_start && !app.bypass_auto_start &&
                            !app.screenshot_mode) {
                          StartLaunch(app);
                        }
                         if (!exe_dir_writable) SetBanner(app, kReadOnlyFolderMessage);
                      }};
  });
}

void RequestDialog(AppState& app, DialogKind kind, const SDL_DialogFileFilter* filters,
                   int filter_count, bool many) {
  {
    std::lock_guard lock(app.dialog.mutex);
    app.dialog.kind = kind;
    app.dialog.ready = false;
    app.dialog.failed = false;
    app.dialog.paths.clear();
  }
  if (kind == DialogKind::GameFolder || kind == DialogKind::Backup) {
    if (const char* selected = std::getenv("WWE2K14_TEST_DIALOG_PATH"); selected && *selected) {
      std::lock_guard lock(app.dialog.mutex);
      app.dialog.ready = true;
      app.dialog.paths.emplace_back(selected);
      return;
    }
  }
  const std::string default_location = PathUtf8(app.paths.exe_dir);
  if (kind == DialogKind::GameFolder) {
    SDL_ShowOpenFolderDialog(
        [](void* userdata, const char* const* filelist, int) {
          auto& app = *static_cast<AppState*>(userdata);
          std::lock_guard lock(app.dialog.mutex);
          app.dialog.ready = true;
          app.dialog.failed = filelist == nullptr;
          if (filelist) {
            for (size_t i = 0; filelist[i]; ++i) app.dialog.paths.emplace_back(filelist[i]);
          }
        },
        &app, app.window, default_location.c_str(), false);
  } else {
    SDL_ShowOpenFileDialog(
        [](void* userdata, const char* const* filelist, int) {
          auto& app = *static_cast<AppState*>(userdata);
          std::lock_guard lock(app.dialog.mutex);
          app.dialog.ready = true;
          app.dialog.failed = filelist == nullptr;
          if (filelist) {
            for (size_t i = 0; filelist[i]; ++i) app.dialog.paths.emplace_back(filelist[i]);
          }
        },
        &app, app.window, filters, filter_count, default_location.c_str(), many);
  }
}

void StartChooseGameFolder(AppState& app, const fs::path& folder) {
  const Paths paths = app.paths;
  const Settings current_settings = app.settings;
  BeginJob(app, "Checking the selected folder…", [&app, paths, folder, current_settings](const ProgressFn&, CancelFlag&) {
    const GameFilesStatus status = CheckGameFolder(folder, paths);
    Settings settings = current_settings;
    settings.game_folder = folder;
    const Result saved = SaveSettings(paths, settings);
    return JobOutcome{saved, [&app, status, settings] {
                        app.game_files = status;
                        app.settings = settings;
                        app.settings.game_folder = status.game_folder.empty() ? settings.game_folder : status.game_folder;
                        SetBanner(app, status.ready() ? "Game files ready." : "Folder checked - see the rows below.",
                                  status.ready());
                      }};
  });
}

void StartAddSongs(AppState& app, std::vector<fs::path> files) {
  if (files.empty()) return;
  const Paths paths = app.paths;
  BeginJob(app, "Adding songs…", [&app, paths, files = std::move(files)](const ProgressFn&, CancelFlag&) {
    std::vector<std::string> skipped;
    const Result result = AddSongs(paths, files, &skipped);
    const std::vector<Song> songs = result.ok ? ListSongs(paths) : std::vector<Song>{};
    return JobOutcome{result, [&app, songs, skipped] {
                        app.songs = songs;
                        if (!skipped.empty()) {
                          std::string message = "Some files were skipped: ";
                          for (size_t i = 0; i < skipped.size(); ++i) {
                            if (i) message += ", ";
                            message += skipped[i];
                          }
                          SetBanner(app, std::move(message));
                        } else {
                          SetBanner(app, "Songs were added to your music folder.", true);
                        }
                      }};
  });
}

void StartRemoveSong(AppState& app, fs::path file) {
  const Paths paths = app.paths;
  BeginJob(app, "Removing song…", [&app, paths, file = std::move(file)](const ProgressFn&, CancelFlag&) {
    const Result result = RemoveSong(paths, file);
    const std::vector<Song> songs = result.ok ? ListSongs(paths) : std::vector<Song>{};
    return JobOutcome{result, [&app, songs] {
                        app.songs = songs;
                        SetBanner(app, "Song removed from your music folder.", true);
                      }};
  });
}

void StartFindGameFiles(AppState& app) {
  const Paths paths = app.paths;
  BeginJob(app, "Finding game files…", [&app, paths](const ProgressFn& progress, CancelFlag& cancel) {
    std::vector<FoundGame> candidates = FindGameFiles(paths, progress, cancel);
    return JobOutcome{Result::Ok(), [&app, candidates = std::move(candidates)]() mutable {
                        app.candidates = std::move(candidates);
                        app.scroll_to_candidates = !app.candidates.empty();
                        if (app.candidates.empty()) SetBanner(app, "No WWE 2K14 folder was found. Choose a folder to check.");
                        else SetBanner(app, "Choose a verified game folder below.", true);
                      }};
  });
}

// Looks in the "WWE 2K14" folder next to the launcher for a disc image, title update or DLC files a player copied
// there, so the Game files page can offer to install them (the README tells players to put files in that folder).
void RefreshFolderInstallables(AppState& app) {
  app.folder_installables = FolderInstallables{};
  if (app.paths.default_game_folder.empty()) return;
  const bool needs_base = app.game_files.base_game != FileState::kFound;
  const bool needs_more = app.game_files.title_update != FileState::kFound ||
                          app.game_files.dlc_installed < app.game_files.dlc_known;
  if (needs_base || needs_more) app.folder_installables = FindInstallableFiles(app.paths.default_game_folder);
}

void StartExtractDisc(AppState& app, fs::path image) {
  const Paths paths = app.paths;
  const fs::path destination = paths.default_game_folder;
  const Settings current_settings = app.settings;
  BeginJob(app, "Preparing the game folder…", [&app, paths, image = std::move(image), destination, current_settings](
                                                        const ProgressFn& progress, CancelFlag& cancel) {
    const Result result = ExtractDiscImage(image, destination, progress, cancel);
    const GameFilesStatus status = result.ok ? CheckGameFolder(destination, paths) : GameFilesStatus{};
    Settings settings = current_settings;
    if (result.ok) settings.game_folder = destination;
    const Result save = result.ok ? SaveSettings(paths, settings) : result;
    return JobOutcome{save, [&app, status, settings] {
                        app.game_files = status;
                        app.settings = settings;
                        SetBanner(app, "Disc image added. Check the Title Update and DLC rows below.", true);
                      }};
  });
}

void StartImportPackages(AppState& app, std::vector<fs::path> packages) {
  if (packages.empty()) return;
  const Paths paths = app.paths;
  const fs::path game_folder = ActiveGameFolder(app);
  BeginJob(app, "Adding update or DLC…", [&app, paths, packages = std::move(packages), game_folder](
                                                        const ProgressFn& progress, CancelFlag& cancel) {
    // Try every picked file: one unsupported file (e.g. an avatar item next to the DLC packs) must not stop the rest.
    Result result = Result::Ok();
    size_t added = 0, already = 0;
    std::vector<std::string> skipped;
    std::string first_error;
    for (const auto& package : packages) {
      if (cancel.load()) {
        result = Result::Fail("The operation was canceled.");
        break;
      }
      const Result one = ImportPackage(package, paths, game_folder, progress, cancel);
      if (one.ok) {
        ++added;
      } else if (one.error == "This content is already installed.") {  // ImportDlc's message
        ++already;
      } else {
        skipped.push_back(PathUtf8(package.filename()));
        if (first_error.empty()) first_error = one.error;
      }
    }
    if (result.ok && added == 0 && already == 0) {
      result = Result::Fail(packages.size() == 1 ? first_error : "None of the selected files could be added: " + first_error);
    }
    const GameFilesStatus status = added + already > 0 ? CheckGameFolder(game_folder, paths) : GameFilesStatus{};
    return JobOutcome{result, [&app, status, added, already, skipped, package_count = packages.size()] {
                        if (!status.game_folder.empty()) app.game_files = status;
                        std::string text;
                        if (added == 0) text = package_count == 1 ? "This pack is already installed." : "These packs are already installed.";
                        else if (package_count == 1) text = "The update or pack was added.";
                        else if (skipped.empty() && already == 0) text = "The selected packs were added.";
                        else text = "Added " + std::to_string(added) + " of " + std::to_string(package_count) + ".";
                        if (added > 0 && already > 0) text += " " + std::to_string(already) + " already installed.";
                        if (!skipped.empty()) {
                          text += " Skipped (not a WWE 2K14 update or pack, or unreadable): ";
                          for (size_t i = 0; i < skipped.size(); ++i) text += (i ? ", " : "") + skipped[i];
                        }
                        SetBanner(app, text, true);
                      }};
  });
}

void StartBackup(AppState& app, bool automatic = false) {
  const Paths paths = app.paths;
  BeginJob(app, automatic ? "Backing up saves before launch…" : "Backing up saves…",
           [&app, paths, automatic](const ProgressFn& progress, CancelFlag&) {
    BackupInfo created;
    const Result result = BackupSaves(paths, automatic, &created);
    const std::vector<BackupInfo> backups = result.ok ? ListBackups(paths) : std::vector<BackupInfo>{};
    return JobOutcome{result, [&app, backups, created] {
                        app.backups = backups;
                        SetBanner(app, "Backup saved: " + PathUtf8(created.file.filename()), true);
                      }};
  });
}

void StartImportBackup(AppState& app, fs::path file) {
  const Paths paths = app.paths;
  BeginJob(app, "Importing backup…", [&app, paths, file = std::move(file)](const ProgressFn&, CancelFlag&) {
    const Result result = ImportBackup(paths, file);
    const std::vector<BackupInfo> backups = result.ok ? ListBackups(paths) : std::vector<BackupInfo>{};
    return JobOutcome{result, [&app, backups] {
                        app.backups = backups;
                        SetBanner(app, "Backup imported.", true);
                      }};
  });
}

void StartRestoreBackup(AppState& app, fs::path file) {
  const Paths paths = app.paths;
  BeginJob(app, "Restoring saves…", [&app, paths, file = std::move(file)](const ProgressFn& progress, CancelFlag&) {
    const Result result = RestoreBackup(paths, file);
    const std::vector<BackupInfo> backups = result.ok ? ListBackups(paths) : std::vector<BackupInfo>{};
    return JobOutcome{result, [&app, backups] {
                        app.backups = backups;
                        SetBanner(app, "Saves restored from the selected backup.", true);
                      }};
  });
}

void StartBugReport(AppState& app) {
  const Paths paths = app.paths;
  BeginJob(app, "Saving a bug report…", [&app, paths](const ProgressFn&, CancelFlag&) {
    fs::path created;
    const Result result = SaveBugReport(paths, &created);
    return JobOutcome{result, [&app, created] {
                        SetBanner(app, "Bug report saved to " + PathUtf8(created), true);
                      }};
  });
}

void ConsumeDialog(AppState& app) {
  DialogKind kind = DialogKind::None;
  bool failed = false;
  std::vector<std::string> selections;
  {
    std::lock_guard lock(app.dialog.mutex);
    if (!app.dialog.ready) return;
    kind = app.dialog.kind;
    failed = app.dialog.failed;
    selections = std::move(app.dialog.paths);
    app.dialog.kind = DialogKind::None;
    app.dialog.ready = false;
    app.dialog.failed = false;
  }
  if (failed) {
    SetBanner(app, "The file picker could not be opened. Try again or choose a folder another way.");
    return;
  }
  if (selections.empty()) return;
  std::vector<fs::path> paths;
  paths.reserve(selections.size());
  for (const auto& selected : selections) paths.push_back(Utf8Path(selected));
  switch (kind) {
    case DialogKind::Music:
      StartAddSongs(app, std::move(paths));
      break;
    case DialogKind::DiscImage:
      StartExtractDisc(app, paths.front());
      break;
    case DialogKind::Package:
      StartImportPackages(app, std::move(paths));
      break;
    case DialogKind::GameFolder:
      StartChooseGameFolder(app, paths.front());
      break;
    case DialogKind::Backup:
      StartImportBackup(app, paths.front());
      break;
    case DialogKind::None:
      break;
  }
}

void QueueDrop(AppState& app, const char* filename) {
  if (!filename) return;
  std::lock_guard lock(app.drop_mutex);
  app.dropped.emplace_back(filename);
}

void ProcessDrops(AppState& app) {
  std::vector<std::string> dropped;
  {
    std::lock_guard lock(app.drop_mutex);
    dropped.swap(app.dropped);
  }
  if (dropped.empty()) return;
  if (app.job.active.load()) {
    app.drop_wait_notice_pending = true;
    SetBanner(app, kDropWaitMessage);
    return;
  }
  if (app.tab == Tab::Music) {
    std::vector<fs::path> files;
    for (const auto& path : dropped) files.push_back(Utf8Path(path));
    StartAddSongs(app, std::move(files));
  } else if (app.tab == Tab::GameFiles) {
    std::vector<fs::path> packages;
    for (const auto& path : dropped) {
      const fs::path file = Utf8Path(path);
      std::error_code error;
      if (fs::is_directory(file, error)) {
        const GameFilesStatus folder_status = CheckGameFolder(file, app.paths);
        if (folder_status.base_game == FileState::kFound) {
          StartChooseGameFolder(app, file);
          return;
        }
        const std::optional<PackageInfo> package = InspectPackage(file);
        if (package && package->title_id == 0x545408B2 && package->kind != PackageKind::kUnknown) {
          packages.push_back(file);
          continue;
        }
        StartChooseGameFolder(app, file);
        return;
      }
      std::string extension = file.extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      if (extension == ".iso" || extension == ".img") {
        StartExtractDisc(app, file);
        return;
      }
      packages.push_back(file);
    }
    StartImportPackages(app, std::move(packages));
  }
}

void DrawTextDisabled(const AppState& app, const char* text) {
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void DrawSecondaryWrapped(const AppState& app, const char* text, ImVec4 color = kMuted) {
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

bool BeginPanel(const AppState& app, const char* id, ImVec2 size = ImVec2(0, 0)) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
  ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 14.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, S(app, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(app, 12.0f, 10.0f));
  return ImGui::BeginChild(id, size, true, ImGuiWindowFlags_NoScrollbar);
}

void EndPanel() {
  ImGui::EndChild();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
}

void DrawPanelHeading(const AppState& app, const char* title, const char* hint) {
  ImGui::PushFont(app.fonts.oswald_semibold);
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  ImGui::TextUnformatted(title);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  if (hint && *hint) {
    DrawSecondaryWrapped(app, hint);
  }
}

void DrawNavButton(AppState& app, Tab tab, const char* label) {
  const bool selected = app.tab == tab;
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(app, 9.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, S(app, 14.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_Button, selected ? kRaised : kSidebar);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRaised);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kRaised);
  ImGui::PushFont(app.fonts.oswald_medium);
  if (ImGui::Button(label, ImVec2(-1.0f, S(app, 46.0f)))) {
    app.tab = tab;
    app.banner.clear();
  }
  if (selected) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    draw->AddRectFilled(ImVec2(min.x, min.y + S(app, 8.0f)), ImVec2(min.x + S(app, 3.0f), max.y - S(app, 8.0f)),
                        ImGui::ColorConvertFloat4ToU32(kAccent), S(app, 2.0f));
  }
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(3);
  ImGui::PopFont();
}

std::string ResolutionName(Resolution value) {
  switch (value) {
    case Resolution::k720p: return "720p";
    case Resolution::k1080p: return "1080p";
    case Resolution::k1440p: return "1440p";
  }
  return "720p";
}

std::string DisplayName(DisplayMode value) {
  return value == DisplayMode::kFullscreen ? "Fullscreen" : "Windowed";
}

std::string CurrentGpuName(const AppState& app) {
  if (app.settings.gpu_id.empty()) {
    if (!app.gpus.empty()) return app.gpus.front().name;
    return "Recommended settings";
  }
  for (const auto& gpu : app.gpus) {
    if (gpu.id == app.settings.gpu_id) return gpu.name;
  }
  return "Selected graphics card";
}

void DrawPill(const AppState& app, const char* label, const std::string& value, float width) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kRaised);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 9.0f));
  ImGui::BeginChild(label, ImVec2(width, S(app, 57.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetCursorPos(S(app, 12.0f, 7.0f));
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::SetCursorPosX(S(app, 12.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  ImGui::TextUnformatted(value.c_str());
  ImGui::PopStyleColor();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

void DrawBanner(AppState& app) {
  if (app.banner.empty()) return;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, app.banner_success ? ImVec4(0.06f, 0.18f, 0.12f, 1.0f)
                                                            : ImVec4(0.24f, 0.08f, 0.10f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 8.0f));
  ImGui::BeginChild("active-tab-banner", ImVec2(0, S(app, 38.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetCursorPos(S(app, 12.0f, 9.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, app.banner_success ? kSuccess : kText);
  ImGui::TextWrapped("%s", app.banner.c_str());
  ImGui::PopStyleColor();
  ImGui::SameLine(ImGui::GetWindowWidth() - S(app, 42.0f));
  if (ImGui::SmallButton("x")) app.banner.clear();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::Spacing();
}

bool AccentButton(const AppState& app, const char* label, ImVec2 size = ImVec2(0, 0)) {
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(app, 10.0f));
  ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.93f, 0.20f, 0.29f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.73f, 0.10f, 0.18f, 1.0f));
  const bool clicked = ImGui::Button(label, size);
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar();
  return clicked;
}

// ------------------------------------------------------------------------------------------- updater
// The check runs on its own thread and stores its result under app.update.mutex. Nothing here blocks the
// UI and nothing is shown when the check failed (offline, no curl, no newer release).
UpdateInfo CurrentUpdateInfo(AppState& app) {
  std::lock_guard lock(app.update.mutex);
  return app.update.info;
}

bool UpdateOffered(AppState& app) {
  if (!app.settings.check_updates) return false;
  if (app.update_hidden_this_session) return false;
  const UpdateInfo info = CurrentUpdateInfo(app);
  if (!info.available) return false;
  return info.version != app.settings.skipped_update_version;
}

void StartUpdateCheck(AppState& app) {
  {
    std::lock_guard lock(app.update.mutex);
    if (app.update.running || app.update.checked) return;
  }
  if (app.update_thread.joinable()) app.update_thread.join();
  {
    std::lock_guard lock(app.update.mutex);
    app.update.running = true;
  }
  PerfLog("update check: started");
  app.update_thread = std::thread([&app]() {
    UpdateInfo info = CheckLatestUpdate(WWE2K14_LAUNCHER_VERSION, 5.0);
    std::lock_guard lock(app.update.mutex);
    app.update.info = std::move(info);
    app.update.checked = true;
    app.update.running = false;
    PerfLog("update check: available=%d version=%s", app.update.info.available ? 1 : 0,
            app.update.info.version.c_str());
  });
}

void StartUpdate(AppState& app) {
  if (app.job.active.load()) return;
  // Never replace program files while the game is running: it holds the same files open.
  if (GameProcessRunning(app.paths)) {
    PerfLog("update refused: game is running");
    SetBanner(app, "WWE 2K14 is running. Close the game window, then try again.");
    return;
  }
  const UpdateInfo info = CurrentUpdateInfo(app);
  if (!info.available) return;
  PerfLog("update start: version=%s size=%llu", info.version.c_str(),
          static_cast<unsigned long long>(info.size));
  const Paths paths = app.paths;
  const std::string old_version = WWE2K14_LAUNCHER_VERSION;
  app.update_job = true;
  app.update_hidden_this_session = false;
  BeginJob(app, "Getting the update ready…", [&app, paths, info, old_version](const ProgressFn& progress,
                                                                             CancelFlag& cancel) {
    fs::path downloaded;
    const Result download = DownloadUpdate(paths, info, progress, cancel, &downloaded);
    if (!download.ok) {
      PerfLog("update download failed: %s", download.error.c_str());
      if (download.error.empty()) {  // cancelled while downloading
        return JobOutcome{Result::Ok(), [&app] { SetBanner(app, "Update cancelled. Nothing was changed."); }};
      }
      return JobOutcome{download, {}};
    }
    PerfLog("update downloaded: %s", PathUtf8(downloaded).c_str());
    const Result applied = ApplyUpdate(paths, downloaded, info, old_version, progress, cancel);
    if (!applied.ok) {
      PerfLog("update apply failed: %s", applied.error.c_str());
      return JobOutcome{applied, {}};
    }
    PerfLog("update applied: version=%s", info.version.c_str());
    std::error_code error;
    fs::remove(downloaded, error);  // the new files are in place; keep the player's disk tidy
    const std::string version = info.version;
    return JobOutcome{Result::Ok(), [&app, version] {
                        SetBanner(app, "WWE 2K14 Recomp " + version + " installed. The launcher is restarting…",
                                  true);
                        const fs::path executable = LauncherExecutablePath();
                        const Result relaunch = RelaunchLauncher(executable);
                        PerfLog("update relaunch ok=%d path=%s error=%s", relaunch.ok ? 1 : 0,
                                PathUtf8(executable).c_str(), relaunch.error.c_str());
                        if (relaunch.ok) {
                          app.running = false;
                        } else {
                          SetBanner(app, relaunch.error);
                        }
                      }};
  });
}

void PollUpdateCheck(AppState& app) {
  bool checked = false;
  UpdateInfo info;
  {
    std::lock_guard lock(app.update.mutex);
    checked = app.update.checked;
    info = app.update.info;
  }
  if (!checked || !info.available) return;
  if (app.settings.skipped_update_version == info.version) return;
  // Tests only: accept automatically so the whole download/apply/restart flow can run unattended.
  if (const char* value = std::getenv("WWE2K14_UPDATE_AUTO_ACCEPT"); value && std::string_view(value) == "1" &&
      !app.update_auto_attempted && app.initialized && !app.job.active.load()) {
    app.update_auto_attempted = true;
    PerfLog("update auto-accept: starting");
    StartUpdate(app);
  }
}

void DrawUpdateBanner(AppState& app) {
  if (!UpdateOffered(app) || app.update_job) return;
  const UpdateInfo info = CurrentUpdateInfo(app);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.13f, 0.23f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.26f, 0.33f, 0.56f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, S(app, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(app, 16.0f, 10.0f));
  ImGui::BeginChild("update-banner", ImVec2(0.0f, S(app, 104.0f)), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PushFont(app.fonts.barlow_semibold);
  ImGui::TextUnformatted(("Version " + info.version + " is available").c_str());
  ImGui::PopFont();
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
  ImGui::TextWrapped("You have %s. Updating replaces the program files only - your saves, game files, "
                     "settings and music stay exactly where they are.", WWE2K14_LAUNCHER_VERSION);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::Spacing();
  const ImVec2 button_size(S(app, 116.0f), S(app, 30.0f));
  if (ImGui::Button("What's new", button_size)) app.update_notes_open = true;
  ImGui::SameLine();
  if (AccentButton(app, "Update", button_size)) StartUpdate(app);
  ImGui::SameLine();
  if (ImGui::Button("Not now", button_size)) app.update_hidden_this_session = true;
  ImGui::SameLine();
  if (ImGui::Button("Skip this version", ImVec2(S(app, 150.0f), S(app, 30.0f)))) {
    app.settings.skipped_update_version = info.version;
    SaveSettings(app);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
  ImGui::Spacing();
}

// Player-facing release notes: strip markdown heading marks, turn "- item" into a bullet and keep blank
// lines, so a GitHub release body reads like a native update note rather than raw markdown.
std::string FormatReleaseNotes(const std::string& notes) {
  std::string output;
  size_t offset = 0;
  while (offset <= notes.size()) {
    const size_t end = notes.find('\n', offset);
    std::string line = notes.substr(offset, end == std::string::npos ? notes.size() - offset : end - offset);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t first = line.find_first_not_of(" \t");
    const std::string trimmed = first == std::string::npos ? std::string() : line.substr(first);
    if (!trimmed.empty() && trimmed.front() == '#') {
      size_t hashes = 0;
      while (hashes < trimmed.size() && trimmed[hashes] == '#') ++hashes;
      const std::string heading = trimmed.substr(hashes);
      const size_t heading_first = heading.find_first_not_of(" \t");
      output += (heading_first == std::string::npos ? std::string() : heading.substr(heading_first)) + "\n";
    } else if (trimmed.size() >= 2 &&
               (trimmed.front() == '-' || trimmed.front() == '*' || trimmed.front() == '+') &&
               trimmed[1] == ' ') {
      output += "\xE2\x80\xA2  " + trimmed.substr(2) + "\n";
    } else {
      output += line + "\n";
    }
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return output;
}

void DrawUpdateNotes(AppState& app) {
  if (!app.update_notes_open) return;
  const UpdateInfo info = CurrentUpdateInfo(app);
  const float width = S(app, 640.0f);
  ImGui::OpenPopup("update-whats-new");
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, kPanel);
  ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(app, 14.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, S(app, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(app, 26.0f, 22.0f));
  if (ImGui::BeginPopupModal("update-whats-new", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushFont(app.fonts.oswald_semibold);
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::TextUnformatted(("What's new in " + info.version).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kWindow);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 8.0f));
    if (ImGui::BeginChild("update-notes", ImVec2(0.0f, S(app, 320.0f)), true)) {
      ImGui::PushFont(app.fonts.barlow_small);
      ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
      const std::string formatted =
          FormatReleaseNotes(info.notes.empty() ? "No release notes were provided." : info.notes);
      size_t offset = 0;
      while (offset <= formatted.size()) {
        const size_t end = formatted.find('\n', offset);
        const std::string line =
            formatted.substr(offset, end == std::string::npos ? formatted.size() - offset : end - offset);
        if (line.empty()) ImGui::Spacing();
        else ImGui::TextWrapped("%s", line.c_str());
        if (end == std::string::npos) break;
        offset = end + 1;
      }
      ImGui::PopStyleColor();
      ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0.0f, S(app, 14.0f)));
    const float buttons_width = S(app, 120.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons_width);
    if (AccentButton(app, "Close", ImVec2(buttons_width, S(app, 38.0f)))) {
      app.update_notes_open = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
}

void DrawOneDriveWarning(const AppState& app) {
  if (!IsUnderOneDrive(app.paths.exe_dir) && !IsUnderOneDrive(ActiveGameFolder(app))) return;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.23f, 0.15f, 0.07f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.48f, 0.31f, 0.12f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 9.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, S(app, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(app, 14.0f, 9.0f));
  ImGui::BeginChild("onedrive-warning", ImVec2(0.0f, S(app, 78.0f)), true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PushFont(app.fonts.barlow_semibold);
  ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.32f, 1.0f), "OneDrive folder detected");
  ImGui::PopFont();
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
  ImGui::TextWrapped("OneDrive may keep game files online-only. Keep all WWE 2K14/Recomp files "
                     "downloaded and available offline, or move both folders outside OneDrive before playing.");
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::EndChild();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
  ImGui::Spacing();
}

void DrawLaunchWarning(AppState& app) {
  if (app.launch_warning_pending) {
    ImGui::OpenPopup("WWE 2K14 PC Recompiled##startup-warning");
    app.launch_warning_pending = false;
  }
  if (app.launch_warning.empty()) return;
  const float width = S(app, 520.0f);
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("WWE 2K14 PC Recompiled##startup-warning", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::PushFont(app.fonts.barlow_regular);
    ImGui::TextWrapped("%s", app.launch_warning.c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    const float report_width = S(app, 190.0f);
    const float close_width = S(app, 100.0f);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - report_width - close_width -
                          ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    if (AccentButton(app, "Save a Bug Report", ImVec2(report_width, S(app, 38.0f)))) {
      app.launch_warning.clear();
      ImGui::CloseCurrentPopup();
      StartBugReport(app);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(close_width, S(app, 38.0f)))) {
      app.launch_warning.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::End();
  }
}

void DrawPlay(AppState& app) {
  const Settings current = ActiveSettings(app);
  const float width = ImGui::GetContentRegionAvail().x;
  const bool compact = ImGui::GetIO().DisplaySize.y < S(app, 700.0f);
  const float hero_height = S(app, compact ? 236.0f : 330.0f);
  const ImVec2 hero_size(width, hero_height);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
  ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 14.0f));
  ImGui::BeginChild("play-hero", hero_size, true, ImGuiWindowFlags_NoScrollbar);
  ImGui::SetCursorPos(S(app, 28.0f, compact ? 50.0f : 92.0f));
  ImGui::PushFont(app.fonts.oswald_bold);
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  ImGui::TextUnformatted("READY TO PLAY");
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::SetCursorPos(S(app, 30.0f, compact ? 128.0f : 181.0f));
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
  ImGui::TextWrapped("Recommended for this PC: %s · settings picked automatically",
                     app.recommendation.reason.c_str());
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);

  ImGui::Spacing();
  DrawOneDriveWarning(app);
  DrawUpdateBanner(app);
  if (app.update_job) DrawProgress(app);
  const float row_width = ImGui::GetContentRegionAvail().x;
  const float row_gap = ImGui::GetStyle().ItemSpacing.x;
  const float pill_width = std::min(S(app, 150.0f), (row_width - S(app, 124.0f) - 2.0f * row_gap) / 2.0f);
  DrawPill(app, "OUTPUT SIZE", ResolutionName(current.resolution), pill_width);
  ImGui::SameLine();
  DrawPill(app, "DISPLAY", DisplayName(current.display), pill_width);
  ImGui::SameLine();
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(app, 10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, S(app, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Button, kWindow);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRaised);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kPanel);
  ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
  if (ImGui::Button("Change…", S(app, 124.0f, 57.0f))) app.tab = Tab::Settings;
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(2);

  ImGui::Spacing();
  // Measure the controller status first: it sits right-aligned after the checkbox column when
  // there is room and moves to its own line below it otherwise, so the two never overlap.
  const char* const pad_line1 =
      app.gamepad_connected ? "Xbox controller connected" : "No controller connected";
  const char* const pad_line2 = app.gamepad_connected ? "Ready to play" : "Keyboard works too";
  ImGui::PushFont(app.fonts.barlow_small);
  const float pad_width = std::max(ImGui::CalcTextSize(pad_line1).x, ImGui::CalcTextSize(pad_line2).x);
  ImGui::PopFont();
  const float check_width = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                            ImGui::CalcTextSize("Start straight away next time").x;
  const float gap = S(app, 24.0f);
  const float controls_width = ImGui::GetContentRegionAvail().x;
  const float play_width = S(app, 310.0f);
  const bool pad_inline = play_width + gap + check_width + gap + pad_width <= controls_width;
  const float controls_height = pad_inline ? S(app, 103.0f) : S(app, 150.0f);
  ImGui::BeginChild("play-controls", ImVec2(0, controls_height), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::BeginDisabled(!app.game_files.ready() || app.job.active.load());
  ImGui::PushFont(app.fonts.oswald_bold);
  if (AccentButton(app, "PLAY", ImVec2(play_width, S(app, 64.0f)))) StartLaunch(app);
  ImGui::PopFont();
  ImGui::EndDisabled();
  ImGui::SameLine(0.0f, gap);
  const float column_x = ImGui::GetCursorPosX();
  const float column_y = ImGui::GetCursorPosY();
  ImGui::BeginGroup();
  ImGui::SetCursorPosY(column_y + S(app, 10.0f));
  if (ImGui::Checkbox("Start straight away next time", &app.settings.auto_start)) SaveSettings(app);
  DrawTextDisabled(app, "Hold Shift to come back here");
  ImGui::EndGroup();
  if (pad_inline) {
    ImGui::SameLine(std::max(column_x + check_width + gap,
                             ImGui::GetWindowWidth() - pad_width - S(app, 20.0f)));
    ImGui::SetCursorPosY(column_y + S(app, 10.0f));
  } else {
    ImGui::SetCursorPos(ImVec2(column_x, ImGui::GetCursorPosY() + S(app, 6.0f)));
  }
  ImGui::BeginGroup();
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::PushStyleColor(ImGuiCol_Text, app.gamepad_connected ? kSuccess : kSecondary);
  ImGui::TextUnformatted(pad_line1);
  ImGui::PopStyleColor();
  DrawTextDisabled(app, pad_line2);
  ImGui::PopFont();
  ImGui::EndGroup();
  ImGui::EndChild();
}

void DrawSegment(const AppState& app, const char* label, bool selected, float width,
                 const std::function<void()>& action) {
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(app, 7.0f));
  ImGui::PushStyleColor(ImGuiCol_Button, selected ? kAccent : kRaised);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? kAccent : ImVec4(0.16f, 0.17f, 0.20f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
  if (ImGui::Button(label, ImVec2(width, S(app, 34.0f)))) action();
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar();
}

void DrawSettings(AppState& app) {
  const Settings current = ActiveSettings(app);
  const bool compact = ImGui::GetIO().DisplaySize.y < S(app, 700.0f);
  const float recommended_width = S(app, 226.0f);
  ImGui::SetCursorPosX(std::max(0.0f, ImGui::GetWindowWidth() - recommended_width - S(app, 20.0f)));
  ImGui::PushFont(app.fonts.barlow_medium);
  const bool use_recommended = AccentButton(app, "Use Recommended for This PC", ImVec2(recommended_width, S(app, 30.0f)));
  ImGui::PopFont();
  if (use_recommended) {
    const std::string selected_gpu_id = app.settings.gpu_id;
    const fs::path game_folder = app.settings.game_folder;
    const bool auto_start = app.settings.auto_start;
    const bool auto_backup = app.settings.auto_backup;
    const bool check_updates = app.settings.check_updates;
    const bool sync_frames_to_monitor = app.settings.sync_frames_to_monitor;
    const bool stretch_to_fill_wide_screens = app.settings.stretch_to_fill_wide_screens;
    const int picture_scale = app.settings.picture_scale;
    const std::string skipped_update_version = app.settings.skipped_update_version;
    app.settings = app.recommendation.settings;
    app.settings.gpu_id = selected_gpu_id;
    app.settings.game_folder = game_folder;
    app.settings.auto_start = auto_start;
    app.settings.auto_backup = auto_backup;
    app.settings.check_updates = check_updates;
    app.settings.sync_frames_to_monitor = sync_frames_to_monitor;
    app.settings.stretch_to_fill_wide_screens = stretch_to_fill_wide_screens;
    app.settings.picture_scale = picture_scale;
    app.settings.skipped_update_version = skipped_update_version;
    app.settings.explicit_choice = false;
    SaveSettings(app);
  }
  ImGui::Spacing();
  DrawOneDriveWarning(app);
  const float available_width = ImGui::GetContentRegionAvail().x;
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float card_width = (available_width - gap) * 0.5f;
  if (ImGui::BeginTable("settings-grid", 2, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextColumn();
    if (BeginPanel(app, "settings-resolution", ImVec2(card_width, S(app, compact ? 166.0f : 186.0f)))) {
      DrawPanelHeading(app, "OUTPUT SIZE", "The size of the game window or fullscreen picture.");
      ImGui::Spacing();
      const std::array<std::pair<Resolution, const char*>, 3> choices{{
          {Resolution::k720p, "720p"}, {Resolution::k1080p, "1080p"},
          {Resolution::k1440p, "1440p"},
      }};
      const float segment_width =
          (ImGui::GetContentRegionAvail().x - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
      for (size_t i = 0; i < choices.size(); ++i) {
        if (i) ImGui::SameLine();
        DrawSegment(app, choices[i].second, current.resolution == choices[i].first, segment_width, [&app, value = choices[i].first] {
          KeepManualSettings(app);
          app.settings.resolution = value;
          SaveSettings(app);
        });
      }
      DrawTextDisabled(app, "For a sharper picture, also try 'Sharper picture' below.");
    }
    EndPanel();

    ImGui::TableNextColumn();
    if (BeginPanel(app, "settings-display", ImVec2(card_width, S(app, compact ? 166.0f : 186.0f)))) {
      DrawPanelHeading(app, "DISPLAY", "Choose how the game fills your screen.");
      ImGui::Spacing();
      const float segment_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
      DrawSegment(app, "Fullscreen", current.display == DisplayMode::kFullscreen, segment_width, [&app] {
        KeepManualSettings(app);
        app.settings.display = DisplayMode::kFullscreen;
        SaveSettings(app);
      });
      ImGui::SameLine();
      DrawSegment(app, "Windowed", current.display == DisplayMode::kWindowed, segment_width, [&app] {
        KeepManualSettings(app);
        app.settings.display = DisplayMode::kWindowed;
        SaveSettings(app);
      });
      DrawTextDisabled(app, current.display == DisplayMode::kFullscreen ? "Use the full screen" : "Keep the game in a window");
    }
    EndPanel();
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kRaised);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 10.0f));
  ImGui::BeginChild("settings-gpu-row", ImVec2(0, S(app, compact ? 64.0f : 80.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetCursorPos(S(app, 14, 13));
  ImGui::TextUnformatted("GRAPHICS CARD");
  ImGui::SameLine(S(app, 170.0f));
  const std::string selected_gpu = app.settings.gpu_id.empty() ? "Automatic" : CurrentGpuName(app);
  if (ImGui::BeginCombo("##gpu", selected_gpu.c_str(), ImGuiComboFlags_HeightLarge)) {
    if (ImGui::Selectable("Automatic", app.settings.gpu_id.empty())) {
      KeepManualSettings(app);
      app.settings.gpu_id.clear();
      SaveSettings(app);
    }
    for (const auto& gpu : app.gpus) {
      std::string label = gpu.name + (gpu.integrated ? " (integrated)" : "");
      if (ImGui::Selectable(label.c_str(), app.settings.gpu_id == gpu.id)) {
        KeepManualSettings(app);
        app.settings.gpu_id = gpu.id;
        SaveSettings(app);
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // Screen options are personal preferences, so they never switch off Recommended.
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kRaised);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 10.0f));
  ImGui::BeginChild("settings-screen-row", ImVec2(0, S(app, compact ? 44.0f : 52.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetCursorPos(S(app, 14, compact ? 10.0f : 14.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, kWindow);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kPanel);
  ImGui::PushStyleColor(ImGuiCol_Border, kMuted);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, S(app, 1.0f));
  if (ImGui::Checkbox("Stretch to fill wide screens", &app.settings.stretch_to_fill_wide_screens)) {
    SaveSettings(app);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Fill a 21:9 or 16:10 screen instead of showing black bars. The 16:9 picture is stretched.");
  }
#ifdef _WIN32
  // Host-display sync exists only in the Windows runtime; on Linux the game always uses its own timer.
  ImGui::SameLine(0.0f, S(app, 28.0f));
  if (ImGui::Checkbox("Sync frames to the monitor", &app.settings.sync_frames_to_monitor)) SaveSettings(app);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Turn off if the game runs too fast or stutters in fullscreen on a high-refresh, "
                      "G-SYNC or FreeSync monitor.");
  }
#endif
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // Higher internal resolution is experimental and independent from the output-size selector.
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kRaised);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 10.0f));
  ImGui::BeginChild("settings-sharper-picture-row", ImVec2(0, S(app, compact ? 44.0f : 52.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetCursorPos(S(app, 14, compact ? 10.0f : 14.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, kWindow);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kPanel);
  ImGui::PushStyleColor(ImGuiCol_Border, kMuted);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, S(app, 1.0f));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Picture detail (experimental)");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("The game normally draws at 1280x720. 1440p and 4K draw it at 2x or 3x that size for a "
                      "sharper picture on any screen. Needs a stronger graphics card (4K: a high-end one), and "
                      "some effects (for example Batista's entrance pyro) can be missing.");
  }
  {
    const std::array<std::pair<int, const char*>, 3> scales{{{1, "Normal (720p)"}, {2, "1440p"}, {3, "4K"}}};
    const float scale_width = S(app, compact ? 104.0f : 118.0f);
    ImGui::SameLine(0.0f, S(app, 16.0f));
    for (size_t i = 0; i < scales.size(); ++i) {
      if (i) ImGui::SameLine();
      DrawSegment(app, scales[i].second, app.settings.picture_scale == scales[i].first, scale_width,
                  [&app, value = scales[i].first] {
                    app.settings.picture_scale = value;
                    SaveSettings(app);
                  });
    }
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // Updates are a personal preference like auto backup, so they never switch off Recommended.
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kRaised);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 10.0f));
  ImGui::BeginChild("settings-updates-row", ImVec2(0, S(app, compact ? 44.0f : 52.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetCursorPos(S(app, 14, compact ? 10.0f : 14.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, kWindow);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kPanel);
  ImGui::PushStyleColor(ImGuiCol_Border, kMuted);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, S(app, 1.0f));
  if (ImGui::Checkbox("Check for updates when the launcher starts", &app.settings.check_updates)) {
    SaveSettings(app);
    if (app.settings.check_updates) StartUpdateCheck(app);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("The launcher asks GitHub for the newest version. It sends nothing about you or your PC.");
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

std::string FormatDuration(double seconds) {
  if (seconds <= 0.0 || !std::isfinite(seconds)) return "—";
  const int total = static_cast<int>(seconds);
  std::ostringstream output;
  output << total / 60 << ':' << std::setw(2) << std::setfill('0') << total % 60;
  return output.str();
}

void DrawDropZone(AppState& app) {
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  const ImVec2 size(width, S(app, 112.0f));
  ImGui::InvisibleButton("music-drop-zone", size);
  const ImVec2 end = ImGui::GetItemRectMax();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImU32 border = ImGui::ColorConvertFloat4ToU32(kBorder);
  for (float x = start.x + S(app, 8.0f); x < end.x - S(app, 8.0f); x += S(app, 12.0f)) {
    draw->AddLine(ImVec2(x, start.y), ImVec2(std::min(x + S(app, 6.0f), end.x - S(app, 8.0f)), start.y), border, S(app, 1.0f));
    draw->AddLine(ImVec2(x, end.y), ImVec2(std::min(x + S(app, 6.0f), end.x - S(app, 8.0f)), end.y), border, S(app, 1.0f));
  }
  for (float y = start.y + S(app, 8.0f); y < end.y - S(app, 8.0f); y += S(app, 12.0f)) {
    draw->AddLine(ImVec2(start.x, y), ImVec2(start.x, std::min(y + S(app, 6.0f), end.y - S(app, 8.0f))), border,
                  S(app, 1.0f));
    draw->AddLine(ImVec2(end.x, y), ImVec2(end.x, std::min(y + S(app, 6.0f), end.y - S(app, 8.0f))), border,
                  S(app, 1.0f));
  }
  const char* title = "Drop songs here";
  const ImVec2 title_size = ImGui::CalcTextSize(title);
  draw->AddText(ImVec2(start.x + (width - title_size.x) * 0.5f, start.y + S(app, 25.0f)),
                ImGui::ColorConvertFloat4ToU32(kText), title);
  const char* subtitle = "mp3, flac, wav or ogg  ·  or choose files…";
  const ImVec2 subtitle_size = app.fonts.barlow_small->CalcTextSizeA(
      app.fonts.barlow_small->LegacySize, FLT_MAX, 0.0f, subtitle);
  draw->AddText(app.fonts.barlow_small, app.fonts.barlow_small->LegacySize,
                 ImVec2(start.x + (width - subtitle_size.x) * 0.5f, start.y + S(app, 56.0f)),
                ImGui::ColorConvertFloat4ToU32(kMuted), subtitle);
  if (ImGui::IsItemHovered()) {
    draw->AddRect(start, end, ImGui::ColorConvertFloat4ToU32(kAccent), S(app, 8.0f), 0, S(app, 1.5f));
  }
  (void)app;
}

void DrawSongTitle(const AppState& app, const std::string& title) {
  constexpr char kCheckmark[] = "\xE2\x9C\x93";
  const float font_size = ImGui::GetFontSize();
  float x = ImGui::GetCursorScreenPos().x;
  const float y = ImGui::GetCursorScreenPos().y;
  const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  size_t offset = 0;
  size_t check = title.find(kCheckmark);
  if (check == std::string::npos) {
    ImGui::TextUnformatted(title.c_str());
    return;
  }
  const auto draw_text = [&](const char* begin, const char* end, float* cursor_x) {
    if (begin == end) return;
    draw->AddText(ImVec2(*cursor_x, y), color, begin, end);
    *cursor_x += ImGui::GetFont()->CalcTextSizeA(font_size, 100000.0f, 0.0f, begin, end).x;
  };
  while (check != std::string::npos) {
    draw_text(title.c_str() + offset, title.c_str() + check, &x);
    const float thickness = std::max(S(app, 1.4f), font_size * 0.11f);
    draw->AddLine(ImVec2(x + S(app, 1.0f), y + font_size * 0.53f),
                  ImVec2(x + font_size * 0.30f, y + font_size * 0.78f), color, thickness);
    draw->AddLine(ImVec2(x + font_size * 0.30f, y + font_size * 0.78f),
                  ImVec2(x + font_size * 0.82f, y + font_size * 0.22f), color, thickness);
    x += font_size * 0.96f;
    offset = check + sizeof(kCheckmark) - 1;
    check = title.find(kCheckmark, offset);
  }
  draw_text(title.c_str() + offset, title.c_str() + title.size(), &x);
  ImGui::Dummy(ImVec2(x - ImGui::GetCursorScreenPos().x, ImGui::GetTextLineHeight()));
}

void DrawMusic(AppState& app) {
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kSecondary, "Pick these in the entrance editor under User Playlist.");
  ImGui::PopFont();
  ImGui::Spacing();
  DrawDropZone(app);
  if (ImGui::Button("Choose Files…")) {
    static const SDL_DialogFileFilter filters[]{{"Music", "mp3;flac;wav;ogg"}};
    RequestDialog(app, DialogKind::Music, filters, 1, true);
  }
  ImGui::Spacing();
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, S(app, 14.0f, 8.0f));
  if (ImGui::BeginTable("song-table", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("SONG", ImGuiTableColumnFlags_WidthStretch, 0.54f);
    ImGui::TableSetupColumn("LENGTH", ImGuiTableColumnFlags_WidthFixed, S(app, 88.0f));
    ImGui::TableSetupColumn("TYPE", ImGuiTableColumnFlags_WidthFixed, S(app, 90.0f));
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(app, 95.0f));
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < app.songs.size(); ++i) {
      const Song song = app.songs[i];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, S(app, 36.0f));
      ImGui::TableSetColumnIndex(0);
      DrawSongTitle(app, song.title);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(FormatDuration(song.seconds).c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(song.format.c_str());
      ImGui::TableSetColumnIndex(3);
      const std::string id = "Remove##song" + std::to_string(i);
      if (ImGui::SmallButton(id.c_str())) StartRemoveSong(app, song.file);
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  ImGui::Spacing();
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kMuted, "%zu songs · copied into the game's music folder", app.songs.size());
  ImGui::PopFont();
}

const char* FileStateName(FileState state) {
  switch (state) {
    case FileState::kFound: return "Verified";
    case FileState::kMissing: return "Missing";
    case FileState::kWrongVersion: return "Needs an update";
    case FileState::kCorrupt: return "Needs attention";
  }
  return "Check needed";
}

ImVec4 FileStateColor(FileState state) {
  return state == FileState::kFound ? kSuccess : state == FileState::kWrongVersion ? ImVec4(1.0f, 0.68f, 0.25f, 1.0f) : kDanger;
}

void DrawVerifiedIcon(const AppState& app, ImDrawList* draw, const ImVec2& center, float radius = 7.0f) {
  draw->AddCircleFilled(center, S(app, radius), ImGui::ColorConvertFloat4ToU32(kSuccess));
  draw->AddLine(ImVec2(center.x - S(app, 3.0f), center.y), ImVec2(center.x - S(app, 1.0f), center.y + S(app, 2.5f)),
                IM_COL32(8, 9, 11, 255), S(app, 1.5f));
  draw->AddLine(ImVec2(center.x - S(app, 1.0f), center.y + S(app, 2.5f)),
                ImVec2(center.x + S(app, 3.5f), center.y - S(app, 3.0f)), IM_COL32(8, 9, 11, 255),
                S(app, 1.5f));
}

void DrawGameFilesRow(AppState& app, const char* title, const char* detail, const char* status,
                      ImVec4 status_color, bool verified) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 9.0f));
  ImGui::BeginChild(title, ImVec2(0, S(app, 62.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetCursorPos(S(app, 14.0f, 8.0f));
  ImGui::PushFont(app.fonts.barlow_semibold);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  ImGui::SetCursorPos(S(app, 14.0f, 34.0f));
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kMuted, "%s", detail);
  ImGui::PopFont();

  const float status_width = ImGui::CalcTextSize(status).x;
  const float status_x = ImGui::GetWindowWidth() - S(app, 14.0f) - status_width - (verified ? S(app, 22.0f) : 0.0f);
  if (verified) {
    const ImVec2 window_pos = ImGui::GetWindowPos();
    DrawVerifiedIcon(app, ImGui::GetWindowDrawList(),
                     ImVec2(window_pos.x + status_x + S(app, 7.0f), window_pos.y + S(app, 31.0f)));
  }
  ImGui::SetCursorPos(ImVec2(status_x + (verified ? S(app, 22.0f) : 0.0f), S(app, 22.0f)));
  ImGui::TextColored(status_color, "%s", status);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

void DrawProgress(AppState& app) {
  if (!app.job.active.load()) return;
  uint64_t done = 0;
  uint64_t total = 0;
  std::string label;
  std::chrono::steady_clock::time_point started;
  {
    std::lock_guard lock(app.job.mutex);
    done = app.job.done;
    total = app.job.total;
    label = app.job.label;
    started = app.job.started;
  }
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kRaised);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 10.0f));
  ImGui::BeginChild("operation-progress", ImVec2(0, S(app, 94.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetCursorPos(S(app, 14, 10));
  ImGui::TextUnformatted(label.empty() ? "Working…" : label.c_str());
  ImGui::SameLine(ImGui::GetWindowWidth() - S(app, 90.0f));
  if (ImGui::SmallButton("Cancel")) app.job.cancel.store(true);
  ImGui::SetCursorPos(S(app, 14, 39));
  const float fraction = total ? static_cast<float>(std::min(done, total)) / static_cast<float>(total) : -1.0f;
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
  ImGui::ProgressBar(fraction, ImVec2(-1.0f, S(app, 10.0f)), "");
  ImGui::PopStyleColor();
  ImGui::SetCursorPos(S(app, 14, 58));
  if (total) {
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double rate = elapsed > 0.05 ? static_cast<double>(done) / elapsed : 0.0;
    const double eta = rate > 0.0 && total > done ? static_cast<double>(total - done) / rate : 0.0;
    ImGui::PushFont(app.fonts.barlow_small);
    // Totals are bytes for copies/extractions (shown as MB/GB) and item counts otherwise.
    auto size_text = [](uint64_t value) {
      char text[32];
      if (value >= (1ull << 30)) std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(value) / (1ull << 30));
      else std::snprintf(text, sizeof(text), "%.0f MB", static_cast<double>(value) / (1ull << 20));
      return std::string(text);
    };
    const std::string amount = total >= (1ull << 20) ? size_text(done) + " of " + size_text(total)
                                                     : std::to_string(done) + " of " + std::to_string(total);
    const std::string remaining =
        eta > 0.0 ? (eta >= 90.0 ? "about " + std::to_string(static_cast<int>(eta / 60.0 + 0.5)) + " min left"
                                 : "about " + std::to_string(static_cast<int>(eta)) + " sec left")
                  : "finishing up";
    ImGui::TextColored(kMuted, "%s  ·  %s", amount.c_str(), remaining.c_str());
    ImGui::PopFont();
  } else {
    ImGui::PushFont(app.fonts.barlow_small);
    ImGui::TextColored(kMuted, "Please wait while this finishes.");
    ImGui::PopFont();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

void DrawFolderInstallables(AppState& app) {
  const FolderInstallables& found = app.folder_installables;
  const bool offer_disc = app.game_files.base_game != FileState::kFound && !found.disc_images.empty();
  const bool offer_packages = !offer_disc && app.game_files.base_game == FileState::kFound && !found.packages.empty() &&
                              ((found.has_title_update && app.game_files.title_update != FileState::kFound) ||
                               app.game_files.dlc_installed < app.game_files.dlc_known);
  if (!offer_disc && !offer_packages) return;
  const std::string text =
      offer_disc ? "Found " + PathUtf8(found.disc_images.front().filename()) + " in your WWE 2K14 folder."
                 : "Found " + std::to_string(found.packages.size()) +
                       (found.packages.size() == 1 ? " update or DLC file" : " update / DLC files") +
                       " in your WWE 2K14 folder.";
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.20f, 0.14f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 9.0f));
  ImGui::BeginChild("folder-installables", ImVec2(0, S(app, 58.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetCursorPos(S(app, 14.0f, 17.0f));
  ImGui::PushFont(app.fonts.barlow_semibold);
  ImGui::TextColored(kSuccess, "%s", text.c_str());
  ImGui::PopFont();
  const float button_width = S(app, 150.0f);
  ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - button_width - S(app, 12.0f), S(app, 11.0f)));
  if (AccentButton(app, offer_disc ? "Extract It" : "Install", ImVec2(button_width, S(app, 36.0f)))) {
    if (offer_disc) StartExtractDisc(app, found.disc_images.front());
    else StartImportPackages(app, found.packages);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::Spacing();
}

void DrawGameFiles(AppState& app) {
  // While something is being installed, its progress goes at the top (always visible) and the install offers hide.
  const bool busy = app.job.active.load();
  if (busy) {
    DrawProgress(app);
    ImGui::Spacing();
  } else {
    DrawFolderInstallables(app);
  }
  const char* base_hint = app.game_files.base_game == FileState::kFound
                              ? "Base game files are in the selected folder."
                              : "Use Find Automatically or Choose a Folder with the base game.";
  const char* update_hint = app.game_files.title_update == FileState::kFound
                                ? "Title Update 1.0.3 is ready."
                                : "Use Find Automatically or Choose a Folder with Title Update 1.0.3.";
  DrawGameFilesRow(app, "Base game", base_hint, FileStateName(app.game_files.base_game),
                   FileStateColor(app.game_files.base_game), app.game_files.base_game == FileState::kFound);
  ImGui::Spacing();
  DrawGameFilesRow(app, "Title update 1.0.3", update_hint, FileStateName(app.game_files.title_update),
                   FileStateColor(app.game_files.title_update), app.game_files.title_update == FileState::kFound);
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 9.0f));
  ImGui::BeginChild("dlc-status", ImVec2(0, S(app, 62.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  const bool dlc_ready = app.game_files.dlc_installed >= app.game_files.dlc_known;
  const std::string dlc_title = "DLC packs " + std::to_string(app.game_files.dlc_installed) + " of " +
                                std::to_string(app.game_files.dlc_known);
  ImGui::SetCursorPos(S(app, 14.0f, 8.0f));
  ImGui::PushFont(app.fonts.barlow_semibold);
  ImGui::TextUnformatted(dlc_title.c_str());
  ImGui::PopFont();
  ImGui::SetCursorPos(S(app, 14.0f, 34.0f));
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kMuted, "Add your installed packs at any time.");
  ImGui::PopFont();
  const char* dlc_status = dlc_ready ? "Verified" : "Optional";
  const float status_width = ImGui::CalcTextSize(dlc_status).x;
  const float status_x = ImGui::GetWindowWidth() - S(app, 14.0f) - status_width - (dlc_ready ? S(app, 22.0f) : 0.0f);
  if (dlc_ready) {
    const ImVec2 window_pos = ImGui::GetWindowPos();
    DrawVerifiedIcon(app, ImGui::GetWindowDrawList(),
                     ImVec2(window_pos.x + status_x + S(app, 7.0f), window_pos.y + S(app, 31.0f)));
  }
  ImGui::SetCursorPos(ImVec2(status_x + (dlc_ready ? S(app, 22.0f) : 0.0f), S(app, 22.0f)));
  ImGui::TextColored(dlc_ready ? kSuccess : ImVec4(1.0f, 0.68f, 0.25f, 1.0f), "%s", dlc_status);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::Spacing();

  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float tile = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
  if (ImGui::BeginTable("game-actions", 2, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextColumn();
    if (BeginPanel(app, "find-game", ImVec2(tile, S(app, 88.0f)))) {
      if (ImGui::Button("Find Automatically", ImVec2(-1.0f, S(app, 32.0f)))) StartFindGameFiles(app);
      DrawTextDisabled(app, "Look for your game folder");
    }
    EndPanel();
    ImGui::TableNextColumn();
    if (BeginPanel(app, "use-disc", ImVec2(tile, S(app, 88.0f)))) {
      if (ImGui::Button("Use a Disc Image", ImVec2(-1.0f, S(app, 32.0f)))) {
        static const SDL_DialogFileFilter filters[]{{"Disc images", "iso;img"}};
        RequestDialog(app, DialogKind::DiscImage, filters, 1, false);
      }
      DrawTextDisabled(app, "Choose an .iso to extract");
    }
    EndPanel();
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (BeginPanel(app, "add-package", ImVec2(tile, S(app, 88.0f)))) {
      if (ImGui::Button("Add TU or DLC", ImVec2(-1.0f, S(app, 32.0f)))) {
        // Real TU/DLC packages have no usual extension (TU_1A5225K_...0000000000082, "WWE 2K14 - Pack 1 (World) (DLC)"),
        // so show every file; the launcher checks what was picked.
        static const SDL_DialogFileFilter filters[]{{"Updates and packs", "*"}};
        RequestDialog(app, DialogKind::Package, filters, 1, true);
      }
      DrawTextDisabled(app, "Choose or drop an update or pack");
    }
    EndPanel();
    ImGui::TableNextColumn();
    if (BeginPanel(app, "choose-game-folder", ImVec2(tile, S(app, 88.0f)))) {
      if (ImGui::Button("Choose a Folder", ImVec2(-1.0f, S(app, 32.0f)))) {
        RequestDialog(app, DialogKind::GameFolder, nullptr, 0, false);
      }
      DrawTextDisabled(app, "Check and save a game folder");
    }
    EndPanel();
    ImGui::EndTable();
  }

  if (!app.candidates.empty()) {
    ImGui::Spacing();
    // Tall enough for every folder found (one row each), so none is cut off; the page scrolls if needed.
    const float candidates_height =
        S(app, 48.0f) + static_cast<float>(app.candidates.size()) * (ImGui::GetFrameHeightWithSpacing() + S(app, 2.0f));
    if (BeginPanel(app, "found-game-candidates", ImVec2(0, candidates_height))) {
      ImGui::TextUnformatted("FOLDERS FOUND");
      for (size_t i = 0; i < app.candidates.size(); ++i) {
        ImGui::Text("%s", PathUtf8(app.candidates[i].folder).c_str());
        ImGui::SameLine();
        const std::string id = "Use this folder##candidate" + std::to_string(i);
        if (ImGui::SmallButton(id.c_str())) StartChooseGameFolder(app, app.candidates[i].folder);
      }
    }
    EndPanel();
    if (app.scroll_to_candidates) {
      ImGui::SetScrollHereY(1.0f);
      app.scroll_to_candidates = false;
    }
  }
}

std::string FriendlyDate(const std::string& iso) {
  std::tm parsed{};
  std::istringstream input(iso);
  input >> std::get_time(&parsed, "%Y-%m-%dT%H:%M:%S");
  if (input.fail()) return iso;
  parsed.tm_isdst = -1;
  const std::time_t timestamp = std::mktime(&parsed);
  const std::time_t now = std::time(nullptr);
  std::tm now_value{};
  std::tm date_value{};
#ifdef _WIN32
  if (localtime_s(&now_value, &now) != 0 || localtime_s(&date_value, &timestamp) != 0) return iso;
#else
  if (!localtime_r(&now, &now_value) || !localtime_r(&timestamp, &date_value)) return iso;
#endif
  char time_buffer[32]{};
  if (!std::strftime(time_buffer, sizeof(time_buffer), "%I:%M %p", &date_value)) return iso;
  std::string clock = time_buffer;
  if (!clock.empty() && clock.front() == '0') clock.erase(clock.begin());
  if (now_value.tm_year == date_value.tm_year && now_value.tm_yday == date_value.tm_yday) {
    return "Today, " + clock;
  }
  char date_buffer[32]{};
  if (!std::strftime(date_buffer, sizeof(date_buffer), "%b %d", &date_value)) return iso;
  std::string date = date_buffer;
  if (date.size() > 4 && date[4] == '0') date.erase(4, 1);
  return date + ", " + clock;
}

std::string FriendlyBytes(uint64_t bytes) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(1) << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
  return output.str();
}

std::string FileUrl(const fs::path& path) {
  const auto generic = path.generic_u8string();
  const std::string utf8(reinterpret_cast<const char*>(generic.data()), generic.size());
  std::string url = utf8.starts_with('/') ? "file://" : "file:///";
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : utf8) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '/' || c == ':' || c == '-' || c == '_' || c == '.' || c == '~') {
      url.push_back(static_cast<char>(c));
    } else {
      url.push_back('%');
      url.push_back(hex[c >> 4]);
      url.push_back(hex[c & 0x0f]);
    }
  }
  return url;
}

// ------------------------------------------------------------------------------------------------ controls
// SDL key -> ReXGlue key name (launcher2/core/controls.cpp, ReXGlue src/ui/keybinds.cpp). Letters, digits and
// punctuation follow the keyboard layout (key code), like the game's own Windows virtual keys; keypad and
// modifier keys use the physical key (scancode). nullptr = the game has no name for this key.
const char* SdlKeyToGameKey(const SDL_KeyboardEvent& key) {
  static const char* const kLetters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                         "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
  static const char* const kDigits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
  static const char* const kFunction[] = {"F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",  "F8",
                                          "F9",  "F10", "F11", "F12", "F13", "F14", "F15", "F16",
                                          "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24"};
  switch (key.scancode) {
    case SDL_SCANCODE_KP_0: return "Numpad0";
    case SDL_SCANCODE_KP_1: return "Numpad1";
    case SDL_SCANCODE_KP_2: return "Numpad2";
    case SDL_SCANCODE_KP_3: return "Numpad3";
    case SDL_SCANCODE_KP_4: return "Numpad4";
    case SDL_SCANCODE_KP_5: return "Numpad5";
    case SDL_SCANCODE_KP_6: return "Numpad6";
    case SDL_SCANCODE_KP_7: return "Numpad7";
    case SDL_SCANCODE_KP_8: return "Numpad8";
    case SDL_SCANCODE_KP_9: return "Numpad9";
    case SDL_SCANCODE_KP_ENTER: return "NumpadEnter";
    case SDL_SCANCODE_KP_PLUS: return "NumpadPlus";
    case SDL_SCANCODE_KP_MINUS: return "NumpadMinus";
    case SDL_SCANCODE_KP_MULTIPLY: return "NumpadStar";
    case SDL_SCANCODE_KP_DIVIDE: return "NumpadSlash";
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return "Shift";
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return "Control";
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return "Alt";
    default: break;
  }
  const SDL_Keycode code = key.key;
  if (code >= SDLK_A && code <= SDLK_Z) return kLetters[code - SDLK_A];
  if (code >= SDLK_0 && code <= SDLK_9) return kDigits[code - SDLK_0];
  if (code >= SDLK_F1 && code <= SDLK_F12) return kFunction[code - SDLK_F1];
  if (code >= SDLK_F13 && code <= SDLK_F24) return kFunction[12 + (code - SDLK_F13)];
  switch (code) {
    case SDLK_GRAVE: return "Backtick";
    case SDLK_MINUS: return "Minus";
    case SDLK_EQUALS: return "Plus";
    case SDLK_COMMA: return "Comma";
    case SDLK_PERIOD: return "Period";
    case SDLK_SEMICOLON: return "Semicolon";
    case SDLK_SLASH: return "Slash";
    case SDLK_BACKSLASH: return "Backslash";
    case SDLK_LEFTBRACKET: return "LBracket";
    case SDLK_RIGHTBRACKET: return "RBracket";
    case SDLK_APOSTROPHE: return "Quote";
    case SDLK_ESCAPE: return "Escape";
    case SDLK_RETURN: return "Return";
    case SDLK_SPACE: return "Space";
    case SDLK_TAB: return "Tab";
    case SDLK_BACKSPACE: return "Backspace";
    case SDLK_DELETE: return "Delete";
    case SDLK_INSERT: return "Insert";
    case SDLK_HOME: return "Home";
    case SDLK_END: return "End";
    case SDLK_PAGEUP: return "PageUp";
    case SDLK_PAGEDOWN: return "PageDown";
    case SDLK_LEFT: return "Left";
    case SDLK_RIGHT: return "Right";
    case SDLK_UP: return "Up";
    case SDLK_DOWN: return "Down";
    case SDLK_PRINTSCREEN: return "PrintScreen";
    case SDLK_PAUSE: return "Pause";
    case SDLK_CAPSLOCK: return "CapsLock";
    case SDLK_NUMLOCKCLEAR: return "NumLock";
    case SDLK_SCROLLLOCK: return "ScrollLock";
    default: return nullptr;
  }
}

void SaveControls(AppState& app) {
  const std::vector<bool> duplicates = DuplicateKeyBindings(app.controls);
  if (std::find(duplicates.begin(), duplicates.end(), true) != duplicates.end()) {
    app.controls_status = "Two buttons use the same key (shown in red). Change one of them and it saves.";
    app.controls_status_ok = false;
    return;
  }
  const Result saved = SaveKeyboardControls(app.paths, app.controls);
  app.controls_status = saved.ok ? "Saved. The game uses these keys the next time it starts." : saved.error;
  app.controls_status_ok = saved.ok;
}

// key == nullptr && !cancelled: the pressed key has no name in the game - keep waiting for another key.
void FinishKeyCapture(AppState& app, const char* key, bool cancelled) {
  const int index = app.capture_index;
  if (index < 0 || static_cast<size_t>(index) >= app.controls.keys.size()) {
    app.capture_index = -1;
    return;
  }
  if (cancelled) {
    app.capture_index = -1;
    app.controls_status.clear();
    return;
  }
  if (!key || !IsSupportedKeyName(key)) {
    app.controls_status = "The game cannot use that key. Press another key, or Esc to cancel.";
    app.controls_status_ok = false;
    return;
  }
  app.controls.keys[static_cast<size_t>(index)] = key;
  app.controls.modified[static_cast<size_t>(index)] = true;
  app.capture_index = -1;
  SaveControls(app);
}

void DrawControls(AppState& app) {
  const std::vector<KeyBinding>& bindings = KeyBindings();
  if (app.controls.keys.size() != bindings.size()) app.controls = DefaultKeyboardControls();
  const std::vector<bool> duplicates = DuplicateKeyBindings(app.controls);
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextWrapped("Keyboard keys for each controller button. Click a key, then press the new key "
                     "(Esc cancels). A controller always works as well.");
  ImGui::PopStyleColor();
  ImGui::Spacing();
  const size_t rows = (bindings.size() + 1) / 2;
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, S(app, 12.0f, 3.0f));
  if (ImGui::BeginTable("controls-table", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("BUTTON", ImGuiTableColumnFlags_WidthStretch, 0.30f);
    ImGui::TableSetupColumn("KEY", ImGuiTableColumnFlags_WidthStretch, 0.20f);
    ImGui::TableSetupColumn("BUTTON##2", ImGuiTableColumnFlags_WidthStretch, 0.30f);
    ImGui::TableSetupColumn("KEY##2", ImGuiTableColumnFlags_WidthStretch, 0.20f);
    ImGui::TableHeadersRow();
    for (size_t row = 0; row < rows; ++row) {
      ImGui::TableNextRow(ImGuiTableRowFlags_None, S(app, 32.0f));
      for (size_t half = 0; half < 2; ++half) {
        const size_t i = row + half * rows;
        if (i >= bindings.size()) continue;
        ImGui::TableSetColumnIndex(static_cast<int>(half * 2));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(bindings[i].label);
        ImGui::TableSetColumnIndex(static_cast<int>(half * 2 + 1));
        const bool capturing = app.capture_index == static_cast<int>(i);
        const std::string& key = app.controls.keys[i];
        std::string label = capturing ? "Press a key…" : (key.empty() ? "(none)" : key);
        label += "##key" + std::to_string(i);
        // Own colour so the key stands out on both row shades: accent while waiting, red when shared.
        ImGui::PushStyleColor(ImGuiCol_Button, capturing       ? kAccent
                                               : duplicates[i] ? ImVec4(0.46f, 0.16f, 0.19f, 1.0f)
                                                               : ImVec4(0.21f, 0.22f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, capturing       ? kAccent
                                                      : duplicates[i] ? ImVec4(0.58f, 0.20f, 0.24f, 1.0f)
                                                                      : ImVec4(0.27f, 0.28f, 0.33f, 1.0f));
        const int colors = 2;
        if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f))) {
          app.capture_index = capturing ? -1 : static_cast<int>(i);
          if (!capturing) {
            app.controls_status = std::string("Press the new key for ") + bindings[i].label + ". Esc cancels.";
            app.controls_status_ok = true;
          }
        }
        ImGui::PopStyleColor(colors);
      }
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  ImGui::Spacing();
  if (ImGui::Button("Reset to Defaults", S(app, 175.0f, 38.0f))) {
    const KeyboardControls defaults = DefaultKeyboardControls();
    for (size_t i = 0; i < app.controls.keys.size(); ++i) {
      if (app.controls.keys[i] != defaults.keys[i] || app.controls.present[i]) app.controls.modified[i] = true;
      app.controls.keys[i] = defaults.keys[i];
    }
    app.capture_index = -1;
    SaveControls(app);
  }
  if (app.capture_index >= 0) {
    ImGui::SameLine();
    if (ImGui::Button("Use Esc", S(app, 120.0f, 38.0f))) FinishKeyCapture(app, "Escape", false);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", S(app, 120.0f, 38.0f))) FinishKeyCapture(app, nullptr, true);
  }
  if (!app.controls_status.empty()) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(app.controls_status_ok ? kMuted : kDanger, "%s", app.controls_status.c_str());
  }
}

void DrawSaves(AppState& app) {
  if (AccentButton(app, "Back Up Now", S(app, 175.0f, 42.0f))) StartBackup(app);
  ImGui::SameLine();
  if (ImGui::Button("Import a Backup…", S(app, 175.0f, 42.0f))) {
    static const SDL_DialogFileFilter filters[]{{"Save backups", "zip"}};
    RequestDialog(app, DialogKind::Backup, filters, 1, false);
  }
  ImGui::SameLine();
  if (ImGui::Button("Open Save Folder", S(app, 175.0f, 42.0f))) {
    const std::string url = FileUrl(app.paths.userdata_dir);
    if (!SDL_OpenURL(url.c_str())) SetBanner(app, "The save folder could not be opened.");
  }
  ImGui::Spacing();
  if (ImGui::Checkbox("Back up automatically before each launch (keep last 5)", &app.settings.auto_backup)) {
    SaveSettings(app);
  }
  ImGui::Spacing();
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, S(app, 14.0f, 8.0f));
  bool open_restore_popup = false;
  if (ImGui::BeginTable("backup-table", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("BACKUP", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    ImGui::TableSetupColumn("SIZE", ImGuiTableColumnFlags_WidthFixed, S(app, 100.0f));
    ImGui::TableSetupColumn("TYPE", ImGuiTableColumnFlags_WidthFixed, S(app, 100.0f));
    ImGui::TableSetupColumn("RESTORE", ImGuiTableColumnFlags_WidthFixed, S(app, 105.0f));
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < app.backups.size(); ++i) {
      const BackupInfo backup = app.backups[i];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, S(app, 36.0f));
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(FriendlyDate(backup.when).c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(FriendlyBytes(backup.bytes).c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(backup.automatic ? "Automatic" : "Manual");
      ImGui::TableSetColumnIndex(3);
      const std::string id = "Restore##backup" + std::to_string(i);
      if (ImGui::SmallButton(id.c_str())) {
        app.restore_file = backup.file;
        open_restore_popup = true;
      }
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  if (open_restore_popup) ImGui::OpenPopup("Restore this backup?");
  if (ImGui::BeginPopupModal("Restore this backup?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped("Replace your current saves with this backup?\nYour current saves are backed up first.");
    ImGui::Spacing();
    if (AccentButton(app, "Replace", S(app, 120.0f, 38.0f))) {
      ImGui::CloseCurrentPopup();
      StartRestoreBackup(app, app.restore_file);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", S(app, 120.0f, 38.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  DrawProgress(app);
}

void DrawSidebar(AppState& app) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kSidebar);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(app, 19.0f, 23.0f));
  ImGui::BeginChild("launcher-sidebar", ImVec2(S(app, 232.0f), 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PushFont(app.fonts.oswald_bold);
  ImGui::TextUnformatted("WWE");
  ImGui::SameLine(0.0f, S(app, 6.0f));
  ImGui::TextColored(kAccent, "2K14");
  ImGui::PopFont();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(app, 1.0f));
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kMuted, "P C   R E C O M P I L E D");
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, S(app, 34.0f)));
  for (const auto& [tab, label] : kTabs) DrawNavButton(app, tab, label);

  const float status_y = std::max(ImGui::GetCursorPosY() + S(app, 25.0f), ImGui::GetWindowHeight() - S(app, 157.0f));
  ImGui::SetCursorPosY(status_y);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(app, 10.0f));
  ImGui::BeginChild("sidebar-status", ImVec2(-1.0f, S(app, 88.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetCursorPos(S(app, 13.0f, 12.0f));
  const ImVec2 icon = ImGui::GetCursorScreenPos();
  if (app.game_files.ready()) {
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(icon.x + S(app, 6.0f), icon.y + S(app, 7.0f)), S(app, 6.0f),
                                                 ImGui::ColorConvertFloat4ToU32(kSuccess));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(icon.x + S(app, 3.0f), icon.y + S(app, 7.0f)),
                                        ImVec2(icon.x + S(app, 5.0f), icon.y + S(app, 10.0f)), IM_COL32(8, 9, 11, 255), S(app, 1.5f));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(icon.x + S(app, 5.0f), icon.y + S(app, 10.0f)),
                                        ImVec2(icon.x + S(app, 10.0f), icon.y + S(app, 4.0f)), IM_COL32(8, 9, 11, 255), S(app, 1.5f));
    ImGui::SetCursorPosX(S(app, 31.0f));
    ImGui::TextColored(kSuccess, "Game files ready");
    ImGui::SetCursorPos(S(app, 13.0f, 42.0f));
    ImGui::PushFont(app.fonts.barlow_small);
    ImGui::TextColored(kMuted, "TU 1.0.3 · %d DLC packs", app.game_files.dlc_installed);
    ImGui::PopFont();
  } else {
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(icon.x + S(app, 6.0f), icon.y + S(app, 7.0f)), S(app, 6.0f),
                                                 ImGui::ColorConvertFloat4ToU32(kDanger));
    ImGui::SetCursorPosX(S(app, 31.0f));
    ImGui::TextColored(kDanger, "Game files need attention");
    ImGui::SetCursorPos(S(app, 13.0f, 42.0f));
    ImGui::PushFont(app.fonts.barlow_small);
    ImGui::TextColored(kMuted, "Choose or add your game files");
    ImGui::PopFont();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) app.tab = Tab::GameFiles;
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - S(app, 36.0f));
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kMuted, "v%s · build %s", WWE2K14_LAUNCHER_VERSION, WWE2K14_LAUNCHER_BUILD_COMMIT);
  ImGui::PopFont();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

void DrawFooter(AppState& app) {
  const float y = ImGui::GetWindowHeight() - S(app, 50.0f);
  const ImVec2 window_pos = ImGui::GetWindowPos();
  const float right_edge = ImGui::GetIO().DisplaySize.x;
  ImGui::SetCursorPos(ImVec2(S(app, 30.0f), y + S(app, 14.0f)));
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kMuted, "Controls: keyboard or controller");
  ImGui::PopFont();
  const char* report_label = "Save a Bug Report (logs + settings)";
  const char* readme_label = "Read Me";
  const ImGuiStyle& style = ImGui::GetStyle();
  const float button_padding = style.FramePadding.x;
  const float links_width = app.fonts.barlow_small->CalcTextSizeA(
                                app.fonts.barlow_small->LegacySize, 100000.0f, 0.0f, report_label).x +
                            2.0f * button_padding + style.ItemSpacing.x +
                            app.fonts.barlow_small->CalcTextSizeA(
                                app.fonts.barlow_small->LegacySize, 100000.0f, 0.0f, readme_label).x +
                            2.0f * button_padding;
  ImGui::SetCursorScreenPos(ImVec2(right_edge - S(app, 44.0f) - links_width, window_pos.y + y + S(app, 14.0f)));
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
  if (ImGui::SmallButton(report_label)) StartBugReport(app);
  ImGui::SameLine();
  if (ImGui::SmallButton(readme_label)) {
    fs::path readme = app.paths.exe_dir / "README.md";
    std::error_code error;
#if defined(__linux__)
    if (!fs::exists(readme, error)) {
      readme = app.paths.exe_dir.parent_path() / "README.md";
      error.clear();
    }
#endif
    if (fs::exists(readme, error)) {
      const std::string url = FileUrl(readme);
      if (!SDL_OpenURL(url.c_str())) SetBanner(app, "The read me could not be opened.");
    } else {
      SetBanner(app, "The read me is not next to the launcher.");
    }
  }
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void DrawMain(AppState& app) {
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
  ImGui::Begin("WWE 2K14 PC Recompiled", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
  DrawSidebar(app);
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kWindow);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(app, 30.0f, 24.0f));
  ImGui::BeginChild("launcher-content", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PushFont(app.fonts.oswald_semibold);
  static const std::array<const char*, 6> page_titles{{"PLAY", "SETTINGS", "CONTROLS", "MUSIC", "GAME FILES", "SAVES"}};
  ImGui::TextColored(kText, "%s", page_titles[static_cast<size_t>(app.tab)]);
  ImGui::PopFont();
  ImGui::Spacing();
  DrawBanner(app);
  DrawUpdateNotes(app);
  const float footer_space = ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - S(app, 52.0f);
  // Pages scroll when their content is taller than the window (e.g. Game files with a banner and found folders on
  // a small or scaled-down window); the scrollbar only appears when needed.
  ImGui::BeginChild("active-page", ImVec2(0, std::max(footer_space, S(app, 100.0f))), false,
                    ImGuiWindowFlags_NoBackground);
  if (app.tab != Tab::Controls) app.capture_index = -1;  // leaving the page cancels a pending key capture
  switch (app.tab) {
    case Tab::Play: DrawPlay(app); break;
    case Tab::Settings: DrawSettings(app); break;
    case Tab::Controls: DrawControls(app); break;
    case Tab::Music: DrawMusic(app); break;
    case Tab::GameFiles: DrawGameFiles(app); break;
    case Tab::Saves: DrawSaves(app); break;
  }
  ImGui::EndChild();
  DrawFooter(app);
  DrawLaunchWarning(app);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::End();
}

// Fit-to-window: the layout is designed for at least 1024x640 (times the display scale). When the
// window is smaller (e.g. a Snap tile or a small screen at high scaling), lay out on a virtual canvas
// of the design size and scale rendering (SDL render scale in DrawFrame) and mouse input down
// uniformly, so nothing can overlap.
void ApplyFitToWindow(AppState& app) {
  ImGuiIO& io = ImGui::GetIO();
  const float fit = std::min({1.0f, io.DisplaySize.x / S(app, 1024.0f), io.DisplaySize.y / S(app, 640.0f)});
  app.fit_scale = (fit > 0.0f && std::isfinite(fit)) ? fit : 1.0f;
  if (app.fit_scale >= 1.0f) {
    return;
  }
  io.DisplaySize = ImVec2(io.DisplaySize.x / app.fit_scale, io.DisplaySize.y / app.fit_scale);
  // Mouse positions arrive in window coordinates; convert each queued event exactly once.
  ImGuiContext& g = *GImGui;
  for (ImGuiInputEvent& event : g.InputEventsQueue) {
    if (event.EventId <= app.last_fitted_input_event) {
      continue;
    }
    if (event.Type == ImGuiInputEventType_MousePos && event.MousePos.PosX != -FLT_MAX) {
      event.MousePos.PosX /= app.fit_scale;
      event.MousePos.PosY /= app.fit_scale;
    }
  }
  if (!g.InputEventsQueue.empty()) {
    app.last_fitted_input_event = std::max(app.last_fitted_input_event, g.InputEventsQueue.back().EventId);
  }
}

void DrawFrame(AppState& app) {
  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  if (app.screenshot_mode && app.test_pixel_density > 0.0f) {
    ImGuiIO& io = ImGui::GetIO();
    int window_width = 0;
    int window_height = 0;
    SDL_GetWindowSize(app.window, &window_width, &window_height);
    io.DisplaySize = ImVec2(window_width / app.test_pixel_density,
                            window_height / app.test_pixel_density);
    io.DisplayFramebufferScale = ImVec2(app.test_pixel_density, app.test_pixel_density);
  }
  ApplyFitToWindow(app);
  ImGui::NewFrame();
  DrawMain(app);
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  const float footer_y = display_size.y - S(app, 50.0f);
  ImGui::GetForegroundDrawList()->AddRectFilled(
      ImVec2(S(app, 232.0f), footer_y), ImVec2(display_size.x, footer_y + S(app, 1.0f)),
      ImGui::ColorConvertFloat4ToU32(kBorder));
  ImGui::Render();
  SDL_SetRenderDrawColor(app.renderer, 15, 16, 20, 255);
  SDL_RenderClear(app.renderer);
  // Map logical ImGui coordinates to framebuffer pixels, then apply fit-to-window. The renderer
  // backend leaves clip rectangles in logical coordinates when SDL renderer scaling is active.
  const ImVec2 framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale;
  SDL_SetRenderScale(app.renderer, app.fit_scale * framebuffer_scale.x,
                     app.fit_scale * framebuffer_scale.y);
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), app.renderer);
  SDL_SetRenderScale(app.renderer, 1.0f, 1.0f);
}

void SaveScreenshotFrame(AppState& app, const std::string& name) {
  DrawFrame(app);
  SDL_Surface* pixels = SDL_RenderReadPixels(app.renderer, nullptr);
  if (!pixels) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not read the launcher window for its screenshot: %s", SDL_GetError());
    app.running = false;
    return;
  }
  const fs::path file = app.screenshot_dir / name;
  const bool saved = SDL_SavePNG(pixels, PathUtf8(file).c_str());
  SDL_DestroySurface(pixels);
  if (!saved) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not save launcher screenshot %s: %s",
                 PathUtf8(file).c_str(), SDL_GetError());
    app.running = false;
  }
  SDL_RenderPresent(app.renderer);
}

void CaptureTab(AppState& app, Tab tab) {
  static const std::array<const char*, 6> names{{"play", "settings", "controls", "music", "game-files", "saves"}};
  app.tab = tab;
  SaveScreenshotFrame(app, std::string(names[static_cast<size_t>(tab)]) + ".png");
}

// Test-only: capture the four updater states (available banner, release notes, download progress, done).
// Update info comes from WWE2K14_TEST_UPDATE_JSON (a fixture file) or from the local test server named by
// WWE2K14_UPDATE_API_URL. Returns false once every state has been captured.
bool CaptureUpdateState(AppState& app) {
  if (!app.update.checked) {
    if (app.update_thread.joinable()) app.update_thread.join();
    UpdateInfo info;
    if (const char* fixture = std::getenv("WWE2K14_TEST_UPDATE_JSON"); fixture && *fixture) {
      std::ifstream input(Utf8Path(fixture), std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
      if (auto parsed = ParseLatestReleaseJson(text, WWE2K14_LAUNCHER_VERSION)) info = *parsed;
    } else {
      info = CheckLatestUpdate(WWE2K14_LAUNCHER_VERSION, 10.0);
    }
    std::lock_guard lock(app.update.mutex);
    app.update.info = std::move(info);
    app.update.checked = true;
  }
  {
    std::lock_guard lock(app.update.mutex);
    if (!app.update.info.available) {  // a fixture without a matching version still captures the UI
      app.update.info.available = true;
      if (app.update.info.version.empty()) app.update.info.version = "1.1.0";
      if (app.update.info.notes.empty()) {
        app.update.info.notes = "- Improved stability.\n- Smaller download.\n- Updates now install in place.";
      }
    }
  }
  app.tab = Tab::Play;
  switch (app.update_screenshot_index++) {
    case 0:
      SaveScreenshotFrame(app, "update-available.png");
      return true;
    case 1:
      app.update_notes_open = true;
      DrawFrame(app);  // an auto-sized popup stays hidden on its first frame
      SDL_RenderPresent(app.renderer);
      SaveScreenshotFrame(app, "update-whats-new.png");
      app.update_notes_open = false;
      return true;
    case 2: {
      app.update_job = true;
      app.job.active.store(true);
      {
        std::lock_guard lock(app.job.mutex);
        app.job.done = 68ull * 1024u * 1024u;
        app.job.total = 128ull * 1024u * 1024u;
        app.job.label = "Downloading update…";
        app.job.started = std::chrono::steady_clock::now() - std::chrono::seconds(3);
      }
      SaveScreenshotFrame(app, "update-progress.png");
      app.job.active.store(false);
      app.update_job = false;
      return true;
    }
    case 3:
      app.update_hidden_this_session = true;
      SetBanner(app, "WWE 2K14 Recomp 1.1.0 installed. The launcher is restarting…", true);
      SaveScreenshotFrame(app, "update-done.png");
      return true;
    case 4:
      app.tab = Tab::Settings;
      SaveScreenshotFrame(app, "update-settings.png");
      return true;
    default:
      return false;
  }
}

void LoadFonts(AppState& app) {
  ImGuiIO& io = ImGui::GetIO();
  const float scale = ui::ComputeUiScale(SDL_GetWindowDisplayScale(app.window),
                                         GetWindowPixelDensity(app));
  auto add = [scale](const unsigned char* data, unsigned int size, float pixels) {
    ImFont* font = ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(data, static_cast<int>(size), pixels * scale);
    return font ? font : ImGui::GetIO().Fonts->AddFontDefault();
  };
  app.fonts.oswald_medium = add(OswaldMedium_compressed_data, OswaldMedium_compressed_size, 21.0f);
  app.fonts.oswald_semibold = add(OswaldSemiBold_compressed_data, OswaldSemiBold_compressed_size, 27.0f);
  app.fonts.oswald_bold = add(OswaldBold_compressed_data, OswaldBold_compressed_size, 54.0f);
  app.fonts.barlow_regular = add(BarlowRegular_compressed_data, BarlowRegular_compressed_size, 16.0f);
  app.fonts.barlow_small = add(BarlowRegular_compressed_data, BarlowRegular_compressed_size, 13.0f);
  app.fonts.barlow_medium = add(BarlowMedium_compressed_data, BarlowMedium_compressed_size, 16.0f);
  app.fonts.barlow_semibold = add(BarlowSemiBold_compressed_data, BarlowSemiBold_compressed_size, 16.0f);
  static const ImWchar kNotoTextRanges[] = {
      0x0020, 0x024F, 0x0370, 0x052F, 0x1E00, 0x1EFF, 0};
  static const ImWchar kNotoSymbolRanges[] = {
      0x2000, 0x27FF, 0x2900, 0x2BFF, 0};
  const auto merge_fallbacks = [&](ImFont* destination, float pixels) {
    ImFontConfig text_config{};
    text_config.MergeMode = true;
    text_config.DstFont = destination;
    io.Fonts->AddFontFromMemoryCompressedTTF(
        NotoSans_compressed_data, static_cast<int>(NotoSans_compressed_size), pixels * scale,
        &text_config, kNotoTextRanges);
    ImFontConfig symbol_config{};
    symbol_config.MergeMode = true;
    symbol_config.DstFont = destination;
    io.Fonts->AddFontFromMemoryCompressedTTF(
        NotoSansSymbols2_compressed_data, static_cast<int>(NotoSansSymbols2_compressed_size),
        pixels * scale, &symbol_config, kNotoSymbolRanges);
    destination->FallbackChar = 0x25A1;
  };
  merge_fallbacks(app.fonts.barlow_regular, 16.0f);
  merge_fallbacks(app.fonts.barlow_small, 13.0f);
  merge_fallbacks(app.fonts.barlow_medium, 16.0f);
  merge_fallbacks(app.fonts.barlow_semibold, 16.0f);
  io.FontDefault = app.fonts.barlow_regular;
  ImGui::GetStyle().ScaleAllSizes(scale / app.display_scale);
  app.display_scale = scale;
}

bool Initialize(AppState& app, int argc, char** argv) {
  if (const char* shift = std::getenv("WWE2K14_TEST_SHIFT_HELD")) {
    const std::string_view value(shift);
    app.test_shift_held = value == "1" || value == "true" || value == "TRUE";
  }
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--settings") app.bypass_auto_start = true;
    if (argument == "--test-shift-held") app.test_shift_held = true;
    if (argument == "--test-window-size" && i + 1 < argc) {
      const std::string dimensions = argv[++i];
      const size_t separator = dimensions.find('x');
      if (separator != std::string::npos) {
        const int width = std::atoi(dimensions.substr(0, separator).c_str());
        const int height = std::atoi(dimensions.substr(separator + 1).c_str());
        if (width >= 1024 && height >= 640) {
          app.startup_width = width;
          app.startup_height = height;
          app.test_window_size = true;
        }
      }
    }
    if (argument == "--test-pixel-density" && i + 1 < argc) {
      const float density = std::strtof(argv[++i], nullptr);
      if (density >= 0.5f && density <= 4.0f && std::isfinite(density)) {
        app.test_pixel_density = density;
      }
    }
    if (argument == "--test-drop-tab" && i + 1 < argc) {
      const std::string tab = argv[++i];
      if (tab == "music") app.test_drop_tab = Tab::Music;
      else if (tab == "game-files") app.test_drop_tab = Tab::GameFiles;
    }
    if (argument == "--test-drop-file" && i + 1 < argc) {
      app.test_drop_files.push_back(Utf8Path(argv[++i]));
    }
    if (argument == "--screenshot-update") {
      app.screenshot_mode = true;
      app.update_screenshot_mode = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') app.screenshot_dir = Utf8Path(argv[++i]);
      else app.screenshot_dir = fs::path("out/l1/ui");
    }
    if (argument == "--screenshot-tabs") {
      app.screenshot_mode = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') app.screenshot_dir = Utf8Path(argv[++i]);
      else app.screenshot_dir = fs::path("out/l1/ui");
    }
  }
  if (app.screenshot_mode) {
    std::error_code error;
    fs::create_directories(app.screenshot_dir, error);
    if (error) {
      SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not create screenshot folder: %s", error.message().c_str());
      return false;
    }
  }
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not open the launcher window: %s", SDL_GetError());
    return false;
  }
  app.window = SDL_CreateWindow("WWE 2K14 PC Recompiled v" WWE2K14_LAUNCHER_VERSION, app.startup_width, app.startup_height,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!app.window) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not open the launcher window: %s", SDL_GetError());
    return false;
  }
  SDL_PumpEvents();
  app.shift_at_start = app.test_shift_held || wwe2k14::launcher::ui::ShiftDownAtLaunch(app.window);
  app.renderer = SDL_CreateRenderer(app.window, nullptr);
  if (!app.renderer) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not draw the launcher window: %s", SDL_GetError());
    return false;
  }
  SDL_SetRenderVSync(app.renderer, 1);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.ConfigNavCaptureKeyboard = true;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.Colors[ImGuiCol_WindowBg] = kWindow;
  style.Colors[ImGuiCol_ChildBg] = kWindow;
  style.Colors[ImGuiCol_Text] = kText;
  style.Colors[ImGuiCol_TextDisabled] = kMuted;
  style.Colors[ImGuiCol_Border] = kBorder;
  style.Colors[ImGuiCol_FrameBg] = kRaised;
  style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.16f, 0.19f, 1.0f);
  style.Colors[ImGuiCol_FrameBgActive] = kRaised;
  style.Colors[ImGuiCol_Header] = kRaised;
  style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.15f, 0.16f, 0.19f, 1.0f);
  style.Colors[ImGuiCol_HeaderActive] = kAccent;
  style.Colors[ImGuiCol_CheckMark] = kAccent;
  style.Colors[ImGuiCol_SliderGrab] = kAccent;
  style.Colors[ImGuiCol_Button] = kRaised;
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.15f, 0.16f, 0.19f, 1.0f);
  style.Colors[ImGuiCol_ButtonActive] = kAccent;
  style.Colors[ImGuiCol_PlotHistogram] = kAccent;
  style.WindowRounding = 0.0f;
  style.ChildRounding = 14.0f;
  style.FrameRounding = 8.0f;
  style.PopupRounding = 8.0f;
  style.ScrollbarRounding = 8.0f;
  style.WindowBorderSize = 0.0f;
  style.FrameBorderSize = 0.0f;
  style.WindowPadding = ImVec2(0.0f, 0.0f);
  style.ItemSpacing = ImVec2(12.0f, 10.0f);
  LoadFonts(app);
  SetMinimumWindowSize(app);
  if (!app.test_window_size) {
    int width = static_cast<int>(std::lround(S(app, static_cast<float>(app.startup_width))));
    int height = static_cast<int>(std::lround(S(app, static_cast<float>(app.startup_height))));
    const SDL_Rect usable = UsableWindowArea(app);
    if (usable.w > 0 && usable.h > 0 && (width > usable.w || height > usable.h)) {
      // Shrink to fit, keeping the design aspect ratio (at 200% scaling the window
      // was taller than the screen and hung off the bottom right).
      const float fit = std::min(static_cast<float>(usable.w) / width, static_cast<float>(usable.h) / height);
      width = static_cast<int>(width * fit);
      height = static_cast<int>(height * fit);
    }
    SDL_SetWindowSize(app.window, width, height);
    SDL_SetWindowPosition(app.window, SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetDisplayForWindow(app.window)),
                          SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetDisplayForWindow(app.window)));
  }
  if (!ImGui_ImplSDL3_InitForSDLRenderer(app.window, app.renderer)) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not initialize the launcher controls: %s", SDL_GetError());
    return false;
  }
  app.imgui_platform_ready = true;
  if (!ImGui_ImplSDLRenderer3_Init(app.renderer)) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not initialize the launcher controls: %s", SDL_GetError());
    return false;
  }
  app.imgui_renderer_ready = true;
  app.gamepad_connected = SDL_HasGamepad();
  StartInitialization(app);
  return true;
}

void HandleEvent(AppState& app, const SDL_Event& event) {
  // While a Controls row waits for a key, keyboard (and right/middle mouse) input belongs to it, not to the UI.
  if (app.capture_index >= 0 && app.tab == Tab::Controls) {
    if (event.type == SDL_EVENT_KEY_DOWN) {
      if (!event.key.repeat) FinishKeyCapture(app, event.key.key == SDLK_ESCAPE ? nullptr : SdlKeyToGameKey(event.key), event.key.key == SDLK_ESCAPE);
      return;
    }
    if (event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_TEXT_INPUT) return;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE)) {
      FinishKeyCapture(app, event.button.button == SDL_BUTTON_RIGHT ? "RMB" : "MMB", false);
      return;
    }
  }
  ImGui_ImplSDL3_ProcessEvent(&event);
  switch (event.type) {
    case SDL_EVENT_QUIT:
      app.running = false;
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (event.window.windowID == SDL_GetWindowID(app.window)) app.running = false;
      break;
    case SDL_EVENT_DROP_FILE:
      if (app.tab == Tab::Music || app.tab == Tab::GameFiles) QueueDrop(app, event.drop.data);
      break;
    case SDL_EVENT_KEY_DOWN:
      if (event.key.key == SDLK_RETURN && !event.key.repeat && app.initialized && app.tab == Tab::Play) {
        StartLaunch(app);
      }
      break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      if (ImGui::GetCurrentContext()) {
        ImGui_ImplSDLRenderer3_DestroyDeviceObjects();
        ImGui::GetIO().Fonts->Clear();
        LoadFonts(app);
        SetMinimumWindowSize(app);
      }
      break;
    case SDL_EVENT_GAMEPAD_ADDED:
      app.gamepad_connected = true;
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      app.gamepad_connected = SDL_HasGamepad();
      break;
    default:
      break;
  }
}

void Shutdown(AppState& app) {
  app.job.cancel.store(true);
  if (app.job.thread.joinable()) app.job.thread.join();
  if (app.update_thread.joinable()) app.update_thread.join();
  if (app.imgui_renderer_ready) ImGui_ImplSDLRenderer3_Shutdown();
  if (app.imgui_platform_ready) ImGui_ImplSDL3_Shutdown();
  if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  if (app.renderer) SDL_DestroyRenderer(app.renderer);
  if (app.window) SDL_DestroyWindow(app.window);
  SDL_Quit();
}

}  // namespace

int main(int argc, char** argv) {
  AppState app;
  if (!Initialize(app, argc, argv)) {
    Shutdown(app);
    return 1;
  }
  // Vsync paces active frames. After a few frames without input (and with nothing running),
  // block until the next event instead of polling, so the UI reacts immediately but idles cheaply.
  {
    std::error_code error;
    // app.paths is filled by the background initialization job; resolve the logs folder directly.
    const fs::path logs_dir = ResolvePaths(LauncherExecutablePath()).logs_dir;
    fs::create_directories(logs_dir, error);
#ifdef _WIN32
    g_perf_log = _wfopen((logs_dir / "launcher.log").c_str(), L"w");
#else
    g_perf_log = std::fopen((logs_dir / "launcher.log").c_str(), "w");
#endif
    int vsync = 0;
    SDL_GetRenderVSync(app.renderer, &vsync);
    int width = 0;
    int height = 0;
    int logical_width = 0;
    int logical_height = 0;
    SDL_GetWindowSizeInPixels(app.window, &width, &height);
    SDL_GetWindowSize(app.window, &logical_width, &logical_height);
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(app.window));
    PerfLog("renderer=%s vsync=%d window_pixels=%dx%d window_coords=%dx%d pixel_density=%.2f "
            "display_scale=%.2f ui_scale=%.2f refresh=%.1f Hz video_driver=%s",
            SDL_GetRendererName(app.renderer), vsync, width, height, logical_width, logical_height,
            GetWindowPixelDensity(app), SDL_GetWindowDisplayScale(app.window), app.display_scale,
            mode ? mode->refresh_rate : 0.0f, SDL_GetCurrentVideoDriver());
  }
  int quiet_frames = 0;
  // WWE2K14_LAUNCHER_FRAME_LOG=1: report every frame that takes > 20 ms, split into its stages, so a
  // "clicking is slow" report names the stage instead of guessing.
  const bool frame_log = std::getenv("WWE2K14_LAUNCHER_FRAME_LOG") != nullptr;
  while (app.running) {
    const Uint64 frame_start = SDL_GetTicksNS();
    Uint64 events_ns = 0;
    Uint64 work_ns = 0;
    Uint64 draw_ns = 0;
    SDL_Event event;
    const bool busy = !app.initialized || app.job.active.load() || app.screenshot_mode ||
                      !app.test_drop_files.empty();
    if (!busy && quiet_frames > 3 && SDL_WaitEventTimeout(&event, 100)) {
      HandleEvent(app, event);
      quiet_frames = 0;
    }
    const Uint64 poll_start = SDL_GetTicksNS();
    while (SDL_PollEvent(&event)) {
      HandleEvent(app, event);
      quiet_frames = 0;
    }
    events_ns = SDL_GetTicksNS() - poll_start;
    const Uint64 work_start = SDL_GetTicksNS();
    ++quiet_frames;
    PollJob(app);
    PollUpdateCheck(app);
    ConsumeDialog(app);
    if (!app.test_drop_files.empty() && app.initialized && !app.job.active.load()) {
      app.tab = app.test_drop_tab;
      for (const fs::path& path : app.test_drop_files) {
        const std::string data = PathUtf8(path);
        SDL_Event drop{};
        drop.type = SDL_EVENT_DROP_FILE;
        drop.drop.data = data.c_str();
        HandleEvent(app, drop);
      }
      app.test_drop_files.clear();
    }
    ProcessDrops(app);
    if (app.screenshot_mode && app.initialized && !app.job.active.load()) {
      if (app.update_screenshot_mode) {
        if (!CaptureUpdateState(app)) {
          app.running = false;
          break;
        }
        continue;
      }
      if (app.screenshot_index >= kTabs.size()) {
        app.running = false;
        break;
      }
      const Tab tab = kTabs[app.screenshot_index].first;
      CaptureTab(app, tab);
      ++app.screenshot_index;
      continue;
    }
    work_ns = SDL_GetTicksNS() - work_start;
    const Uint64 draw_start = SDL_GetTicksNS();
    DrawFrame(app);
    draw_ns = SDL_GetTicksNS() - draw_start;
    const Uint64 present_start = SDL_GetTicksNS();
    SDL_RenderPresent(app.renderer);
    const Uint64 present_ns = SDL_GetTicksNS() - present_start;
    // Exclude the idle wait (frame_start..poll_start) - only time spent working counts.
    const Uint64 active_ns = events_ns + work_ns + draw_ns + present_ns;
    if (active_ns > 40000000) {
      PerfLog("slow frame %.1f ms: events+handlers %.1f, jobs %.1f, draw %.1f, present %.1f (tab %d)",
              active_ns / 1e6, events_ns / 1e6, work_ns / 1e6, draw_ns / 1e6, present_ns / 1e6,
              static_cast<int>(app.tab));
    }
    if (frame_log && active_ns > 20000000) {
      SDL_Log("[frame] slow %.1f ms: events+handlers %.1f, jobs %.1f, draw %.1f, present %.1f (tab %d)",
              active_ns / 1e6, events_ns / 1e6, work_ns / 1e6, draw_ns / 1e6, present_ns / 1e6,
              static_cast<int>(app.tab));
    }
    (void)frame_start;
  }
  Shutdown(app);
  if (g_perf_log) std::fclose(g_perf_log);
  return 0;
}
