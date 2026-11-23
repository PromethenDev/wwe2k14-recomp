#include "launcher_core.h"
// v1.7.0: the new-renderer Settings layout (Vulkan / DirectX 12 / DirectX 11 / Xbox) is the Windows UI; screenshot
// builds on Linux can force it with -DWWE2K14_NEW_RENDERER_UI=1.
#ifndef WWE2K14_NEW_RENDERER_UI
#define WWE2K14_NEW_RENDERER_UI 1  // v1.7.1: Linux too (Vulkan + Xbox choices there)
#endif
#include "grid_face_repair.h"
#include "launcher_version.h"
#include "native_keys.h"
#include "display_scale.h"
#include "update.h"
#include "wwe13_install.h"
#include "mods_switch.h"
#include "wwe13_arena_install.h"
#include "wwe15_install.h"
#include "internal/paint_editor.h"
#include "internal/paint_tool.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "launcher_embedded_fonts.h"
#include "launcher_embedded_images.h"

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
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <exception>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <set>
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

// Card LOG1 (owner 2026-10-31): launcher.log records what the player clicked, ticked and picked, so a bug report
// shows the steps before a failure. Labels only; the "##id" part is dropped unless it is all there is.
void PerfLog(const char* format, ...);
void LogClick(const char* kind, const char* label) {
  std::string_view text(label ? label : "");
  const size_t hidden = text.find("##");
  std::string_view shown = text.substr(0, hidden);
  if (shown.empty() && hidden != std::string_view::npos) {
    shown = text.substr(hidden);
    shown.remove_prefix(shown.find_first_not_of('#') == std::string_view::npos ? shown.size()
                                                                                : shown.find_first_not_of('#'));
  }
  PerfLog("ui %s: %.*s", kind, static_cast<int>(shown.size()), shown.data());
}
bool LoggedButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
  const bool hit = ImGui::Button(label, size);
  if (hit) LogClick("button", label);
  return hit;
}
bool LoggedSmallButton(const char* label) {
  const bool hit = ImGui::SmallButton(label);
  if (hit) LogClick("button", label);
  return hit;
}
bool LoggedCheckbox(const char* label, bool* value) {
  const bool changed = ImGui::Checkbox(label, value);
  if (changed) LogClick(*value ? "ticked" : "unticked", label);
  return changed;
}
bool LoggedSelectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0,
                      const ImVec2& size = ImVec2(0, 0)) {
  const bool hit = ImGui::Selectable(label, selected, flags, size);
  if (hit) LogClick("picked", label);
  return hit;
}

using namespace wwe2k14::launcher;

// Main Event theme: all palette values and procedural artwork live here.
constexpr ImVec4 Rgba(int r, int g, int b, float a = 1.0f) {
  return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
constexpr ImVec4 kWindow = Rgba(10, 10, 11);
constexpr ImVec4 kSidebar = Rgba(10, 10, 11, 0.94f);
constexpr ImVec4 kPanel = Rgba(23, 23, 26, 0.94f);
constexpr ImVec4 kRaised = Rgba(32, 32, 36);
constexpr ImVec4 kHover = Rgba(46, 46, 52);
constexpr ImVec4 kBorder = Rgba(46, 46, 52);
constexpr ImVec4 kText = Rgba(242, 239, 232);
constexpr ImVec4 kSecondary = Rgba(201, 194, 180);
constexpr ImVec4 kMuted = Rgba(169, 161, 148);  // mockup #9A9387, lifted for small-text contrast
constexpr ImVec4 kAccent = Rgba(224, 58, 62);
constexpr ImVec4 kSuccess = Rgba(95, 203, 133);
constexpr ImVec4 kDanger = Rgba(255, 138, 143);

namespace MainEventTheme {
// A triangle fan with transparent outer vertices creates a soft elliptical gradient,
// without image assets, textures, or per-frame rasterization.
void Ellipse(ImDrawList* draw, ImVec2 center, ImVec2 radius, ImVec4 color, float angle = 0.0f) {
  constexpr int segments = 96;
  const float c = std::cos(angle), s = std::sin(angle);
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  draw->PrimReserve(segments * 3, segments + 1);
  const ImDrawIdx base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
  draw->PrimWriteVtx(center, uv, ImGui::ColorConvertFloat4ToU32(color));
  color.w = 0.0f;
  for (int i = 0; i < segments; ++i) {
    const float t = i * 6.2831853f / segments;
    const float x = std::cos(t) * radius.x, y = std::sin(t) * radius.y;
    draw->PrimWriteVtx(ImVec2(center.x + x*c - y*s, center.y + x*s + y*c), uv,
                       ImGui::ColorConvertFloat4ToU32(color));
  }
  for (int i = 0; i < segments; ++i) {
    draw->PrimWriteIdx(base);
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (i + 1) % segments));
  }
}
void Background(ImVec2 size) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  const float x = size.x / 1280.0f, y = size.y / 760.0f;
  d->AddRectFilled(ImVec2(0,0), size, IM_COL32(10,10,11,255));
  Ellipse(d, ImVec2(1160*x,120*y), ImVec2(560*x,560*y), Rgba(200,30,40,0.26f));
}
// Home-screen tile: a real ImGui button (keyboard/controller focus, activation, disabled state) drawn as a flat
// tile; the hero tile has a red gradient, an accent frame and a cut corner (Titantron mockup B).
bool Tile(const char* id, ImVec2 size, bool hero) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0,0,0,0));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0,0,0,0));
  const bool hit = LoggedButton(id, size);
  ImGui::PopStyleColor(3);
  const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
  const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
  ImDrawList* d = ImGui::GetWindowDrawList();
  const ImVec2 end(pos.x + size.x, pos.y + size.y);
  if (hero) {
    const float cut = size.y * 0.07f;
    ImVec2 points[] = {{pos.x,pos.y}, {end.x,pos.y}, {end.x,end.y-cut}, {end.x-cut,end.y}, {pos.x,end.y}};
    const ImU32 top = ImGui::GetColorU32(disabled ? Rgba(70,40,42) : hovered ? Rgba(170,30,40) : Rgba(142,22,32));
    const ImU32 bottom = ImGui::GetColorU32(disabled ? Rgba(36,26,27) : Rgba(58,10,15));
    const int start = d->VtxBuffer.Size;
    d->AddConvexPolyFilled(points, 5, top);
    for (int i = start; i < d->VtxBuffer.Size; ++i) {
      auto& v = d->VtxBuffer[i];
      const float t = std::clamp((v.pos.y - pos.y) / size.y * 1.3f, 0.0f, 1.0f);
      ImVec4 a = ImGui::ColorConvertU32ToFloat4(top), b = ImGui::ColorConvertU32ToFloat4(bottom);
      v.col = ImGui::GetColorU32(ImVec4(a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, 1.0f));
    }
    d->AddPolyline(points, 5, ImGui::GetColorU32(disabled ? kBorder : focused ? kText : kAccent),
                   ImDrawFlags_Closed, 3.0f);
  } else {
    d->AddRectFilled(pos, end, ImGui::GetColorU32(hovered ? kHover : Rgba(23,23,26)));
    if (focused) d->AddRect(pos, end, ImGui::GetColorU32(kText), 0.0f, 0, 2.0f);
  }
  return hit;
}
// Keep a real ImGui button for keyboard/controller focus, activation and disabled semantics.
bool SlantedButton(const char* id, const char* text, ImVec2 size, bool filled, bool gradient = false) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0,0,0,0));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0,0,0,0));
  const bool hit = LoggedButton(id, size);
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* d = ImGui::GetWindowDrawList();
  const float skew = size.y * 0.22f;
  if (filled || hovered) {
    ImVec2 points[] = {{pos.x+skew,pos.y}, {pos.x+size.x,pos.y},
                      {pos.x+size.x-skew,pos.y+size.y}, {pos.x,pos.y+size.y}};
    const int start = d->VtxBuffer.Size;
    d->AddConvexPolyFilled(points, 4, ImGui::GetColorU32(filled ? kAccent : kRaised));
    if (gradient) {
      for (int i=start; i<d->VtxBuffer.Size; ++i) {
        auto& v = d->VtxBuffer[i];
        const float t = (v.pos.y-pos.y)/size.y;
        const ImVec4 top = Rgba(240,72,79), bottom = Rgba(142,18,25);
        ImVec4 col(top.x+(bottom.x-top.x)*t, top.y+(bottom.y-top.y)*t, top.z+(bottom.z-top.z)*t, 1);
        const ImU32 alpha = v.col & IM_COL32_A_MASK;
        v.col = (ImGui::GetColorU32(col) & ~IM_COL32_A_MASK) | alpha;
      }
    }
  }
  const ImVec2 ts = ImGui::CalcTextSize(text);
  d->AddText(ImVec2(pos.x+(size.x-ts.x)*0.5f,pos.y+(size.y-ts.y)*0.5f),
             ImGui::GetColorU32(filled ? kText : kSecondary), text);
  if (ImGui::IsItemFocused()) d->AddRect(pos, ImVec2(pos.x+size.x,pos.y+size.y), ImGui::GetColorU32(kText));
  ImGui::PopStyleColor(3);
  return hit;
}
void ApplyStyle() {
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.Colors[ImGuiCol_WindowBg] = kWindow;
  style.Colors[ImGuiCol_ChildBg] = kWindow;
  style.Colors[ImGuiCol_Text] = kText;
  style.Colors[ImGuiCol_TextDisabled] = kMuted;
  style.Colors[ImGuiCol_Border] = kBorder;
  style.Colors[ImGuiCol_FrameBg] = kRaised;
  style.Colors[ImGuiCol_FrameBgHovered] = kHover;
  style.Colors[ImGuiCol_FrameBgActive] = kRaised;
  style.Colors[ImGuiCol_Header] = kRaised;
  style.Colors[ImGuiCol_HeaderHovered] = kHover;
  style.Colors[ImGuiCol_HeaderActive] = kAccent;
  style.Colors[ImGuiCol_CheckMark] = kAccent;
  style.Colors[ImGuiCol_SliderGrab] = kAccent;
  style.Colors[ImGuiCol_Button] = kRaised;
  style.Colors[ImGuiCol_ButtonHovered] = kHover;
  style.Colors[ImGuiCol_ButtonActive] = kAccent;
  style.Colors[ImGuiCol_PlotHistogram] = kAccent;
  style.Colors[ImGuiCol_TableHeaderBg] = kRaised;
  style.Colors[ImGuiCol_TableBorderLight] = kBorder;
  style.Colors[ImGuiCol_TableBorderStrong] = kBorder;
  style.Colors[ImGuiCol_TableRowBgAlt] = Rgba(255,255,255,0.03f);
  style.Colors[ImGuiCol_SliderGrabActive] = kDanger;
  style.Colors[ImGuiCol_NavCursor] = kDanger;
  style.WindowRounding = 0.0f;
  style.ChildRounding = 0.0f;
  style.FrameRounding = 0.0f;
  style.PopupRounding = 0.0f;
  style.ScrollbarRounding = 0.0f;
  style.WindowBorderSize = 0.0f;
  style.FrameBorderSize = 0.0f;
  style.WindowPadding = ImVec2(0.0f, 0.0f);
  style.ItemSpacing = ImVec2(12.0f, 10.0f);
}
} // namespace MainEventTheme
constexpr char kDropWaitMessage[] = "Please wait until the current task finishes, then drop the file again.";
constexpr char kCloseGameCreationsMessage[] = "Close WWE 2K14 before importing or removing creations.";
constexpr char kReadOnlyFolderMessage[] =
    "This folder is read-only. Move the WWE2K14-Recomp folder somewhere like Documents or Desktop and start it again.";

enum class Tab { Play, Settings, Controls, GameFiles, Saves, CreationSuite, Mods, BugReports };
enum class DialogKind {
  None, Music, DiscImage, Package, GameFolder, ImportFolder, WWE13ImportFolder, WWE15ImportFolder, WWE13ImportDisc, WWE15ImportDisc, Backup,
  CreationSuperstarFile, CreationArenaFile, CreationSuperstarFolder, CreationArenaFolder, ImageFile,
  CreationExportFile, StoryImportFile, StoryExportFile, PaintImportFile, PaintSavePng
};

enum class PaintTool { Brush, Eraser, Line, Rectangle, Ellipse, Fill, Eyedropper, Text };
enum class PaintImageDrag { None, Move, Scale };

struct PaintEditorState {
  bool open = false;
  bool dirty = false;
  bool confirm_discard = false;
  bool filled_shapes = false;
  std::optional<uint32_t> target_slot;
  internal::PaintCanvas canvas;
  internal::PaintHistory history{30};
  SDL_Texture* texture = nullptr;
  PaintTool tool = PaintTool::Brush;
  ImVec4 color = ImVec4(1, 0, 0, 1);
  std::array<ImVec4, 8> recent_colors{};
  size_t recent_color_count = 0;
  float brush_size = 8.0f;
  float hardness = 100.0f;
  float zoom = 1.5f;
  std::array<char, 128> text{};
  float text_size = 28.0f;
  bool stroke_active = false;
  internal::PaintPoint last_point{};
  bool shape_active = false;
  bool shape_tap_pending = false;
  internal::PaintPoint shape_start{};
  internal::PaintPoint shape_preview_point{};
  internal::PaintClickDrawState click_draw;
  bool brush_checkpoint_available = false;
  internal::PaintPoint brush_checkpoint{};
  PaintTool brush_checkpoint_tool = PaintTool::Brush;
  bool fullscreen = false;
  int windowed_x = 0, windowed_y = 0, windowed_width = 0, windowed_height = 0;
  std::optional<internal::PaintCanvas> pending_image;
  int image_x = 0, image_y = 0, image_width = 0, image_height = 0;
  PaintImageDrag image_drag = PaintImageDrag::None;
  internal::PaintPoint drag_start{};
  int drag_origin_x = 0, drag_origin_y = 0, drag_origin_width = 0, drag_origin_height = 0;
};

enum class MusicPlayerSource { None, Song, Editor, AllSongs };

// Read by the audio thread: the player and an "ended" flag it sets when the song (or trim range) runs out.
struct MusicPlayerFeed {
  explicit MusicPlayerFeed(const DecodedMusic& decoded) : player(decoded) {}
  MusicLivePlayer player;
  std::atomic<bool> ended{false};
};

struct MusicEditorState {
  bool open = false;
  Song song;
  fs::path source_file;
  std::shared_ptr<DecodedMusic> decoded;
  std::vector<MusicWaveformPoint> waveform;
  MusicTrackSettings settings;
  // Live player shared by the song list (plays the game's file as is), the song editor and "Settings for all songs"
  // (both play the original with the sliders applied while they move). One song plays at a time.
  MusicPlayerSource player_source = MusicPlayerSource::None;
  std::shared_ptr<MusicPlayerFeed> player;
  fs::path player_file;
  MusicTrackSettings player_base;  // "Settings for all songs": the song's own edit the sliders are laid over
  SDL_AudioStream* player_stream = nullptr;
  bool player_paused = false;
  bool hear_original = false;
  bool audio_subsystem_started = false;
};

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
  bool recommendation_confirmation_pending = false;
  Settings recommendation_current;
  Settings recommendation_suggestion;
  std::vector<GpuInfo> recommendation_gpus;
  std::string recommendation_reason;
  std::string recommendation_gpu_id;  // the card the button recommends (strongest found)
  GameFilesStatus game_files;
  std::vector<FoundGame> candidates;
  std::vector<Song> songs;
  MusicEditorState music_editor;
  MusicPreset music_preset_draft;
  std::set<fs::path> selected_songs;  // Music tab tick boxes
  std::optional<std::vector<CreationSaveRecord>> creation_records;
  // Creation Suite > Images: the Paint Tool save's logo slots (nullopt = unreadable save) and their thumbnails.
  std::optional<LogoList> logo_list;
  bool logo_list_loaded = false;
  std::vector<std::pair<uint32_t, SDL_Texture*>> logo_textures;  // slot index -> 256x256 texture
  PaintEditorState paint_editor;
  // Home-screen tile icons (embedded PNGs): play, creation suite, settings, controls, saves, game files, mods.
  std::array<SDL_Texture*, 7> tile_textures{};
  std::array<ImVec2, 7> tile_texture_sizes{};
  // Creation Suite > Stories: the story files in the save (nullopt = save folder unreadable).
  std::optional<std::vector<StoryInfo>> story_list;
  bool story_list_loaded = false;
  fs::path pending_import_story;
  bool confirm_import_story = false;
  std::string dialog_default_name;  // suggested location/file name for a creation or story export save dialog
  CreationSaveRecord pending_export_creation;
  StoryInfo pending_export_story;
  std::vector<BackupInfo> backups;
  KeyboardControls controls = DefaultKeyboardControls();
  bool scroll_to_candidates = false;
  FolderInstallables folder_installables;  // ISO / TU / DLC files dropped into the "WWE 2K14" folder  // Game files: bring the "folders found" list into view once
  int capture_index = -1;  // Controls row waiting for a key press, -1 = none
  std::string controls_status;
  bool controls_status_ok = true;
  Tab tab = Tab::Play;
  CreationSaveKind pending_import_creation_kind = CreationSaveKind::kSuperstar;
  fs::path pending_import_creation_folder;
  bool confirm_import_creation = false;
  CreationSaveRecord pending_remove_creation;
  bool confirm_remove_creation = false;
  CreationSaveRecord pending_rename_creation;
  std::array<char, 64> rename_creation_name{};
  bool confirm_rename_creation = false;
  std::string creation_recovery_error;
  bool initialized = false;
  bool running = true;
  bool drop_wait_notice_pending = false;
  bool shift_at_start = false;
  bool test_shift_held = false;
  bool bypass_auto_start = false;
  // Mods page. Reloaded whenever the page is opened, so a mod installed while the launcher is open shows up.
  std::vector<ModWrestler> mod_wrestlers;
  ExtraSuperstarHeadsStatus extra_superstar_heads;
  std::map<uint32_t, ModTheme> mod_themes;  // sliders only for these
  std::set<uint32_t> selected_mod_wrestlers;
  std::optional<uint32_t> edit_mod_wrestler;
  bool mod_editor_popup_requested = false;
  Tab mods_loaded_for = Tab::Play;
  std::array<char, 64> mods_search{};
  int mods_filter = 0;  // 0 In the game, 1 Men, 2 Divas, 3 Removed
  std::map<uint32_t, float> mod_theme_levels;  // 0 on the slider = this setting = WWE 2K14's level
  bool mods_list_popout = false;
  bool mods_list_popout_requested = false;
  WWE13ImportScan wwe13_import_scan;
  bool wwe13_import_open = false;
  bool wwe13_import_requested = false;  // Import tab card button: the picker opens at page level
  bool wwe15_import_requested = false;
  bool wwe13_import_loading = false;
  bool wwe13_import_choose_pending = false;  // the search is stopping; open the folder picker when it has
  DialogKind wwe13_import_choose_kind = DialogKind::WWE13ImportFolder;  // folder or disc-image picker
  bool wwe13_import_scan_done = false;
  WWE15ImportScan wwe15_import_scan;
  bool wwe15_import_open = false;
  bool wwe15_import_scan_done = false;
  bool wwe15_import_loading = false;         // the whole-PC search for WWE 2K15 files is running
  bool wwe15_import_choose_pending = false;  // the search is stopping; open the folder picker when it has
  DialogKind wwe15_import_choose_kind = DialogKind::WWE15ImportFolder;  // folder or disc-image picker
  bool wwe15_looks = false;  // also give the 47 wrestlers already in 2K14 their 2K15 attires (off by default)
  // Import WWE '13 arenas picker (owner 11-13: choose which arenas, all ticked by default).
  bool wwe13_arenas_open = false;
  bool wwe13_arenas_loading = false;
  std::vector<WWE13ArenaItem> wwe13_arena_items;
  std::set<uint32_t> wwe13_arena_selected;
  size_t wwe13_arena_free = 0;
  std::string wwe13_arena_error;
  std::set<uint32_t> wwe15_selected;
  bool screenshot_mode = false;
  int settings_row = 1;  // v1.7.0 Settings (game-menu style): the highlighted row
  bool paint_screenshot_mode = false;
  size_t paint_screenshot_index = 0;
  bool import_list_open = false;
  bool installation_search_running = false;
  bool import_confirmation_pending = false;
  // First start of a fresh installation (no settings file, no saves/DLC yet): look for a previous installation once
  // and offer to bring everything over (game folder incl. title update, DLC, saves, music, keys, settings).
  bool first_run_search_pending = false;
  bool first_run_scan_pending = false;
  bool import_first_run = false;
  bool first_run_offer_visible = false;  // Game files: "Previous installation found" box (+ note on Play)
  fs::path first_run_candidate;
  bool refresh_game_files_pending = false;
  bool screenshot_import_queued = false;
  InstallationImportInfo import_info;
  std::vector<FoundInstallationImport> import_candidates;
  std::filesystem::path screenshot_dir;
  float test_pixel_density = 0.0f;  // Screenshot-only simulated high-density framebuffer.
#if !defined(WWE2K14_LAUNCHER_NO_TEST_TOOLS)
  float test_display_scale = 0.0f;  // Screenshot-only simulated operating-system UI scale.
#endif
  size_t screenshot_index = 0;
  std::vector<fs::path> test_drop_files;
  Tab test_drop_tab = Tab::CreationSuite;
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
  bool launch_window_minimized = false;  // UI thread only; the launch job also reads launch_process_started
  std::string launch_warning;
  bool launch_warning_pending = false;
  // Bug Reports page + crash pop-up (owner 2026-11-09).
  std::vector<GameSession> bug_sessions;
  std::vector<BugReportFile> bug_reports;
  bool bug_page_loaded = false;
  bool game_running_now = false;          // read when the page loads: the newest session is still being played
  std::optional<GameSession> crash_notice;  // the crashed session the pop-up is about
  std::string crash_notice_detail;          // extra advice (start-up failures)
  bool crash_notice_pending = false;
  bool after_crash = false;                 // started by the crash watcher (--after-crash)
  Uint64 crash_poll_ns = 0;
  bool crash_check_pending = false;         // run StartCrashCheck once no other job is running
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

float GetWindowDisplayScale(const AppState& app) {
#if !defined(WWE2K14_LAUNCHER_NO_TEST_TOOLS)
  if (app.screenshot_mode && app.test_display_scale > 0.0f) return app.test_display_scale;
#endif
  return SDL_GetWindowDisplayScale(app.window);
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

const std::array<std::pair<Tab, const char*>, 8> kTabs{{
    {Tab::Play, "Play"}, {Tab::Settings, "Settings"}, {Tab::Controls, "Controls"},
    {Tab::GameFiles, "Game files"}, {Tab::Saves, "Saves"}, {Tab::CreationSuite, "Creation Suite"}, {Tab::Mods, "Mods (beta)"},
    {Tab::BugReports, "Bug Reports"},
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

Recommendation CurrentRecommendation(const AppState& app) {
  return RecommendedSettingsFor(app.gpus, app.settings.gpu_id);
}

Settings ActiveSettings(const AppState& app) {
  return ApplyRecommendedSettings(app.settings, CurrentRecommendation(app).settings);
}

void KeepManualSettings(AppState& app, RecommendedField field) {
  app.settings.recommendation_fields_tracked = true;
  app.settings.explicit_choice = true;
  switch (field) {
    case RecommendedField::kResolution: app.settings.manual_resolution = true; break;
    case RecommendedField::kPictureSize: app.settings.manual_picture_size = true; break;
    case RecommendedField::kFrameRate: app.settings.manual_frame_rate = true; break;
    case RecommendedField::kXboxFrameRate: app.settings.manual_xbox_frame_rate = true; break;
  }
}

void SetBanner(AppState& app, std::string text, bool success = false) {
  app.banner = std::move(text);
  app.banner_success = success;
}

void StopMusicPreview(MusicEditorState& editor) {
  if (editor.player_stream) {
    SDL_PauseAudioStreamDevice(editor.player_stream);
    SDL_DestroyAudioStream(editor.player_stream);  // unbinds the callback before the feed is released below
    editor.player_stream = nullptr;
  }
  editor.player.reset();
  editor.player_source = MusicPlayerSource::None;
  editor.player_file.clear();
  editor.player_paused = false;
  if (editor.audio_subsystem_started) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    editor.audio_subsystem_started = false;
  }
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

void PerfLog(const char* format, ...);
void BeginJob(AppState& app, std::string label, JobTask task) {
  if (app.job.active.load()) {
    PerfLog("job refused (another job is running): %s", label.c_str());
    return;
  }
  PerfLog("job start: %s", label.c_str());
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
void PerfLog(const char* format, ...);
void DrawInstallationImportList(AppState& app);
void DrawInstallationImportConfirmation(AppState& app);
void DrawProgress(AppState& app);  // defined with the Game files page; reused by the updater
void QueueCrashNotice(AppState& app, GameSession session, std::string detail = {});  // Bug Reports page
void DrawUpdateBanner(AppState& app);
void DrawUpdateNotes(AppState& app);
void StartUpdate(AppState& app);
void StartUpdateCheck(AppState& app);
void PollUpdateCheck(AppState& app);

void PollJob(AppState& app) {
  // StartGame reports process creation separately because it may keep watching for an early crash.
  // Keep SDL window changes on the UI thread while the worker waits.
  if (!app.launch_window_minimized && app.launch_process_started.load(std::memory_order_acquire)) {
    app.launch_window_minimized = true;
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
  PerfLog("job end: ok=%d %s", result.ok ? 1 : 0, result.ok ? "" : result.error.c_str());
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
constexpr int kPerfLogMaxLines = 3000;  // card LOG1: clicks and setting changes are logged too

void PerfLog(const char* format, ...) {
  if (!g_perf_log || g_perf_log_lines >= kPerfLogMaxLines) return;
  char line[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  // Card LOG1: bug reports are posted publicly - replace the account name in "\Users\<name>\" / "/home/<name>/".
  std::string text = line;
  for (const char* marker : {"\\Users\\", "/Users/", "/home/", "\\users\\"}) {
    for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + 1)) {
      const size_t name = at + std::strlen(marker);
      const size_t end = text.find_first_of("\\/", name);
      if (end != std::string::npos && end > name) text.replace(name, end - name, "<user>");
    }
  }
  // The update check logs from its own thread; serialise so lines never interleave.
  static std::mutex log_mutex;
  std::lock_guard lock(log_mutex);
  if (g_perf_log_lines >= kPerfLogMaxLines) return;
  std::fprintf(g_perf_log, "[%8.3f s] %s\n", SDL_GetTicksNS() / 1e9, text.c_str());
  std::fflush(g_perf_log);
  ++g_perf_log_lines;
}

// key=value lines of the settings file (missing file = empty).
std::map<std::string, std::string> ReadSettingsLines(const fs::path& file) {
  std::map<std::string, std::string> lines;
  std::ifstream in(file, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == '\0')) line.pop_back();
    std::string key;
    for (const char c : line) {
      if (c != '\0') key += c;  // the file is UTF-16 on Windows; keys and values are ASCII
    }
    const size_t equals = key.find('=');
    if (equals != std::string::npos) lines[key.substr(0, equals)] = key.substr(equals + 1);
  }
  return lines;
}

void SaveSettings(AppState& app) {
  const Uint64 start = SDL_GetTicksNS();
  const auto before = ReadSettingsLines(app.paths.settings_file);
  const Result result = wwe2k14::launcher::SaveSettings(app.paths, app.settings);
  // Card LOG1: one line per changed setting ("setting shadows: 1 -> 0").
  if (result.ok) {
    for (const auto& [key, value] : ReadSettingsLines(app.paths.settings_file)) {
      const auto old = before.find(key);
      if (old == before.end()) PerfLog("setting %s: %s", key.c_str(), value.c_str());
      else if (old->second != value) PerfLog("setting %s: %s -> %s", key.c_str(), old->second.c_str(), value.c_str());
    }
  }
  const double ms = (SDL_GetTicksNS() - start) / 1e6;
  if (ms > 10.0) PerfLog("settings save took %.1f ms", ms);
  if (!result.ok) SetBanner(app, result.error);
}

void SetLogoList(AppState& app, std::optional<LogoList> list);

void StartRefreshGameFiles(AppState& app) {
  const Paths paths = app.paths;
  const fs::path folder = ActiveGameFolder(app);
  BeginJob(app, "Checking game files…", [&app, paths, folder](const ProgressFn&, CancelFlag&) {
    const GameFilesStatus status = CheckGameFolder(folder, paths);
    const auto songs = ListSongs(paths);
    const auto creations = ListCreationSaveRecords(paths);
    auto logos = ListLogoImages(paths);
    auto stories = ListStories(paths);
    return JobOutcome{Result::Ok(), [&app, status, songs, creations, logos = std::move(logos),
                                     stories = std::move(stories)]() mutable {
                        app.game_files = status;
                        app.songs = songs;
                        app.creation_records = creations;
                        SetLogoList(app, std::move(logos));
                        app.story_list = std::move(stories);
                        app.story_list_loaded = true;
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
  if (const size_t repaired = RepairOwnSlotsFromRosters(paths))
    PerfLog("[own-slots] own-slots.txt was missing; rebuilt it with %zu imported wrestler id(s) from mods/*/roster.tsv", repaired);
  const bool modded = HasEnabledModWrestlers(paths);
  // GitHub #105: tell the player when antivirus removed the shader translator; the game still starts.
#if WWE2K14_NEW_RENDERER_UI
  const bool tools_missing = false;  // v1.7.0: the new renderer (Windows) does not use the shader translator
#else
  const bool tools_missing = settings.renderer == Renderer::kNative && !ShaderToolsPresent(paths);
#endif
  if (tools_missing) {
    PerfLog("shader translator missing: shadertools/%s", ShaderToolsPath(paths).filename().string().c_str());
    SetBanner(app, std::string("A graphics helper file is missing (shadertools/") +
                       ShaderToolsPath(paths).filename().string() +
                       "), so some wrestlers or items may look black. Your antivirus may have removed it: restore it from "
                       "the download (or extract the release again) and add an exception for the game folder.");
  }
  app.launch_process_started.store(false, std::memory_order_relaxed);
  app.launch_window_minimized = false;
  BeginJob(app, "Starting WWE 2K14…", [&app, paths, settings, gpus, modded, tools_missing](const ProgressFn&, CancelFlag& cancel) {
    if (settings.imports_in_grid && settings.mods_enabled) {
      // Owner 11-16: imported wrestlers get select-grid faces (missing ones from imports before v1.6.2, see-through ones
      // from v1.6.2 tests). Quick when nothing changed; a failure never blocks Play.
      GridFaceRepairReport faces;
      std::string why;
      const fs::path folder = settings.game_folder.empty() ? paths.default_game_folder : fs::path(settings.game_folder);
      const bool ok = RepairImportGridFaces(paths, folder, &faces, &why);
      if (!faces.unchanged || !ok)
        PerfLog("grid faces: ok=%d added=%zu replaced=%zu no_portrait=%zu %s", ok ? 1 : 0, faces.added, faces.replaced,
                faces.no_portrait, why.c_str());
    }
    LaunchPlan plan = BuildLaunchPlan(paths, settings, gpus);
    if (!settings.mods_enabled) {
      // Owner 11-18: Mods off = a clean view of the original files with separate saves (mods_switch.h).
      ModsOffView view;
      const fs::path folder = settings.game_folder.empty() ? paths.default_game_folder : fs::path(settings.game_folder);
      const Result clean = PrepareModsOffView(paths, folder, &view);
      PerfLog("mods off: ok=%d linked=%zu copied=%zu originals=%zu removed=%zu %s", clean.ok ? 1 : 0, view.linked,
              view.copied, view.originals, view.removed, clean.error.c_str());
      if (!clean.ok) return JobOutcome{clean, [] {}};
      UseModsOffView(view, &plan);
    }
    const StaleMemoryCleanup stale = RemoveStaleGuestMemoryFiles();
    if (stale.files) PerfLog("removed %zu leftover game memory file(s) from a forced close, %.0f MB freed", stale.files,
                             double(stale.bytes) / (1024.0 * 1024.0));
    uint64_t game_process_id = 0;
    const Result result = StartGame(plan, 90.0, &app.launch_process_started, &cancel, &game_process_id);
    if (app.launch_process_started.load(std::memory_order_acquire)) {
      (void)MarkLastPlayModded(paths, modded);
    }
    const fs::path game_log = LaunchPlanLogFile(plan);
    if (!result.ok && result.error == kGameClosedEarlyMessage && !game_log.empty()) {
      // Owner 11-09: the game ended with an error while starting. Save that session's report and show the crash
      // pop-up (with the old driver advice) instead of only a message.
      GameSession session = ReadGameSession(paths, game_log);
      if (session.end != SessionEnd::kCrashed) session.end = SessionEnd::kNoEnding;
      fs::path created;
      if (SaveBugReport(paths, &created, game_log).ok) session.report = created;
      return JobOutcome{Result::Ok(), [&app, session]() mutable {
                          QueueCrashNotice(app, std::move(session),
                                           "On Intel graphics, installing the newest Intel graphics driver may help.");
                        }};
    }
    if (result.ok && game_process_id != 0 && !game_log.empty() && GameProcessRunning(paths)) {
      // The launcher still closes; a small copy of it without a window waits for the game and reports a crash.
      const Result watcher = StartCrashWatcher(LauncherExecutablePath(), game_process_id, game_log);
      PerfLog("crash watcher start ok=%d %s", watcher.ok ? 1 : 0, watcher.error.c_str());
    }
    return JobOutcome{result, [&app, tools_missing] {
                        if (tools_missing && app.launch_process_started.load()) return;  // stay open so the notice is read
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
  StopMusicPreview(app.music_editor);
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
    const Result creation_recovery = RecoverCreationSaveFiles(paths);
    const bool exe_dir_writable = CanWriteExeDirectory(paths.exe_dir);
    std::error_code first_run_error;
    const bool first_run = exe_dir_writable && !fs::exists(paths.settings_file, first_run_error) &&
                           !fs::exists(paths.userdata_dir / "B13EBABEBABEBABE", first_run_error) &&
                           !fs::exists(paths.userdata_dir / "0000000000000000", first_run_error);
    Settings settings = LoadSettings(paths);
    const std::vector<GpuInfo> gpus = ListGpus();
    const Recommendation recommendation = RecommendedSettingsFor(gpus, settings.gpu_id);
    const GameFilesStatus game_files = CheckGameFolder(
        settings.game_folder.empty() ? paths.default_game_folder : settings.game_folder, paths);
    const std::vector<Song> songs = ListSongs(paths);
    const auto creations = creation_recovery.ok ? ListCreationSaveRecords(paths)
                                                : std::optional<std::vector<CreationSaveRecord>>{};
    const std::vector<BackupInfo> backups = ListBackups(paths);
    auto logos = ListLogoImages(paths);
    auto stories = ListStories(paths);
    if (exe_dir_writable) EnsureKeyboardDefaults(paths);  // first start: write the modern default keys
    if (exe_dir_writable) CleanupUpdateLeftovers(paths);  // remove *.old / *.part from a previous update
    const KeyboardControls controls = LoadKeyboardControls(paths);
    const bool crash_unseen = UnseenRecentCrash(paths).has_value();  // reads the newest game log: off the UI thread
    return JobOutcome{Result::Ok(), [&app, paths, settings, first_run,
                                      exe_dir_writable, controls, crash_unseen,
                                      gpus, recommendation, game_files, songs, creations, backups,
                                      creation_recovery, logos = std::move(logos),
                                      stories = std::move(stories)]() mutable {
                          SetLogoList(app, std::move(logos));
                          app.story_list = std::move(stories);
                          app.story_list_loaded = true;
                          app.paths = paths;
                          app.settings = settings;
                          app.music_preset_draft = settings.music_preset;
                          app.gpus = gpus;
                          app.recommendation = recommendation;
                          app.game_files = game_files;
                          app.songs = songs;
                          app.creation_records = creations;
                          app.creation_recovery_error = creation_recovery.ok ? std::string{} : creation_recovery.error;
                          app.backups = backups;
                          app.controls = controls;
                          app.initialized = true;
                          app.first_run_search_pending = first_run && !app.screenshot_mode;
                         RefreshFolderInstallables(app);
                         if (app.settings.check_updates && (!app.screenshot_mode || app.update_screenshot_mode)) {
                           StartUpdateCheck(app);
                         }
                         if (!app.game_files.ready() && (!app.folder_installables.disc_images.empty() ||
                                                         !app.folder_installables.packages.empty())) {
                           app.tab = Tab::GameFiles;
                         }
                         // Owner 11-09: a crash the player has not seen yet (the watcher opened this launcher, or the
                         // launcher was closed when it happened) gets its pop-up instead of starting the game again.
                         const bool crash_pending = TakeCrashPending(paths);
                         const bool unseen = crash_unseen && !app.screenshot_mode;
                         if (unseen) app.bypass_auto_start = true;
                         if (app.settings.auto_start && !app.shift_at_start && !app.bypass_auto_start &&
                            !app.screenshot_mode) {
                          StartLaunch(app);
                        }
                         if (unseen || crash_pending || app.after_crash) app.crash_check_pending = true;
                         if (!exe_dir_writable) SetBanner(app, kReadOnlyFolderMessage);
                      }};
  });
}

bool IsFolderWithin(const fs::path& candidate, const fs::path& root) {
  if (candidate.empty() || root.empty()) return true;
  std::error_code error;
  const fs::path canonical_root = fs::weakly_canonical(root, error);
  if (error) return true;
  const fs::path canonical_candidate = fs::weakly_canonical(candidate, error);
  if (error) return true;
  auto equal_root = [](const fs::path& left, const fs::path& right) {
    std::string a = PathUtf8(left), b = PathUtf8(right);
    if (a.size() == 2 && b.size() == 2 && a[1] == ':' && b[1] == ':') {
      a[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(a[0])));
      b[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(b[0])));
    }
    return a == b;
  };
  if (!equal_root(canonical_candidate.root_name(), canonical_root.root_name()) ||
      canonical_candidate.root_directory() != canonical_root.root_directory()) return false;
  const fs::path relative = canonical_candidate.lexically_relative(canonical_root);
  if (relative.empty()) return canonical_candidate == canonical_root;
  if (relative.is_absolute()) return false;
  const auto first = relative.begin();
  return first == relative.end() || *first != "..";
}

fs::path CreationExportDefaultFolder(const Paths& paths) {
  for (const SDL_Folder folder : {SDL_FOLDER_DOCUMENTS, SDL_FOLDER_DESKTOP}) {
    const char* user_folder = SDL_GetUserFolder(folder);
    if (!user_folder || !*user_folder) continue;
    const fs::path candidate = Utf8Path(user_folder);
    if (!IsFolderWithin(candidate, paths.userdata_dir)) return candidate;
  }
  return paths.exe_dir;
}

void StartLoadPaintImage(AppState& app, const fs::path& picture);
void StartSavePaintPng(AppState& app, fs::path output);

void RequestDialog(AppState& app, DialogKind kind, const SDL_DialogFileFilter* filters,
                   int filter_count, bool many) {
  {
    std::lock_guard lock(app.dialog.mutex);
    app.dialog.kind = kind;
    app.dialog.ready = false;
    app.dialog.failed = false;
    app.dialog.paths.clear();
  }
  if (kind == DialogKind::GameFolder || kind == DialogKind::ImportFolder ||
      kind == DialogKind::WWE13ImportFolder || kind == DialogKind::WWE15ImportFolder || kind == DialogKind::Backup ||
      kind == DialogKind::WWE13ImportDisc || kind == DialogKind::WWE15ImportDisc ||
      kind == DialogKind::CreationSuperstarFile || kind == DialogKind::CreationArenaFile ||
      kind == DialogKind::CreationSuperstarFolder || kind == DialogKind::CreationArenaFolder ||
      kind == DialogKind::ImageFile || kind == DialogKind::PaintImportFile || kind == DialogKind::StoryImportFile ||
      kind == DialogKind::CreationExportFile || kind == DialogKind::StoryExportFile) {
    if (const char* selected = std::getenv("WWE2K14_TEST_DIALOG_PATH"); selected && *selected) {
      std::lock_guard lock(app.dialog.mutex);
      app.dialog.ready = true;
      app.dialog.paths.emplace_back(selected);
      return;
    }
  }
  const std::string default_location = PathUtf8(app.paths.exe_dir);
  if (kind == DialogKind::GameFolder || kind == DialogKind::ImportFolder ||
      kind == DialogKind::WWE13ImportFolder || kind == DialogKind::WWE15ImportFolder ||
      kind == DialogKind::CreationSuperstarFolder || kind == DialogKind::CreationArenaFolder) {
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
  } else if (kind == DialogKind::CreationExportFile || kind == DialogKind::StoryExportFile ||
             kind == DialogKind::PaintSavePng) {
    // many=true is not used here; app.dialog_default_name carries the suggested full file path.
    SDL_ShowSaveFileDialog(
        [](void* userdata, const char* const* filelist, int) {
          auto& app = *static_cast<AppState*>(userdata);
          std::lock_guard lock(app.dialog.mutex);
          app.dialog.ready = true;
          app.dialog.failed = filelist == nullptr;
          if (filelist) {
            for (size_t i = 0; filelist[i]; ++i) app.dialog.paths.emplace_back(filelist[i]);
          }
        },
        &app, app.window, filters, filter_count, app.dialog_default_name.c_str());
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

// The import windows' pickers: a folder (extracted disc / add-ons) or the disc image file itself. A folder picker
// hides files on Windows, so an .iso can only be picked with the file picker (owner 11-13).
void RequestImportPicker(AppState& app, DialogKind kind) {
  static const SDL_DialogFileFilter disc_filters[]{{"Disc images (.iso, .img)", "iso;img"}};
  const bool disc = kind == DialogKind::WWE13ImportDisc || kind == DialogKind::WWE15ImportDisc;
  RequestDialog(app, kind, disc ? disc_filters : nullptr, disc ? 1 : 0, false);
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
  Settings settings = app.settings;
  BeginJob(app, "Removing song…", [&app, paths, settings, file = std::move(file)](const ProgressFn&, CancelFlag&) mutable {
    const Result result = RemoveSong(paths, file, &settings);
    const std::vector<Song> songs = result.ok ? ListSongs(paths) : std::vector<Song>{};
    return JobOutcome{result, [&app, songs, settings] {
                        app.songs = songs;
                        app.settings = settings;
                        SetBanner(app, "Song removed from your music folder.", true);
                      }};
  });
}

void OpenMusicEditor(AppState& app, const Song& song) {
  const Paths paths = app.paths;
  const Settings settings = app.settings;
  StopMusicPreview(app.music_editor);
  BeginJob(app, "Opening song…", [&app, paths, settings, song](const ProgressFn&, CancelFlag&) {
    const fs::path source = MusicSourcePath(paths, settings, song.file);
    auto decoded = std::make_shared<DecodedMusic>();
    const Result result = DecodeMusicFile(source, decoded.get());
    std::vector<MusicWaveformPoint> waveform;
    if (result.ok) waveform = BuildMusicWaveform(*decoded, 640);
    const MusicTrackSettings edit = MusicSettingsForSong(settings, song.file);
    return JobOutcome{result, [&app, song, source, decoded, waveform = std::move(waveform), edit]() mutable {
                        MusicEditorState& editor = app.music_editor;
                        editor.open = true;
                        editor.song = song;
                        editor.source_file = source;
                        editor.decoded = decoded;
                        editor.waveform = std::move(waveform);
                        editor.settings = edit;
                      }};
  });
}

void SDLCALL FeedMusicPlayer(void* userdata, SDL_AudioStream* stream, int additional_amount, int) {
  auto* feed = static_cast<MusicPlayerFeed*>(userdata);
  if (additional_amount <= 0) return;
  thread_local std::vector<float> buffer;
  const size_t frames = (static_cast<size_t>(additional_amount) + 7) / 8;
  buffer.resize(frames * 2);
  const size_t written = feed->player.Render(buffer.data(), frames);
  if (written) SDL_PutAudioStreamData(stream, buffer.data(), static_cast<int>(written * 2 * sizeof(float)));
  if (written < frames) feed->ended.store(true);
}

// Starts playing `feed` on the default device; settings must already be set on its player.
void StartMusicPlayer(AppState& app, std::shared_ptr<MusicPlayerFeed> feed, MusicPlayerSource source,
                      const fs::path& file) {
  MusicEditorState& editor = app.music_editor;
  StopMusicPreview(editor);
  if (!editor.audio_subsystem_started) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      SetBanner(app, "The launcher could not start audio to play the song.");
      return;
    }
    editor.audio_subsystem_started = true;
  }
  SDL_AudioSpec spec{};
  spec.format = SDL_AUDIO_F32;
  spec.channels = 2;
  spec.freq = static_cast<int>(feed->player.sample_rate());
  editor.player_stream =
      SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, FeedMusicPlayer, feed.get());
  if (!editor.player_stream) {
    SetBanner(app, "The launcher could not open an audio device to play the song.");
    return;
  }
  editor.player = std::move(feed);
  editor.player_source = source;
  editor.player_file = file;
  SDL_ResumeAudioStreamDevice(editor.player_stream);
}

// Song list "Play": the file in the game's music folder, exactly as the game plays it.
void StartSongPlayback(AppState& app, const Song& song) {
  StopMusicPreview(app.music_editor);
  const fs::path file = song.file;
  BeginJob(app, "Loading song…", [&app, file](const ProgressFn&, CancelFlag&) {
    DecodedMusic decoded;
    const Result result = DecodeMusicFile(file, &decoded);
    std::shared_ptr<MusicPlayerFeed> feed;
    if (result.ok) {
      feed = std::make_shared<MusicPlayerFeed>(decoded);
      feed->player.SetSettings(MusicTrackSettings{}, true);
    }
    return JobOutcome{result, [&app, feed, file] { StartMusicPlayer(app, feed, MusicPlayerSource::Song, file); }};
  });
}

// Song editor "Play": the original with the editor's sliders applied live.
void StartEditorPlayback(AppState& app) {
  MusicEditorState& editor = app.music_editor;
  if (!editor.decoded) return;
  auto feed = std::make_shared<MusicPlayerFeed>(*editor.decoded);
  feed->player.SetSettings(editor.settings, editor.hear_original);
  StartMusicPlayer(app, feed, MusicPlayerSource::Editor, editor.song.file);
}

void StartSaveMusicEdit(AppState& app) {
  MusicEditorState& editor = app.music_editor;
  if (!editor.decoded) return;
  StopMusicPreview(editor);
  const Paths paths = app.paths;
  Settings settings = app.settings;
  const fs::path song_file = editor.song.file;
  const auto decoded = editor.decoded;
  const MusicTrackSettings edit = editor.settings;
  BeginJob(app, "Saving song…", [&app, paths, settings, song_file, decoded, edit](const ProgressFn&, CancelFlag&) mutable {
    fs::path saved;
    const Result result = SaveMusicEdit(paths, &settings, song_file, *decoded, edit, &saved);
    const std::vector<Song> songs = result.ok ? ListSongs(paths) : std::vector<Song>{};
    return JobOutcome{result, [&app, settings, songs] {
                        app.settings = settings;
                        app.songs = songs;
                        app.music_editor.open = false;
                        app.music_editor.decoded.reset();
                        SetBanner(app, "Song saved. The game plays the processed copy; your original is kept.", true);
                      }};
  });
}

// Re-renders every song (edited or not) from its original file with edit_for(settings, song file), then saves.
void StartRenderAllSongs(AppState& app, Settings settings, std::vector<Song> songs,
                         std::function<MusicTrackSettings(const Settings&, const fs::path&)> edit_for,
                         std::string label, std::string done_message) {
  StopMusicPreview(app.music_editor);
  const Paths paths = app.paths;
  BeginJob(app, label, [&app, paths, settings, songs, edit_for, label, done_message](const ProgressFn& progress,
                                                                                        CancelFlag& cancel) mutable {
    Result result = SaveSettings(paths, settings);
    uint64_t done = 0;
    for (const Song& song : songs) {
      if (!result.ok || cancel.load()) break;
      progress(done++, songs.size(), label);
      DecodedMusic decoded;
      result = DecodeMusicFile(MusicSourcePath(paths, settings, song.file), &decoded);
      if (!result.ok) break;
      fs::path saved;
      result = SaveMusicEdit(paths, &settings, song.file, decoded, edit_for(settings, song.file), &saved);
    }
    const std::vector<Song> listed = ListSongs(paths);
    return JobOutcome{result, [&app, settings, listed, done_message] {
                        app.settings = settings;
                        app.songs = listed;
                        SetBanner(app, done_message, true);
                      }};
  });
}

// Ticked songs > "Match to the game's level": loudness evening at kGameMusicRmsDbfs; each song's trim/fades/EQ kept.
void StartMatchSelectedSongs(AppState& app) {
  std::vector<Song> songs;
  for (const Song& song : app.songs) {
    if (app.selected_songs.contains(song.file)) songs.push_back(song);
  }
  if (songs.empty()) return;
  app.selected_songs.clear();
  StartRenderAllSongs(app, app.settings, std::move(songs), [](const Settings& current, const fs::path& song) {
    MusicTrackSettings edit = MusicSettingsForSong(current, song);
    edit.normalize = true;
    edit.target_rms_dbfs = kGameMusicRmsDbfs;
    return edit;
  }, "Matching songs to the game's level…", "The selected songs now play at the game's music level.");
}

// The "settings for all songs" sliders laid over one song's own edit (its trim and volume are kept).
MusicTrackSettings WithPreset(MusicTrackSettings edit, const MusicPreset& preset) {
  edit.normalize = true;
  edit.target_rms_dbfs = preset.target_rms_dbfs;
  edit.fade_in_seconds = preset.fade_in_seconds;
  edit.fade_out_seconds = preset.fade_out_seconds;
  edit.bass_gain_db = preset.bass_gain_db;
  edit.bass_frequency_hz = preset.bass_frequency_hz;
  edit.treble_gain_db = preset.treble_gain_db;
  edit.treble_frequency_hz = preset.treble_frequency_hz;
  return edit;
}

void StartApplyPresetToAllSongs(AppState& app) {
  Settings settings = app.settings;
  settings.music_preset = app.music_preset_draft;
  const MusicPreset preset = app.music_preset_draft;
  StartRenderAllSongs(app, settings, app.songs, [preset](const Settings& current, const fs::path& song) {
    return WithPreset(MusicSettingsForSong(current, song), preset);
  }, "Applying settings to all songs…", "These settings are now used by all your songs.");
}

std::string FormatDuration(double seconds);

// "Settings for all songs" > Play: the first song's original with the sliders applied live.
void StartAllSongsPlayback(AppState& app) {
  if (app.songs.empty()) return;
  StopMusicPreview(app.music_editor);
  const Paths paths = app.paths;
  const Settings settings = app.settings;
  const fs::path song = app.songs.front().file;
  BeginJob(app, "Loading song…", [&app, paths, settings, song](const ProgressFn&, CancelFlag&) {
    DecodedMusic decoded;
    const Result result = DecodeMusicFile(MusicSourcePath(paths, settings, song), &decoded);
    std::shared_ptr<MusicPlayerFeed> feed;
    const MusicTrackSettings base = MusicSettingsForSong(settings, song);
    if (result.ok) feed = std::make_shared<MusicPlayerFeed>(decoded);
    return JobOutcome{result, [&app, feed, song, base] {
                        app.music_editor.player_base = base;
                        feed->player.SetSettings(WithPreset(base, app.music_preset_draft), app.music_editor.hear_original);
                        StartMusicPlayer(app, feed, MusicPlayerSource::AllSongs, song);
                      }};
  });
}

// Keeps the live player in step with the sliders and stops it when the song has finished.
void UpdateMusicPlayer(AppState& app) {
  MusicEditorState& editor = app.music_editor;
  if (!editor.player) return;
  if (editor.player->ended.load() && editor.player_stream && SDL_GetAudioStreamQueued(editor.player_stream) == 0) {
    StopMusicPreview(editor);
    return;
  }
  if (editor.player_source == MusicPlayerSource::Editor) {
    if (!editor.open) {
      StopMusicPreview(editor);
      return;
    }
    editor.player->player.SetSettings(editor.settings, editor.hear_original);
  } else if (editor.player_source == MusicPlayerSource::AllSongs) {
    editor.player->player.SetSettings(WithPreset(editor.player_base, app.music_preset_draft), editor.hear_original);
  }
}

// Pause / Stop, a position bar to jump anywhere in the song, the time, and (when sliders apply) "Hear original".
void DrawMusicPlayerBar(AppState& app, MusicPlayerSource source) {
  MusicEditorState& editor = app.music_editor;
  if (!editor.player || editor.player_source != source) return;
  MusicLivePlayer& player = editor.player->player;
  ImGui::PushID(static_cast<int>(source));
  if (LoggedButton(editor.player_paused ? "Resume" : "Pause", S(app, 96.0f, 34.0f))) {
    editor.player_paused = !editor.player_paused;
    if (editor.player_paused) SDL_PauseAudioStreamDevice(editor.player_stream);
    else SDL_ResumeAudioStreamDevice(editor.player_stream);
  }
  ImGui::SameLine();
  if (LoggedButton("Stop", S(app, 80.0f, 34.0f))) {
    StopMusicPreview(editor);
    ImGui::PopID();
    return;
  }
  ImGui::SameLine();
  const float duration = player.sample_rate() ? static_cast<float>(double(player.frame_count()) / player.sample_rate()) : 0.0f;
  float position = static_cast<float>(player.PositionSeconds());
  const std::string time = FormatDuration(position) + " / " + FormatDuration(duration);
  ImGui::SetNextItemWidth(std::max(S(app, 120.0f), ImGui::GetContentRegionAvail().x - S(app, 130.0f)));
  if (ImGui::SliderFloat("##music-position", &position, 0.0f, std::max(duration, 0.1f), "")) {
    player.Seek(position);
    if (editor.player_stream) SDL_ClearAudioStream(editor.player_stream);  // jump now, not after queued audio
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(time.c_str());
  if (source != MusicPlayerSource::Song) {
    LoggedCheckbox("Hear original (switch on and off to compare)", &editor.hear_original);
  }
  ImGui::PopID();
}

// What the song list shows in its LEVEL column.
const char* SongLevelLabel(const AppState& app, const Song& song) {
  const auto record = app.settings.music_tracks.find(PathUtf8(song.file.filename()));
  if (record == app.settings.music_tracks.end()) return "Original";
  const MusicTrackSettings& edit = record->second.settings;
  return edit.normalize && std::abs(edit.target_rms_dbfs - kGameMusicRmsDbfs) < 0.5f ? "Game level" : "Custom";
}

constexpr int kSuiteTabImages = 3;
constexpr int kSuiteTabStories = 4;
int g_suite_tab = [] {  // 0 Superstars, 1 Arenas, 2 Music, 3 Logos, 4 Stories; not persisted (env is for screenshots)
  const char* v = std::getenv("WWE2K14_TEST_SUITE_TAB");
  return v && *v >= '0' && *v <= '4' ? *v - '0' : 0;
}();
LogoImageSize g_logo_import_size = LogoImageSize::k256;

constexpr char kCloseGameImagesMessage[] = "Close WWE 2K14 before adding, editing, or removing logos.";

void OpenCreationSuiteTab(AppState& app, int tab) {
  app.tab = Tab::CreationSuite;
  g_suite_tab = std::clamp(tab, 0, 4);
  app.banner.clear();
}

void OpenCreationImport(AppState& app, CreationSaveKind kind, bool navigate = true) {
  if (navigate) OpenCreationSuiteTab(app, kind == CreationSaveKind::kSuperstar ? 0 : 1);
  if (!app.creation_records.has_value()) return;  // The suite page shows why an unreadable save cannot be changed.
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, kCloseGameCreationsMessage);
    return;
  }
  static const SDL_DialogFileFilter creation_filters[]{{"Creation packages", "rar;zip;cas;car"}};
  RequestDialog(app, kind == CreationSaveKind::kSuperstar ? DialogKind::CreationSuperstarFile
                                                          : DialogKind::CreationArenaFile,
                creation_filters, 1, false);
}

void OpenLogoImport(AppState& app, bool navigate = true) {
  if (navigate) OpenCreationSuiteTab(app, kSuiteTabImages);
  const size_t used = app.logo_list ? app.logo_list->logos.size() : 0;
  if (app.job.active.load() || !app.logo_list.has_value() || used >= kLogoSlotCount) return;
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, kCloseGameImagesMessage);
    return;
  }
  static const SDL_DialogFileFilter image_filters[]{{"Pictures", "png;jpg;jpeg;bmp;tga;gif"}};
  RequestDialog(app, DialogKind::ImageFile, image_filters, 1, true);
}

void OpenMusicImport(AppState& app, bool navigate = true) {
  if (navigate) OpenCreationSuiteTab(app, 2);
  static const SDL_DialogFileFilter filters[]{{"Music", "mp3;flac;wav;ogg"}};
  RequestDialog(app, DialogKind::Music, filters, 1, true);
}

// Rebuilds the thumbnails of the Images tab from a freshly read list (UI thread only).
void SetLogoList(AppState& app, std::optional<LogoList> list) {
  for (auto& [slot, texture] : app.logo_textures) {
    if (texture) SDL_DestroyTexture(texture);
  }
  app.logo_textures.clear();
  app.logo_list = std::move(list);
  app.logo_list_loaded = true;
  if (!app.logo_list || !app.renderer) return;
  for (const LogoImage& logo : app.logo_list->logos) {
    SDL_Texture* texture = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 256, 256);
    if (!texture) continue;
    SDL_UpdateTexture(texture, nullptr, logo.rgba.data(), 256 * 4);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    app.logo_textures.emplace_back(logo.slot_index, texture);
  }
}

void StartRefreshLogos(AppState& app) {
  if (app.job.active.load()) return;
  const Paths paths = app.paths;
  BeginJob(app, "Reading logos…", [&app, paths](const ProgressFn&, CancelFlag&) {
    auto list = ListLogoImages(paths);
    return JobOutcome{Result::Ok(), [&app, list = std::move(list)]() mutable { SetLogoList(app, std::move(list)); }};
  });
}

void StartAddLogos(AppState& app, std::vector<fs::path> files, LogoImageSize size) {
  if (files.empty()) return;
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, kCloseGameImagesMessage);
    return;
  }
  if (app.job.active.load()) {
    SetBanner(app, kDropWaitMessage);
    return;
  }
  const Paths paths = app.paths;
  BeginJob(app, "Adding logos…", [&app, paths, files = std::move(files), size](const ProgressFn&, CancelFlag&) {
    uint32_t added = 0;
    std::string failure;
    uint32_t last_slot = 0;
    for (const fs::path& file : files) {
      uint32_t slot = 0;
      const Result r = AddLogoImage(paths, file, size, &slot);
      if (r.ok) { ++added; last_slot = slot; }
      else { failure = r.error; break; }
    }
    auto list = ListLogoImages(paths);
    const auto backups = ListBackups(paths);
    return JobOutcome{Result::Ok(), [&app, list = std::move(list), backups, added, failure, last_slot]() mutable {
                        SetLogoList(app, std::move(list));
                        app.backups = backups;
                        if (!failure.empty()) {
                          SetBanner(app, added ? "Added " + std::to_string(added) + ", then stopped: " + failure : failure);
                        } else if (added == 1) {
                          SetBanner(app, "Image added to slot " + std::to_string(last_slot + 1) +
                                             ". In the game: Create-A-Superstar > Tattoos > Paint Tool Data.", true);
                        } else {
                          SetBanner(app, std::to_string(added) + " logos added.", true);
                        }
                      }};
  });
}

void StartRemoveLogo(AppState& app, uint32_t slot) {
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, kCloseGameImagesMessage);
    return;
  }
  if (app.job.active.load()) {
    SetBanner(app, kDropWaitMessage);
    return;
  }
  const Paths paths = app.paths;
  BeginJob(app, "Removing logo…", [&app, paths, slot](const ProgressFn&, CancelFlag&) {
    const Result r = RemoveLogoImage(paths, slot);
    auto list = ListLogoImages(paths);
    const auto backups = ListBackups(paths);
    return JobOutcome{r, [&app, list = std::move(list), backups, slot]() mutable {
                        SetLogoList(app, std::move(list));
                        app.backups = backups;
                        SetBanner(app, "Image removed from slot " + std::to_string(slot + 1) + ".", true);
                      }};
  });
}

void DestroyPaintEditorTexture(PaintEditorState* editor) {
  if (editor->texture) SDL_DestroyTexture(editor->texture);
  editor->texture = nullptr;
}

bool SetPaintEditorFullscreen(AppState& app, bool fullscreen) {
  PaintEditorState& editor = app.paint_editor;
  if (editor.fullscreen == fullscreen) return true;
  if (fullscreen) {
    SDL_GetWindowPosition(app.window, &editor.windowed_x, &editor.windowed_y);
    SDL_GetWindowSize(app.window, &editor.windowed_width, &editor.windowed_height);
    if (!SDL_SetWindowFullscreen(app.window, true)) {
      SetBanner(app, "Full screen could not be started: " + std::string(SDL_GetError()));
      return false;
    }
    editor.fullscreen = true;
    return true;
  }

  if (!SDL_SetWindowFullscreen(app.window, false)) {
    SetBanner(app, "Full screen could not be closed: " + std::string(SDL_GetError()));
    return false;
  }
  editor.fullscreen = false;
  if (editor.windowed_width > 0 && editor.windowed_height > 0) {
    SDL_SetWindowSize(app.window, editor.windowed_width, editor.windowed_height);
    SDL_SetWindowPosition(app.window, editor.windowed_x, editor.windowed_y);
  }
  return true;
}

void ClosePaintEditor(AppState& app) {
  if (app.paint_editor.fullscreen) SetPaintEditorFullscreen(app, false);
  DestroyPaintEditorTexture(&app.paint_editor);
  app.paint_editor = PaintEditorState{};
}

void OpenNewPaintEditor(AppState& app) {
  if (!app.logo_list || app.logo_list->logos.size() >= kLogoSlotCount) {
    SetBanner(app, "There is no free logo slot. Remove a logo first.");
    return;
  }
  ClosePaintEditor(app);
  auto& editor = app.paint_editor;
  const int size = static_cast<int>(g_logo_import_size);
  editor.canvas = internal::MakePaintCanvas(size, size, {0, 0, 0, 0});
  editor.open = editor.canvas.Valid();
  editor.zoom = size == 128 ? 3.0f : 1.5f;
  editor.recent_colors[0] = editor.color;
  editor.recent_color_count = 1;
}

void OpenLogoPaintEditor(AppState& app, const LogoImage& logo) {
  if (logo.built_in) {
    SetBanner(app, "The game's own logos cannot be edited.");
    return;
  }
  if (logo.width != logo.height || (logo.width != 128 && logo.width != 256) ||
      logo.rgba.size() != 256u * 256u * 4u) {
    SetBanner(app, "That logo has an unsupported size and cannot be edited.");
    return;
  }
  ClosePaintEditor(app);
  auto& editor = app.paint_editor;
  const int size = static_cast<int>(logo.width);
  editor.canvas = internal::MakePaintCanvas(size, size, {0, 0, 0, 0});
  const int stride = 256 / size;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const size_t from = (static_cast<size_t>(y * stride) * 256 + x * stride) * 4;
      const size_t to = (static_cast<size_t>(y) * size + x) * 4;
      std::copy_n(logo.rgba.data() + from, 4, editor.canvas.rgba.data() + to);
    }
  }
  editor.target_slot = logo.slot_index;
  editor.open = editor.canvas.Valid();
  editor.zoom = size == 128 ? 3.0f : 1.5f;
  editor.recent_colors[0] = editor.color;
  editor.recent_color_count = 1;
}

void StartLoadPaintImage(AppState& app, const fs::path& picture) {
  if (!app.paint_editor.open || app.job.active.load()) return;
  const Paths paths = app.paths;
  const int width = app.paint_editor.canvas.width, height = app.paint_editor.canvas.height;
  BeginJob(app, "Importing picture…", [&app, paths, picture, width, height](const ProgressFn&, CancelFlag&) {
    std::vector<uint8_t> canonical;
    std::string error;
    const bool loaded = width == 128 ? internal::LoadImageFitted128(picture, &canonical, &error)
                                     : internal::LoadImageFitted256(picture, &canonical, &error);
    if (!loaded) return JobOutcome{Result::Fail(error + " The drawing was not changed."), {}};
    internal::PaintCanvas source = internal::MakePaintCanvas(width, height, {0, 0, 0, 0});
    if (!source.Valid() || canonical.size() != 256u * 256u * 4u)
      return JobOutcome{Result::Fail("The picture could not be fitted to this logo size."), {}};
    const int stride = 256 / width;
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const size_t from = (static_cast<size_t>(y * stride) * 256 + x * stride) * 4;
        const size_t to = (static_cast<size_t>(y) * width + x) * 4;
        std::copy_n(canonical.data() + from, 4, source.rgba.data() + to);
      }
    }
    return JobOutcome{Result::Ok(), [&app, source = std::move(source)]() mutable {
                        auto& editor = app.paint_editor;
                        if (!editor.open) return;
                        editor.pending_image = std::move(source);
                        editor.image_x = 0;
                        editor.image_y = 0;
                        editor.image_width = editor.canvas.width;
                        editor.image_height = editor.canvas.height;
                        editor.image_drag = PaintImageDrag::None;
                      }};
  });
}

void StartStorePaintEditor(AppState& app) {
  if (!app.paint_editor.open || app.job.active.load()) return;
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, kCloseGameImagesMessage);
    return;
  }
  if (!app.paint_editor.canvas.Valid() || app.paint_editor.pending_image) return;
  const Paths paths = app.paths;
  const std::optional<uint32_t> target = app.paint_editor.target_slot;
  const LogoImageSize size = app.paint_editor.canvas.width == 128 ? LogoImageSize::k128 : LogoImageSize::k256;
  std::vector<uint8_t> canonical = size == LogoImageSize::k128
                                      ? internal::ExpandRgba128To256(app.paint_editor.canvas.rgba)
                                      : app.paint_editor.canvas.rgba;
  BeginJob(app, target ? "Saving logo changes…" : "Saving logo…",
           [&app, paths, target, size, canonical = std::move(canonical)](const ProgressFn&, CancelFlag&) {
    if (GameProcessRunning(paths)) return JobOutcome{Result::Fail(kCloseGameImagesMessage), {}};
    uint32_t slot = target.value_or(0);
    const Result result = target ? EditLogoImage(paths, *target, canonical)
                                 : AddLogoImage(paths, canonical, size, &slot);
    if (!result.ok) return JobOutcome{result, {}};
    auto logos = ListLogoImages(paths);
    const auto backups = ListBackups(paths);
    return JobOutcome{result, [&app, logos = std::move(logos), backups, slot]() mutable {
                        SetLogoList(app, std::move(logos));
                        app.backups = backups;
                        ClosePaintEditor(app);
                        SetBanner(app, "Saved to logo slot " + std::to_string(slot + 1) +
                                          ". In the game: Create-A-Superstar > Tattoos > Paint Tool Data.", true);
                      }};
  });
}

void StartSavePaintPng(AppState& app, fs::path output) {
  if (!app.paint_editor.open || app.paint_editor.pending_image || app.job.active.load()) return;
  std::string extension = output.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension != ".png") output += ".png";
  const internal::PaintCanvas canvas = app.paint_editor.canvas;
  BeginJob(app, "Saving PNG…", [&app, output, canvas](const ProgressFn&, CancelFlag&) {
    if (!internal::WritePngRgba(output, canvas.rgba.data(), canvas.width, canvas.height))
      return JobOutcome{Result::Fail("The PNG could not be saved."), {}};
    return JobOutcome{Result::Ok(), [&app, output] {
                        SetBanner(app, "Saved PNG to " + PathUtf8(output), true);
                      }};
  });
}

void StartImportCreationSave(AppState& app, CreationSaveKind kind, fs::path folder) {
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, kCloseGameCreationsMessage);
    return;
  }
  if (app.job.active.load()) {
    SetBanner(app, kDropWaitMessage);
    return;
  }
  const Paths paths = app.paths;
  BeginJob(app, "Importing creation and payload…", [&app, paths, kind, folder = std::move(folder)](
                                                        const ProgressFn&, CancelFlag&) {
    uint32_t selected_slot = UINT32_MAX;
    CreationChangeSummary summary;
    const Result result = ImportCreationSave(paths, kind, folder, &selected_slot, &summary);
    std::optional<std::vector<CreationSaveRecord>> creations;
    if (result.ok) creations = ListCreationSaveRecords(paths);
    const auto backups = result.ok ? ListBackups(paths) : std::vector<BackupInfo>{};
    return JobOutcome{result, [&app, kind, selected_slot, summary, creations, backups] {
                        app.creation_records = creations;
                        app.backups = backups;
                        app.creation_recovery_error.clear();
                        const char* label = kind == CreationSaveKind::kSuperstar ? "CAW" : "arena";
                        std::string line = std::string("Imported ") + (summary.name.empty() ? label : summary.name.c_str()) +
                                           " (slot " + std::to_string(selected_slot + 1) + ").";
                        if (summary.registered) line += " Also set up " + std::to_string(summary.registered) + " existing creations.";
                        if (summary.repaired) line += " Also repaired " + std::to_string(summary.repaired) + ".";
                        SetBanner(app, line, true);
                      }};
  });
}

void StartRegisterExistingCreations(AppState& app) {
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, kCloseGameCreationsMessage);
    return;
  }
  if (app.job.active.load()) {
    SetBanner(app, kDropWaitMessage);
    return;
  }
  const Paths paths = app.paths;
  BeginJob(app, "Registering existing creations…", [&app, paths](const ProgressFn&, CancelFlag&) {
    uint32_t registered_count = 0, repaired_count = 0;
    const Result result = RegisterExistingCreationSaves(paths, &registered_count, &repaired_count);
    std::optional<std::vector<CreationSaveRecord>> creations;
    if (result.ok) creations = ListCreationSaveRecords(paths);
    const auto backups = result.ok ? ListBackups(paths) : std::vector<BackupInfo>{};
    return JobOutcome{result, [&app, registered_count, repaired_count, creations, backups] {
                        app.creation_records = creations;
                        app.backups = backups;
                        app.creation_recovery_error.clear();
                        if (registered_count == 0 && repaired_count == 0) {
                          SetBanner(app, "Nothing to register or repair: every creation is already set up.", true);
                        } else {
                          SetBanner(app, "Registered " + std::to_string(registered_count) + ", repaired " +
                                            std::to_string(repaired_count) +
                                            ". A backup was verified first.", true);
                        }
                      }};
  });
}

void StartRemoveCreationSave(AppState& app, CreationSaveRecord record) {
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, kCloseGameCreationsMessage);
    return;
  }
  if (app.job.active.load()) {
    SetBanner(app, kDropWaitMessage);
    return;
  }
  const Paths paths = app.paths;
  BeginJob(app, "Removing creation from the save…", [&app, paths, record](const ProgressFn&, CancelFlag&) {
    CreationChangeSummary summary;
    const Result result = RemoveCreationSave(paths, record.kind, record.slot_index, &summary);
    std::optional<std::vector<CreationSaveRecord>> creations;
    if (result.ok) creations = ListCreationSaveRecords(paths);
    const auto backups = result.ok ? ListBackups(paths) : std::vector<BackupInfo>{};
    return JobOutcome{result, [&app, record, summary, creations, backups] {
                        app.creation_records = creations;
                        app.backups = backups;
                        app.creation_recovery_error.clear();
                        const char* label = record.kind == CreationSaveKind::kSuperstar ? "CAW" : "arena";
                        std::string line = std::string("Removed ") + (summary.name.empty() ? label : summary.name.c_str()) + ".";
                        if (summary.registered) line += " Also set up " + std::to_string(summary.registered) + " existing creations.";
                        if (summary.repaired) line += " Also repaired " + std::to_string(summary.repaired) + ".";
                        SetBanner(app, line, true);
                      }};
  });
}

void StartRenameCreation(AppState& app, CreationSaveRecord record, std::string new_name) {
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, kCloseGameCreationsMessage);
    return;
  }
  if (app.job.active.load()) {
    SetBanner(app, kDropWaitMessage);
    return;
  }
  const Paths paths = app.paths;
  BeginJob(app, "Renaming creation…", [&app, paths, record, new_name = std::move(new_name)](
                                               const ProgressFn&, CancelFlag&) {
    const Result result = RenameCreation(paths, record.kind, record.slot_index, new_name);
    std::optional<std::vector<CreationSaveRecord>> creations;
    if (result.ok) creations = ListCreationSaveRecords(paths);
    const auto backups = result.ok ? ListBackups(paths) : std::vector<BackupInfo>{};
    return JobOutcome{result, [&app, record, new_name, creations, backups] {
                        if (creations) app.creation_records = creations;
                        app.backups = backups;
                        app.creation_recovery_error.clear();
                        const char* label = record.kind == CreationSaveKind::kSuperstar ? "CAW" : "arena";
                        SetBanner(app, std::string("Renamed ") + label + " to \"" + new_name +
                                          "\". A verified save backup was made first.", true);
                      }};
  });
}

constexpr char kCloseGameCreationExportMessage[] = "Close WWE 2K14 before exporting creations.";

void StartExportCreation(AppState& app, CreationSaveRecord record, fs::path output) {
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, kCloseGameCreationExportMessage);
    return;
  }
  if (app.job.active.load()) {
    SetBanner(app, kDropWaitMessage);
    return;
  }
  const std::string extension = record.kind == CreationSaveKind::kSuperstar ? ".cas" : ".car";
  std::string output_extension = output.extension().string();
  std::transform(output_extension.begin(), output_extension.end(), output_extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (output_extension != extension) output += extension;
  const Paths paths = app.paths;
  BeginJob(app, "Exporting creation…", [&app, paths, record, output](const ProgressFn&, CancelFlag&) {
    CreationExportSummary summary;
    const Result result = ExportCreation(paths, record.kind, record.slot_index, output, &summary);
    PerfLog("creation export kind=%s slot=%u destination=%s failing_stage=%s",
            record.kind == CreationSaveKind::kSuperstar ? "superstar" : "arena", record.slot_index,
            PathUtf8(output).c_str(), result.ok ? "none" : result.stage.c_str());
    return JobOutcome{result, [&app, summary, output] {
                        const std::string label = summary.name.empty() ? "creation" : summary.name;
                        SetBanner(app, "Exported \"" + label + "\" to " + PathUtf8(output), true);
                      }};
  });
}

constexpr char kCloseGameStoriesMessage[] = "Close WWE 2K14 before importing a story.";

void StartRefreshStories(AppState& app) {
  if (app.job.active.load()) return;
  const Paths paths = app.paths;
  BeginJob(app, "Reading stories…", [&app, paths](const ProgressFn&, CancelFlag&) {
    auto stories = ListStories(paths);
    return JobOutcome{Result::Ok(), [&app, stories = std::move(stories)]() mutable {
                        app.story_list = std::move(stories);
                        app.story_list_loaded = true;
                      }};
  });
}

void StartImportStory(AppState& app, fs::path source) {
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, kCloseGameStoriesMessage);
    return;
  }
  if (app.job.active.load()) {
    SetBanner(app, kDropWaitMessage);
    return;
  }
  const Paths paths = app.paths;
  BeginJob(app, "Importing story…", [&app, paths, source = std::move(source)](const ProgressFn&, CancelFlag&) {
    uint32_t slot = UINT32_MAX;
    StoryChangeSummary summary;
    const Result result = ImportStory(paths, source, &slot, &summary);
    std::optional<std::vector<StoryInfo>> stories;
    if (result.ok) stories = ListStories(paths);
    const auto backups = result.ok ? ListBackups(paths) : std::vector<BackupInfo>{};
    return JobOutcome{result, [&app, summary, slot, stories = std::move(stories), backups]() mutable {
                        app.story_list = std::move(stories);
                        app.story_list_loaded = true;
                        app.backups = backups;
                        SetBanner(app, "Imported story \"" + summary.name + "\" into story slot " +
                                           std::to_string(slot + 1) + " (in-game listing not yet verified). Backup made.", true);
                      }};
  });
}

void StartExportStory(AppState& app, StoryInfo story, fs::path output) {
  if (app.job.active.load()) {
    SetBanner(app, kDropWaitMessage);
    return;
  }
  {
    std::string extension = output.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".sas") output += ".sas";
  }
  const Paths paths = app.paths;
  BeginJob(app, "Exporting story…", [&app, paths, story, output](const ProgressFn&, CancelFlag&) {
    StoryChangeSummary summary;
    const Result result = ExportStory(paths, story.slot_index, output, &summary);
    return JobOutcome{result, [&app, summary, output] {
                        SetBanner(app, "Exported \"" + summary.name + "\" to " + PathUtf8(output), true);
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
  if (!automatic && !SaveOperationsAllowed(GameProcessRunning(app.paths))) {
    SetBanner(app, kGameRunningBackupMessage);
    return;
  }
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

void StartScanInstallationImport(AppState& app, const fs::path& selected_folder) {
  app.import_list_open = false;
  const Paths paths = app.paths;
  BeginJob(app, "Checking the selected installation…", [&app, paths, selected_folder](
                                                       const ProgressFn&, CancelFlag&) {
    InstallationImportInfo info;
    const Result result = ScanInstallationImport(paths, selected_folder, &info);
    return JobOutcome{result, [&app, info = std::move(info)]() mutable {
                        app.import_info = std::move(info);
                        if (app.import_first_run) {
                          app.import_first_run = false;
                          app.first_run_offer_visible = true;
                          app.tab = Tab::GameFiles;
                        } else {
                          app.import_confirmation_pending = true;
                        }
                      }};
  });
}

void StartFindInstallationImports(AppState& app) {
  if (app.job.active.load()) return;
  app.import_list_open = true;
  app.installation_search_running = true;
  app.import_candidates.clear();
  const Paths paths = app.paths;
  BeginJob(app, "Searching...", [&app, paths](const ProgressFn& progress, CancelFlag& cancel) {
    std::vector<FoundInstallationImport> candidates = FindInstallationImports(paths, progress, cancel);
    return JobOutcome{Result::Ok(), [&app, candidates = std::move(candidates)]() mutable {
                        app.import_candidates = std::move(candidates);
                        app.installation_search_running = false;
                        app.import_list_open = true;
                      }};
  });
}

void StartWWE13ImportScan(AppState& app, const fs::path& selected_folder = {}) {
  if (app.job.active.load()) return;
  app.wwe13_import_open = true;
  app.wwe13_import_loading = true;
  app.wwe13_import_scan_done = false;
  const Paths paths = app.paths;
  BeginJob(app, "Looking for WWE '13 game files and add-ons…",
           [&app, paths, selected_folder](const ProgressFn& progress, CancelFlag& cancel) {
    WWE13ImportScan scan = ScanWWE13ImportSources(paths, selected_folder, progress, cancel);
    return JobOutcome{Result::Ok(), [&app, scan = std::move(scan)]() mutable {
                        PerfLog("wwe13 search: folders=%llu limit=%d cancelled=%d base=%s%s packs=%zu", static_cast<unsigned long long>(scan.directories_visited), scan.search_limit_reached ? 1 : 0, scan.cancelled ? 1 : 0, scan.base_found ? "found" : "none", scan.base_is_disc_image ? " (disc image)" : "", scan.packs.size());
                        app.wwe13_import_scan = std::move(scan);
                        app.wwe13_import_loading = false;
                        app.wwe13_import_scan_done = true;
                      }};
  });
}

void StartWWE13Import(AppState& app) {
  if (app.job.active.load()) return;
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, "Close WWE 2K14 before importing wrestlers.");
    return;
  }
  std::vector<uint32_t> selected;
  for (const WWE13ImportWrestler& wrestler : app.wwe13_import_scan.wrestlers)
    if (wrestler.available && wrestler.selected) selected.push_back(wrestler.source_id);
  if (selected.empty()) {
    SetBanner(app, "Choose at least one available WWE '13 wrestler to import.");
    return;
  }
  const Paths paths = app.paths;
  const fs::path game_folder = ActiveGameFolder(app);
  const WWE13ImportScan scan = app.wwe13_import_scan;
  BeginJob(app, "Importing WWE '13 wrestlers…",
           [&app, paths, game_folder, scan, selected](const ProgressFn& progress, CancelFlag& cancel) {
    WWE13ImportSummary summary;
    const Result result = RunWWE13Import(paths, game_folder, scan, selected, progress, cancel, &summary);
    return JobOutcome{result, [&app, summary = std::move(summary)] {
                        app.mods_loaded_for = Tab::Play;
                        std::string message = "Imported " + std::to_string(summary.imported_wrestlers) +
                                              " WWE '13 wrestlers.";
                        for (const std::string& line : summary.lines) message += " " + line;
                        SetBanner(app, std::move(message), true);
                      }};
  });
}

void StartUndoWWE13Import(AppState& app) {
  if (app.job.active.load()) return;
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, "Close WWE 2K14 before undoing the WWE '13 import.");
    return;
  }
  const Paths paths = app.paths;
  const fs::path game_folder = ActiveGameFolder(app);
  BeginJob(app, "Restoring original files…", [&app, paths, game_folder](const ProgressFn& progress,
                                                                         CancelFlag& cancel) {
    const Result result = UndoWWE13Import(paths, game_folder, progress, cancel);
    return JobOutcome{result, [&app] {
                        app.mods_loaded_for = Tab::Play;
                        SetBanner(app, "The WWE '13 import was undone and original files were restored.", true);
                      }};
  });
}

// WWE 2K15 import: the player's own WWE 2K15 files, found by contents (searched for on this PC when no folder is
// given, like the WWE '13 import); the wrestlers join the WWE '13 import's add-on folder (own undo set; see
// wwe15_install.h for the layer rules).
void StartWWE15ImportScan(AppState& app, const fs::path& folder = {}) {
  if (app.job.active.load()) return;
  app.wwe15_import_open = true;
  app.wwe15_import_loading = true;
  app.wwe15_import_scan_done = false;
  const Paths paths = app.paths;
  BeginJob(app, folder.empty() ? "Searching this PC for WWE 2K15 files…" : "Looking for WWE 2K15 files…",
           [&app, paths, folder](const ProgressFn& progress, CancelFlag& cancel) {
    WWE15ImportScan scan = ScanWWE15Import(paths, folder, progress, &cancel);
    return JobOutcome{Result::Ok(), [&app, scan = std::move(scan)]() mutable {
                        app.wwe15_selected.clear();
                        for (const auto& w : scan.wrestlers) if (w.available) app.wwe15_selected.insert(w.source_id);
                        PerfLog("wwe15 search: folders=%llu limit=%d cancelled=%d found=%s", static_cast<unsigned long long>(scan.directories_visited), scan.search_limit_reached ? 1 : 0, scan.cancelled ? 1 : 0, !scan.disc_image.empty() ? "disc image" : !scan.pac_folder.empty() ? "folder" : "none");
                        app.wwe15_import_scan = std::move(scan);
                        app.wwe15_import_loading = false;
                        app.wwe15_import_scan_done = true;
                      }};
  });
}

void StartWWE15Import(AppState& app) {
  if (app.job.active.load()) return;
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, "Close WWE 2K14 before importing wrestlers.");
    return;
  }
  const std::vector<uint32_t> selected(app.wwe15_selected.begin(), app.wwe15_selected.end());
  if (selected.empty()) {
    SetBanner(app, "Choose at least one WWE 2K15 wrestler to import.");
    return;
  }
  const Paths paths = app.paths;
  const fs::path game_folder = ActiveGameFolder(app);
  const WWE15ImportScan scan = app.wwe15_import_scan;
  WWE15ImportOptions options;
  options.existing_wrestler_looks = app.wwe15_looks;
  BeginJob(app, "Importing WWE 2K15 wrestlers…",
           [&app, paths, game_folder, scan, selected, options](const ProgressFn& progress, CancelFlag& cancel) {
    WWE15ImportSummary summary;
    const Result result = RunWWE15Import(paths, game_folder, scan, selected, progress, cancel, &summary, options);
    return JobOutcome{result, [&app, summary = std::move(summary)] {
                        app.mods_loaded_for = Tab::Play;
                        std::string message = "Imported " + std::to_string(summary.imported_wrestlers) + " WWE 2K15 wrestlers.";
                        if (!summary.skipped.empty()) {
                          message += " Left out " + std::to_string(summary.skipped.size()) + ":";
                          for (const std::string& line : summary.skipped) message += " " + line;
                        }
                        for (const std::string& line : summary.skipped) PerfLog("wwe15 import skipped: %s", line.c_str());
                        // GitHub #155: who kept a borrowed theme or a stand-in titantron
                        for (const std::string& line : summary.lines)
                          if (line.rfind("No WWE 2K15 theme", 0) == 0 || line.rfind("No WWE 2K15 titantron", 0) == 0) message += " " + line;
                        SetBanner(app, std::move(message), summary.skipped.empty());
                      }};
  });
}

void StartRefreshWWE15Import(AppState& app) {
  if (app.job.active.load()) return;
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, "Close WWE 2K14 before updating the WWE 2K15 wrestlers.");
    return;
  }
  const Paths paths = app.paths;
  const fs::path game_folder = ActiveGameFolder(app);
  BeginJob(app, "Updating the WWE 2K15 wrestlers…", [&app, paths, game_folder](const ProgressFn& progress, CancelFlag& cancel) {
    WWE15ImportSummary summary;
    const Result result = RefreshWWE15Import(paths, game_folder, progress, cancel, &summary);
    return JobOutcome{result, [&app, summary = std::move(summary)] {
                        app.mods_loaded_for = Tab::Play;
                        std::string message = "Updated " + std::to_string(summary.imported_wrestlers) + " WWE 2K15 wrestlers.";
                        for (const std::string& line : summary.lines)
                          if (line.rfind("No WWE 2K15 theme", 0) == 0 || line.rfind("No WWE 2K15 titantron", 0) == 0) message += " " + line;
                        SetBanner(app, std::move(message), true);
                      }};
  });
}

void StartUndoWWE15Import(AppState& app) {
  if (app.job.active.load()) return;
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, "Close WWE 2K14 before undoing the WWE 2K15 import.");
    return;
  }
  const Paths paths = app.paths;
  const fs::path game_folder = ActiveGameFolder(app);
  BeginJob(app, "Restoring original files…", [&app, paths, game_folder](const ProgressFn& progress, CancelFlag& cancel) {
    const Result result = UndoWWE15Import(paths, game_folder, progress, cancel);
    return JobOutcome{result, [&app] {
                        app.mods_loaded_for = Tab::Play;
                        SetBanner(app, "The WWE 2K15 import was undone and original files were restored.", true);
                      }};
  });
}

// WWE '13 arenas (G4): every arena WWE 2K14 lacks that the found WWE '13 folder has, onto the free arena numbers in order.
// The arena picker's list: what the WWE '13 base has and how many free arena numbers the game folder has.
void StartWWE13ArenaList(AppState& app) {
  if (app.job.active.load()) return;
  if (!app.wwe13_import_scan.base_found) {
    SetBanner(app, "WWE '13 was not found. Choose its disc image or folder first.");
    return;
  }
  app.wwe13_arenas_open = true;
  app.wwe13_arenas_loading = true;
  app.wwe13_arena_items.clear();
  app.wwe13_arena_selected.clear();
  app.wwe13_arena_error.clear();
  const fs::path game_folder = ActiveGameFolder(app);
  const fs::path wwe13_base = app.wwe13_import_scan.base_folder;
  BeginJob(app, "Reading the WWE '13 arenas…", [&app, game_folder, wwe13_base](const ProgressFn&, CancelFlag&) {
    std::string error;
    std::vector<WWE13ArenaItem> items = ListWWE13Arenas(wwe13_base, &error);
    const size_t free_numbers = FreeWWE13ArenaNumbers(game_folder).size();
    return JobOutcome{Result::Ok(), [&app, items = std::move(items), error, free_numbers]() mutable {
                        app.wwe13_arena_items = std::move(items);
                        app.wwe13_arena_free = free_numbers;
                        app.wwe13_arena_error = error;
                        for (const WWE13ArenaItem& item : app.wwe13_arena_items)
                          if (item.available && app.wwe13_arena_selected.size() < free_numbers)
                            app.wwe13_arena_selected.insert(item.source_id);
                        app.wwe13_arenas_loading = false;
                      }};
  });
}

// Own undo set; the wrestler import refuses to run or undo while arenas are imported (they share game files).
void StartWWE13ArenaImport(AppState& app) {
  if (app.job.active.load()) return;
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, "Close WWE 2K14 before importing arenas.");
    return;
  }
  if (!app.wwe13_import_scan.base_found) {
    SetBanner(app, "WWE '13 was not found. Choose its disc image or folder first.");
    return;
  }
  const Paths paths = app.paths;
  const fs::path game_folder = ActiveGameFolder(app);
  const fs::path wwe13_base = app.wwe13_import_scan.base_folder;
  const std::set<uint32_t> wanted = app.wwe13_arena_selected;
  if (wanted.empty()) {
    SetBanner(app, "Choose at least one WWE '13 arena to import.");
    return;
  }
  BeginJob(app, "Importing WWE '13 arenas…",
           [&app, paths, game_folder, wwe13_base, wanted](const ProgressFn& progress, CancelFlag& cancel) {
    std::string error;
    const std::vector<WWE13ArenaItem> items = ListWWE13Arenas(wwe13_base, &error);
    const size_t free_numbers = FreeWWE13ArenaNumbers(game_folder).size();
    std::vector<uint32_t> chosen;  // the picker's choice, in the '13 list order
    for (const WWE13ArenaItem& item : items)
      if (item.available && wanted.count(item.source_id) && chosen.size() < free_numbers) chosen.push_back(item.source_id);
    fs::path converter = paths.exe_dir / "wwe2k14-bink-convert";
#ifdef _WIN32
    converter += ".exe";
#endif
    std::error_code converter_error;
    if (!fs::is_regular_file(converter, converter_error) || converter_error) converter.clear();
    WWE13ArenaImportSummary summary;
    Result result = chosen.empty()
        ? Result::Fail(error.empty() ? "No WWE '13 arena files were found to import." : error)
        : RunWWE13ArenaImport(paths, game_folder, wwe13_base, chosen, converter, progress, cancel, &summary);
    const size_t count = chosen.size();
    return JobOutcome{result, [&app, count] {
                        app.mods_loaded_for = Tab::Play;
                        SetBanner(app, "Imported " + std::to_string(count) +
                                           " WWE '13 arenas. In Select Arena they are at the end of the WrestleMania page.", true);
                      }};
  });
}

void StartUndoWWE13ArenaImport(AppState& app) {
  if (app.job.active.load()) return;
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, "Close WWE 2K14 before undoing the WWE '13 arenas.");
    return;
  }
  const Paths paths = app.paths;
  const fs::path game_folder = ActiveGameFolder(app);
  BeginJob(app, "Restoring original arena files…", [&app, paths, game_folder](const ProgressFn& progress,
                                                                               CancelFlag& cancel) {
    const Result result = UndoWWE13ArenaImport(paths, game_folder, progress, cancel);
    return JobOutcome{result, [&app] {
                        app.mods_loaded_for = Tab::Play;
                        SetBanner(app, "The WWE '13 arenas were undone and original files were restored.", true);
                      }};
  });
}

void StartExtraSuperstarHeads(AppState& app, bool enable) {
  if (app.job.active.load()) return;
  if (GameProcessRunning(app.paths)) {
    SetBanner(app, "Close WWE 2K14 before adding or undoing extra Superstar Heads.");
    return;
  }
  const Paths paths = app.paths;
  const fs::path game_folder = ActiveGameFolder(app);
  BeginJob(app, enable ? "Adding extra Superstar Heads…" : "Restoring original Superstar Heads…",
           [&app, paths, game_folder, enable](const ProgressFn& progress, CancelFlag& cancel) {
    const Result result = enable
        ? ApplyExtraSuperstarHeads(paths, game_folder, progress, cancel)
        : UndoExtraSuperstarHeads(paths, progress, cancel);
    return JobOutcome{result, [&app, paths, game_folder, enable] {
                        app.extra_superstar_heads = InspectExtraSuperstarHeads(paths, game_folder);
                        app.mods_loaded_for = Tab::Play;
                        SetBanner(app, enable
                                           ? "The extra Superstar Heads were added."
                                           : "The extra Superstar Heads were undone and the original files were restored.",
                                  true);
                      }};
  });
}

// First start: search quietly-but-visibly for a previous installation; the newest one is offered for import.
void StartFirstRunInstallationSearch(AppState& app) {
  if (app.job.active.load()) return;
  app.first_run_search_pending = false;
  const Paths paths = app.paths;
  BeginJob(app, "Looking for your previous WWE 2K14 installation…", [&app, paths](const ProgressFn& progress,
                                                                                  CancelFlag& cancel) {
    std::vector<FoundInstallationImport> candidates = FindInstallationImports(paths, progress, cancel);
    return JobOutcome{Result::Ok(), [&app, candidates = std::move(candidates)]() mutable {
                        app.import_candidates = candidates;
                        const auto newest = std::max_element(
                            candidates.begin(), candidates.end(),
                            [](const FoundInstallationImport& a, const FoundInstallationImport& b) {
                              return a.newest_save_time < b.newest_save_time;
                            });
                        if (newest != candidates.end()) {
                          app.first_run_candidate = newest->folder;
                          app.first_run_scan_pending = true;
                        } else {
                          SaveSettings(app);  // nothing to bring over: do not search again on the next start
                        }
                      }};
  });
}

void StartImportInstallation(AppState& app) {
  const Paths paths = app.paths;
  const InstallationImportInfo info = app.import_info;
  BeginJob(app, "Importing from another installation…", [&app, paths, info](
                                                          const ProgressFn& progress, CancelFlag& cancel) {
    fs::path backup;
    const Result result = ImportInstallation(paths, info, progress, cancel, &backup);
    const std::vector<BackupInfo> backups = result.ok ? ListBackups(paths) : std::vector<BackupInfo>{};
    std::optional<std::vector<CreationSaveRecord>> creations;
    if (result.ok) creations = ListCreationSaveRecords(paths);
    return JobOutcome{result, [&app, paths, backups, backup, creations] {
                        app.backups = backups;
                        app.creation_records = creations;
                        app.settings = LoadSettings(paths);
                        app.controls = LoadKeyboardControls(paths);
                        app.refresh_game_files_pending = true;  // the imported game folder (with its title update)
                        std::string message = "Your saved game data and settings were imported.";
                        if (!backup.empty()) message += " A backup was saved in " + PathUtf8(backup) + ".";
                        SetBanner(app, std::move(message), true);
                      }};
  });
}

void StartRestoreBackup(AppState& app, fs::path file) {
  if (!SaveOperationsAllowed(GameProcessRunning(app.paths))) {
    SetBanner(app, kGameRunningRestoreMessage);
    return;
  }
  const Paths paths = app.paths;
  BeginJob(app, "Restoring saves…", [&app, paths, file = std::move(file)](const ProgressFn& progress, CancelFlag&) {
    const Result result = RestoreBackup(paths, file);
    const std::vector<BackupInfo> backups = result.ok ? ListBackups(paths) : std::vector<BackupInfo>{};
    std::optional<std::vector<CreationSaveRecord>> creations;
    if (result.ok) creations = ListCreationSaveRecords(paths);
    std::optional<std::vector<StoryInfo>> stories;
    if (result.ok) stories = ListStories(paths);
    std::optional<LogoList> logos;
    if (result.ok) logos = ListLogoImages(paths);
    const Settings settings = result.ok ? LoadSettings(paths) : Settings{};
    return JobOutcome{result, [&app, backups, creations, stories, logos = std::move(logos), settings]() mutable {
                         app.backups = backups;
                         app.creation_records = creations;
                         app.settings = settings;
                         app.story_list = std::move(stories);
                         app.story_list_loaded = true;
                         SetLogoList(app, std::move(logos));
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
    case DialogKind::ImportFolder:
      StartScanInstallationImport(app, paths.front());
      break;
    case DialogKind::WWE13ImportFolder:
    case DialogKind::WWE13ImportDisc:
      StartWWE13ImportScan(app, paths.front());
      break;
    case DialogKind::WWE15ImportFolder:
    case DialogKind::WWE15ImportDisc:
      StartWWE15ImportScan(app, paths.front());
      break;
    case DialogKind::Backup:
      StartImportBackup(app, paths.front());
      break;
    case DialogKind::CreationSuperstarFolder:
    case DialogKind::CreationSuperstarFile:
      app.pending_import_creation_kind = CreationSaveKind::kSuperstar;
      app.pending_import_creation_folder = paths.front();
      app.confirm_import_creation = true;
      app.tab = Tab::CreationSuite;
      break;
    case DialogKind::CreationArenaFolder:
    case DialogKind::CreationArenaFile:
      app.pending_import_creation_kind = CreationSaveKind::kArena;
      app.pending_import_creation_folder = paths.front();
      app.confirm_import_creation = true;
      app.tab = Tab::CreationSuite;
      break;
    case DialogKind::ImageFile:
      StartAddLogos(app, std::move(paths), g_logo_import_size);
      break;
    case DialogKind::PaintImportFile:
      StartLoadPaintImage(app, paths.front());
      break;
    case DialogKind::PaintSavePng:
      StartSavePaintPng(app, paths.front());
      break;
    case DialogKind::StoryImportFile:
      app.pending_import_story = paths.front();
      app.confirm_import_story = true;
      app.tab = Tab::CreationSuite;
      break;
    case DialogKind::CreationExportFile:
      StartExportCreation(app, app.pending_export_creation, paths.front());
      break;
    case DialogKind::StoryExportFile:
      StartExportStory(app, app.pending_export_story, paths.front());
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
  if (app.tab == Tab::CreationSuite) {
    std::vector<fs::path> files;
    for (const auto& path : dropped) files.push_back(Utf8Path(path));
    if (g_suite_tab == kSuiteTabImages) {
      StartAddLogos(app, std::move(files), g_logo_import_size);
    } else if (g_suite_tab == kSuiteTabStories) {
      app.pending_import_story = files.front();  // same confirmation as the Import story button
      app.confirm_import_story = true;
    } else {
      StartAddSongs(app, std::move(files));
    }
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
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
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
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextUnformatted(title);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  if (hint && *hint) {
    DrawSecondaryWrapped(app, hint);
  }
}

void DrawNavButton(AppState& app, Tab tab, const char* label, float padding) {
  std::string upper(label);
  for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  ImGui::PushFont(app.fonts.oswald_medium);
  const float width = ImGui::CalcTextSize(upper.c_str()).x + padding;
  const std::string id = "##nav-" + upper;
  if (MainEventTheme::SlantedButton(id.c_str(), upper.c_str(), ImVec2(width, S(app,44)), app.tab == tab)) {
    app.tab = tab;
    app.banner.clear();
  }
  ImGui::PopFont();
}

const char* PictureSizeName(PictureSize value) {
  switch (value) {
    case PictureSize::k360p: return "360p";
    case PictureSize::k480p: return "480p";
    case PictureSize::k540p: return "540p";
    case PictureSize::k720p: return "720p";
    case PictureSize::k1080p: return "1080p";
    case PictureSize::k1440p: return "1440p";
    case PictureSize::k4k: return "4K";
  }
  return "720p";
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
  if (value == DisplayMode::kBorderless) return "Borderless window";
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
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
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
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
  ImGui::BeginChild("active-tab-banner", ImVec2(0, S(app, 38.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetCursorPos(S(app, 12.0f, 9.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, app.banner_success ? kSuccess : kText);
  ImGui::TextWrapped("%s", app.banner.c_str());
  ImGui::PopStyleColor();
  ImGui::SameLine(ImGui::GetWindowWidth() - S(app, 42.0f));
  if (LoggedSmallButton("x")) app.banner.clear();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::Spacing();
}

bool AccentButton(const AppState& app, const char* label, ImVec2 size = ImVec2(0, 0)) {
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.93f, 0.20f, 0.29f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.73f, 0.10f, 0.18f, 1.0f));
  const bool clicked = LoggedButton(label, size);
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
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
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
  if (LoggedButton("What's new", button_size)) app.update_notes_open = true;
  ImGui::SameLine();
  if (AccentButton(app, "Update", button_size)) StartUpdate(app);
  ImGui::SameLine();
  if (LoggedButton("Not now", button_size)) app.update_hidden_this_session = true;
  ImGui::SameLine();
  if (LoggedButton("Skip this version", ImVec2(S(app, 150.0f), S(app, 30.0f)))) {
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
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
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
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
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
    if (LoggedButton("Close", ImVec2(close_width, S(app, 38.0f)))) {
      app.launch_warning.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::End();
  }
}

const char* FileStateName(FileState state);

bool LinkButton(const char* label);

// Decodes the embedded tile icons once (SDL's built-in PNG loader). A failed icon is simply not drawn.
void LoadTileImages(AppState& app) {
  for (size_t i = 0; i < app.tile_textures.size(); ++i) {
    SDL_IOStream* io = SDL_IOFromConstMem(kTileImages[i].data, kTileImages[i].size);
    SDL_Surface* surface = io ? SDL_LoadPNG_IO(io, true) : nullptr;
    if (!surface) continue;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(app.renderer, surface);
    if (texture) {
      SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
      SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
      app.tile_textures[i] = texture;
      app.tile_texture_sizes[i] = ImVec2(static_cast<float>(surface->w), static_cast<float>(surface->h));
    }
    SDL_DestroySurface(surface);
  }
}

// Draws tile icon `index` over a tile, aspect preserved and centred (the PNGs are full-tile transparent overlays).
void DrawTileImage(const AppState& app, size_t index, ImVec2 pos, ImVec2 size) {
  SDL_Texture* texture = app.tile_textures[index];
  const ImVec2 image = app.tile_texture_sizes[index];
  if (!texture || image.x <= 0.0f || image.y <= 0.0f) return;
  const float scale = std::min(size.x / image.x, size.y / image.y);
  const ImVec2 drawn(image.x * scale, image.y * scale);
  const ImVec2 at(pos.x + (size.x - drawn.x) * 0.5f, pos.y + (size.y - drawn.y) * 0.5f);
  ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)), at,
                                       ImVec2(at.x + drawn.x, at.y + drawn.y));
}

std::string FrameRateName(FrameRate rate) {
  return rate == FrameRate::kLocked30 ? "Locked 30" : rate == FrameRate::kStandard30_60 ? "30 / 60" : "60 everywhere";
}

// v1.7.0: the renderer as the player picked it (Settings page names).
const char* RendererLabel(const Settings& s) {
  if (s.renderer == Renderer::kXbox) return "Xbox";
#if WWE2K14_NEW_RENDERER_UI
#ifndef _WIN32
  return "Vulkan";  // Linux: the new renderer is Vulkan only
#endif
  switch (s.native_api) {
    case NativeApi::kD3D12: return "DirectX 12";
    case NativeApi::kD3D11: return "DirectX 11";
    case NativeApi::kVulkan: break;
  }
  return "Vulkan";
#else
  return "Native";
#endif
}

// One-line game status, shown in the header pill and on the Play tile.
std::string StatusLine(const AppState& app) {
  const Settings current = ActiveSettings(app);
  const bool native = current.renderer == Renderer::kNative;
  const std::string picture = native ? PictureSizeName(current.picture_size) : ResolutionName(current.resolution);
  return std::string(app.game_files.ready() ? "Game ready" : "Game files need attention") + " \xC2\xB7 " +
         RendererLabel(current) + " \xC2\xB7 " + picture + " \xC2\xB7 " +
         FrameRateName(native ? current.frame_rate : current.xbox_frame_rate) +
         (app.settings.mods_enabled ? "" : " \xC2\xB7 Mods off");
}

// Title + hint text inside a tile at the top-left (icon tiles) or bottom-left (text tiles).
void TileText(const AppState& app, ImVec2 pos, ImVec2 size, const char* title, const std::string& hint,
              float title_px, ImVec4 hint_color = kMuted) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImFont* title_font = app.fonts.oswald_bold;
  ImFont* hint_font = app.fonts.barlow_small;
  const float pad = S(app, 22.0f);
  const float title_h = S(app, title_px);
  const float hint_h = hint.empty() ? 0.0f : hint_font->LegacySize + S(app, 4.0f);
  const float y = pos.y + size.y - pad - hint_h - title_h;
  d->AddText(title_font, title_h, ImVec2(pos.x + pad, y), ImGui::GetColorU32(kText), title);
  if (!hint.empty())
    d->AddText(hint_font, hint_font->LegacySize, ImVec2(pos.x + pad, y + title_h + S(app, 4.0f)),
               ImGui::GetColorU32(hint_color), hint.c_str());
}

void DrawPlay(AppState& app) {
  const Settings current = ActiveSettings(app);
  const float width = ImGui::GetContentRegionAvail().x;
  const float gap = S(app, 14.0f);
  const float col = (width - 3.0f * gap) / 4.0f;
  const bool compact = ImGui::GetIO().DisplaySize.y < S(app, 700.0f);
  const float row1 = S(app, compact ? 220.0f : 290.0f), row2 = S(app, compact ? 130.0f : 160.0f);
  const ImVec2 origin = ImGui::GetCursorPos();

  // PLAY: the hero tile (2 x 2).
  const ImVec2 hero_size(2.0f * col + gap, row1 + gap + row2);
  const ImVec2 hero_screen = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(!app.game_files.ready() || app.job.active.load());
  if (MainEventTheme::Tile("##tile-play", hero_size, true)) StartLaunch(app);
  ImGui::EndDisabled();
  DrawTileImage(app, 0, hero_screen, hero_size);
  {
    ImDrawList* d = ImGui::GetWindowDrawList();
    const float pad = S(app, 30.0f);
    const float big = S(app, compact ? 96.0f : 120.0f);
    const std::string reason = "Settings picked for your " + CurrentRecommendation(app).reason;
    const float hint_y = hero_screen.y + hero_size.y - pad - app.fonts.barlow_small->LegacySize;
    const float big_y = hint_y - S(app, 8.0f) - big;
    const float kicker_y = big_y - S(app, 26.0f);
    d->AddText(app.fonts.oswald_semibold, S(app, 17.0f), ImVec2(hero_screen.x + pad, kicker_y),
               ImGui::GetColorU32(Rgba(240,184,188)),
               app.game_files.ready() ? "PRESS A OR ENTER" : "ADD YOUR GAME FILES FIRST");
    d->AddText(app.fonts.oswald_bold, big, ImVec2(hero_screen.x + pad, big_y), ImGui::GetColorU32(kText), "PLAY");
    d->AddText(app.fonts.barlow_small, app.fonts.barlow_small->LegacySize, ImVec2(hero_screen.x + pad, hint_y),
               ImGui::GetColorU32(Rgba(233,201,203)), reason.c_str());
  }

  // Right half: Creation Suite + Settings (row 1), Controls + Saves/Game files (row 2).
  auto tile = [&](const char* id, ImVec2 local, ImVec2 size, const char* title, const std::string& hint,
                  float title_px, Tab target, size_t image, ImVec4 hint_color = kMuted) {
    ImGui::SetCursorPos(ImVec2(origin.x + local.x, origin.y + local.y));
    const ImVec2 screen = ImGui::GetCursorScreenPos();
    if (MainEventTheme::Tile(id, size, false)) {
      app.tab = target;
      app.banner.clear();
    }
    DrawTileImage(app, image, screen, size);
    TileText(app, screen, size, title, hint, title_px, hint_color);
  };
  const float x2 = 2.0f * (col + gap), x3 = 3.0f * (col + gap);
  tile("##tile-creation", ImVec2(x2, 0), ImVec2(col, row1), "CREATION SUITE",
       "Superstars, arenas, music, logos, stories", 34.0f, Tab::CreationSuite, 1);
  const std::string picture = current.renderer == Renderer::kNative ? PictureSizeName(current.picture_size)
                                                                     : ResolutionName(current.resolution);
  tile("##tile-settings", ImVec2(x3, 0), ImVec2(col, row1), "SETTINGS",
       std::string(RendererLabel(current)) + " \xC2\xB7 " + picture + " \xC2\xB7 " +
           FrameRateName(current.renderer == Renderer::kNative ? current.frame_rate : current.xbox_frame_rate),
       34.0f, Tab::Settings, 2);
  const float half = (row2 - gap) / 2.0f;
  tile("##tile-controls", ImVec2(x2, row1 + gap), ImVec2(col, half), "CONTROLS", "", 24.0f, Tab::Controls, 3);
  tile("##tile-mods", ImVec2(x2, row1 + gap + half + gap), ImVec2(col, half), "MODS", "", 24.0f, Tab::Mods, 6);
  tile("##tile-saves", ImVec2(x3, row1 + gap), ImVec2(col, half), "SAVES", "", 24.0f, Tab::Saves, 4);
  tile("##tile-files", ImVec2(x3, row1 + gap + half + gap), ImVec2(col, half), "GAME FILES", "", 24.0f,
       Tab::GameFiles, 5);
  if (!app.game_files.ready()) {  // flag the tile that needs attention
    // Next to the GAME FILES title (the tile's right side holds the folder icon).
    const float title_w = app.fonts.oswald_bold->CalcTextSizeA(S(app, 24.0f), FLT_MAX, 0.0f, "GAME FILES").x;
    const ImVec2 at(ImGui::GetWindowPos().x + origin.x + x3 + S(app, 22.0f) + title_w + S(app, 14.0f),
                    ImGui::GetWindowPos().y + origin.y - ImGui::GetScrollY() + row1 + gap + half + gap + half / 2.0f);
    ImGui::GetWindowDrawList()->AddCircleFilled(at, S(app, 6.0f), ImGui::GetColorU32(kDanger));
  }

  ImGui::SetCursorPos(ImVec2(origin.x, origin.y + row1 + gap + row2 + S(app, 16.0f)));
  if (LoggedCheckbox("Start straight away next time", &app.settings.auto_start)) SaveSettings(app);
  if (app.first_run_offer_visible) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
    ImGui::TextUnformatted("Your previous WWE 2K14 installation was found.");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (AccentButton(app, "Bring everything over...", S(app, 230.0f, 32.0f))) app.tab = Tab::GameFiles;
  }

  ImGui::Spacing();
  DrawOneDriveWarning(app);
  DrawUpdateBanner(app);
  if (app.update_job) DrawProgress(app);
}

void DrawSegment(const AppState& app, const char* label, bool selected, float width,
                 const std::function<void()>& action) {
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Button, selected ? kAccent : kRaised);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? kAccent : Rgba(70, 29, 33));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
  if (LoggedButton(label, ImVec2(width, S(app, 34.0f)))) action();
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar();
}

void RequestRecommendedSettings(AppState& app) {
  const Settings current = ActiveSettings(app);
  app.recommendation_current = current;
  app.recommendation_gpus = ListGpus();
  // The whole PC, not just the selected card: on a laptop with an iGPU and a stronger card, recommend the stronger
  // card (and switching to it) rather than settings for whichever card happens to be selected (owner 11-02).
  const Recommendation fresh = RecommendedSettings(app.recommendation_gpus);
  app.recommendation_suggestion = fresh.settings;
  app.recommendation_reason = fresh.reason;
  app.recommendation_gpu_id = fresh.gpu_id;
  app.recommendation_confirmation_pending = true;
}

void DrawSettingsSectionHeading(const AppState& app, const char* title, const char* hint) {
  ImGui::Spacing();
  ImGui::PushFont(app.fonts.oswald_semibold);
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  ImGui::TextUnformatted(title);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  if (hint && *hint) DrawSecondaryWrapped(app, hint, kSecondary);
  ImGui::Separator();
  ImGui::Spacing();
}

fs::path ModMusicPack(const AppState& app) {
  const fs::path folder = app.settings.game_folder.empty() ? app.paths.default_game_folder : app.settings.game_folder;
  return folder / "sound" / "DLC_01.pck";
}

// Mods page. A section per kind of mod; add a row to `sections` and the tabs appear (Creation Suite's layout).
// The small tag beside a name: which game this wrestler came from. Same game, same colour, whatever is installed.
void DrawPill(AppState& app, const std::string& label, const ImVec4& colour) {
  const ImVec2 text = ImGui::CalcTextSize(label.c_str());
  const ImVec2 pad = S(app, 7.0f, 2.0f);
  const ImVec2 size(text.x + pad.x * 2.0f, text.y + pad.y * 2.0f);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), ImGui::ColorConvertFloat4ToU32(colour), S(app, 4.0f));
  draw->AddText(ImVec2(at.x + pad.x, at.y + pad.y), IM_COL32(255, 255, 255, 255), label.c_str());
  ImGui::Dummy(size);
}

void DrawGameTag(AppState& app, const std::string& game) {
  static const ImVec4 colours[] = {Rgba(176, 58, 58), Rgba(46, 112, 168), Rgba(52, 130, 92), Rgba(138, 88, 168), Rgba(170, 112, 40)};
  uint32_t hash = 2166136261u;
  for (const char c : game) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
  // Fixed so '13 and 2K15, the first two, can never land on one colour.
  const size_t colour = game == "WWE '13" ? 0 : game == "WWE 2K15" ? 1 : hash % std::size(colours);
  DrawPill(app, game, colours[colour]);
}

void DrawSaveTag(AppState& app, bool modded) {
  DrawPill(app, modded ? "MODDED" : "NORMAL", modded ? Rgba(176, 58, 58) : Rgba(52, 130, 92));
}

void DrawModEditPopup(AppState& app) {
  if (app.mod_editor_popup_requested) {
    ImGui::OpenPopup("Edit theme volume");
    app.mod_editor_popup_requested = false;
  }
  ImGui::PushStyleColor(ImGuiCol_PopupBg, kPanel);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kRaised);
  const bool open = ImGui::BeginPopupModal("Edit theme volume", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleColor(2);
  if (!open) return;

  const auto wrestler = std::find_if(app.mod_wrestlers.begin(), app.mod_wrestlers.end(), [&app](const ModWrestler& w) {
    return app.edit_mod_wrestler && w.id == *app.edit_mod_wrestler;
  });
  if (wrestler == app.mod_wrestlers.end()) {
    ImGui::TextUnformatted("Select one wrestler to edit its theme volume.");
  } else {
    ImGui::PushFont(app.fonts.barlow_semibold);
    ImGui::TextUnformatted(wrestler->name.c_str());
    ImGui::PopFont();
    if (!wrestler->game.empty()) {
      ImGui::SameLine(0.0f, S(app, 10.0f));
      DrawGameTag(app, wrestler->game);
    }
    ImGui::Spacing();
    const auto theme = app.mod_themes.find(wrestler->id);
    if (theme == app.mod_themes.end()) {
      ImGui::TextWrapped("No volume control for this song.");
    } else {
      const auto level = app.mod_theme_levels.find(wrestler->id);
      const float base = level == app.mod_theme_levels.end() ? 0.0f : level->second;  // 0 on the slider is WWE 2K14's level
      const auto apply = [&app, &theme](float db) {
        const uint32_t sound = theme->second.sound;
        const Result result = GameProcessRunning(app.paths)
                                  ? Result::Fail("Close WWE 2K14 first, then try again.")
                                  : SetModThemeVolume(ModMusicPack(app), theme->second.offset, db);
        if (!result.ok) {
          SetBanner(app, result.error);
          app.mods_loaded_for = Tab::Play;  // reload what is really in the file
          ImGui::CloseCurrentPopup();
        } else {
          for (auto& [id, other] : app.mod_themes)
            if (other.sound == sound) other.db = db;  // wrestlers on one song move together
        }
      };
      ImGui::SetNextItemWidth(S(app, 360.0f));
      const std::string slider = "Theme volume##mod-edit-" + std::to_string(wrestler->id);
      float shown = theme->second.db - base;
      if (ImGui::SliderFloat(slider.c_str(), &shown, kModThemeMinDb - base, kModThemeMaxDb - base, "%+.1f dB"))
        theme->second.db = shown + base;
      if (ImGui::IsItemDeactivatedAfterEdit()) apply(theme->second.db);
      if (level != app.mod_theme_levels.end()) {
        ImGui::TextColored(kMuted, "0 is WWE 2K14's own theme volume.");
        ImGui::BeginDisabled(std::abs(theme->second.db - base) < 0.05f);
        if (LoggedButton("Back to WWE 2K14's volume", S(app, 300.0f, 34.0f))) apply(base);
        ImGui::EndDisabled();
      }
      std::string partners;
      for (const auto& [id, other] : app.mod_themes) {
        if (id == wrestler->id || other.sound != theme->second.sound) continue;
        for (const ModWrestler& w : app.mod_wrestlers)
          if (w.id == id) partners += (partners.empty() ? "" : ", ") + w.name;
      }
      if (!partners.empty()) ImGui::TextColored(kMuted, "Shared with %s", partners.c_str());
    }
  }
  ImGui::Spacing();
  if (LoggedButton("Done##mod-edit-done", S(app, 120.0f, 38.0f))) {
    app.edit_mod_wrestler.reset();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void DrawModWrestlers(AppState& app) {
  std::vector<ModWrestler>& list = app.mod_wrestlers;
  size_t men = 0, divas = 0, removed = 0;
  for (const ModWrestler& w : list) {
    if (!w.enabled) ++removed;
    else if (w.female) ++divas;
    else ++men;
  }
  const size_t in_game = men + divas;
  ImGui::SetNextItemWidth(S(app, 300.0f));
  ImGui::InputTextWithHint("##mods-search", "Search by name", app.mods_search.data(), app.mods_search.size());
  ImGui::SameLine(0.0f, S(app, 14.0f));
  const std::string labels[4] = {"All " + std::to_string(in_game), "Men " + std::to_string(men),
                                 "Divas " + std::to_string(divas), "Removed " + std::to_string(removed)};
  const float segment_width = (ImGui::GetContentRegionAvail().x - 3.0f * ImGui::GetStyle().ItemSpacing.x) / 4.0f;
  for (int i = 0; i < 4; ++i) {
    if (i) ImGui::SameLine();
    DrawSegment(app, labels[i].c_str(), app.mods_filter == i, segment_width, [&app, i] { app.mods_filter = i; });
  }
  DrawSecondaryWrapped(app,
                       app.settings.imports_in_grid
                           ? "Imported wrestlers are in the main select grid and in Downloaded Superstars. To change a theme's "
                             "volume, tick one wrestler and choose Edit. Changes apply the next time you press Play."
                           : "Imported wrestlers are in Downloaded Superstars on the select screen. To change a theme's volume, "
                             "tick one wrestler and choose Edit. Changes apply the next time you press Play.",
                       kMuted);
  ImGui::Spacing();
  auto lower = [](std::string text) {
    for (char& c : text) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    return text;
  };
  const std::string needle = lower(app.mods_search.data());
  std::vector<size_t> rows;
  for (size_t i = 0; i < list.size(); ++i) {
    const ModWrestler& w = list[i];
    const bool shown = app.mods_filter == 3 ? !w.enabled
                       : !w.enabled        ? false
                       : app.mods_filter == 1 ? !w.female
                       : app.mods_filter == 2 ? w.female
                                              : true;
    if (shown && (needle.empty() || lower(w.name).find(needle) != std::string::npos)) rows.push_back(i);
  }

  size_t selected = 0;
  std::vector<uint32_t> selected_enabled, selected_removed;
  for (const ModWrestler& wrestler : list) {
    if (!app.selected_mod_wrestlers.contains(wrestler.id)) continue;
    ++selected;
    (wrestler.enabled ? selected_enabled : selected_removed).push_back(wrestler.id);
  }
  if (LoggedButton("Select all shown", S(app, 155.0f, 34.0f))) {
    for (const size_t index : rows) app.selected_mod_wrestlers.insert(list[index].id);
  }
  ImGui::SameLine();
  if (LoggedButton("Clear", S(app, 76.0f, 34.0f))) app.selected_mod_wrestlers.clear();
  ImGui::SameLine();
  ImGui::BeginDisabled(selected != 1);
  if (LoggedButton("Edit", S(app, 76.0f, 34.0f))) {
    for (const ModWrestler& wrestler : list)
      if (app.selected_mod_wrestlers.contains(wrestler.id)) app.edit_mod_wrestler = wrestler.id;
    app.mod_editor_popup_requested = true;
  }
  ImGui::EndDisabled();
  const bool add_back = app.mods_filter == 3;
  const std::vector<uint32_t>& action_ids = add_back ? selected_removed : selected_enabled;
  const std::string action = std::string(add_back ? "Add back selected (" : "Remove selected (") +
                             std::to_string(action_ids.size()) + ")";
  ImGui::SameLine();
  ImGui::BeginDisabled(action_ids.empty());
  if (LoggedButton(action.c_str(), S(app, 190.0f, 34.0f))) {
    if (GameProcessRunning(app.paths)) {
      SetBanner(app, "Close WWE 2K14 first, then try again.");
    } else {
      const Result result = SetModWrestlersEnabled(app.paths, action_ids, add_back);
      if (!result.ok) {
        SetBanner(app, result.error);
      } else {
        for (ModWrestler& wrestler : list)
          if (std::find(action_ids.begin(), action_ids.end(), wrestler.id) != action_ids.end()) wrestler.enabled = add_back;
        for (const uint32_t id : action_ids) app.selected_mod_wrestlers.erase(id);  // done with them: nobody stays ticked out of sight
        SetBanner(app, std::to_string(action_ids.size()) + (add_back ? " wrestlers added back." : " wrestlers removed."), true);
      }
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine(ImGui::GetContentRegionMax().x - S(app, 150.0f));
  if (app.mods_list_popout) {
    if (AccentButton(app, "Close", S(app, 150.0f, 34.0f))) {
      app.mods_list_popout = false;
      ImGui::CloseCurrentPopup();
    }
  } else if (LoggedButton("Pop out list", S(app, 150.0f, 34.0f))) {
    app.mods_list_popout = true;
    app.mods_list_popout_requested = true;
  }
  ImGui::Spacing();

  if (rows.empty()) {
    DrawSecondaryWrapped(app, !needle.empty()         ? "Nobody matches that search."
                              : app.mods_filter == 3 ? "Nobody removed. Anyone you remove is listed here, ready to add back."
                                                     : "Nobody here. Check the Removed list.",
                         kSecondary);
    DrawModEditPopup(app);
    return;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, S(app, 14.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(app, 20.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, S(app, 44.0f));  // easy to catch with 100 rows
  if (ImGui::BeginTable("mods-wrestlers", 2,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_ScrollY,
                        ImVec2(0.0f, ImGui::GetContentRegionAvail().y - S(app, app.job.active.load() ? 112.0f : 6.0f)))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("LIST", ImGuiTableColumnFlags_WidthFixed, S(app, 110.0f));
    ImGui::TableHeadersRow();
    for (const size_t index : rows) {
      const ModWrestler wrestler = list[index];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, S(app, 32.0f));
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      bool ticked = app.selected_mod_wrestlers.contains(wrestler.id);
      const std::string tick = "##select-mod-" + std::to_string(wrestler.id);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgba(74, 74, 84));  // visible on both row stripes
      if (LoggedCheckbox(tick.c_str(), &ticked)) {
        if (ticked) app.selected_mod_wrestlers.insert(wrestler.id);
        else app.selected_mod_wrestlers.erase(wrestler.id);
      }
      ImGui::PopStyleColor();
      ImGui::SameLine(0.0f, S(app, 9.0f));
      ImGui::TextUnformatted(wrestler.name.c_str());
      if (!wrestler.game.empty()) {
        ImGui::SameLine(0.0f, S(app, 10.0f));
        DrawGameTag(app, wrestler.game);
      }
      ImGui::TableSetColumnIndex(1);
      ImGui::TextColored(kSecondary, "%s", wrestler.female ? "Divas" : "Superstars");
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar(3);
  DrawModEditPopup(app);
}

// One card per game on the Import tab: what is in, the import button, and the undo buttons.
void DrawImportCard(AppState& app, const char* id, const char* title, size_t wrestlers, const char* about,
                    const std::function<void()>& extra) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
  ImGui::BeginChild(id, ImVec2(0.0f, S(app, 200.0f)), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  ImGui::PushFont(app.fonts.oswald_semibold);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  if (wrestlers)
    ImGui::TextColored(kAccent, "%zu wrestler%s imported", wrestlers, wrestlers == 1 ? "" : "s");
  else
    ImGui::TextColored(kMuted, "Nothing imported yet");
  DrawSecondaryWrapped(app, about, kSecondary);
  ImGui::Spacing();
  extra();
  ImGui::EndChild();
}

void DrawModImports(AppState& app) {
  size_t from13 = 0, from15 = 0;
  for (const ModWrestler& w : app.mod_wrestlers) {
    if (w.game == "WWE '13") ++from13;
    else if (w.game == "WWE 2K15") ++from15;
  }
  const bool busy = app.job.active.load();
  const bool have13 = HasWWE13Import(app.paths), arenas13 = HasWWE13ArenaImport(app.paths), have15 = HasWWE15Import(app.paths);
  const bool outdated15 = have15 && WWE15ImportOutdated(app.paths);
  if (ImGui::BeginTable("mods-import-cards", 2, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextColumn();
    DrawImportCard(app, "import-card-13", "WWE '13", from13,
                   arenas13 ? "Wrestlers and arenas from your WWE '13 files. Arenas are imported."
                            : "Wrestlers and arenas from your WWE '13 files.",
                   [&] {
                     const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                     ImGui::BeginDisabled(busy);
                     if (AccentButton(app, "Import from WWE '13", ImVec2(-1.0f, S(app, 40.0f))))
                       app.wwe13_import_requested = true;
                     if (have13 && LoggedButton("Undo wrestlers##undo13", ImVec2(arenas13 ? half : -1.0f, S(app, 34.0f))))
                       StartUndoWWE13Import(app);
                     if (arenas13) {
                       if (have13) ImGui::SameLine();
                       if (LoggedButton("Undo arenas##undo13a", ImVec2(have13 ? half : -1.0f, S(app, 34.0f))))
                         StartUndoWWE13ArenaImport(app);
                     }
                     ImGui::EndDisabled();
                   });
    ImGui::TableNextColumn();
    DrawImportCard(app, "import-card-15", "WWE 2K15", from15,
                   "Wrestlers from your WWE 2K15 files, with their own themes and ring-announcer names.", [&] {
                     ImGui::BeginDisabled(busy);
                     // Once in, the picker only says "already imported": the card offers Undo instead.
                     if (!have15 && AccentButton(app, "Import from WWE 2K15", ImVec2(-1.0f, S(app, 40.0f))))
                       app.wwe15_import_requested = true;
                     if (have15 && outdated15) {
                       // GitHub #155: imports made with an older launcher keep a stand-in titantron (and, before 1.6.1,
                       // a borrowed theme) until they are imported again.
                       DrawSecondaryWrapped(app, "These wrestlers were imported with an older launcher. Update them to get "
                                                 "their own WWE 2K15 titantrons, themes and ring-announcer names.");
                       if (AccentButton(app, "Update wrestlers##refresh15", ImVec2(-1.0f, S(app, 40.0f))))
                         StartRefreshWWE15Import(app);
                     }
                     if (have15 && LoggedButton("Undo wrestlers##undo15", ImVec2(-1.0f, S(app, 34.0f)))) StartUndoWWE15Import(app);
                     ImGui::EndDisabled();
                   });
    ImGui::EndTable();
  }
  ImGui::Spacing();
  DrawSecondaryWrapped(app,
                       "Import from either game in any order. Undo puts WWE 2K14's own files back. Changes apply the next "
                       "time you press Play.",
                       kMuted);
}

void DrawSettingsCheckbox(AppState& app, const char* label, bool* value, const char* hint,
                          const std::function<void()>& on_change);

void DrawModGameplayBody(AppState& app);
// Scrolls inside its own box so the Mods tab bar above keeps its width (no page scrollbar).
void DrawModGameplay(AppState& app) {
  ImGui::BeginChild("mods-gameplay", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - S(app, app.job.active.load() ? 112.0f : 6.0f)));
  DrawModGameplayBody(app);
  ImGui::EndChild();
}

void DrawModGameplayBody(AppState& app) {
  DrawSecondaryWrapped(app, "Changes apply the next time you press Play.", kMuted);
  DrawSettingsSectionHeading(app, "Matches", nullptr);
  DrawSettingsCheckbox(app, "Weapon wheel in Hell in a Cell", &app.settings.hiac_weapon_wheel,
                       "Pull any object from under the ring, as in Extreme Rules: chairs, ladders, tables and more. "
                       "Computer wrestlers can use it too. Exhibition matches only.",
                       [&app] { SaveSettings(app); });
  ImGui::BeginDisabled(!app.settings.hiac_weapon_wheel);
  DrawSettingsCheckbox(app, "Also in other match types", &app.settings.weapon_wheel_more,
                       "One on One, tag, Triple Threat, Fatal 4-Way, 6-Man, Handicap, Special Referee, I Quit, Ladder, "
                       "Table and TLC. Never in Steel Cage, Elimination Chamber, Royal Rumble, Battle Royal, Inferno "
                       "or Backstage Brawl. Exhibition matches only.",
                       [&app] { SaveSettings(app); });
  ImGui::EndDisabled();
  // Owner 11-16: the select-screen option before Unlocks (grouped, tidier page).
  DrawSettingsSectionHeading(app, "Select screen", nullptr);
  // Owner 11-16: imported wrestlers (WWE '13, WWE 2K15) in the main Superstars grid's free places, with their faces.
  DrawSettingsCheckbox(app, "Imported wrestlers in the main select grid", &app.settings.imports_in_grid,
                       "Puts your imported wrestlers in the Superstars and Divas grids, so you can pick them without opening "
                       "Downloaded Superstars. They fill the free places first; press Down past the bottom row for the "
                       "next page of imports, and Up past the top row to go back.",
                       [&app] { SaveSettings(app); });
  DrawSettingsSectionHeading(app, "Unlocks", nullptr);
  // Owner 11-15: unlock everything as a mod. The game keeps what it unlocks in the save, so turning it on first makes a
  // kept (not automatic) save backup; turning it off later does not lock things again.
  DrawSettingsCheckbox(app, "Unlock everything", &app.settings.unlock_all,
                       "Opens every Unlockable (superstars, arenas, attires, championships and more) and every 30 Years of "
                       "WrestleMania era and match. Your save keeps what is unlocked, even if you untick this later, so "
                       "a save backup is made first.",
                       [&app] {
                         SaveSettings(app);
                         if (!app.settings.unlock_all) return;
                         const Paths paths = app.paths;
                         BeginJob(app, "Backing up saves before unlocking…", [&app, paths](const ProgressFn&, CancelFlag&) {
                           BackupInfo created;
                           const Result result = BackupSaves(paths, false, &created);
                           if (!result.ok) {
                             // No backup, no unlocking: the option goes back off (the text promises a backup first).
                             const std::string why = result.error;
                             return JobOutcome{Result::Ok(), [&app, why] {
                                                 app.settings.unlock_all = false;
                                                 SaveSettings(app);
                                                 SetBanner(app, "Unlock everything was turned off again: the save backup "
                                                                "could not be made" + (why.empty() ? std::string(".") : " (" + why + ")."));
                                               }};
                           }
                           std::vector<BackupInfo> backups = ListBackups(paths);
                           return JobOutcome{result, [&app, backups = std::move(backups)]() mutable {
                                               app.backups = std::move(backups);
                                             }};
                         });
                       });
  // v1.6.0 (owner 11-11): Full editing with Superstar Heads and Extra Superstar Heads are left out of this release
  // (easier design wanted). The code stays; the Mods page does not show them.
}

void DrawWWE13ImportPicker(AppState& app) {
  if (!app.wwe13_import_open) return;
  ImGui::SetNextWindowSize(S(app, 760.0f, 640.0f), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Import from WWE '13##picker", &app.wwe13_import_open,
                              ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;

  ImGui::PushFont(app.fonts.oswald_semibold);
  ImGui::TextUnformatted("IMPORT FROM WWE '13");
  ImGui::PopFont();
  DrawSecondaryWrapped(app,
                       "Choose the wrestlers to bring over. The scan only checks your game folders; it does not change them.",
                       kSecondary);
  if (HasWWE15Import(app.paths))  // owner 11-16: import in any order
    DrawSecondaryWrapped(app,
                         "Your WWE 2K15 wrestlers stay: the import takes them out for a moment and puts the same ones back "
                         "from your WWE 2K15 files, so it takes a few minutes longer.",
                         kSecondary);
  ImGui::Spacing();

  if (app.wwe13_import_loading) {
    uint64_t folders = 0;
    std::string where;
    {
      std::lock_guard lock(app.job.mutex);
      folders = app.job.done;
      where = app.job.label;
    }
    ImGui::PushFont(app.fonts.oswald_semibold);
    const bool stopping = app.job.cancel.load();
    ImGui::TextColored(kAccent, "%s%s", stopping ? "STOPPING THE SEARCH" : "SEARCHING THIS PC FOR WWE '13 FILES",
                       std::array<const char*, 4>{"", ".", "..", "..."}[static_cast<size_t>(ImGui::GetTime() * 2.0) % 4]);
    ImGui::PopFont();
    ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.0f, S(app, 10.0f)), "");
    ImGui::TextColored(kSecondary, "%llu folders checked", static_cast<unsigned long long>(folders));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kMuted, "%s", where.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    DrawSecondaryWrapped(app, "No need to wait: press Choose a disc image to pick your .iso file, or Choose a folder for extracted files. "
                              "The search checks every drive and stops as soon as it finds your game.", kSecondary);
  } else if (app.wwe13_import_scan_done) {
    if (app.wwe13_import_scan.cancelled)
      ImGui::TextColored(kMuted, "Search stopped. Choose a disc image or a folder, or search again.");
    if (app.wwe13_import_scan.base_found) {
      ImGui::TextColored(kSuccess, app.wwe13_import_scan.base_is_disc_image ? "Base game found (disc image)" : "Base game found");
      ImGui::SameLine();
      ImGui::TextColored(kMuted, "%s", PathUtf8(app.wwe13_import_scan.base_folder).c_str());
    } else {
      ImGui::TextColored(kDanger, "Base game not found");
      DrawSecondaryWrapped(app, "Press Choose a disc image to pick your WWE '13 .iso file, or Choose a folder with the extracted WWE '13 files.",
                           kSecondary);
    }
    const size_t available_count = static_cast<size_t>(std::count_if(
        app.wwe13_import_scan.wrestlers.begin(), app.wwe13_import_scan.wrestlers.end(),
        [](const WWE13ImportWrestler& wrestler) { return wrestler.available; }));
    ImGui::TextColored(kSecondary, "%zu wrestlers available · %zu add-on packs found",
                       available_count, app.wwe13_import_scan.packs.size());
    if (!app.wwe13_import_scan.error.empty())
      DrawSecondaryWrapped(app, app.wwe13_import_scan.error.c_str(), kDanger);
    if (!app.wwe13_import_scan.packs.empty()) {
      for (const WWE13ImportPack& pack : app.wwe13_import_scan.packs)
        ImGui::BulletText("%s", pack.display_name.c_str());
    }

    std::vector<WWE13ImportWrestler*> available;
    for (WWE13ImportWrestler& wrestler : app.wwe13_import_scan.wrestlers)
      if (wrestler.available) available.push_back(&wrestler);
    std::sort(available.begin(), available.end(), [](const auto* left, const auto* right) {
      std::string a = left->name, b = right->name;
      std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return a == b ? left->source_id < right->source_id : a < b;
    });
    ImGui::Spacing();
    if (available.empty()) {
      DrawSecondaryWrapped(app, "No wrestler files were found in the selected folders.", kSecondary);
    } else {
      if (LoggedButton("Select all"))
        for (WWE13ImportWrestler* wrestler : available) wrestler->selected = true;
      ImGui::SameLine();
      if (LoggedButton("Clear"))
        for (WWE13ImportWrestler* wrestler : available) wrestler->selected = false;
      ImGui::BeginChild("wwe13-import-wrestlers", ImVec2(0.0f, S(app, 292.0f)), true);
      for (WWE13ImportWrestler* wrestler : available) {
        ImGui::PushID(static_cast<int>(wrestler->source_id));
        LoggedCheckbox("##selected", &wrestler->selected);
        ImGui::SameLine(0.0f, S(app, 10.0f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(wrestler->name.c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - S(app, 166.0f));
        ImGui::TextColored(kMuted, "%s", wrestler->source_label.c_str());
        ImGui::PopID();
      }
      ImGui::EndChild();
    }
    const size_t missing_count = app.wwe13_import_scan.wrestlers.size() - available.size();
    if (missing_count > 0 && ImGui::CollapsingHeader(("Unavailable wrestlers (" +
                                                       std::to_string(missing_count) + ")##wwe13-missing").c_str())) {
      ImGui::BeginChild("wwe13-import-missing", ImVec2(0.0f, S(app, 156.0f)), true);
      for (const WWE13ImportWrestler& wrestler : app.wwe13_import_scan.wrestlers) {
        if (wrestler.available) continue;
        ImGui::PushID(static_cast<int>(wrestler.source_id));
        ImGui::TextColored(kMuted, "%s", wrestler.name.c_str());
        ImGui::SameLine(0.0f, S(app, 10.0f));
        ImGui::TextColored(kSecondary, "%s", wrestler.missing_reason.empty()
                                                   ? "Required WWE '13 files were not found."
                                                   : wrestler.missing_reason.c_str());
        ImGui::PopID();
      }
      ImGui::EndChild();
    }
  }

  ImGui::Spacing();
  if (app.wwe13_import_choose_pending && !app.job.active.load()) {
    app.wwe13_import_choose_pending = false;
    RequestImportPicker(app, app.wwe13_import_choose_kind);
  }
  ImGui::BeginDisabled(app.job.active.load() && !app.wwe13_import_loading);
  for (const auto& [label, kind] : {std::pair{"Choose a disc image", DialogKind::WWE13ImportDisc},
                                     std::pair{"Choose a folder", DialogKind::WWE13ImportFolder}}) {
    if (kind == DialogKind::WWE13ImportFolder) ImGui::SameLine();
    if (LoggedButton(label)) {
      app.wwe13_import_choose_kind = kind;
      if (app.job.active.load()) {  // stop the search first; the picker opens as soon as it has stopped
        app.wwe13_import_choose_pending = true;
        app.job.cancel.store(true);
      } else {
        RequestImportPicker(app, kind);
      }
    }
  }
  ImGui::SameLine();
  if (app.wwe13_import_loading) {
    ImGui::BeginDisabled(app.job.cancel.load());
    if (LoggedButton("Stop search")) app.job.cancel.store(true);
    ImGui::EndDisabled();
  } else if (LoggedButton("Search again")) {
    StartWWE13ImportScan(app);
  }
  ImGui::SameLine(ImGui::GetWindowWidth() - S(app, 220.0f));
  const bool can_import = app.wwe13_import_scan_done && !app.wwe13_import_loading &&
                          app.wwe13_import_scan.base_found &&
                          std::any_of(app.wwe13_import_scan.wrestlers.begin(), app.wwe13_import_scan.wrestlers.end(),
                                      [](const WWE13ImportWrestler& wrestler) {
                                        return wrestler.available && wrestler.selected;
                                      });
  ImGui::BeginDisabled(!can_import || app.job.active.load());
  if (AccentButton(app, "Import", S(app, 112.0f, 34.0f))) {
    StartWWE13Import(app);
    // The progress bar is on the Mods page behind this window: get out of its way.
    if (app.job.active.load()) {
      app.wwe13_import_open = false;
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  DrawSecondaryWrapped(app, "Original files are backed up and can be restored with Undo WWE '13 import.", kSecondary);
  ImGui::Separator();
  {
    const bool arenas_in = HasWWE13ArenaImport(app.paths);
    const bool can_import_arenas = app.wwe13_import_scan_done && !app.wwe13_import_loading &&
                                   app.wwe13_import_scan.base_found && !arenas_in;
    ImGui::BeginDisabled(!can_import_arenas || app.job.active.load());
    if (LoggedButton("Import WWE '13 arenas", S(app, 230.0f, 34.0f))) {
      StartWWE13ArenaList(app);  // opens the arena picker
      if (app.wwe13_arenas_open) {
        app.wwe13_import_open = false;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndDisabled();
    DrawSecondaryWrapped(app, arenas_in
                                  ? "The WWE '13 arenas are already imported. Undo them on the Mods page (Import tab) to import again."
                                  : "Experimental: adds the WWE '13 arenas WWE 2K14 does not have (up to 25, including the "
                                    "Attitude Era arenas); you choose which. Known issues: no titantron on the big screen, and "
                                    "wrestlers can walk through the ramp. Import wrestlers first: while arenas are imported, wrestlers cannot be "
                                    "imported or undone. Undo WWE '13 arenas restores the original files.",
                         kSecondary);
  }
  if (app.wwe13_import_scan.search_limit_reached)
    DrawSecondaryWrapped(app, "The search stopped after five minutes and may have missed files. Choose your disc image or folder directly.",
                         kMuted);
  if (LoggedButton("Close", S(app, 96.0f, 32.0f))) {
    app.wwe13_import_open = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void DrawWWE13ArenaPicker(AppState& app) {
  if (!app.wwe13_arenas_open) return;
  if (!ImGui::IsPopupOpen("Import WWE '13 arenas##arenas")) ImGui::OpenPopup("Import WWE '13 arenas##arenas");
  // Fixed size like the other import windows: sizing to the contents happened while the list was still loading,
  // which squashed it (owner laptop, 200 % display scale, 11-13).
  ImGui::SetNextWindowSize(S(app, 640.0f, 600.0f), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Import WWE '13 arenas##arenas", &app.wwe13_arenas_open,
                              ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;
  ImGui::PushFont(app.fonts.oswald_semibold);
  ImGui::TextUnformatted("IMPORT WWE '13 ARENAS");
  ImGui::PopFont();
  DrawSecondaryWrapped(app, "Experimental. Known issues: no titantron on the arena's big screen, and wrestlers can walk "
                            "through the entrance ramp. Tick the arenas you want. In Select Arena they are at the end of the WrestleMania page. "
                            "Undo arenas on the Mods page (Import tab) restores the original files.", kSecondary);
  ImGui::Spacing();
  std::vector<const WWE13ArenaItem*> available;
  size_t missing = 0;
  for (const WWE13ArenaItem& item : app.wwe13_arena_items) {
    if (item.available) available.push_back(&item);
    else ++missing;
  }
  if (app.wwe13_arenas_loading) {
    ImGui::TextColored(kAccent, "Reading the WWE '13 arenas%s",
                       std::array<const char*, 4>{"", ".", "..", "..."}[static_cast<size_t>(ImGui::GetTime() * 2.0) % 4]);
  } else if (available.empty()) {
    DrawSecondaryWrapped(app, app.wwe13_arena_error.empty() ? "No WWE '13 arena files were found to import."
                                                            : app.wwe13_arena_error.c_str(), kDanger);
  } else {
    if (LoggedButton("Select all"))
      for (const WWE13ArenaItem* item : available)
        if (app.wwe13_arena_selected.size() < app.wwe13_arena_free) app.wwe13_arena_selected.insert(item->source_id);
    ImGui::SameLine();
    if (LoggedButton("Clear")) app.wwe13_arena_selected.clear();
    ImGui::BeginChild("wwe13-arenas", ImVec2(0.0f, S(app, 300.0f)), true);
    for (const WWE13ArenaItem* item : available) {
      ImGui::PushID(static_cast<int>(item->source_id));
      bool ticked = app.wwe13_arena_selected.count(item->source_id) > 0;
      if (LoggedCheckbox("##arena", &ticked)) {
        if (ticked) app.wwe13_arena_selected.insert(item->source_id);
        else app.wwe13_arena_selected.erase(item->source_id);
      }
      ImGui::SameLine(0.0f, S(app, 10.0f));
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(item->name.c_str());
      ImGui::PopID();
    }
    ImGui::EndChild();
    if (missing > 0)
      ImGui::TextColored(kMuted, "%zu more WWE '13 arenas were not found in your WWE '13 files.", missing);
  }
  const size_t chosen = app.wwe13_arena_selected.size();
  const bool too_many = chosen > app.wwe13_arena_free;
  if (!app.wwe13_arenas_loading && !available.empty())
    ImGui::TextColored(too_many ? kDanger : kSecondary, too_many ? "%zu chosen, but only %zu arena slots are free."
                                                                 : "%zu chosen (room for %zu).",
                       chosen, app.wwe13_arena_free);
  ImGui::Spacing();
  ImGui::BeginDisabled(app.wwe13_arenas_loading || chosen == 0 || too_many || app.job.active.load());
  if (AccentButton(app, "Import", S(app, 112.0f, 34.0f))) {
    StartWWE13ArenaImport(app);
    if (app.job.active.load()) {  // the progress bar is on the Mods page behind this window
      app.wwe13_arenas_open = false;
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (LoggedButton("Close", S(app, 96.0f, 32.0f))) {
    app.wwe13_arenas_open = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void DrawWWE15ImportPicker(AppState& app) {
  if (!app.wwe15_import_open) {
    // closed with the window's X while searching: stop the search too
    if (app.wwe15_import_loading && app.job.active.load()) app.job.cancel.store(true);
    app.wwe15_import_choose_pending = false;
    return;
  }
  if (!ImGui::IsPopupOpen("Import from WWE 2K15##picker")) ImGui::OpenPopup("Import from WWE 2K15##picker");
  ImGui::SetNextWindowSize(S(app, 760.0f, 640.0f), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Import from WWE 2K15##picker", &app.wwe15_import_open,
                              ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;
  ImGui::PushFont(app.fonts.oswald_semibold);
  ImGui::TextUnformatted("IMPORT FROM WWE 2K15");
  ImGui::PopFont();
  DrawSecondaryWrapped(app,
                       "Brings WWE 2K15 wrestlers over from your own WWE 2K15 (Xbox 360) game. The launcher searches "
                       "this PC for your WWE 2K15 disc image (.iso) or extracted disc, or you can choose the .iso file or folder; the scan "
                       "only reads it.",
                       kSecondary);
  const bool have_15 = HasWWE15Import(app.paths), have_arenas = HasWWE13ArenaImport(app.paths);
  if (have_15) DrawSecondaryWrapped(app, "WWE 2K15 wrestlers are already imported. To change the list, use Undo wrestlers on the WWE 2K15 card (Mods page, Import tab), then import again.", kDanger);
  if (have_arenas) DrawSecondaryWrapped(app, "Undo the WWE '13 arenas first (they change some of the same files), then import them again afterwards.", kDanger);
  ImGui::Spacing();
  if (app.wwe15_import_loading) {
    uint64_t folders = 0;
    std::string where;
    {
      std::lock_guard lock(app.job.mutex);
      folders = app.job.done;
      where = app.job.label;
    }
    ImGui::PushFont(app.fonts.oswald_semibold);
    const bool stopping = app.job.cancel.load();
    ImGui::TextColored(kAccent, "%s%s", stopping ? "STOPPING THE SEARCH" : "SEARCHING THIS PC FOR WWE 2K15 FILES",
                       std::array<const char*, 4>{"", ".", "..", "..."}[static_cast<size_t>(ImGui::GetTime() * 2.0) % 4]);
    ImGui::PopFont();
    ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.0f, S(app, 10.0f)), "");
    ImGui::TextColored(kSecondary, "%llu folders checked", static_cast<unsigned long long>(folders));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kMuted, "%s", where.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    DrawSecondaryWrapped(app, "No need to wait: press Choose a disc image to pick your .iso file, or Choose a folder for extracted files. "
                              "The search checks every drive and stops as soon as it finds your game.", kSecondary);
  } else if (!app.wwe15_import_scan_done) {
    DrawSecondaryWrapped(app, "Press Search this PC, Choose a disc image or Choose a folder to start.", kSecondary);
  } else if (!app.wwe15_import_scan.error.empty()) {
    DrawSecondaryWrapped(app, app.wwe15_import_scan.error.c_str(), kDanger);
  } else {
    const bool from_disc = !app.wwe15_import_scan.disc_image.empty();
    ImGui::TextColored(kSuccess, from_disc ? "WWE 2K15 disc image found" : "WWE 2K15 files found");
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "%s",
                       PathUtf8(from_disc ? app.wwe15_import_scan.disc_image : app.wwe15_import_scan.pac_folder).c_str());
    std::vector<const WWE15ImportWrestler*> available;
    for (const auto& w : app.wwe15_import_scan.wrestlers) if (w.available) available.push_back(&w);
    ImGui::TextColored(kSecondary, "%zu wrestlers available · %zu chosen", available.size(), app.wwe15_selected.size());
    if (LoggedButton("Select all")) for (const auto* w : available) app.wwe15_selected.insert(w->source_id);
    ImGui::SameLine();
    if (LoggedButton("Clear")) app.wwe15_selected.clear();
    ImGui::BeginChild("wwe15-import-wrestlers", ImVec2(0.0f, S(app, 250.0f)), true);
    for (const auto* w : available) {
      ImGui::PushID(static_cast<int>(w->source_id));
      bool on = app.wwe15_selected.count(w->source_id) > 0;
      if (LoggedCheckbox("##selected", &on)) { if (on) app.wwe15_selected.insert(w->source_id); else app.wwe15_selected.erase(w->source_id); }
      ImGui::SameLine(0.0f, S(app, 10.0f));
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(w->name.c_str());
      ImGui::SameLine(ImGui::GetWindowWidth() - S(app, 120.0f));
      ImGui::TextColored(kMuted, "slot %u", w->target_id);
      ImGui::PopID();
    }
    ImGui::EndChild();
    for (const auto& w : app.wwe15_import_scan.wrestlers)
      if (!w.available) DrawSecondaryWrapped(app, (w.name + ": " + w.missing_reason).c_str(), kMuted);
  }
  ImGui::Spacing();
  DrawSecondaryWrapped(app,
                       "Before importing: play WWE 2K14 once and save, so a save file exists. Choose everyone you want in one "
                       "go: to add more later, undo this import and import again with the longer list. Wrestlers also in the "
                       "WWE '13 import stay separate (marked '15). For now they use their stand-in's entrance music and a "
                       "stand-in titantron. Undo WWE 2K15 import restores the original files.",
                       kSecondary);
  ImGui::Spacing();
  ImGui::BeginDisabled(app.job.active.load());
  ImGui::Checkbox("Also give wrestlers already in WWE 2K14 their 2K15 look", &app.wwe15_looks);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Updates the attires of 47 wrestlers WWE 2K14 already has (The Rock, Seth Rollins, Big Show ...)\n"
                      "to their WWE 2K15 versions. Undo WWE 2K15 import puts the originals back.");
  ImGui::Spacing();
  if (app.wwe15_import_choose_pending && !app.job.active.load()) {
    app.wwe15_import_choose_pending = false;
    RequestImportPicker(app, app.wwe15_import_choose_kind);
  }
  ImGui::BeginDisabled(app.job.active.load() && !app.wwe15_import_loading);
  for (const auto& [label, kind] : {std::pair{"Choose a disc image", DialogKind::WWE15ImportDisc},
                                     std::pair{"Choose a folder", DialogKind::WWE15ImportFolder}}) {
    if (kind == DialogKind::WWE15ImportFolder) ImGui::SameLine();
    if (LoggedButton(label)) {
      app.wwe15_import_choose_kind = kind;
      if (app.job.active.load()) {  // stop the search first; the picker opens as soon as it has stopped
        app.wwe15_import_choose_pending = true;
        app.job.cancel.store(true);
      } else {
        RequestImportPicker(app, kind);
      }
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (app.wwe15_import_loading) {
    ImGui::BeginDisabled(app.job.cancel.load());
    if (LoggedButton("Stop search")) app.job.cancel.store(true);
    ImGui::EndDisabled();
  } else {
    ImGui::BeginDisabled(app.job.active.load());
    if (LoggedButton(app.wwe15_import_scan_done ? "Search again" : "Search this PC")) StartWWE15ImportScan(app);
    ImGui::EndDisabled();
  }
  ImGui::SameLine(ImGui::GetWindowWidth() - S(app, 220.0f));
  const bool can_import = app.wwe15_import_scan_done && !app.wwe15_import_loading && app.wwe15_import_scan.error.empty() &&
                          !app.wwe15_selected.empty() && !have_15 && !have_arenas && !app.job.active.load();
  ImGui::BeginDisabled(!can_import);
  if (AccentButton(app, "Import", S(app, 112.0f, 34.0f))) {
    StartWWE15Import(app);
    if (app.job.active.load()) {
      app.wwe15_import_open = false;
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (LoggedButton("Close", S(app, 96.0f, 32.0f))) {
    if (app.wwe15_import_loading) app.job.cancel.store(true);  // closing stops the search
    app.wwe15_import_choose_pending = false;
    app.wwe15_import_open = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void DrawMods(AppState& app) {
  if (app.mods_loaded_for != Tab::Mods) {
    app.selected_mod_wrestlers.clear();
    app.edit_mod_wrestler.reset();
    app.mod_editor_popup_requested = false;
    app.mod_wrestlers = LoadModWrestlers(app.paths);
    app.extra_superstar_heads = InspectExtraSuperstarHeads(app.paths, ActiveGameFolder(app));
    std::vector<uint32_t> ids;
    for (const ModWrestler& wrestler : app.mod_wrestlers) ids.push_back(wrestler.id);
    app.mod_themes = ReadModThemes(ModMusicPack(app), ids);
    app.mod_theme_levels = ReadModThemeLevels(app.paths);
    app.mods_loaded_for = Tab::Mods;
  }
  // Owner 11-18: mods are experimental and not fully functional - say so at the top of the page, every time.
  {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.23f, 0.15f, 0.07f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.48f, 0.31f, 0.12f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, S(app, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(app, 14.0f, 9.0f));
    ImGui::BeginChild("mods-experimental", ImVec2(0.0f, S(app, 62.0f)), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(app.fonts.barlow_semibold);
    ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.32f, 1.0f), "Experimental - not fully functional");
    ImGui::PopFont();
    ImGui::PushFont(app.fonts.barlow_small);
    ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
    ImGui::TextWrapped("Imported wrestlers and arenas and the gameplay options are still in progress: some moves, "
                       "entrances, titantrons or music can be wrong, and some combinations can crash. Keep a backup of your saves.");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::Spacing();
  }
  // Owner 11-18: Mods on/off. Off = the game starts from its original files with separate saves; imports stay installed.
  DrawSettingsCheckbox(app, "Use mods", &app.settings.mods_enabled,
                       "Off: the original game with its own separate saves. Your imports stay installed for when you switch back.",
                       [&app] { SaveSettings(app); });
  if (!app.settings.mods_enabled) {
    DrawSecondaryWrapped(app, "Mods are off: the next Play starts the original game. The settings below are kept for later.");
    ImGui::Spacing();
  }
  // Owner 11-16: tidier page. The import buttons used to share one crowded row above the tabs; now each game has its own
  // card on the Import tab, and the page is three tabs: Wrestlers, Import, Gameplay.
  struct Section {
    std::string label;
    void (*draw)(AppState&);
    bool present;
  };
  const std::array<Section, 3> sections{{
      {"Wrestlers (" + std::to_string(app.mod_wrestlers.size()) + ")", DrawModWrestlers, !app.mod_wrestlers.empty()},
      {"Import", DrawModImports, true},
      {"Gameplay", DrawModGameplay, true}}};
  static size_t selected = [] {
#if !defined(WWE2K14_LAUNCHER_NO_TEST_TOOLS)
    if (const char* v = std::getenv("WWE2K14_TEST_MODS_SECTION"); v && *v >= '0' && *v <= '2') return size_t(*v - '0');
#endif
    return size_t{0};
  }();
  size_t shown = 0;
  for (const Section& section : sections) shown += section.present;
  if (selected >= sections.size() || !sections[selected].present) {
    selected = 0;
    while (!sections[selected].present) ++selected;
  }
  {
    const float width = (ImGui::GetContentRegionAvail().x - static_cast<float>(shown - 1) * ImGui::GetStyle().ItemSpacing.x) /
                        static_cast<float>(shown);
    bool first = true;
    for (size_t i = 0; i < sections.size(); ++i) {
      if (!sections[i].present) continue;
      if (!first) ImGui::SameLine();
      first = false;
      DrawSegment(app, sections[i].label.c_str(), selected == i, width, [i] { selected = i; });
    }
    ImGui::Spacing();
  }
  // One copy of the list at a time: its Edit window must open from the copy on top.
  if (!app.mods_list_popout) sections[selected].draw(app);
  if (app.mods_list_popout) {
    if (app.mods_list_popout_requested) {
      ImGui::OpenPopup("Superstars##mods-popout");
      app.mods_list_popout_requested = false;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(display.x * 0.94f, display.y * 0.94f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(kPanel.x, kPanel.y, kPanel.z, 1.0f));  // solid: nothing shows through
    const bool shown_popup = ImGui::BeginPopupModal("Superstars##mods-popout", nullptr,
                                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleColor();
    if (shown_popup) {
      DrawModWrestlers(app);
      ImGui::EndPopup();
    }
  }
  if (app.wwe13_import_requested) {
    app.wwe13_import_requested = false;
    app.wwe13_import_open = true;
    ImGui::OpenPopup("Import from WWE '13##picker");
    StartWWE13ImportScan(app);
  }
  if (app.wwe15_import_requested) {
    app.wwe15_import_requested = false;
    app.wwe15_import_open = true;
    ImGui::OpenPopup("Import from WWE 2K15##picker");
    // Search this PC straight away, like the WWE '13 import (not when an import is already in: nothing to choose).
    if (!HasWWE15Import(app.paths)) StartWWE15ImportScan(app);
  }
  DrawWWE13ImportPicker(app);
  DrawWWE13ArenaPicker(app);
  DrawWWE15ImportPicker(app);
  DrawProgress(app);
}

void DrawSettingsCheckbox(AppState& app, const char* label, bool* value, const char* hint,
                          const std::function<void()>& on_change) {
  ImGui::PushStyleColor(ImGuiCol_FrameBg, kWindow);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kPanel);
  ImGui::PushStyleColor(ImGuiCol_Border, kMuted);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, S(app, 1.0f));
  if (LoggedCheckbox(label, value)) on_change();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  DrawSecondaryWrapped(app, hint, kSecondary);
  ImGui::Spacing();
}

void DrawRecommendedSettingsPopup(AppState& app) {
  if (app.recommendation_confirmation_pending) {
    ImGui::OpenPopup("Recommended settings");
    app.recommendation_confirmation_pending = false;
  }
  if (ImGui::BeginPopupModal("Recommended settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped("Recommended settings for %s:",
                       app.recommendation_reason.empty() ? "this PC" : app.recommendation_reason.c_str());
    std::vector<RecommendationChange> changes =
        CompareRecommendedSettings(app.recommendation_current, app.recommendation_suggestion);
    // Switch the Graphics card setting when another card is selected (Automatic counts as a change on a PC with
    // more than one card, because the game then uses whichever card Windows lists first).
    const bool switch_card = !app.recommendation_gpu_id.empty() && app.settings.gpu_id != app.recommendation_gpu_id &&
                             !(app.settings.gpu_id.empty() && app.recommendation_gpus.size() == 1);
    if (switch_card) {
      std::string from = "Automatic", to = app.recommendation_gpu_id;
      for (const GpuInfo& gpu : app.recommendation_gpus) {
        if (gpu.id == app.settings.gpu_id) from = gpu.name;
        if (gpu.id == app.recommendation_gpu_id) to = gpu.name;
      }
      changes.insert(changes.begin(), RecommendationChange{"Graphics card", from, to, !app.settings.gpu_id.empty()});
    }
    if (changes.empty()) {
      DrawSecondaryWrapped(app, "Your current settings already match the recommendation.", kSecondary);
    } else {
      for (const RecommendationChange& change : changes) {
        std::string line = change.label + ": " + change.current + " → " + change.recommended;
        if (change.manually_changed) line += " (you changed this by hand)";
        DrawSecondaryWrapped(app, line.c_str(), kSecondary);
      }
      DrawSecondaryWrapped(app, "Your current choices change only if you apply these settings.", kSecondary);
    }
    ImGui::Spacing();
    const bool can_apply = !changes.empty();
    if (!can_apply) ImGui::BeginDisabled();
    if (AccentButton(app, "Apply recommended settings", ImVec2(S(app, 220.0f), S(app, 36.0f)))) {
      app.gpus = app.recommendation_gpus;
      app.recommendation = {app.recommendation_suggestion, app.recommendation_reason, app.recommendation_gpu_id};
      app.settings = ApplyRecommendedSettings(app.recommendation_current, app.recommendation_suggestion, true);
      if (switch_card) app.settings.gpu_id = app.recommendation_gpu_id;
      SaveSettings(app);
      SetBanner(app, "Recommended settings updated for this PC.", true);
      ImGui::CloseCurrentPopup();
    }
    if (!can_apply) ImGui::EndDisabled();
    ImGui::SameLine();
    if (LoggedButton(can_apply ? "Cancel" : "Done", ImVec2(S(app, 90.0f), S(app, 36.0f)))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

// v1.7.0 (owner 2026-11-22, design D): Settings as a game-style options list. One row per setting, the value on the
// right with arrows; the highlighted row is explained in the box on the right. Mouse: click the left arrow to go
// back, anywhere else on the row to go forward. Keyboard/controller: Up/Down pick a row, Left/Right change it.
struct SettingsMenuRow {
  const char* label = nullptr;  // nullptr = section heading (title in help_title)
  std::string value;
  std::string help_title;
  std::string help;
  std::function<void(int)> change;
};

int CycleIndex(int index, int count, int direction) {
  return count <= 0 ? 0 : ((index + direction) % count + count) % count;
}

void DrawSettingsMenuStyle(AppState& app) {
  const Settings current = ActiveSettings(app);
  DrawOneDriveWarning(app);
  const bool xbox = current.renderer == Renderer::kXbox;
  std::vector<SettingsMenuRow> rows;
  auto heading = [&rows](const char* title) { SettingsMenuRow row; row.help_title = title; rows.push_back(row); };
  auto on_off = [](bool value) { return std::string(value ? "On" : "Off"); };

  heading("Picture");
  {
    struct Choice { const char* name; Renderer renderer; NativeApi api; const char* help; };
    static const std::array<Choice, 4> kChoices{{
        {"Vulkan", Renderer::kNative, NativeApi::kVulkan, "The new renderer. Fastest on most PCs, and the one we recommend."},
        {"DirectX 12", Renderer::kNative, NativeApi::kD3D12, "The new renderer through DirectX 12. Try it if Vulkan has problems on your PC."},
        {"DirectX 11", Renderer::kNative, NativeApi::kD3D11, "The new renderer through DirectX 11, for older graphics cards and drivers."},
        {"Xbox", Renderer::kXbox, NativeApi::kVulkan, "The original Xbox 360 renderer. It needs a stronger graphics card."},
    }};
    int index = 3;
    if (!xbox) index = current.native_api == NativeApi::kD3D12 ? 1 : current.native_api == NativeApi::kD3D11 ? 2 : 0;
#ifndef _WIN32
    // Linux: Vulkan (new renderer) or Xbox only; DirectX choices do not exist there.
    if (!xbox) index = 0;
    static const std::array<int, 2> kLinuxChoices{0, 3};
    const int linux_slot = xbox ? 1 : 0;
#endif
    SettingsMenuRow row{"Renderer", kChoices[index].name, kChoices[index].name, kChoices[index].help, nullptr};
    if (!xbox) row.help += "\n\nIn the game: F1 (or Back + Start) opens the game's own settings, F3 shows the frame rate.";
    row.change = [&app, index
#ifndef _WIN32
                  , linux_slot
#endif
    ](int direction) {
#ifndef _WIN32
      const Choice& next = kChoices[kLinuxChoices[CycleIndex(linux_slot, 2, direction)]];
      (void)index;
#else
      const Choice& next = kChoices[CycleIndex(index, 4, direction)];
#endif
      app.settings.renderer = next.renderer;
      if (next.renderer == Renderer::kNative) app.settings.native_api = next.api;
      SaveSettings(app);
    };
    rows.push_back(row);
  }
  if (!xbox) {
    // The new renderer draws at 480p, 720p, 1080p, 1440p or 4K (w13r_bridge.cpp maps the launcher's scale to one of
    // them: an older 360p setting draws at 480p, 540p at 720p), so only those five are offered (Fable v1.7.0 item 6).
    static const std::array<PictureSize, 5> kSizes{PictureSize::k480p, PictureSize::k720p, PictureSize::k1080p,
                                                   PictureSize::k1440p, PictureSize::k4k};
    PictureSize shown = current.picture_size;
    if (shown == PictureSize::k360p) shown = PictureSize::k480p;
    if (shown == PictureSize::k540p) shown = PictureSize::k720p;
    int index = 1;
    for (int i = 0; i < 5; ++i) if (kSizes[i] == shown) index = i;
    rows.push_back({"Picture size", PictureSizeName(shown), PictureSizeName(shown),
                    "The size the game draws at. Lower is faster: 480p suits integrated graphics; 1080p and above "
                    "need a stronger card.",
                    [&app, index](int direction) {
                      KeepManualSettings(app, RecommendedField::kPictureSize);
                      app.settings.picture_size = kSizes[CycleIndex(index, 5, direction)];
                      SaveSettings(app);
                    }});
  } else {
    static const std::array<Resolution, 3> kResolutions{Resolution::k720p, Resolution::k1080p, Resolution::k1440p};
    int index = 0;
    for (int i = 0; i < 3; ++i) if (kResolutions[i] == current.resolution) index = i;
    rows.push_back({"Resolution", ResolutionName(current.resolution), ResolutionName(current.resolution),
                    "The resolution of the Xbox renderer. Higher needs a stronger graphics card.",
                    [&app, index](int direction) {
                      KeepManualSettings(app, RecommendedField::kResolution);
                      app.settings.resolution = kResolutions[CycleIndex(index, 3, direction)];
                      SaveSettings(app);
                    }});
  }
  {
    static const std::array<std::pair<DisplayMode, const char*>, 3> kModes{{
        {DisplayMode::kFullscreen, "Fullscreen"}, {DisplayMode::kBorderless, "Borderless"}, {DisplayMode::kWindowed, "Windowed"}}};
    static const std::array<const char*, 3> kHelp{{
        "Fills the screen.",
        "Also fills the screen, but never changes your monitor's refresh rate, so switching to other windows is quick and "
        "does not flicker. On 144/165 Hz screens motion can be a little less even.",
        "Keeps the game in a window you can resize."}};
    int index = 0;
    for (int i = 0; i < 3; ++i) if (kModes[i].first == current.display) index = i;
    rows.push_back({"Display", kModes[index].second, kModes[index].second, kHelp[index], [&app, index](int direction) {
                      app.settings.display = kModes[CycleIndex(index, 3, direction)].first;
                      SaveSettings(app);
                    }});
  }
  {
    std::vector<std::pair<std::string, std::string>> cards{{"", "Automatic"}};
    for (const GpuInfo& gpu : app.gpus) cards.emplace_back(gpu.id, gpu.name);
    int index = 0;
    for (size_t i = 0; i < cards.size(); ++i) if (cards[i].first == app.settings.gpu_id) index = static_cast<int>(i);
    const std::string name = index == 0 && !app.settings.gpu_id.empty() ? CurrentGpuName(app) : cards[index].second;
    rows.push_back({"Graphics card", name, name,
                    "Automatic lets the game choose (a separate graphics card first). Pick one here if your PC has "
                    "more than one, for example a laptop with built-in graphics and an NVIDIA card.",
                    [&app, cards, index](int direction) {
                      app.settings.gpu_id = cards[CycleIndex(index, static_cast<int>(cards.size()), direction)].first;
                      SaveSettings(app);
                    }});
  }
  // Stretch and Sync frames act only on the Xbox renderer: the new renderer presents by itself (Fable v1.7.0 item 5).
  if (xbox) {
    rows.push_back({"Stretch to fill wide screens", on_off(current.stretch_to_fill_wide_screens), "Stretch to fill",
                    "Fills a 21:9 or 16:10 screen by stretching the 16:9 picture.", [&app](int) {
                      app.settings.stretch_to_fill_wide_screens = !app.settings.stretch_to_fill_wide_screens;
                      SaveSettings(app);
                    }});
    rows.push_back({"Anti-aliasing", on_off(current.xbox_anti_aliasing), "Anti-aliasing",
                    "Smooths edges in the finished picture. Turning it off may improve speed.", [&app](int) {
                      app.settings.xbox_anti_aliasing = !app.settings.xbox_anti_aliasing;
                      SaveSettings(app);
                    }});
  }

  heading("Speed and game");
  {
    static const std::array<std::pair<FrameRate, const char*>, 3> kRates{{
        {FrameRate::kLocked30, "Locked 30"}, {FrameRate::kStandard30_60, "30 / 60"}, {FrameRate::k60Everywhere, "60 everywhere"}}};
    static const std::array<const char*, 3> kHelp{{"30 fps in matches and entrances.",
                                                   "60 fps in matches, 30 fps in entrances, as the game was made.",
                                                   "60 fps in matches and entrances."}};
    const FrameRate rate = xbox ? current.xbox_frame_rate : current.frame_rate;
    int index = 1;
    for (int i = 0; i < 3; ++i) if (kRates[i].first == rate) index = i;
    rows.push_back({"Frame rate", kRates[index].second, kRates[index].second, kHelp[index], [&app, index, xbox](int direction) {
                      KeepManualSettings(app, xbox ? RecommendedField::kXboxFrameRate : RecommendedField::kFrameRate);
                      (xbox ? app.settings.xbox_frame_rate : app.settings.frame_rate) = kRates[CycleIndex(index, 3, direction)].first;
                      SaveSettings(app);
                    }});
  }
  {
    const int count = static_cast<int>(std::size(kGameLanguages));
    int index = 0;
    for (int i = 0; i < count; ++i) if (kGameLanguages[i].id == app.settings.language) index = i;
    rows.push_back({"Language", kGameLanguages[index].name, kGameLanguages[index].name,
                    "Changes the game's menus and text the next time it starts.", [&app, index, count](int direction) {
                      app.settings.language = kGameLanguages[CycleIndex(index, count, direction)].id;
                      SaveSettings(app);
                    }});
  }
  rows.push_back({"Check for updates", on_off(app.settings.check_updates), "Check for updates",
                  "Checks for a newer version when the launcher starts. It sends no information about you or your PC.",
                  [&app](int) {
                    app.settings.check_updates = !app.settings.check_updates;
                    SaveSettings(app);
                    if (app.settings.check_updates) StartUpdateCheck(app);
                  }});
  if (xbox) {
    rows.push_back({"Sync frames to the monitor", on_off(app.settings.sync_frames_to_monitor), "Sync frames",
                    "Keep this on. Turn it off only if the game runs too fast or stutters on a high-refresh display.",
                    [&app](int) {
                      app.settings.sync_frames_to_monitor = !app.settings.sync_frames_to_monitor;
                      SaveSettings(app);
                    }});
  }
#ifndef WWE2K14_LAUNCHER_NO_TEST_TOOLS
  if (!TestBuildMarkerRoot(app.paths).empty()) {
    rows.push_back({"Graphics capture mode", on_off(app.settings.glitch_capture_mode), "Graphics capture mode",
                    "For test builds. Starts a capture tool with the next game run and saves images under logs.",
                    [&app](int) {
                      app.settings.glitch_capture_mode = !app.settings.glitch_capture_mode;
                      SaveSettings(app);
                    }});
  }
#endif

  if (app.settings_row < 0 || app.settings_row >= static_cast<int>(rows.size()) || !rows[app.settings_row].label)
    app.settings_row = 1;

  const float gap = S(app, 22.0f);
  const float help_width = S(app, 300.0f);
  const float footer = ImGui::GetTextLineHeightWithSpacing() + S(app, 8.0f);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float list_height = std::max(S(app, 200.0f), avail.y - footer);
  ImDrawList* draw = nullptr;
  const ImU32 row_bg = ImGui::ColorConvertFloat4ToU32(Rgba(28, 28, 32));
  const ImU32 row_on_left = ImGui::ColorConvertFloat4ToU32(Rgba(62, 22, 25));
  const ImU32 accent = ImGui::ColorConvertFloat4ToU32(kAccent);
  const ImU32 text = ImGui::ColorConvertFloat4ToU32(kText);
  const ImU32 muted = ImGui::ColorConvertFloat4ToU32(kMuted);

  ImGui::BeginChild("settings-menu-list", ImVec2(std::max(S(app, 300.0f), avail.x - help_width - gap), list_height));
  draw = ImGui::GetWindowDrawList();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, S(app, 4.0f)));
  const float row_height = S(app, 36.0f);
  // One highlighted row: the one under the mouse, else the one the keyboard/controller is on (last frame's choice is
  // drawn, so the two never light up together).
  const int shown_row = app.settings_row;
  int hovered_row = -1, focused_row = -1;
  for (size_t i = 0; i < rows.size(); ++i) {
    SettingsMenuRow& row = rows[i];
    if (!row.label) {
      if (i > 0) ImGui::Dummy(S(app, 1.0f, 10.0f));
      ImGui::PushFont(app.fonts.oswald_medium);
      ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
      std::string title = row.help_title;
      for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      ImGui::TextUnformatted(title.c_str());
      ImGui::PopStyleColor();
      ImGui::PopFont();
      continue;
    }
    ImGui::PushID(static_cast<int>(i));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool pressed = ImGui::InvisibleButton("row", ImVec2(width, row_height));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    if (hovered) hovered_row = static_cast<int>(i);
    if (focused && ImGui::GetIO().NavVisible) focused_row = static_cast<int>(i);
    const float value_width = std::min(S(app, 280.0f), width * 0.5f);
    const float value_left = at.x + width - value_width;
    int direction = 0;
    if (pressed) {
      const bool by_mouse = ImGui::IsMouseReleased(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Left);
      direction = by_mouse && ImGui::GetIO().MousePos.x < value_left + S(app, 34.0f) ? -1 : 1;
    }
    if (focused) {
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) direction = -1;
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) direction = 1;
    }
    const bool active = shown_row == static_cast<int>(i);
    const ImVec2 end(at.x + width, at.y + row_height);
    if (active) {
      draw->AddRectFilledMultiColor(at, end, row_on_left, row_bg, row_bg, row_on_left);
      draw->AddRectFilled(at, ImVec2(at.x + S(app, 3.0f), end.y), accent);
    } else {
      draw->AddRectFilled(at, end, row_bg);
    }
    ImGui::PushFont(app.fonts.barlow_medium);
    const float text_y = at.y + (row_height - ImGui::GetFontSize()) * 0.5f;
    draw->AddText(ImVec2(at.x + S(app, 16.0f), text_y), text, row.label);
    ImGui::PopFont();
    // value with arrows: triangles drawn, so no symbol font is needed
    const float arrow = S(app, 5.5f);
    const float mid_y = at.y + row_height * 0.5f;
    const ImU32 arrow_color = active ? accent : muted;
    const float left_x = value_left + S(app, 12.0f);
    const float right_x = end.x - S(app, 14.0f);
    draw->AddTriangleFilled(ImVec2(left_x, mid_y), ImVec2(left_x + arrow * 1.6f, mid_y - arrow),
                            ImVec2(left_x + arrow * 1.6f, mid_y + arrow), arrow_color);
    draw->AddTriangleFilled(ImVec2(right_x, mid_y), ImVec2(right_x - arrow * 1.6f, mid_y - arrow),
                            ImVec2(right_x - arrow * 1.6f, mid_y + arrow), arrow_color);
    ImGui::PushFont(app.fonts.barlow_semibold);
    const float value_space = value_width - S(app, 64.0f);
    std::string shown = row.value;
    while (shown.size() > 3 && ImGui::CalcTextSize(shown.c_str()).x > value_space) shown.pop_back();
    if (shown.size() < row.value.size()) shown += "...";
    const float value_text = ImGui::CalcTextSize(shown.c_str()).x;
    draw->AddText(ImVec2(value_left + (value_width - value_text) * 0.5f, at.y + (row_height - ImGui::GetFontSize()) * 0.5f),
                  text, shown.c_str());
    ImGui::PopFont();
    if (direction != 0 && row.change) {
      LogClick(direction < 0 ? "setting-back" : "setting", row.label);
      row.change(direction);
    }
    ImGui::PopID();
  }
  ImGui::PopStyleVar();
  ImGui::EndChild();
  if (hovered_row >= 0) app.settings_row = hovered_row;
  else if (focused_row >= 0) app.settings_row = focused_row;

  ImGui::SameLine(0.0f, gap);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, Rgba(18, 18, 21));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(app, 18.0f, 16.0f));
  ImGui::BeginChild("settings-menu-help", ImVec2(help_width, list_height), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoNav);
  {
    const SettingsMenuRow& row = rows[shown_row];  // the highlighted row
    ImGui::PushFont(app.fonts.barlow_small);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    std::string label = row.label;
    for (char& c : label) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushFont(app.fonts.oswald_semibold);
    ImGui::TextWrapped("%s", row.help_title.c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    DrawSecondaryWrapped(app, row.help.c_str(), kSecondary);
    ImGui::Dummy(S(app, 1.0f, 14.0f));
    ImGui::PushFont(app.fonts.barlow_medium);
    if (AccentButton(app, "Recommended settings for my PC", ImVec2(-1.0f, S(app, 36.0f)))) RequestRecommendedSettings(app);
    ImGui::PopFont();
    DrawSecondaryWrapped(app, "Suggests a picture size and frame rate for this PC. You review the changes first.",
                         kMuted);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  DrawSecondaryWrapped(app, "Up / Down: pick a setting.   Left / Right or click: change it.", kMuted);
}

void DrawSettings(AppState& app) {
#if WWE2K14_NEW_RENDERER_UI
  DrawSettingsMenuStyle(app);
  DrawRecommendedSettingsPopup(app);
  return;
#endif
  const Settings current = ActiveSettings(app);
  DrawOneDriveWarning(app);

  DrawSettingsSectionHeading(app, "Picture", "Choose how the game is drawn and displayed.");
  if (ImGui::BeginTable("settings-recommended", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("recommended-action", ImGuiTableColumnFlags_WidthFixed, S(app, 310.0f));
    ImGui::TableSetupColumn("recommended-description", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextColumn();
    ImGui::PushFont(app.fonts.barlow_medium);
    if (AccentButton(app, "Recommended settings for my PC", ImVec2(-1.0f, S(app, 38.0f))))
      RequestRecommendedSettings(app);
    ImGui::PopFont();
    ImGui::TableNextColumn();
    DrawSecondaryWrapped(app,
                         "Suggests a picture size and frame rate for this PC. Review changes before applying.",
                         kSecondary);
    ImGui::EndTable();
  }
  ImGui::Spacing();

#if WWE2K14_NEW_RENDERER_UI
  // v1.7.0 (owner 2026-11-22): the new renderer in three graphics APIs, plus the original Xbox renderer.
  if (ImGui::BeginTable("settings-renderer", 4, ImGuiTableFlags_SizingStretchSame)) {
    struct Choice { const char* label; Renderer renderer; NativeApi api; const char* description; };
    const std::array<Choice, 4> choices{{
        {"Vulkan", Renderer::kNative, NativeApi::kVulkan, "New renderer. Fastest on most PCs. Recommended."},
        {"DirectX 12", Renderer::kNative, NativeApi::kD3D12, "New renderer. Try it if Vulkan has problems on your PC."},
        {"DirectX 11", Renderer::kNative, NativeApi::kD3D11, "New renderer for older graphics cards and drivers."},
        {"Xbox", Renderer::kXbox, NativeApi::kVulkan, "The original Xbox 360 picture. Needs a stronger graphics card."},
    }};
    for (const Choice& choice : choices) {
      ImGui::TableNextColumn();
      const bool selected = current.renderer == choice.renderer &&
                            (choice.renderer == Renderer::kXbox || current.native_api == choice.api);
      DrawSegment(app, choice.label, selected, ImGui::GetContentRegionAvail().x, [&app, choice] {
        app.settings.renderer = choice.renderer;
        if (choice.renderer == Renderer::kNative) app.settings.native_api = choice.api;
        SaveSettings(app);
      });
      DrawSecondaryWrapped(app, choice.description, kSecondary);
    }
    ImGui::EndTable();
  }
#else
  const float two_column_width =
      (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  if (ImGui::BeginTable("settings-renderer", 2, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextColumn();
    DrawSegment(app, "Native", current.renderer == Renderer::kNative, two_column_width, [&app] {
      app.settings.renderer = Renderer::kNative;
      SaveSettings(app);
    });
    DrawSecondaryWrapped(app, "The newer renderer, built for this port. Best for modern and integrated graphics.",
                         kSecondary);
    ImGui::TableNextColumn();
    DrawSegment(app, "Xbox", current.renderer == Renderer::kXbox, two_column_width, [&app] {
      app.settings.renderer = Renderer::kXbox;
      SaveSettings(app);
    });
    DrawSecondaryWrapped(app, "The original Xbox 360 picture. Needs a stronger graphics card.",
                         kSecondary);
    ImGui::EndTable();
  }
#endif
  ImGui::Spacing();

  ImGui::TextUnformatted("Resolution");
  if (current.renderer == Renderer::kNative) {
    const std::array<std::pair<PictureSize, const char*>, 7> choices{{
        {PictureSize::k360p, "360p"}, {PictureSize::k480p, "480p"}, {PictureSize::k540p, "540p"},
        {PictureSize::k720p, "720p"}, {PictureSize::k1080p, "1080p"}, {PictureSize::k1440p, "1440p"},
        {PictureSize::k4k, "4K"},
    }};
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, S(app, 420.0f)));
    if (ImGui::BeginCombo("##picture-size", PictureSizeName(current.picture_size), ImGuiComboFlags_HeightLarge)) {
      for (const auto& [value, name] : choices) {
        const bool selected = current.picture_size == value;
        if (LoggedSelectable(name, selected)) {
          KeepManualSettings(app, RecommendedField::kPictureSize);
          app.settings.picture_size = value;
          SaveSettings(app);
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    DrawSecondaryWrapped(app, "Lower settings can run faster. 360p to 540p suit integrated graphics; 1080p and above need a stronger card.",
                         kSecondary);
  } else {
    const std::array<std::pair<Resolution, const char*>, 3> choices{{
        {Resolution::k720p, "720p"}, {Resolution::k1080p, "1080p"}, {Resolution::k1440p, "1440p"},
    }};
    const std::string selected_resolution = ResolutionName(current.resolution);
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, S(app, 420.0f)));
    if (ImGui::BeginCombo("##picture-size", selected_resolution.c_str(), ImGuiComboFlags_HeightLarge)) {
      for (const auto& [value, name] : choices) {
        const bool selected = current.resolution == value;
        if (LoggedSelectable(name, selected)) {
          KeepManualSettings(app, RecommendedField::kResolution);
          app.settings.resolution = value;
          SaveSettings(app);
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    DrawSecondaryWrapped(app, "Choose the resolution used by the Xbox renderer.", kSecondary);
  }
  ImGui::Spacing();

  ImGui::TextUnformatted("Display");
  const float display_width = (ImGui::GetContentRegionAvail().x - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
  DrawSegment(app, "Fullscreen", current.display == DisplayMode::kFullscreen, display_width, [&app] {
    app.settings.display = DisplayMode::kFullscreen;
    SaveSettings(app);
  });
  ImGui::SameLine();
  DrawSegment(app, "Borderless", current.display == DisplayMode::kBorderless, display_width, [&app] {
    app.settings.display = DisplayMode::kBorderless;
    SaveSettings(app);
  });
  ImGui::SameLine();
  DrawSegment(app, "Windowed", current.display == DisplayMode::kWindowed, display_width, [&app] {
    app.settings.display = DisplayMode::kWindowed;
    SaveSettings(app);
  });
  DrawSecondaryWrapped(app, "Fullscreen fills the screen. Borderless also fills it but never changes your monitor's refresh "
                       "rate, so switching to other windows is quick and does not flicker (on 144/165 Hz screens motion "
                       "can be a little less even). Windowed keeps the game in a resizable window.", kSecondary);
  ImGui::Spacing();

  ImGui::TextUnformatted("Graphics card");
  const std::string selected_gpu = app.settings.gpu_id.empty() ? "Automatic" : CurrentGpuName(app);
  ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, S(app, 560.0f)));
  if (ImGui::BeginCombo("##gpu", selected_gpu.c_str(), ImGuiComboFlags_HeightLarge)) {
    if (LoggedSelectable("Automatic", app.settings.gpu_id.empty())) {
      app.settings.gpu_id.clear();
      SaveSettings(app);
    }
    for (const auto& gpu : app.gpus) {
      const std::string label = gpu.name + (gpu.integrated ? " (integrated)" : "");
      if (LoggedSelectable(label.c_str(), app.settings.gpu_id == gpu.id)) {
        app.settings.gpu_id = gpu.id;
        SaveSettings(app);
      }
    }
    ImGui::EndCombo();
  }
  DrawSecondaryWrapped(app, "Automatic uses the graphics card selected by your PC. Choose one here if you have more than one.",
                       kSecondary);
  ImGui::Spacing();

  DrawSettingsCheckbox(
      app, "Stretch to fill wide screens", &app.settings.stretch_to_fill_wide_screens,
      "Fills a 21:9 or 16:10 screen by stretching the 16:9 picture.",
      [&app] { SaveSettings(app); });
  if (current.renderer == Renderer::kNative) {
#if !WWE2K14_NEW_RENDERER_UI
    DrawSettingsCheckbox(app, "Anti-aliasing", &app.settings.anti_aliasing,
                         "Smooths edges. Turning it off may improve speed on a weaker graphics card.",
                         [&app] { SaveSettings(app); });
#endif
  } else {
    DrawSettingsCheckbox(app, "Anti-aliasing", &app.settings.xbox_anti_aliasing,
                         "Smooths edges in the finished picture. Turning it off may improve speed.",
                         [&app] { SaveSettings(app); });
  }

  DrawSettingsSectionHeading(app, "Frame Rate & Speed", "Pick a steady frame rate or trade picture details for speed.");
  const bool native = current.renderer == Renderer::kNative;
#if WWE2K14_NEW_RENDERER_UI
  // v1.7.0: the new renderer has its own picture options (in-game Settings, F1); the old native renderer's
  // speed options below do not apply to it, so they show only where the old renderer still runs (Linux).
  const bool old_native = false;
#else
  const bool old_native = native;
#endif
  const FrameRate current_rate = native ? current.frame_rate : current.xbox_frame_rate;
  const std::array<std::pair<FrameRate, const char*>, 3> rates{{
      {FrameRate::kLocked30, "Locked 30"},
      {FrameRate::kStandard30_60, "30 / 60"},
      {FrameRate::k60Everywhere, "60 Everywhere"},
  }};
  if (ImGui::BeginTable("settings-frame-rate", 3, ImGuiTableFlags_SizingStretchSame)) {
    const std::array<const char*, 3> descriptions{{
        "30 fps in matches and entrances.",
        "60 fps in matches; 30 fps in entrances.",
        "60 fps in matches and entrances.",
    }};
    for (size_t i = 0; i < rates.size(); ++i) {
      ImGui::TableNextColumn();
      DrawSegment(app, rates[i].second, current_rate == rates[i].first,
                  ImGui::GetContentRegionAvail().x,
                  [&app, native, value = rates[i].first] {
                    KeepManualSettings(app, native ? RecommendedField::kFrameRate
                                                   : RecommendedField::kXboxFrameRate);
                    (native ? app.settings.frame_rate : app.settings.xbox_frame_rate) = value;
                    SaveSettings(app);
                  });
      DrawSecondaryWrapped(app, descriptions[i], kSecondary);
    }
    ImGui::EndTable();
  }
  if (native && !old_native) {
    DrawSecondaryWrapped(app, "In the game, F1 (or Back + Start on a controller) opens its own settings; F3 shows the "
                         "frame rate.", kSecondary);
  }
  if (old_native) {
    DrawSettingsCheckbox(app, "Performance mode", &app.settings.performance_mode,
                         "Faster on weak graphics, with a slightly softer picture: anti-aliasing off and softer texture filtering. Your other choices stay saved.",
                         [&app] { SaveSettings(app); });
    DrawSettingsCheckbox(app, "Lower CPU use", &app.settings.low_power,
                         "Lets the processor rest while the game waits for the graphics card. This can make the PC cooler and quieter; on some PCs play may feel less smooth.",
                         [&app] { SaveSettings(app); });
    if (current_rate == FrameRate::kLocked30) {
      DrawSettingsCheckbox(app, "Faster loading", &app.settings.fast_loading,
                           "Loading screens run at 60 fps. Matches stay at 30 fps.",
                           [&app] { SaveSettings(app); });
    }

    if (ImGui::BeginTable("settings-speed-details", 4, ImGuiTableFlags_SizingStretchSame)) {
      ImGui::TableNextColumn();
      ImGui::TextUnformatted("Crowd");
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##crowd", app.settings.crowd == 0 ? "None" : "Full")) {
        if (LoggedSelectable("Full", app.settings.crowd != 0)) {
          app.settings.crowd = 1;
          SaveSettings(app);
        }
        if (LoggedSelectable("None", app.settings.crowd == 0)) {
          app.settings.crowd = 0;
          SaveSettings(app);
        }
        ImGui::EndCombo();
      }
      DrawSecondaryWrapped(app, "None removes the crowd and reduces processor work.", kSecondary);

      ImGui::TableNextColumn();
      ImGui::TextUnformatted("Shadows");
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##shadows", app.settings.shadows == 0 ? "Off" : "On")) {
        if (LoggedSelectable("On", app.settings.shadows != 0)) {
          app.settings.shadows = 1;
          SaveSettings(app);
        }
        if (LoggedSelectable("Off", app.settings.shadows == 0)) {
          app.settings.shadows = 0;
          SaveSettings(app);
        }
        ImGui::EndCombo();
      }
      DrawSecondaryWrapped(app, "Off removes the shadows under the wrestlers and the ring.", kSecondary);
      {
        bool full_shadows = MultiShadowsOn(app.settings, app.gpus);
        ImGui::BeginDisabled(app.settings.shadows == 0);
        if (LoggedCheckbox("Full shadows in big matches", &full_shadows)) {
          app.settings.multi_shadows = full_shadows ? 1 : 0;
          SaveSettings(app);
        }
        ImGui::EndDisabled();
        DrawSecondaryWrapped(app, "Matches with 3 or more wrestlers get the same soft rope and wrestler shadows on the ring as "
                                  "one-on-one matches. Two-man ladder matches keep their usual look.", kSecondary);
      }

      ImGui::TableNextColumn();
      ImGui::TextUnformatted("Texture filtering");
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##texture_filtering", app.settings.texture_filtering == 0 ? "Standard" : "Sharp")) {
        if (LoggedSelectable("Sharp", app.settings.texture_filtering != 0)) {
          app.settings.texture_filtering = 1;
          SaveSettings(app);
        }
        if (LoggedSelectable("Standard", app.settings.texture_filtering == 0)) {
          app.settings.texture_filtering = 0;
          SaveSettings(app);
        }
        ImGui::EndCombo();
      }
      DrawSecondaryWrapped(app, "Standard is a little faster. Floors at an angle look a little softer.", kSecondary);

      ImGui::TableNextColumn();
      ImGui::TextUnformatted("Textures");
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##textures", app.settings.textures == 0 ? "Low" : "Normal")) {
        if (LoggedSelectable("Normal", app.settings.textures != 0)) {
          app.settings.textures = 1;
          SaveSettings(app);
        }
        if (LoggedSelectable("Low", app.settings.textures == 0)) {
          app.settings.textures = 0;
          SaveSettings(app);
        }
        ImGui::EndCombo();
      }
      DrawSecondaryWrapped(app, "Low uses softer textures to help reduce slowdowns on laptop graphics.", kSecondary);
      ImGui::EndTable();
    }
    ImGui::Spacing();
  }

  DrawSettingsSectionHeading(app, "Updates", "Choose whether the launcher checks for a newer version.");
  DrawSettingsCheckbox(
      app, "Check for updates", &app.settings.check_updates,
      "Checks for a newer version when the launcher starts. It sends no information about you or your PC.",
      [&app] {
        SaveSettings(app);
        if (app.settings.check_updates) StartUpdateCheck(app);
      });

  DrawSettingsSectionHeading(app, "Advanced", "Other choices for how the game starts.");
  const char* current_language = kGameLanguages[0].name;
  for (const GameLanguage& language : kGameLanguages) {
    if (language.id == app.settings.language) current_language = language.name;
  }
  ImGui::TextUnformatted("Language");
  ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, S(app, 420.0f)));
  if (ImGui::BeginCombo("##language", current_language)) {
    for (const GameLanguage& language : kGameLanguages) {
      const bool selected = language.id == app.settings.language;
      if (LoggedSelectable(language.name, selected)) {
        app.settings.language = language.id;
        SaveSettings(app);
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  DrawSecondaryWrapped(app, "Changes the game's menus and text the next time it starts.", kSecondary);
  ImGui::Spacing();

#ifdef _WIN32
  DrawSettingsCheckbox(app, "Sync frames to the monitor", &app.settings.sync_frames_to_monitor,
                       "Turn this off if the game runs too fast or stutters on a high-refresh display.",
                       [&app] { SaveSettings(app); });
#endif
  if (old_native) {
    DrawSettingsCheckbox(
        app, "Intel compatibility shaders", &app.settings.intel_lake_shaders,
        "Uses the older Intel shader set. The game usually chooses compatible shaders automatically.",
        [&app] { SaveSettings(app); });
  }

#ifndef WWE2K14_LAUNCHER_NO_TEST_TOOLS
  if (!TestBuildMarkerRoot(app.paths).empty()) {
    DrawSettingsCheckbox(app, "Graphics capture mode", &app.settings.glitch_capture_mode,
                         "For test builds. Starts a capture tool with the next game run and saves images under logs.",
                         [&app] { SaveSettings(app); });
  }
#endif

  DrawRecommendedSettingsPopup(app);
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
  // Clicking the box opens the file picker, as its "or choose files…" line promises.
  if (ImGui::InvisibleButton("music-drop-zone", size) && !app.job.active.load()) {
    LogClick("button", "music-drop-zone");
    OpenMusicImport(app, false);
  }
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
  const char* title = "Click to add songs, or drop them here";
  const ImVec2 title_size = ImGui::CalcTextSize(title);
  draw->AddText(ImVec2(start.x + (width - title_size.x) * 0.5f, start.y + S(app, 25.0f)),
                ImGui::ColorConvertFloat4ToU32(kText), title);
  const char* subtitle = "mp3, flac, wav or ogg";
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

void DrawMusicSlider(const char* label, float* value, float low, float high, const char* format) {
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::SliderFloat(label, value, low, high, format);
}

void DrawMusicWaveform(AppState& app, MusicEditorState& editor) {
  const float width = ImGui::GetContentRegionAvail().x;
  const ImVec2 size(width, S(app, 96.0f));
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("music-waveform", size);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), ImGui::ColorConvertFloat4ToU32(kPanel),
                      S(app, 6.0f));
  const float middle = origin.y + size.y * 0.5f;
  const size_t points = editor.waveform.size();
  for (size_t i = 0; i < points; ++i) {
    const MusicWaveformPoint& point = editor.waveform[i];
    const float high = std::max(point.maximum_left, point.maximum_right);
    const float low = std::min(point.minimum_left, point.minimum_right);
    const float x = origin.x + size.x * (static_cast<float>(i) + 0.5f) / static_cast<float>(points);
    draw->AddLine(ImVec2(x, middle - std::clamp(high, -1.0f, 1.0f) * size.y * 0.48f),
                  ImVec2(x, middle - std::clamp(low, -1.0f, 1.0f) * size.y * 0.48f),
                  ImGui::ColorConvertFloat4ToU32(kSecondary));
  }
  const double duration = editor.decoded && editor.decoded->sample_rate
                              ? static_cast<double>(editor.decoded->frame_count()) / editor.decoded->sample_rate
                              : 0.0;
  if (duration > 0.0) {
    const float start_x = origin.x + size.x * static_cast<float>(editor.settings.trim_start_seconds / duration);
    const float end_seconds = editor.settings.trim_end_seconds > 0.0f ? editor.settings.trim_end_seconds
                                                                      : static_cast<float>(duration);
    const float end_x = origin.x + size.x * static_cast<float>(std::min<double>(end_seconds, duration) / duration);
    const ImU32 shade = IM_COL32(0, 0, 0, 150);
    draw->AddRectFilled(origin, ImVec2(std::clamp(start_x, origin.x, origin.x + size.x), origin.y + size.y), shade);
    draw->AddRectFilled(ImVec2(std::clamp(end_x, origin.x, origin.x + size.x), origin.y),
                        ImVec2(origin.x + size.x, origin.y + size.y), shade);
    draw->AddLine(ImVec2(start_x, origin.y), ImVec2(start_x, origin.y + size.y), ImGui::ColorConvertFloat4ToU32(kAccent),
                  S(app, 2.0f));
    draw->AddLine(ImVec2(end_x, origin.y), ImVec2(end_x, origin.y + size.y), ImGui::ColorConvertFloat4ToU32(kAccent),
                  S(app, 2.0f));
  }
}

void DrawMusicEditor(AppState& app) {
  MusicEditorState& editor = app.music_editor;
  if (!editor.open || !editor.decoded) return;
  MusicTrackSettings& s = editor.settings;
  const double duration = static_cast<double>(editor.decoded->frame_count()) / editor.decoded->sample_rate;
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
  ImGui::BeginChild("music-editor", ImVec2(0, S(app, 580.0f)), ImGuiChildFlags_Borders);
  ImGui::Text("Edit: %s", editor.song.title.c_str());
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kSecondary, "Your original file is kept. The game plays a processed copy at 48 kHz.");
  ImGui::PopFont();
  DrawMusicWaveform(app, editor);
  s.trim_start_seconds = std::clamp(s.trim_start_seconds, 0.0f, static_cast<float>(duration));
  DrawMusicSlider("Start (s)", &s.trim_start_seconds, 0.0f, static_cast<float>(duration), "%.1f");
  float end = s.trim_end_seconds > 0.0f ? s.trim_end_seconds : static_cast<float>(duration);
  DrawMusicSlider("End (s)", &end, 0.0f, static_cast<float>(duration), "%.1f");
  s.trim_end_seconds = end >= static_cast<float>(duration) - 0.05f ? 0.0f : end;
  DrawMusicSlider("Volume (dB)", &s.volume_db, -24.0f, 12.0f, "%.1f");
  if (AccentButton(app, "Match the game's music level", S(app, 260.0f, 34.0f))) {
    s.normalize = true;
    s.target_rms_dbfs = kGameMusicRmsDbfs;
  }
  ImGui::SameLine();
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kSecondary, "Press Play, then move any slider - you hear the change straight away.");
  ImGui::PopFont();
  LoggedCheckbox("Even out loudness", &s.normalize);
  if (s.normalize) DrawMusicSlider("Loudness target (dB)", &s.target_rms_dbfs, -36.0f, -6.0f, "%.0f");
  DrawMusicSlider("Fade in (s)", &s.fade_in_seconds, 0.0f, 10.0f, "%.2f");
  DrawMusicSlider("Fade out (s)", &s.fade_out_seconds, 0.0f, 10.0f, "%.2f");
  DrawMusicSlider("Bass (dB)", &s.bass_gain_db, -12.0f, 12.0f, "%.1f");
  DrawMusicSlider("Treble (dB)", &s.treble_gain_db, -12.0f, 12.0f, "%.1f");
  ImGui::Spacing();
  const bool busy = app.job.active.load();
  if (editor.player_source == MusicPlayerSource::Editor) {
    DrawMusicPlayerBar(app, MusicPlayerSource::Editor);
  } else if (AccentButton(app, "Play", S(app, 120.0f, 34.0f)) && !busy) {
    StartEditorPlayback(app);
  }
  if (LoggedButton("Save") && !busy) StartSaveMusicEdit(app);
  ImGui::SameLine();
  if (LoggedButton("Use my default settings")) {
    s = MusicSettingsForSong(Settings{.music_preset = app.settings.music_preset}, editor.song.file);
  }
  ImGui::SameLine();
  if (LoggedButton("Close")) {
    StopMusicPreview(editor);
    editor.open = false;
    editor.decoded.reset();
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void DrawMusicPreset(AppState& app) {
  MusicPreset& preset = app.music_preset_draft;
  if (!ImGui::CollapsingHeader("Settings for all songs", ImGuiTreeNodeFlags_DefaultOpen)) return;
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kSecondary, "Press Play to hear your first song, move the sliders while it plays, then Apply to all songs.");
  ImGui::PopFont();
  DrawMusicSlider("Loudness target (dB)##preset", &preset.target_rms_dbfs, -36.0f, -6.0f, "%.0f");
  DrawMusicSlider("Fade in (s)##preset", &preset.fade_in_seconds, 0.0f, 10.0f, "%.2f");
  DrawMusicSlider("Fade out (s)##preset", &preset.fade_out_seconds, 0.0f, 10.0f, "%.2f");
  DrawMusicSlider("Bass (dB)##preset", &preset.bass_gain_db, -12.0f, 12.0f, "%.1f");
  DrawMusicSlider("Treble (dB)##preset", &preset.treble_gain_db, -12.0f, 12.0f, "%.1f");
  const bool busy = app.job.active.load();
  if (app.music_editor.player_source == MusicPlayerSource::AllSongs) DrawMusicPlayerBar(app, MusicPlayerSource::AllSongs);
  ImGui::BeginDisabled(busy || app.songs.empty());
  if (app.music_editor.player_source != MusicPlayerSource::AllSongs &&
      LoggedButton("Play##preset", S(app, 150.0f, 38.0f))) {
    StartAllSongsPlayback(app);
  }
  ImGui::SameLine();
  if (AccentButton(app, "Apply to all songs", S(app, 200.0f, 38.0f))) StartApplyPresetToAllSongs(app);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (LoggedButton("Reset sliders", S(app, 150.0f, 38.0f))) preset = MusicPreset{};
  ImGui::Spacing();
}

void DrawMusic(AppState& app) {
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kSecondary, "Pick these in the entrance editor under User Playlist.");
  ImGui::PopFont();
  ImGui::Spacing();
  DrawDropZone(app);
  UpdateMusicPlayer(app);
  if (app.songs.empty()) {
    DrawSecondaryWrapped(app, "No music has been added yet. Click the box above or drop songs on it.", kSecondary);
  } else {
    // Tick boxes: forget songs that were removed, then offer actions for the ticked ones.
    std::erase_if(app.selected_songs, [&app](const fs::path& file) {
      return std::none_of(app.songs.begin(), app.songs.end(), [&file](const Song& song) { return song.file == file; });
    });
    bool all = app.selected_songs.size() == app.songs.size();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgba(74, 74, 84));
    const bool select_all_clicked = LoggedCheckbox("Select all", &all);
    ImGui::PopStyleColor();
    if (select_all_clicked) {
      app.selected_songs.clear();
      if (all) for (const Song& song : app.songs) app.selected_songs.insert(song.file);
    }
    if (!app.selected_songs.empty()) {
      ImGui::SameLine(0.0f, S(app, 24.0f));
      ImGui::BeginDisabled(app.job.active.load());
      const std::string label = "Match " + std::to_string(app.selected_songs.size()) +
                                (app.selected_songs.size() == 1 ? " song" : " songs") + " to the game's level";
      if (AccentButton(app, label.c_str(), S(app, 300.0f, 34.0f))) StartMatchSelectedSongs(app);
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (LoggedButton("Clear selection", S(app, 150.0f, 34.0f))) app.selected_songs.clear();
    }
  }
  ImGui::Spacing();
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, S(app, 14.0f, 8.0f));
  if (ImGui::BeginTable("song-table", 6,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(app, 32.0f));
    ImGui::TableSetupColumn("SONG", ImGuiTableColumnFlags_WidthStretch, 0.54f);
    ImGui::TableSetupColumn("LENGTH", ImGuiTableColumnFlags_WidthFixed, S(app, 88.0f));
    ImGui::TableSetupColumn("TYPE", ImGuiTableColumnFlags_WidthFixed, S(app, 70.0f));
    ImGui::TableSetupColumn("LEVEL", ImGuiTableColumnFlags_WidthFixed, S(app, 110.0f));
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(app, 230.0f));
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < app.songs.size(); ++i) {
      const Song song = app.songs[i];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, S(app, 36.0f));
      ImGui::TableSetColumnIndex(0);
      bool ticked = app.selected_songs.contains(song.file);
      const std::string tick_id = "##select-song" + std::to_string(i);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgba(74, 74, 84));  // visible on both row stripes
      if (ImGui::Checkbox(tick_id.c_str(), &ticked)) {
        if (ticked) app.selected_songs.insert(song.file);
        else app.selected_songs.erase(song.file);
      }
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      DrawSongTitle(app, song.title);
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(FormatDuration(song.seconds).c_str());
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(song.format.c_str());
      ImGui::TableSetColumnIndex(4);
      const char* level = SongLevelLabel(app, song);
      ImGui::TextColored(std::string_view(level) == "Game level" ? kAccent : kSecondary, "%s", level);
      ImGui::TableSetColumnIndex(5);
      const bool playing = app.music_editor.player_source == MusicPlayerSource::Song && app.music_editor.player_file == song.file;
      const std::string play_id = (playing ? "Stop##play" : "Play##play") + std::to_string(i);
      if (LoggedSmallButton(play_id.c_str())) {
        if (playing) StopMusicPreview(app.music_editor);
        else if (!app.job.active.load()) StartSongPlayback(app, song);
      }
      ImGui::SameLine();
      const std::string edit_id = "Edit##song" + std::to_string(i);
      if (LoggedSmallButton(edit_id.c_str()) && !app.job.active.load()) OpenMusicEditor(app, song);
      ImGui::SameLine();
      const std::string id = "Remove##song" + std::to_string(i);
      if (LoggedSmallButton(id.c_str())) StartRemoveSong(app, song.file);
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  ImGui::Spacing();
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kMuted, "%zu songs · copied into the game's music folder", app.songs.size());
  ImGui::PopFont();
  DrawMusicPlayerBar(app, MusicPlayerSource::Song);
  DrawMusicEditor(app);
  ImGui::Spacing();
  DrawMusicPreset(app);
}

const char* CreationSaveKindLabel(CreationSaveKind kind) {
  return kind == CreationSaveKind::kSuperstar ? "superstar" : "arena";
}

// One card of the Creation Suite: title, one-line hint, actions, and the list of that kind from the save.
void DrawCreationCard(AppState& app, const char* id, const char* title, const char* hint,
                      CreationSaveKind kind) {
  const bool supported = app.creation_records.has_value();
  size_t rows = 0;
  if (supported) {
    for (const CreationSaveRecord& record : *app.creation_records) rows += record.kind == kind;
  }
  const float table_height = S(app, 34.0f) + S(app, 38.0f) * static_cast<float>(std::clamp<size_t>(rows, 1, 5));
  BeginPanel(app, id, ImVec2(0, S(app, 122.0f) + table_height));
  ImGui::PushFont(app.fonts.barlow_semibold);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kSecondary, "%s", hint);
  ImGui::PopFont();
  const bool superstar = kind == CreationSaveKind::kSuperstar;
  auto draw_actions = [&] {
    ImGui::BeginDisabled(!supported || app.job.active.load());
    if (AccentButton(app, superstar ? "Add superstar" : "Add arena", S(app, 190.0f, 38.0f)))
      OpenCreationImport(app, kind, false);
    ImGui::SameLine();
    const std::string more_id = std::string("##more-ways-") + id;
    ImGui::SetNextItemWidth(S(app, 220.0f));
    if (ImGui::BeginCombo(more_id.c_str(), "More ways to add")) {
      if (LoggedSelectable("Add from an unpacked folder…")) {
        if (GameProcessRunning(app.paths)) {
          SetBanner(app, kCloseGameCreationsMessage);
        } else {
          RequestDialog(app, superstar ? DialogKind::CreationSuperstarFolder : DialogKind::CreationArenaFolder,
                        nullptr, 0, false);
        }
      }
      // Also on Arenas: this updates arenas imported by earlier launcher versions.
      if (LoggedSelectable("Find or fix existing")) StartRegisterExistingCreations(app);
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
  };
  ImGui::Spacing();
  if (!supported || rows > 0) draw_actions();
  ImGui::Spacing();
  if (supported) {
    // Tolerant save gate (#115/#75): say once per distinct state when unrecognized data is being kept as it is.
    static std::string last_tolerance;
    static bool tolerance_checked = false;
    if (!tolerance_checked) {
      tolerance_checked = true;
      last_tolerance = DescribeCreationTolerance(app.paths);
      if (!last_tolerance.empty()) PerfLog("%s", last_tolerance.c_str());
    }
  }
  if (!supported) {
    // Read the save once per refusal (not every frame) and log why it was refused (#115).
    static bool refusal_logged = false;
    if (!refusal_logged) {
      refusal_logged = true;
      PerfLog("creation save refused: %s", DescribeCreationStateProblem(app.paths).c_str());
    }
    DrawSecondaryWrapped(app,
                         "The save file could not be read safely, so no changes were made. Close WWE 2K14, check the save folder, and try again.",
                         kDanger);
  } else if (rows == 0) {
    DrawSecondaryWrapped(app, superstar
                                  ? "No custom superstars yet. Add one here, or open More ways to add for another method."
                                  : "No custom arenas yet. Add one here, or open More ways to add for another method.",
                         kSecondary);
    ImGui::Spacing();
    draw_actions();
  } else {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, S(app, 14.0f, 8.0f));
    const std::string table_id = std::string("saved-") + id;
    if (ImGui::BeginTable(table_id.c_str(), 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, table_height))) {
      ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch, 0.58f);
      ImGui::TableSetupColumn("GAME SLOT", ImGuiTableColumnFlags_WidthFixed, S(app, 120.0f));
      ImGui::TableSetupColumn("ACTION", ImGuiTableColumnFlags_WidthFixed, S(app, 225.0f));
      ImGui::TableHeadersRow();
      static const SDL_DialogFileFilter caw_export_filters[]{{"CAW packages", "cas"}};
      static const SDL_DialogFileFilter arena_export_filters[]{{"Arena packages", "car"}};
      for (size_t index = 0; index < app.creation_records->size(); ++index) {
        const CreationSaveRecord record = (*app.creation_records)[index];
        if (record.kind != kind) continue;
        ImGui::TableNextRow(ImGuiTableRowFlags_None, S(app, 38.0f));
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(record.name.empty() ? "(unnamed)" : record.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%u", record.slot_index + 1);
        ImGui::TableSetColumnIndex(2);
        ImGui::BeginDisabled(app.job.active.load());
        const std::string rename_button_id = "Rename##saved-creation-" + std::to_string(index);
        if (LoggedSmallButton(rename_button_id.c_str())) {
          if (GameProcessRunning(app.paths)) {
            SetBanner(app, kCloseGameCreationsMessage);
          } else {
            app.pending_rename_creation = record;
            app.rename_creation_name.fill('\0');
            const size_t copy_size = std::min(record.name.size(), app.rename_creation_name.size() - 1);
            std::copy_n(record.name.data(), copy_size, app.rename_creation_name.data());
            app.confirm_rename_creation = true;
          }
        }
        ImGui::SameLine();
        const std::string export_button_id = "Export##saved-creation-" + std::to_string(index);
        if (LoggedSmallButton(export_button_id.c_str())) {
          if (GameProcessRunning(app.paths)) {
            SetBanner(app, kCloseGameCreationExportMessage);
          } else {
            app.pending_export_creation = record;
            std::string file_name = record.name.empty()
                                        ? (superstar ? "CAW" : "Arena")
                                        : record.name;
            for (char& c : file_name) {
              if (std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 0x20) c = '_';
            }
            const std::string extension = superstar ? ".cas" : ".car";
            app.dialog_default_name = PathUtf8(CreationExportDefaultFolder(app.paths) /
                                               (file_name + extension));
            RequestDialog(app, DialogKind::CreationExportFile,
                          superstar ? caw_export_filters : arena_export_filters, 1, false);
          }
        }
        ImGui::SameLine();
        const std::string button_id = "Remove##saved-creation-" + std::to_string(index);
        if (LoggedSmallButton(button_id.c_str())) {
          app.pending_remove_creation = record;
          app.confirm_remove_creation = true;
        }
        ImGui::EndDisabled();
      }
      ImGui::EndTable();
    }
    ImGui::PopStyleVar();
  }
  EndPanel();
}

internal::PaintColor ToPaintColor(const ImVec4& value) {
  const auto byte = [](float v) {
    return static_cast<uint8_t>(std::clamp(std::lround(v * 255.0f), 0l, 255l));
  };
  return {byte(value.x), byte(value.y), byte(value.z), byte(value.w)};
}

void RememberPaintColor(PaintEditorState* editor, ImVec4 color) {
  for (size_t i = 0; i < editor->recent_color_count; ++i) {
    const ImVec4 old = editor->recent_colors[i];
    if (old.x == color.x && old.y == color.y && old.z == color.z && old.w == color.w) return;
  }
  const size_t last = std::min(editor->recent_color_count, editor->recent_colors.size() - 1);
  for (size_t i = last; i > 0; --i) editor->recent_colors[i] = editor->recent_colors[i - 1];
  editor->recent_colors[0] = color;
  editor->recent_color_count = std::min(editor->recent_color_count + 1, editor->recent_colors.size());
}

bool PaintTextAt(AppState& app, PaintEditorState& editor, int x, int y) {
  const char* text = editor.text.data();
  const size_t text_length = std::char_traits<char>::length(text);
  if (!text_length || !app.fonts.barlow_medium || editor.text_size <= 0.0f) return false;
  ImFont* font = app.fonts.barlow_medium;
  ImFontBaked* baked = font->GetFontBaked(editor.text_size);
  if (!baked) return false;
  const float line_height = editor.text_size * 1.2f;
  struct GlyphPlacement { ImWchar codepoint; int line; float pen; };
  std::vector<GlyphPlacement> glyphs;
  float pen = 0.0f, widest = 0.0f;
  int line = 0;
  const char* current = text;
  const char* end = text + text_length;
  while (current < end) {
    unsigned int codepoint = 0;
    const int bytes = ImTextCharFromUtf8(&codepoint, current, end);
    if (bytes <= 0) break;
    current += bytes;
    if (codepoint == '\n') {
      widest = std::max(widest, pen);
      pen = 0.0f;
      ++line;
      continue;
    }
    const ImWchar character = static_cast<ImWchar>(codepoint);
    const ImFontGlyph* glyph = baked->FindGlyph(character);
    if (!glyph) continue;
    glyphs.push_back({character, line, pen});
    pen += glyph->AdvanceX;
  }
  widest = std::max(widest, pen);
  const int mask_width = std::clamp(static_cast<int>(std::ceil(widest)), 1, 2048);
  const int mask_height = std::clamp(static_cast<int>(std::ceil((line + 1) * line_height)), 1, 512);
  ImFontAtlas* font_atlas = font->OwnerAtlas;
  ImTextureData* atlas = font_atlas ? font_atlas->TexData : nullptr;
  if (!atlas || !atlas->Pixels || atlas->Width <= 0 || atlas->Height <= 0) return false;
  std::vector<uint8_t> mask(static_cast<size_t>(mask_width) * mask_height, 0);
  for (const GlyphPlacement& placement : glyphs) {
    const ImFontGlyph* glyph = baked->FindGlyph(placement.codepoint);
    if (!glyph || !glyph->Visible) continue;
    const float gx0f = placement.pen + glyph->X0;
    const float gy0f = placement.line * line_height + baked->Ascent + glyph->Y0;
    const float gwf = glyph->X1 - glyph->X0;
    const float ghf = glyph->Y1 - glyph->Y0;
    const int gx0 = static_cast<int>(std::floor(gx0f));
    const int gy0 = static_cast<int>(std::floor(gy0f));
    const int gw = static_cast<int>(std::ceil(gwf));
    const int gh = static_cast<int>(std::ceil(ghf));
    if (gw <= 0 || gh <= 0) continue;
    for (int gy = 0; gy < gh; ++gy) {
      const int dy = y + gy0 + gy;
      if (dy < 0 || dy >= editor.canvas.height) continue;
      const float v = glyph->V0 + (static_cast<float>(gy) + 0.5f) / gh * (glyph->V1 - glyph->V0);
      const int sy = std::clamp(static_cast<int>(v * atlas->Height), 0, atlas->Height - 1);
      for (int gx = 0; gx < gw; ++gx) {
        const int dx = x + gx0 + gx;
        if (dx < 0 || dx >= editor.canvas.width) continue;
        const float u = glyph->U0 + (static_cast<float>(gx) + 0.5f) / gw * (glyph->U1 - glyph->U0);
        const int sx = std::clamp(static_cast<int>(u * atlas->Width), 0, atlas->Width - 1);
        const size_t from = (static_cast<size_t>(sy) * atlas->Width + sx) * atlas->BytesPerPixel;
        const size_t to = static_cast<size_t>(dy - y) * mask_width + (dx - x);
        const uint8_t alpha = atlas->BytesPerPixel == 1 ? atlas->Pixels[from] : atlas->Pixels[from + 3];
        mask[to] = std::max(mask[to], alpha);
      }
    }
  }
  return internal::PaintMask(&editor.canvas, x, y, mask_width, mask_height, mask, ToPaintColor(editor.color));
}

const char* PaintToolLabel(PaintTool tool) {
  switch (tool) {
    case PaintTool::Brush: return "Brush";
    case PaintTool::Eraser: return "Eraser";
    case PaintTool::Line: return "Line";
    case PaintTool::Rectangle: return "Rectangle";
    case PaintTool::Ellipse: return "Ellipse";
    case PaintTool::Fill: return "Fill bucket";
    case PaintTool::Eyedropper: return "Colour picker";
    case PaintTool::Text: return "Text";
  }
  return "Brush";
}

void DrawPaintCanvas(AppState& app, PaintEditorState& editor, const ImVec2& child_size) {
  if (!ImGui::BeginChild("paint-canvas", child_size, true, ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::EndChild();
    return;
  }
  const ImVec2 available = ImGui::GetContentRegionAvail();
  float zoom = std::clamp(editor.zoom, 1.0f, 8.0f);
  if (editor.fullscreen && editor.canvas.width > 0 && editor.canvas.height > 0) {
    const float fit = std::min(available.x / editor.canvas.width, available.y / editor.canvas.height);
    zoom = std::clamp(fit, 1.0f, 8.0f);
  }
  const ImVec2 image_size(editor.canvas.width * zoom, editor.canvas.height * zoom);
  ImVec2 cursor = ImGui::GetCursorPos();
  if (image_size.x < available.x) cursor.x += (available.x - image_size.x) * 0.5f;
  if (image_size.y < available.y) cursor.y += (available.y - image_size.y) * 0.5f;
  ImGui::SetCursorPos(cursor);
  const ImVec2 image_min = ImGui::GetCursorScreenPos();
  const ImVec2 image_max(image_min.x + image_size.x, image_min.y + image_size.y);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 clip_min = draw->GetClipRectMin(), clip_max = draw->GetClipRectMax();
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool inside = mouse.x >= image_min.x && mouse.y >= image_min.y &&
                      mouse.x < image_max.x && mouse.y < image_max.y &&
                      mouse.x >= clip_min.x && mouse.y >= clip_min.y &&
                      mouse.x < clip_max.x && mouse.y < clip_max.y;
  auto point_at = [&](ImVec2 at) {
    return internal::PaintPoint{
        std::clamp(static_cast<int>(std::floor((at.x - image_min.x) / zoom)), 0, editor.canvas.width - 1),
        std::clamp(static_cast<int>(std::floor((at.y - image_min.y) / zoom)), 0, editor.canvas.height - 1)};
  };
  const internal::PaintColor color = ToPaintColor(editor.color);
  const internal::PaintColor erase_color{color.r, color.g, color.b, 255};
  const bool left_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const bool left_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
  const bool left_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
  const bool double_clicked = left_clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  bool started_stroke = false;
  if (!app.job.active.load()) {
    const internal::PaintPoint p = point_at(mouse);
    const bool touch_was_active = editor.click_draw.Active();
    internal::PaintClickDrawTool click_draw_tool = internal::PaintClickDrawTool::None;
    if (!editor.pending_image) {
      if (editor.tool == PaintTool::Brush || editor.tool == PaintTool::Eraser)
        click_draw_tool = internal::PaintClickDrawTool::Brush;
      else if (editor.tool == PaintTool::Line || editor.tool == PaintTool::Rectangle ||
               editor.tool == PaintTool::Ellipse)
        click_draw_tool = internal::PaintClickDrawTool::Shape;
    }
    internal::PaintClickDrawInput click_draw_input{};
    click_draw_input.double_click = double_clicked;
    click_draw_input.click = left_clicked;
    click_draw_input.inside = inside;
    click_draw_input.point = p;
    click_draw_input.reuse_brush_checkpoint = editor.brush_checkpoint_available &&
                                               editor.brush_checkpoint_tool == editor.tool;
    click_draw_input.brush_checkpoint = editor.brush_checkpoint;
    const internal::PaintClickDrawUpdate click_draw_update =
        editor.click_draw.Update(click_draw_tool, click_draw_input);
    const bool click_draw_consumed = touch_was_active ||
                                     click_draw_update.action != internal::PaintClickDrawAction::None;

    const auto paint_shape = [&](internal::PaintPoint from, internal::PaintPoint to, bool record_history) {
      if (record_history) editor.history.Record(editor.canvas);
      bool changed = false;
      if (editor.tool == PaintTool::Line)
        changed = internal::PaintLine(&editor.canvas, from, to, editor.brush_size, editor.hardness / 100.0f, color);
      else if (editor.tool == PaintTool::Rectangle)
        changed = internal::PaintRectangle(&editor.canvas, from, to, editor.filled_shapes,
                                             editor.brush_size, editor.hardness / 100.0f, color);
      else if (editor.tool == PaintTool::Ellipse)
        changed = internal::PaintEllipse(&editor.canvas, from, to, editor.filled_shapes,
                                           editor.brush_size, editor.hardness / 100.0f, color);
      editor.dirty = changed || editor.dirty;
      editor.shape_active = false;
      editor.shape_tap_pending = false;
    };

    switch (click_draw_update.action) {
      case internal::PaintClickDrawAction::StartBrush:
        if (click_draw_update.record_history) {
          editor.history.Record(editor.canvas);
          editor.dirty = internal::PaintBrush(&editor.canvas, static_cast<float>(p.x), static_cast<float>(p.y),
                                                editor.brush_size, editor.hardness / 100.0f,
                                                editor.tool == PaintTool::Eraser ? erase_color : color,
                                                editor.tool == PaintTool::Eraser) || editor.dirty;
        } else if (click_draw_update.from.x != click_draw_update.to.x ||
                   click_draw_update.from.y != click_draw_update.to.y) {
          editor.dirty = internal::PaintLine(&editor.canvas, click_draw_update.from, click_draw_update.to,
                                               editor.brush_size, editor.hardness / 100.0f,
                                               editor.tool == PaintTool::Eraser ? erase_color : color,
                                               editor.tool == PaintTool::Eraser) || editor.dirty;
        }
        editor.stroke_active = false;
        editor.brush_checkpoint_available = false;
        break;
      case internal::PaintClickDrawAction::ExtendBrush:
        editor.dirty = internal::PaintLine(&editor.canvas, click_draw_update.from, click_draw_update.to,
                                             editor.brush_size, editor.hardness / 100.0f,
                                             editor.tool == PaintTool::Eraser ? erase_color : color,
                                             editor.tool == PaintTool::Eraser) || editor.dirty;
        if (click_draw_update.stop_after) editor.brush_checkpoint_available = false;
        break;
      case internal::PaintClickDrawAction::StopBrush:
        editor.brush_checkpoint_available = false;
        break;
      case internal::PaintClickDrawAction::PreviewShape:
        editor.shape_active = true;
        editor.shape_tap_pending = false;
        editor.shape_start = click_draw_update.from;
        editor.shape_preview_point = click_draw_update.to;
        break;
      case internal::PaintClickDrawAction::FinishShape:
        paint_shape(click_draw_update.from, click_draw_update.to, click_draw_update.record_history);
        break;
      case internal::PaintClickDrawAction::CancelShape:
        editor.shape_active = false;
        editor.shape_tap_pending = false;
        break;
      case internal::PaintClickDrawAction::None:
        break;
    }

    if (!click_draw_consumed && editor.pending_image) {
      if (left_clicked) editor.brush_checkpoint_available = false;
      if (editor.image_drag == PaintImageDrag::None && left_clicked && inside &&
          p.x >= editor.image_x && p.x < editor.image_x + editor.image_width &&
          p.y >= editor.image_y && p.y < editor.image_y + editor.image_height) {
        const ImVec2 handle(image_min.x + (editor.image_x + editor.image_width) * zoom,
                            image_min.y + (editor.image_y + editor.image_height) * zoom);
        editor.image_drag = std::abs(mouse.x - handle.x) <= S(app, 10.0f) &&
                                    std::abs(mouse.y - handle.y) <= S(app, 10.0f)
                                ? PaintImageDrag::Scale : PaintImageDrag::Move;
        editor.drag_start = p;
        editor.drag_origin_x = editor.image_x;
        editor.drag_origin_y = editor.image_y;
        editor.drag_origin_width = editor.image_width;
        editor.drag_origin_height = editor.image_height;
      }
      if (editor.image_drag != PaintImageDrag::None && (left_down || left_released)) {
        const int dx = p.x - editor.drag_start.x, dy = p.y - editor.drag_start.y;
        if (editor.image_drag == PaintImageDrag::Move) {
          editor.image_x = editor.drag_origin_x + dx;
          editor.image_y = editor.drag_origin_y + dy;
        } else {
          const float sx = static_cast<float>(editor.drag_origin_width + dx) / std::max(1, editor.drag_origin_width);
          const float sy = static_cast<float>(editor.drag_origin_height + dy) / std::max(1, editor.drag_origin_height);
          const float factor = std::clamp(std::max(sx, sy), 0.05f, 4.0f);
          editor.image_width = std::max(1, static_cast<int>(std::lround(editor.drag_origin_width * factor)));
          editor.image_height = std::max(1, static_cast<int>(std::lround(editor.drag_origin_height * factor)));
        }
        if (left_released) editor.image_drag = PaintImageDrag::None;
      }
    } else if (!click_draw_consumed && !editor.pending_image) {
      if (left_clicked && !inside) {
        editor.brush_checkpoint_available = false;
        if (editor.shape_tap_pending) {
          editor.shape_active = false;
          editor.shape_tap_pending = false;
        }
      }
      bool started_shape = false;
      bool shape_endpoint_click = false;
      if (left_clicked && inside) {
        switch (editor.tool) {
          case PaintTool::Brush:
          case PaintTool::Eraser:
            editor.brush_checkpoint_available = false;
            editor.history.Record(editor.canvas);
            editor.dirty = internal::PaintBrush(&editor.canvas, static_cast<float>(p.x), static_cast<float>(p.y),
                                                  editor.brush_size, editor.hardness / 100.0f,
                                                  editor.tool == PaintTool::Eraser ? erase_color : color,
                                                  editor.tool == PaintTool::Eraser) || editor.dirty;
            editor.stroke_active = true;
            editor.last_point = p;
            started_stroke = true;
            break;
          case PaintTool::Line:
          case PaintTool::Rectangle:
          case PaintTool::Ellipse:
            if (editor.shape_tap_pending) {
              editor.shape_tap_pending = false;
              editor.shape_preview_point = p;
              shape_endpoint_click = true;
            } else {
              editor.shape_active = true;
              editor.shape_start = p;
              editor.shape_preview_point = p;
              started_shape = true;
            }
            break;
          case PaintTool::Fill:
            editor.brush_checkpoint_available = false;
            editor.history.Record(editor.canvas);
            editor.dirty = internal::PaintFill(&editor.canvas, p.x, p.y, color) || editor.dirty;
            break;
          case PaintTool::Eyedropper: {
            editor.brush_checkpoint_available = false;
            const internal::PaintColor picked = internal::PickPaintColor(editor.canvas, p.x, p.y);
            editor.color = ImVec4(picked.r / 255.0f, picked.g / 255.0f, picked.b / 255.0f, picked.a / 255.0f);
            RememberPaintColor(&editor, editor.color);
            break;
          }
          case PaintTool::Text:
            editor.brush_checkpoint_available = false;
            editor.history.Record(editor.canvas);
            editor.dirty = PaintTextAt(app, editor, p.x, p.y) || editor.dirty;
            break;
        }
      }
      if (editor.stroke_active && !started_stroke && (left_down || left_released)) {
        if (p.x != editor.last_point.x || p.y != editor.last_point.y) {
          editor.dirty = internal::PaintLine(&editor.canvas, editor.last_point, p, editor.brush_size,
                                               editor.hardness / 100.0f,
                                               editor.tool == PaintTool::Eraser ? erase_color : color,
                                               editor.tool == PaintTool::Eraser) || editor.dirty;
          editor.last_point = p;
        }
        if (left_released) {
          editor.stroke_active = false;
          editor.brush_checkpoint_available = true;
          editor.brush_checkpoint = editor.last_point;
          editor.brush_checkpoint_tool = editor.tool;
        }
      }
      if (started_stroke && left_released) {
        editor.stroke_active = false;
        editor.brush_checkpoint_available = true;
        editor.brush_checkpoint = p;
        editor.brush_checkpoint_tool = editor.tool;
      }
      if (editor.shape_active) {
        if (started_shape) {
          if (left_released) editor.shape_tap_pending = true;
        } else if (!editor.shape_tap_pending || shape_endpoint_click) {
          if (left_down || left_released || shape_endpoint_click) editor.shape_preview_point = p;
          if (left_released) paint_shape(editor.shape_start, p, true);
        }
      }
    }
  }

  internal::PaintCanvas display = editor.canvas;
  if (editor.shape_active && !editor.pending_image) {
    if (editor.tool == PaintTool::Line)
      internal::PaintLine(&display, editor.shape_start, editor.shape_preview_point, editor.brush_size,
                          editor.hardness / 100.0f, color);
    else if (editor.tool == PaintTool::Rectangle)
      internal::PaintRectangle(&display, editor.shape_start, editor.shape_preview_point, editor.filled_shapes,
                               editor.brush_size, editor.hardness / 100.0f, color);
    else if (editor.tool == PaintTool::Ellipse)
      internal::PaintEllipse(&display, editor.shape_start, editor.shape_preview_point, editor.filled_shapes,
                             editor.brush_size, editor.hardness / 100.0f, color);
  }
  if (editor.pending_image) {
    internal::CompositePaintImage(&display, *editor.pending_image, editor.image_x, editor.image_y,
                                  editor.image_width, editor.image_height);
  }
  if (!editor.texture) {
    editor.texture = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                       editor.canvas.width, editor.canvas.height);
    if (editor.texture) SDL_SetTextureBlendMode(editor.texture, SDL_BLENDMODE_BLEND);
    if (editor.texture) SDL_SetTextureScaleMode(editor.texture, SDL_SCALEMODE_NEAREST);
  }
  if (!editor.texture || !SDL_UpdateTexture(editor.texture, nullptr, display.rgba.data(), editor.canvas.width * 4)) {
    ImGui::TextColored(kDanger, "The canvas could not be displayed. Close and reopen the editor.");
    ImGui::EndChild();
    return;
  }

  const float checker = std::max(4.0f, 8.0f * zoom);
  for (float y = 0.0f; y < image_size.y; y += checker) {
    for (float x = 0.0f; x < image_size.x; x += checker) {
      const int cell_x = static_cast<int>(x / checker), cell_y = static_cast<int>(y / checker);
      const ImU32 tile = ((cell_x + cell_y) & 1) ? IM_COL32(49, 50, 55, 255) : IM_COL32(71, 72, 77, 255);
      draw->AddRectFilled(ImVec2(image_min.x + x, image_min.y + y),
                          ImVec2(image_min.x + std::min(x + checker, image_size.x),
                                 image_min.y + std::min(y + checker, image_size.y)), tile);
    }
  }
  ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(editor.texture)), image_size);
  if (editor.pending_image) {
    const ImVec2 top_left(image_min.x + editor.image_x * zoom, image_min.y + editor.image_y * zoom);
    const ImVec2 bottom_right(top_left.x + editor.image_width * zoom, top_left.y + editor.image_height * zoom);
    draw->AddRect(top_left, bottom_right, ImGui::GetColorU32(kAccent), 0.0f, 0, 2.0f);
    draw->AddRectFilled(ImVec2(bottom_right.x - 6, bottom_right.y - 6),
                        ImVec2(bottom_right.x + 6, bottom_right.y + 6), ImGui::GetColorU32(kAccent));
  }
  if (editor.shape_active && !editor.click_draw.ShapeActive() && !editor.shape_tap_pending)
    ImGui::SetTooltip("Drag to draw %s", PaintToolLabel(editor.tool));
  if (editor.click_draw.BrushActive()) {
    ImGui::TextColored(kAccent, "Drawing - click to stop");
  } else if (editor.click_draw.ShapeActive()) {
    ImGui::TextColored(kAccent, "Drawing - click to set the end point");
  } else if (editor.shape_tap_pending) {
    ImGui::TextColored(kMuted, "Double-click a start point, then click an end point.");
  } else {
    ImGui::TextColored(kMuted, "Scroll to pan. Zoom %.1fx%s", zoom, editor.fullscreen ? " (fit)" : "");
  }
  ImGui::EndChild();
}

void DrawPaintEditor(AppState& app) {
  PaintEditorState& editor = app.paint_editor;
  if (!editor.open) return;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    const bool drawing = editor.click_draw.Active() || editor.stroke_active || editor.shape_active;
    if (drawing) {
      editor.click_draw.Cancel();
      editor.stroke_active = false;
      editor.shape_active = false;
      editor.shape_tap_pending = false;
      editor.brush_checkpoint_available = false;
      editor.image_drag = PaintImageDrag::None;
    } else if (editor.fullscreen) {
      SetPaintEditorFullscreen(app, false);
    }
  }
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  if (editor.fullscreen) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(screen, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);  // Full screen hides the launcher behind it.
  } else {
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::max(720.0f, std::min(1180.0f, screen.x - 32.0f)),
                                    std::max(520.0f, std::min(820.0f, screen.y - 32.0f))), ImGuiCond_Always);
  }
  ImGui::OpenPopup("Logo paint editor");
  ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoSavedSettings;
  if (editor.fullscreen) window_flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
  if (!ImGui::BeginPopupModal("Logo paint editor", nullptr, window_flags))
    return;

  ImGui::PushFont(app.fonts.oswald_semibold);
  ImGui::TextUnformatted(editor.target_slot ? "EDIT LOGO" : "DRAW LOGO");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::TextColored(kSecondary, "%u x %u", editor.canvas.width, editor.canvas.height);
  if (editor.target_slot) {
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "Slot %u", *editor.target_slot + 1);
  }
  if (editor.dirty || editor.pending_image) {
    ImGui::SameLine();
    ImGui::TextColored(kAccent, "Unsaved changes");
  }
  ImGui::SameLine();
  if (LoggedButton(editor.fullscreen ? "Exit full screen" : "Full screen", S(app, 142.0f, 30.0f)))
    SetPaintEditorFullscreen(app, !editor.fullscreen);
  ImGui::Spacing();
  ImGui::BeginDisabled(app.job.active.load());
  if (LoggedButton("Undo", S(app, 74.0f, 30.0f))) editor.history.Undo(&editor.canvas);
  ImGui::SameLine();
  if (LoggedButton("Redo", S(app, 74.0f, 30.0f))) editor.history.Redo(&editor.canvas);
  ImGui::SameLine();
  if (LoggedButton("Clear", S(app, 74.0f, 30.0f))) {
    editor.history.Record(editor.canvas);
    editor.dirty = internal::ClearPaintCanvas(&editor.canvas, {0, 0, 0, 0}) || editor.dirty;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextColored(kMuted, "History %zu / 30", editor.history.UndoCount());
  ImGui::Separator();

  const ImVec2 room = ImGui::GetContentRegionAvail();
  const float panel_height = std::max(S(app, 260.0f), room.y - S(app, 64.0f));
  const float canvas_width = std::max(S(app, 360.0f), room.x * 0.69f);
  if (ImGui::BeginTable("paint-workspace", 2, ImGuiTableFlags_SizingStretchProp, ImVec2(0, panel_height))) {
    ImGui::TableSetupColumn("canvas", ImGuiTableColumnFlags_WidthStretch, 2.25f);
    ImGui::TableSetupColumn("tools", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableNextColumn();
    DrawPaintCanvas(app, editor, ImVec2(canvas_width, panel_height));
    ImGui::TableNextColumn();
    ImGui::BeginChild("paint-tools", ImVec2(0, panel_height), true);
    ImGui::TextUnformatted("Tools");
    if (editor.tool == PaintTool::Brush || editor.tool == PaintTool::Eraser)
      ImGui::TextWrapped("Double-click the canvas to draw without holding. Click to stop.");
    else if (editor.tool == PaintTool::Line || editor.tool == PaintTool::Rectangle ||
             editor.tool == PaintTool::Ellipse)
      ImGui::TextWrapped("Double-click the canvas to set a start point, then click an end point.");
    const auto tool = [&](const char* label, PaintTool value) {
      if (ImGui::Selectable(label, editor.tool == value, 0, ImVec2(0, S(app, 28.0f)))) editor.tool = value;
    };
    tool("Brush", PaintTool::Brush);
    tool("Eraser", PaintTool::Eraser);
    tool("Line", PaintTool::Line);
    tool("Rectangle", PaintTool::Rectangle);
    tool("Ellipse", PaintTool::Ellipse);
    tool("Fill bucket", PaintTool::Fill);
    tool("Colour picker", PaintTool::Eyedropper);
    tool("Text", PaintTool::Text);
    ImGui::Separator();
    ImGui::ColorEdit4("Colour", &editor.color.x,
                      ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_PickerHueWheel);
    if (ImGui::IsItemEdited()) RememberPaintColor(&editor, editor.color);
    if (editor.recent_color_count) {
      ImGui::TextColored(kSecondary, "Recent colours");
      for (size_t i = 0; i < editor.recent_color_count; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::ColorButton("##recent", editor.recent_colors[i], ImGuiColorEditFlags_AlphaPreview,
                               S(app, 24.0f, 24.0f))) {
          editor.color = editor.recent_colors[i];
          RememberPaintColor(&editor, editor.color);
        }
        ImGui::PopID();
      }
    }
    if (editor.tool == PaintTool::Brush || editor.tool == PaintTool::Eraser || editor.tool == PaintTool::Line ||
        editor.tool == PaintTool::Rectangle || editor.tool == PaintTool::Ellipse) {
      ImGui::SliderFloat("Brush size", &editor.brush_size, 1.0f, 48.0f, "%.0f px");
      ImGui::SliderFloat("Hardness", &editor.hardness, 0.0f, 100.0f, "%.0f%%", 0);
      if (editor.tool == PaintTool::Rectangle || editor.tool == PaintTool::Ellipse)
        ImGui::Checkbox("Filled shape", &editor.filled_shapes);
    }
    if (editor.tool == PaintTool::Text) {
      ImGui::InputText("Text", editor.text.data(), editor.text.size());
      ImGui::SliderFloat("Text size", &editor.text_size, 6.0f, 64.0f, "%.0f px");
      ImGui::TextWrapped("Click the canvas to place the text.");
    }
    ImGui::Separator();
    ImGui::SliderFloat("Zoom", &editor.zoom, 1.0f, 8.0f, "%.1fx");
    ImGui::TextWrapped("Use the canvas scrollbars to pan. Transparent pixels show as a checkerboard.");
    ImGui::Spacing();
    static const SDL_DialogFileFilter paint_import_filter[]{{"Pictures", "png;jpg;jpeg;bmp;tga;gif"}};
    ImGui::BeginDisabled(app.job.active.load());
    if (LoggedButton(editor.pending_image ? "Replace picture…" : "Import picture…", ImVec2(-1, S(app, 34.0f)))) {
      RequestDialog(app, DialogKind::PaintImportFile, paint_import_filter, 1, false);
    }
    if (editor.pending_image) {
      ImGui::TextWrapped("Drag the picture to move it. Drag the red corner to resize it.");
      if (AccentButton(app, "Place picture", ImVec2(-1, S(app, 34.0f)))) {
        editor.history.Record(editor.canvas);
        editor.dirty = internal::CompositePaintImage(&editor.canvas, *editor.pending_image, editor.image_x,
                                                       editor.image_y, editor.image_width, editor.image_height) || editor.dirty;
        editor.pending_image.reset();
        editor.image_drag = PaintImageDrag::None;
      }
      if (LoggedButton("Discard picture", ImVec2(-1, S(app, 30.0f)))) {
        editor.pending_image.reset();
        editor.image_drag = PaintImageDrag::None;
      }
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::EndTable();
  }

  ImGui::Separator();
  const bool busy = app.job.active.load();
  const bool can_add_slot = app.logo_list && app.logo_list->logos.size() < kLogoSlotCount;
  ImGui::BeginDisabled(busy || editor.pending_image || (!editor.target_slot && !can_add_slot));
  if (AccentButton(app, editor.target_slot ? "Save to slot" : "Save to a logo slot", S(app, 210.0f, 38.0f)))
    StartStorePaintEditor(app);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(busy || editor.pending_image);
  if (LoggedButton("Save as PNG…", S(app, 150.0f, 38.0f))) {
    static const SDL_DialogFileFilter png_filter[]{{"PNG images", "png"}};
    app.dialog_default_name = PathUtf8(app.paths.exe_dir / "logo.png");
    RequestDialog(app, DialogKind::PaintSavePng, png_filter, 1, false);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(busy);
  if (LoggedButton("Cancel", S(app, 112.0f, 38.0f))) {
    if (editor.dirty || editor.pending_image) {
      editor.confirm_discard = true;
    } else {
      ClosePaintEditor(app);
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndDisabled();

  if (editor.confirm_discard) ImGui::OpenPopup("Discard this drawing?");
  if (ImGui::BeginPopupModal("Discard this drawing?", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextWrapped("Discard these unsaved changes?");
    if (AccentButton(app, "Discard", S(app, 110.0f, 36.0f))) {
      ClosePaintEditor(app);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (LoggedButton("Keep editing", S(app, 130.0f, 36.0f))) {
      editor.confirm_discard = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::EndPopup();
}

// Images tab: the game's 20 custom-logo slots (Paint Tool save), thumbnails, Draw / Edit / Add / Remove.
void DrawImages(AppState& app) {
  if (!app.logo_list_loaded && !app.job.active.load()) StartRefreshLogos(app);
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kSecondary, "Add pictures for custom logos, or draw one here.");
  ImGui::PopFont();
  ImGui::Spacing();
  size_t used = app.logo_list ? app.logo_list->logos.size() : 0;
  const bool empty = app.logo_list && app.logo_list->logos.empty();
  ImGui::BeginDisabled(app.job.active.load() || !app.logo_list.has_value() || used >= kLogoSlotCount);
  if (AccentButton(app, empty ? "Add your first logo" : "Add logo…", S(app, 190.0f, 38.0f))) {
    OpenLogoImport(app, false);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(app.job.active.load() || !app.logo_list.has_value() || used >= kLogoSlotCount);
  if (AccentButton(app, "Draw logo…", S(app, 150.0f, 38.0f))) OpenNewPaintEditor(app);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextColored(kSecondary, "Size:");
  ImGui::SameLine();
  ImGui::BeginDisabled(app.job.active.load());
  DrawSegment(app, "256 x 256", g_logo_import_size == LogoImageSize::k256, S(app, 100.0f), [] {
    g_logo_import_size = LogoImageSize::k256;
  });
  ImGui::SameLine();
  DrawSegment(app, "128 x 128", g_logo_import_size == LogoImageSize::k128, S(app, 100.0f), [] {
    g_logo_import_size = LogoImageSize::k128;
  });
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::PushFont(app.fonts.barlow_small);
  if (!app.logo_list_loaded) {
    ImGui::TextColored(kMuted, "Reading the saved logos…");
  } else if (!app.logo_list.has_value()) {
    ImGui::TextColored(kDanger, "The Paint Tool save is not in a supported format, so it is left alone.");
  } else if (used >= kLogoSlotCount) {
    ImGui::TextColored(kDanger, "All %u slots are full. Remove a logo to add another.", kLogoSlotCount);
  } else {
    ImGui::TextColored(kMuted, "%zu of %u slots used (includes the game's own logos) · Remove deletes that logo from the list · drop pictures here", used, kLogoSlotCount);
  }
  ImGui::PopFont();
  ImGui::Spacing();
  if (!app.logo_list.has_value()) {
    if (app.logo_list_loaded)
      DrawSecondaryWrapped(app, "Saved logos could not be read safely, so no changes were made. Close WWE 2K14 and try again.", kDanger);
    return;
  }
  if (empty) {
    DrawSecondaryWrapped(app, "No custom logos yet. Add a PNG or JPG picture and it will be fitted to a logo slot.", kSecondary);
    return;
  }
  const float thumb = S(app, 112.0f);
  const float cell_w = thumb + S(app, 24.0f);
  const float avail = ImGui::GetContentRegionAvail().x;
  const int per_row = std::max(1, static_cast<int>(avail / cell_w));
  int column = 0;
  for (const LogoImage& logo : app.logo_list->logos) {
    if (column > 0) ImGui::SameLine();
    ImGui::BeginGroup();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Checkerboard-free backdrop so transparent logos stay readable.
    draw->AddRectFilled(origin, ImVec2(origin.x + thumb, origin.y + thumb), IM_COL32(60, 64, 74, 255), S(app, 4.0f));
    SDL_Texture* texture = nullptr;
    for (const auto& [slot, tex] : app.logo_textures) if (slot == logo.slot_index) texture = tex;
    if (texture) {
      ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)), ImVec2(thumb, thumb));
    } else {
      ImGui::Dummy(ImVec2(thumb, thumb));
    }
    ImGui::PushFont(app.fonts.barlow_small);
    ImGui::TextColored(kSecondary, "Slot %u · %u x %u", logo.slot_index + 1, logo.width, logo.height);
    if (logo.built_in) {
      ImGui::TextColored(kMuted, "Game logo");
    } else {
      ImGui::BeginDisabled(app.job.active.load());
      const std::string edit_id = "Edit##logo" + std::to_string(logo.slot_index);
      if (LoggedSmallButton(edit_id.c_str())) OpenLogoPaintEditor(app, logo);
      ImGui::SameLine();
      const std::string id = "Remove##logo" + std::to_string(logo.slot_index);
      if (LoggedSmallButton(id.c_str())) StartRemoveLogo(app, logo.slot_index);
      ImGui::EndDisabled();
    }
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::SameLine(0.0f, S(app, 16.0f));
    ImGui::Dummy(ImVec2(0, 0));
    if (++column >= per_row) { column = 0; ImGui::NewLine(); }
  }
  if (column) ImGui::NewLine();
}

// Stories tab: the story files in the save, Import story, and Export per row. Same card style as Superstars/Arenas.
void DrawStories(AppState& app) {
  if (!app.story_list_loaded && !app.job.active.load()) StartRefreshStories(app);
  const bool supported = app.story_list.has_value();
  const bool loading = !app.story_list_loaded;
  const size_t rows = supported ? app.story_list->size() : 0;
  const float table_height = S(app, 34.0f) + S(app, 38.0f) * static_cast<float>(std::clamp<size_t>(rows, 1, 5));
  BeginPanel(app, "suite-stories", ImVec2(0, S(app, 122.0f) + table_height));
  ImGui::PushFont(app.fonts.barlow_semibold);
  ImGui::TextUnformatted("Stories");
  ImGui::PopFont();
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::TextColored(kSecondary, "Share your custom stories as .sas files. Import adds one to the next free slot; drop a file here too.");
  ImGui::PopFont();
  ImGui::Spacing();
  static const SDL_DialogFileFilter story_filters[]{{"Story files", "sas;zip"}};
  auto import_story = [&] {
    if (GameProcessRunning(app.paths)) {
      SetBanner(app, kCloseGameStoriesMessage);
    } else {
      RequestDialog(app, DialogKind::StoryImportFile, story_filters, 1, false);
    }
  };
  if (loading || !supported || rows > 0) {
    ImGui::BeginDisabled(!supported || loading || app.job.active.load());
    if (AccentButton(app, "Import story…##suite-stories", S(app, 176.0f, 38.0f))) import_story();
    ImGui::EndDisabled();
  }
  ImGui::Spacing();
  if (loading) {
    DrawTextDisabled(app, "Reading the saved stories…");
  } else if (!supported) {
    DrawSecondaryWrapped(app, "The save folder could not be read safely, so no changes were made. Close WWE 2K14 and try again.", kDanger);
  } else if (rows == 0) {
    DrawSecondaryWrapped(app, "No stories yet. Import a story file or start the game once to create a save.", kSecondary);
    ImGui::Spacing();
    ImGui::BeginDisabled(app.job.active.load());
    if (AccentButton(app, "Add a story", S(app, 176.0f, 38.0f))) import_story();
    ImGui::EndDisabled();
  } else {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, S(app, 14.0f, 8.0f));
    if (ImGui::BeginTable("saved-suite-stories", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, table_height))) {
      ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch, 0.58f);
      ImGui::TableSetupColumn("GAME SLOT", ImGuiTableColumnFlags_WidthFixed, S(app, 120.0f));
      ImGui::TableSetupColumn("ACTION", ImGuiTableColumnFlags_WidthFixed, S(app, 110.0f));
      ImGui::TableHeadersRow();
      for (size_t index = 0; index < app.story_list->size(); ++index) {
        const StoryInfo story = (*app.story_list)[index];
        ImGui::TableNextRow(ImGuiTableRowFlags_None, S(app, 38.0f));
        ImGui::TableSetColumnIndex(0);
        if (story.damaged) {
          ImGui::TextColored(kDanger, "(unreadable)");
        } else {
          ImGui::TextUnformatted(story.name.empty() ? "(unnamed)" : story.name.c_str());
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%u", story.slot_index + 1);
        ImGui::TableSetColumnIndex(2);
        ImGui::BeginDisabled(app.job.active.load() || story.damaged);
        const std::string button_id = "Export##saved-story-" + std::to_string(index);
        if (LoggedSmallButton(button_id.c_str())) {
          app.pending_export_story = story;
          std::string file_name = story.name.empty() ? "story" : story.name;
          for (char& c : file_name) {
            if (std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 0x20) c = '_';
          }
          app.dialog_default_name = PathUtf8(app.paths.exe_dir / (file_name + ".sas"));
          static const SDL_DialogFileFilter export_filters[]{{"Story files", "sas"}};
          RequestDialog(app, DialogKind::StoryExportFile, export_filters, 1, false);
        }
        ImGui::EndDisabled();
      }
      ImGui::EndTable();
    }
    ImGui::PopStyleVar();
  }
  EndPanel();
}

void DrawCreationSuite(AppState& app) {
  int& suite_tab = g_suite_tab;
  DrawSecondaryWrapped(app, kCloseGameCreationsMessage, kDanger);
  DrawSecondaryWrapped(app, "A full save backup is made before every change.", ImVec4(1.0f, 0.68f, 0.25f, 1.0f));
  ImGui::Spacing();
  static const char* const labels[5] = {"Superstars", "Arenas", "Music", "Logos", "Stories"};
  const float tab_width = (ImGui::GetContentRegionAvail().x - 4.0f * ImGui::GetStyle().ItemSpacing.x) / 5.0f;
  for (int i = 0; i < 5; ++i) {
    if (i) ImGui::SameLine();
    DrawSegment(app, labels[i], suite_tab == i, tab_width, [i] { suite_tab = i; });
  }
  ImGui::Spacing();
  if (suite_tab == 0) {
    DrawCreationCard(app, "suite-caw", "Superstars (CAWs)",
                     "Add custom wrestlers. Use More ways to add to choose another import method.",
                     CreationSaveKind::kSuperstar);
  } else if (suite_tab == 1) {
    DrawCreationCard(app, "suite-arena", "Arenas", "Add custom arenas to your save.",
                     CreationSaveKind::kArena);
  } else if (suite_tab == kSuiteTabImages) {
    DrawImages(app);
  } else if (suite_tab == kSuiteTabStories) {
    DrawStories(app);
  } else {
    DrawMusic(app);
  }
  DrawPaintEditor(app);
  ImGui::Spacing();

  if (app.confirm_import_creation) ImGui::OpenPopup("Confirm creation import?");
  if (ImGui::BeginPopupModal("Confirm creation import?", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    const CreationSaveKind kind = app.pending_import_creation_kind;
    const auto free_slot = FindFreeCreationSaveSlot(app.paths, kind);
    if (free_slot) {
      ImGui::TextWrapped("Add this %s and its saved files to game slot %u?",
                         CreationSaveKindLabel(kind), *free_slot + 1);
    } else {
      ImGui::TextWrapped("There is no empty slot for this %s. Remove one or choose a different save, then try again.",
                         CreationSaveKindLabel(kind));
    }
    ImGui::TextWrapped("A full save backup is made and checked before any files are changed.");
    ImGui::Spacing();
    ImGui::BeginDisabled(!free_slot || app.pending_import_creation_folder.empty() || app.job.active.load());
    if (AccentButton(app, "Import", S(app, 120.0f, 38.0f))) {
      const fs::path folder = app.pending_import_creation_folder;
      app.confirm_import_creation = false;
      app.pending_import_creation_folder.clear();
      ImGui::CloseCurrentPopup();
      StartImportCreationSave(app, kind, folder);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (LoggedButton("Cancel##creation-import", S(app, 120.0f, 38.0f))) {
      app.confirm_import_creation = false;
      app.pending_import_creation_folder.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (app.confirm_import_story) ImGui::OpenPopup("Import this story?");
  if (ImGui::BeginPopupModal("Import this story?", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    const auto free_slot = FindFreeStorySlot(app.paths);
    if (free_slot) {
      ImGui::TextWrapped("Add this story to story slot %u?", *free_slot + 1);
    } else {
      ImGui::TextWrapped("There is no empty story slot. Remove a saved story and try again.");
    }
    ImGui::TextWrapped("The story file and a full save backup are checked before the save is changed.");
    ImGui::Spacing();
    ImGui::BeginDisabled(!free_slot || app.pending_import_story.empty() || app.job.active.load());
    if (AccentButton(app, "Import", S(app, 120.0f, 38.0f))) {
      const fs::path source = app.pending_import_story;
      app.confirm_import_story = false;
      app.pending_import_story.clear();
      ImGui::CloseCurrentPopup();
      StartImportStory(app, source);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (LoggedButton("Cancel##story-import", S(app, 120.0f, 38.0f))) {
      app.confirm_import_story = false;
      app.pending_import_story.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (app.confirm_remove_creation) ImGui::OpenPopup("Remove this saved creation?");
  if (ImGui::BeginPopupModal("Remove this saved creation?", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    const CreationSaveRecord record = app.pending_remove_creation;
    ImGui::TextWrapped("Remove this %s from game slot %u? A full save backup is made first.",
                       CreationSaveKindLabel(record.kind), record.slot_index + 1);
    if (record.slot_index < 2) {
      DrawSecondaryWrapped(app, "This slot may contain an original game creation. Check its name before removing it.", kDanger);
    }
    ImGui::Spacing();
    if (AccentButton(app, "Remove", S(app, 120.0f, 38.0f))) {
      app.confirm_remove_creation = false;
      ImGui::CloseCurrentPopup();
      StartRemoveCreationSave(app, record);
    }
    ImGui::SameLine();
    if (LoggedButton("Cancel", S(app, 120.0f, 38.0f))) {
      app.confirm_remove_creation = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (app.confirm_rename_creation) ImGui::OpenPopup("Rename this creation?");
  if (ImGui::BeginPopupModal("Rename this creation?", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    const CreationSaveRecord record = app.pending_rename_creation;
    const size_t limit = record.kind == CreationSaveKind::kSuperstar ? 42 : 22;
    ImGui::TextWrapped("Current name: %s", record.name.empty() ? "(unnamed)" : record.name.c_str());
    ImGui::SetNextItemWidth(S(app, 360.0f));
    ImGui::InputText("New name", app.rename_creation_name.data(), app.rename_creation_name.size());
    ImGui::TextWrapped("Use 1 to %zu printable characters. The save and creation file will get the same new name.",
                       limit);
    ImGui::Spacing();
    ImGui::BeginDisabled(app.job.active.load());
    if (AccentButton(app, "Save", S(app, 120.0f, 38.0f))) {
      std::string new_name(app.rename_creation_name.data());
      app.confirm_rename_creation = false;
      ImGui::CloseCurrentPopup();
      StartRenameCreation(app, record, std::move(new_name));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (LoggedButton("Cancel##rename-creation", S(app, 120.0f, 38.0f))) {
      app.confirm_rename_creation = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  DrawProgress(app);
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
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
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
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
  ImGui::BeginChild("operation-progress", ImVec2(0, S(app, 94.0f)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::SetCursorPos(S(app, 14, 10));
  ImGui::TextUnformatted(label.empty() ? "Working…" : label.c_str());
  ImGui::SameLine(ImGui::GetWindowWidth() - S(app, 90.0f));
  if (LoggedSmallButton("Cancel")) app.job.cancel.store(true);
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
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
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

// First start of a new installation: the previous installation that was found, offered once at the top of Game files.
void DrawFirstRunImportOffer(AppState& app) {
  const InstallationImportInfo& info = app.import_info;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
  ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, S(app, 1.5f));
  ImGui::BeginChild("first-run-import-offer", ImVec2(0, S(app, 150.0f)), true, ImGuiWindowFlags_NoScrollbar);
  ImGui::SetCursorPos(S(app, 14.0f, 10.0f));
  ImGui::PushFont(app.fonts.barlow_semibold);
  ImGui::TextUnformatted("PREVIOUS INSTALLATION FOUND");
  ImGui::PopFont();
  ImGui::SetCursorPosX(S(app, 14.0f));
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
  ImGui::TextWrapped("%s", PathUtf8(info.source_root).c_str());
  ImGui::SetCursorPosX(S(app, 14.0f));
  ImGui::TextWrapped("Bring over its game folder and title update, %llu DLC files, %llu save files, music, keys and "
                     "settings. Nothing in the old folder is changed.",
                     static_cast<unsigned long long>(info.dlc_files), static_cast<unsigned long long>(info.save_files));
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::SetCursorPos(ImVec2(S(app, 14.0f), ImGui::GetWindowHeight() - S(app, 50.0f)));
  if (AccentButton(app, "Bring everything over", S(app, 220.0f, 38.0f))) {
    PerfLog("first-run offer: bring everything over from %s", PathUtf8(app.import_info.source_root).c_str());
    app.first_run_offer_visible = false;
    StartImportInstallation(app);
  }
  ImGui::SameLine();
  if (LoggedButton("Not now", S(app, 120.0f, 38.0f))) {
    app.first_run_offer_visible = false;
    SaveSettings(app);  // offered once; Import Previous Installation below stays available
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
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
  if (app.first_run_offer_visible && !busy) {
    DrawFirstRunImportOffer(app);
    ImGui::Spacing();
  }
  DrawInstallationImportConfirmation(app);
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
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
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
      if (LoggedButton("Find Automatically", ImVec2(-1.0f, S(app, 32.0f)))) StartFindGameFiles(app);
      DrawTextDisabled(app, "Look for your game folder");
    }
    EndPanel();
    ImGui::TableNextColumn();
    if (BeginPanel(app, "use-disc", ImVec2(tile, S(app, 88.0f)))) {
      if (LoggedButton("Use a Disc Image", ImVec2(-1.0f, S(app, 32.0f)))) {
        static const SDL_DialogFileFilter filters[]{{"Disc images", "iso;img"}};
        RequestDialog(app, DialogKind::DiscImage, filters, 1, false);
      }
      DrawTextDisabled(app, "Choose an .iso to extract");
    }
    EndPanel();
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (BeginPanel(app, "add-package", ImVec2(tile, S(app, 88.0f)))) {
      if (LoggedButton("Add TU or DLC", ImVec2(-1.0f, S(app, 32.0f)))) {
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
      if (LoggedButton("Choose a Folder", ImVec2(-1.0f, S(app, 32.0f)))) {
        RequestDialog(app, DialogKind::GameFolder, nullptr, 0, false);
      }
      DrawTextDisabled(app, "Check and save a game folder");
    }
    EndPanel();
    ImGui::EndTable();
  }

  // Older version or test package on this PC: bring over its game folder (with the title update), DLC, saves,
  // music, keys and settings. The launcher also offers this once on the first start of a new installation.
  ImGui::Spacing();
  if (BeginPanel(app, "import-previous-installation", ImVec2(0, S(app, 88.0f)))) {
    if (LoggedButton("Import Previous Installation", ImVec2(-1.0f, S(app, 32.0f)))) StartFindInstallationImports(app);
    DrawTextDisabled(app, "Bring over game folder, title update, DLC and saves from an older version");
  }
  EndPanel();
  DrawInstallationImportList(app);

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
        if (LoggedSmallButton(id.c_str())) StartChooseGameFolder(app, app.candidates[i].folder);
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

void DrawInstallationImportList(AppState& app) {
  if (!app.import_list_open) return;
  ImGui::Spacing();
  const float panel_height = app.import_candidates.empty() ? S(app, 154.0f) : S(app, 345.0f);
  if (BeginPanel(app, "found-installations-to-import", ImVec2(0, panel_height))) {
    DrawPanelHeading(app, "INSTALLATIONS FOUND", "Choose saved data from another WWE 2K14 installation.");
    if (AccentButton(app, "Choose a Folder...", S(app, 174.0f, 36.0f))) {
      RequestDialog(app, DialogKind::ImportFolder, nullptr, 0, false);
    }
    ImGui::SameLine();
    if (LoggedButton("Search Again", S(app, 130.0f, 36.0f))) StartFindInstallationImports(app);

    if (app.installation_search_running && app.import_candidates.empty()) {
      ImGui::Spacing();
      DrawSecondaryWrapped(app, "Searching for saved games and DLC...");
    } else if (app.import_candidates.empty()) {
      ImGui::Spacing();
      DrawSecondaryWrapped(
          app,
          "Nothing was found. We searched Downloads, Desktop, Documents, your home folder, and available drives. "
          "Choose the old game folder, its saved-data folder, a folder inside saved data, or an archive folder. "
          "Look for saved games or DLC. An archive folder may contain WWE2K14-Recomp-x64 or Linux-x86_64.");
    } else {
      ImGui::Spacing();
      if (ImGui::BeginTable("installation-import-table", 6,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                            ImVec2(0, S(app, 215.0f)))) {
        ImGui::TableSetupColumn("INSTALLATION", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("SAVES", ImGuiTableColumnFlags_WidthFixed, S(app, 62.0f));
        ImGui::TableSetupColumn("DLC", ImGuiTableColumnFlags_WidthFixed, S(app, 52.0f));
        ImGui::TableSetupColumn("SIZE", ImGuiTableColumnFlags_WidthFixed, S(app, 78.0f));
        ImGui::TableSetupColumn("NEWEST SAVE", ImGuiTableColumnFlags_WidthFixed, S(app, 112.0f));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(app, 82.0f));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < app.import_candidates.size(); ++i) {
          const FoundInstallationImport candidate = app.import_candidates[i];
          ImGui::TableNextRow(ImGuiTableRowFlags_None, S(app, 36.0f));
          ImGui::TableSetColumnIndex(0);
          const std::string path = PathUtf8(candidate.folder);
          ImGui::TextUnformatted(path.c_str());
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
          ImGui::TableSetColumnIndex(1);
          ImGui::Text("%llu", static_cast<unsigned long long>(candidate.saves));
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%llu", static_cast<unsigned long long>(candidate.dlc_packs));
          ImGui::TableSetColumnIndex(3);
          ImGui::TextUnformatted(FriendlyBytes(candidate.bytes).c_str());
          ImGui::TableSetColumnIndex(4);
          const std::string newest = candidate.newest_save_when.empty()
                                         ? std::string("—")
                                         : FriendlyDate(candidate.newest_save_when);
          ImGui::TextUnformatted(newest.c_str());
          ImGui::TableSetColumnIndex(5);
          const std::string use_id = "Use this##installation" + std::to_string(i);
          if (LoggedSmallButton(use_id.c_str())) StartScanInstallationImport(app, candidate.folder);
        }
        ImGui::EndTable();
      }
    }
  }
  EndPanel();
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
  DrawSettingsCheckbox(app, "Controller vibration", &app.settings.controller_vibration,
                       "Untick to stop the controller rumbling during play.",
                       [&app] { SaveSettings(app); });
  DrawSettingsCheckbox(app, "Keep my keys as buttons while typing names", &app.settings.keybinds_while_typing,
                       "On name and text screens, letter and number keys you set below press their controller "
                       "button instead of typing. Other keys still type.",
                       [&app] { SaveSettings(app); });
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
                                                               : kRaised);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, capturing       ? kAccent
                                                      : duplicates[i] ? ImVec4(0.58f, 0.20f, 0.24f, 1.0f)
                                                                      : Rgba(70, 29, 33));
        const int colors = 2;
        if (LoggedButton(label.c_str(), ImVec2(-1.0f, 0.0f))) {
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
  if (LoggedButton("Reset to Defaults", S(app, 175.0f, 38.0f))) {
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
    if (LoggedButton("Use Esc", S(app, 120.0f, 38.0f))) FinishKeyCapture(app, "Escape", false);
    ImGui::SameLine();
    if (LoggedButton("Cancel", S(app, 120.0f, 38.0f))) FinishKeyCapture(app, nullptr, true);
  }
  if (!app.controls_status.empty()) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(app.controls_status_ok ? kMuted : kDanger, "%s", app.controls_status.c_str());
  }
}

// "Bring over your previous installation?": started from Game files (Import Previous Installation) or Saves, so both
// pages draw it (owner 11-13: on Game files it never appeared, so nothing was brought over).
void DrawInstallationImportConfirmation(AppState& app) {
  if (app.import_confirmation_pending) {
    ImGui::OpenPopup("Bring over your previous installation?");
    app.import_confirmation_pending = false;
  }
  if (ImGui::BeginPopupModal("Bring over your previous installation?", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    if (app.import_first_run) {
      ImGui::TextWrapped("Welcome to version %s. We found your previous WWE 2K14 installation. Bring everything over "
                         "(game folder and title update, DLC, saves, music, keys and settings)? Nothing in the old "
                         "folder is changed.", WWE2K14_LAUNCHER_VERSION);
      ImGui::Spacing();
    }
    ImGui::TextWrapped("Found another installation at:");
    ImGui::TextWrapped("%s", PathUtf8(app.import_info.source_root).c_str());
    ImGui::Spacing();
    ImGui::BulletText("Saved game data: %llu files, %s",
                      static_cast<unsigned long long>(app.import_info.save_files),
                      FriendlyBytes(app.import_info.save_bytes).c_str());
    ImGui::BulletText("DLC: %llu files, %s",
                      static_cast<unsigned long long>(app.import_info.dlc_files),
                      FriendlyBytes(app.import_info.dlc_bytes).c_str());
    ImGui::BulletText("Other userdata: %llu files, %s",
                      static_cast<unsigned long long>(app.import_info.other_userdata_files),
                      FriendlyBytes(app.import_info.other_userdata_bytes).c_str());
    ImGui::BulletText("Music: %llu files, %s",
                      static_cast<unsigned long long>(app.import_info.music_files),
                      FriendlyBytes(app.import_info.music_bytes).c_str());
    ImGui::TextUnformatted(app.import_info.has_ini ? "Launcher choices found (game folder, GPU, display, stretch)."
                                                   : "No launcher settings file found.");
    ImGui::TextUnformatted(app.import_info.has_keyboard_settings ? "Keyboard settings found." : "No keyboard settings found.");
    if (app.import_info.backup_required) {
      ImGui::Spacing();
      ImGui::TextWrapped("Your current saves, DLC, and music will be backed up to the backups folder first.");
    }
    ImGui::Spacing();
    if (AccentButton(app, app.import_first_run ? "Bring everything over" : "Import",
                     S(app, app.import_first_run ? 220.0f : 120.0f, 38.0f))) {
      ImGui::CloseCurrentPopup();
      app.import_first_run = false;
      StartImportInstallation(app);
    }
    ImGui::SameLine();
    if (LoggedButton(app.import_first_run ? "Not now" : "Cancel", S(app, 120.0f, 38.0f))) {
      ImGui::CloseCurrentPopup();
      if (app.import_first_run) {
        app.import_first_run = false;
        SaveSettings(app);  // offered once; the Saves page keeps Import from another installation
      }
    }
    ImGui::EndPopup();
  }
}

void DrawSaves(AppState& app) {
  if (AccentButton(app, "Back Up Now", S(app, 175.0f, 42.0f))) StartBackup(app);
  ImGui::SameLine();
  if (LoggedButton("Import a Backup…", S(app, 175.0f, 42.0f))) {
    static const SDL_DialogFileFilter filters[]{{"Save backups", "zip"}};
    RequestDialog(app, DialogKind::Backup, filters, 1, false);
  }
  ImGui::SameLine();
  if (LoggedButton("Open Save Folder", S(app, 175.0f, 42.0f))) {
    const std::string url = FileUrl(app.paths.userdata_dir);
    if (!SDL_OpenURL(url.c_str())) SetBanner(app, "The save folder could not be opened.");
  }
  ImGui::Spacing();
  if (LoggedCheckbox("Back up automatically before each launch (keep last 5)", &app.settings.auto_backup)) {
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
      if (backup.tag_known) {
        ImGui::SameLine(0.0f, S(app, 12.0f));
        DrawSaveTag(app, backup.modded);
      }
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(FriendlyBytes(backup.bytes).c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(backup.automatic ? "Automatic" : "Manual");
      ImGui::TableSetColumnIndex(3);
      const std::string id = "Restore##backup" + std::to_string(i);
      if (LoggedSmallButton(id.c_str())) {
        if (!SaveOperationsAllowed(GameProcessRunning(app.paths))) {
          SetBanner(app, kGameRunningRestoreMessage);
        } else {
          app.restore_file = backup.file;
          open_restore_popup = true;
        }
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
    if (LoggedButton("Cancel", S(app, 120.0f, 38.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  DrawInstallationImportConfirmation(app);
  DrawProgress(app);
}

constexpr float kHeaderHeight = 112.0f;

// Header (Titantron mockup B): WWE 2K14 wordmark + version on the left, one status pill on the right. No tabs: the
// home screen's tiles open the pages and every other page has a Back button (B / Esc).
void DrawTopBar(AppState& app) {
  ImGui::BeginChild("launcher-topbar", ImVec2(0, S(app, kHeaderHeight)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
  ImGui::SetCursorPos(S(app, 48, 26));
  ImGui::PushFont(app.fonts.oswald_bold, S(app, 52));
  ImGui::TextUnformatted("WWE");
  ImGui::SameLine(0, S(app, 12));
  ImGui::TextColored(kAccent, "2K14");
  ImGui::PopFont();
  ImGui::SetCursorPos(S(app, 48, 84));
  ImGui::PushFont(app.fonts.oswald_semibold, S(app, 15));
  ImGui::TextColored(kMuted, "P C   R E C O M P I L E D   \xC2\xB7   v%s", WWE2K14_LAUNCHER_VERSION);
  ImGui::PopFont();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Build %s", WWE2K14_LAUNCHER_BUILD_COMMIT);

  const bool game_ready = app.game_files.ready();
  const std::string status = StatusLine(app);
  ImGui::PushFont(app.fonts.barlow_small);
  const ImVec2 text = ImGui::CalcTextSize(status.c_str());
  const ImVec2 pill(text.x + S(app, 52.0f), S(app, 42.0f));
  ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - S(app, 48.0f) - pill.x, S(app, 50.0f)));
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::PushStyleColor(ImGuiCol_Button, Rgba(20, 20, 22));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kHover);
  if (LoggedButton("##status-pill", pill) && !game_ready) app.tab = Tab::GameFiles;
  ImGui::PopStyleColor(3);
  if (ImGui::IsItemHovered() && game_ready)
    ImGui::SetTooltip("TU 1.0.3 \xC2\xB7 %d DLC packs", app.game_files.dlc_installed);
  ImDrawList* d = ImGui::GetWindowDrawList();
  d->AddRect(at, ImVec2(at.x + pill.x, at.y + pill.y), ImGui::GetColorU32(kBorder));
  d->AddCircleFilled(ImVec2(at.x + S(app, 21.0f), at.y + pill.y / 2.0f), S(app, 5.0f),
                     ImGui::GetColorU32(game_ready ? kSuccess : kDanger));
  d->AddText(ImVec2(at.x + S(app, 36.0f), at.y + (pill.y - text.y) / 2.0f), ImGui::GetColorU32(kText),
             status.c_str());
  ImGui::PopFont();
  ImGui::EndChild();
}

// Text link look for small actions (no button box): transparent frame, lighter text on hover.
bool LinkButton(const char* label) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.10f));
  const bool pressed = LoggedSmallButton(label);
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImGui::PopStyleColor(3);
  return pressed;
}

void DrawFooter(AppState& app) {
  ImGui::SetCursorPos(ImVec2(0,ImGui::GetWindowHeight()-S(app,56)));
  ImGui::PushStyleColor(ImGuiCol_ChildBg,kSidebar);
  ImGui::BeginChild("launcher-footer", ImVec2(0,S(app,56)), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  const float y = 0;
  const ImVec2 window_pos = ImGui::GetWindowPos();
  const float right_edge = ImGui::GetIO().DisplaySize.x;
  // Footer hint (owner 11-02: the A/B controller icons were removed; controllers still work).
  {
    ImGui::PushFont(app.fonts.barlow_small);
    ImDrawList* d = ImGui::GetWindowDrawList();
    const float x = window_pos.x + S(app, 48.0f);
    const float cy = window_pos.y + S(app, 28.0f);
    const char* shift = "Hold Shift at start to come back here";
    d->AddText(ImVec2(x, cy - ImGui::CalcTextSize(shift).y / 2.0f), ImGui::GetColorU32(kMuted), shift);
    ImGui::PopFont();
  }
  const char* report_label = "Bug Reports";
  const char* readme_label = "Read Me";
  const char* discord_label = "Join our Discord";
  const ImGuiStyle& style = ImGui::GetStyle();
  const float button_padding = style.FramePadding.x;
  const float links_width = app.fonts.barlow_small->CalcTextSizeA(
                                app.fonts.barlow_small->LegacySize, 100000.0f, 0.0f, discord_label).x +
                            2.0f * button_padding + style.ItemSpacing.x +
                            app.fonts.barlow_small->CalcTextSizeA(
                                app.fonts.barlow_small->LegacySize, 100000.0f, 0.0f, report_label).x +
                            2.0f * button_padding + style.ItemSpacing.x +
                            app.fonts.barlow_small->CalcTextSizeA(
                                app.fonts.barlow_small->LegacySize, 100000.0f, 0.0f, readme_label).x +
                            2.0f * button_padding;
  ImGui::SetCursorScreenPos(ImVec2(right_edge - S(app, 44.0f) - links_width, window_pos.y + y + S(app, 14.0f)));
  ImGui::PushFont(app.fonts.barlow_small);
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  // Community Discord (owner 2026-10-25). Opens the never-expiring invite in the browser; the launcher itself never
  // contacts Discord.
  if (LinkButton(discord_label) && !SDL_OpenURL("https://discord.gg/EFnDwGB332"))
    SetBanner(app, "The Discord invite could not be opened.");
  ImGui::SameLine();
  if (LinkButton(report_label)) {
    app.tab = Tab::BugReports;
    app.bug_page_loaded = false;  // re-read: a session may have ended since
    app.banner.clear();
  }
  ImGui::SameLine();
  if (LinkButton(readme_label)) {
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
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------------------------- Bug Reports page
// Owner 2026-11-09: one place to send us a report: the recent game sessions (crashes marked), the saved reports and a
// link to GitHub. The crash watcher saves a crashed session's report by itself, and the crash pop-up opens this page.
void StartLoadBugPage(AppState& app) {
  if (app.job.active.load()) return;
  app.bug_page_loaded = true;
  const Paths paths = app.paths;
  BeginJob(app, "Reading recent game sessions…", [&app, paths](const ProgressFn&, CancelFlag&) {
    std::vector<GameSession> sessions = ListRecentGameSessions(paths, 10);
    std::vector<BugReportFile> reports = ListBugReports(paths, 10);
    const bool running = GameProcessRunning(paths);
    return JobOutcome{Result::Ok(), [&app, sessions = std::move(sessions), reports = std::move(reports),
                                     running]() mutable {
                        app.bug_sessions = std::move(sessions);
                        app.bug_reports = std::move(reports);
                        app.game_running_now = running;
                      }};
  });
}

void StartSessionBugReport(AppState& app, const fs::path& session_log) {
  if (app.job.active.load()) return;
  const Paths paths = app.paths;
  PerfLog("bug report for session %s", PathUtf8(session_log.filename()).c_str());
  BeginJob(app, "Saving a bug report…", [&app, paths, session_log](const ProgressFn&, CancelFlag&) {
    fs::path created;
    const Result result = SaveBugReport(paths, &created, session_log);
    std::vector<GameSession> sessions;
    std::vector<BugReportFile> reports;
    if (result.ok) {
      sessions = ListRecentGameSessions(paths, 10);
      reports = ListBugReports(paths, 10);
    }
    return JobOutcome{result, [&app, created, sessions = std::move(sessions), reports = std::move(reports)]() mutable {
                        app.bug_sessions = std::move(sessions);
                        app.bug_reports = std::move(reports);
                        SetBanner(app, "Bug report saved: " + PathUtf8(created.filename()) +
                                           ". Click Report on GitHub and drag this file into your message.", true);
                      }};
  });
}

void ShowReportFile(AppState& app, const fs::path& file) {
  if (ShowFileInFolder(file)) return;
  const std::string url = FileUrl(file.parent_path());
  if (!SDL_OpenURL(url.c_str())) SetBanner(app, "The folder could not be opened. The report is at " + PathUtf8(file));
}

std::string SessionStartedText(const GameSession& session) {
  std::string iso = session.started;
  if (iso.size() > 10) iso[10] = 'T';
  return iso.empty() ? PathUtf8(session.log.filename()) : FriendlyDate(iso);
}

void DrawBugReports(AppState& app) {
  if (!app.bug_page_loaded && app.initialized && !app.job.active.load()) StartLoadBugPage(app);
  ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
  ImGui::TextUnformatted("Did the game crash or something look wrong? Send us a bug report so we can fix it:");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ImGui::BulletText("Find the game below (crashes are marked in red). A crash already has its report saved.");
  ImGui::BulletText("Click Save Report on that game (saving again updates it), then Show Report to find the file.");
  ImGui::BulletText("Click Report on GitHub, say what happened, and drag the report file into your message.");
  ImGui::Spacing();
  if (AccentButton(app, "Report on GitHub", S(app, 190.0f, 42.0f)) && !SDL_OpenURL(kBugReportIssueUrl)) {
    SetBanner(app, std::string("The web page could not be opened. Go to ") + kBugReportIssueUrl);
  }
  ImGui::SameLine();
  if (LoggedButton("Open Reports Folder", S(app, 190.0f, 42.0f))) {
    const std::string url = FileUrl(app.paths.exe_dir);
    if (!SDL_OpenURL(url.c_str())) SetBanner(app, "The folder could not be opened.");
  }
  ImGui::SameLine();
  if (LoggedButton("Refresh", S(app, 110.0f, 42.0f))) StartLoadBugPage(app);
  ImGui::Spacing();
  ImGui::Spacing();

  ImGui::PushFont(app.fonts.oswald_semibold);
  ImGui::TextUnformatted("RECENT GAMES");
  ImGui::PopFont();
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, S(app, 14.0f, 6.0f));
  if (app.bug_sessions.empty()) {
    DrawSecondaryWrapped(app, app.bug_page_loaded && !app.job.active.load()
                                  ? "No game logs yet. They appear here after you play."
                                  : "Reading…");
  } else if (ImGui::BeginTable("bug-sessions", 4,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                   ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("STARTED", ImGuiTableColumnFlags_WidthStretch, 0.34f);
    ImGui::TableSetupColumn("HOW IT ENDED", ImGuiTableColumnFlags_WidthStretch, 0.46f);
    ImGui::TableSetupColumn("LOG", ImGuiTableColumnFlags_WidthFixed, S(app, 90.0f));
    // Owner 11-09 (laptop): both actions on every row, side by side - Save Report (again replaces it) and Show Report.
    ImGui::TableSetupColumn("REPORT", ImGuiTableColumnFlags_WidthFixed, S(app, 250.0f));
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < app.bug_sessions.size(); ++i) {
      const GameSession& session = app.bug_sessions[i];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, S(app, 32.0f));
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(SessionStartedText(session).c_str());
      ImGui::TableSetColumnIndex(1);
      const bool running = i == 0 && app.game_running_now && session.end == SessionEnd::kNoEnding;
      switch (session.end) {
        case SessionEnd::kCrashed:
          if (session.crash_code.empty()) ImGui::TextColored(kDanger, "Crashed");
          else ImGui::TextColored(kDanger, "Crashed (error %s)", session.crash_code.c_str());
          break;
        case SessionEnd::kClosed: ImGui::TextUnformatted("Closed normally"); break;
        case SessionEnd::kFroze: ImGui::TextColored(kDanger, "Froze, then was closed"); break;
        case SessionEnd::kNoEnding:
          ImGui::TextColored(kMuted, running ? "Playing now" : "Stopped without closing normally");
          break;
      }
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(FriendlyBytes(session.log_bytes).c_str());
      ImGui::TableSetColumnIndex(3);
      const float action_width = S(app, 118.0f);
      ImGui::BeginDisabled(running);  // the game is still writing this log
      const std::string save_id = "Save Report##bug-session-" + std::to_string(i);
      if (LoggedButton(save_id.c_str(), ImVec2(action_width, 0.0f))) StartSessionBugReport(app, session.log);
      ImGui::EndDisabled();
      ImGui::SameLine(0.0f, S(app, 8.0f));
      ImGui::BeginDisabled(session.report.empty());
      const std::string show_id = "Show Report##bug-session-" + std::to_string(i);
      if (LoggedButton(show_id.c_str(), ImVec2(action_width, 0.0f))) ShowReportFile(app, session.report);
      ImGui::EndDisabled();
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  ImGui::Spacing();
  ImGui::PushFont(app.fonts.oswald_semibold);
  ImGui::TextUnformatted("SAVED REPORTS");
  ImGui::PopFont();
  if (app.bug_reports.empty()) {
    DrawSecondaryWrapped(app, "No bug reports saved yet.");
  } else if (ImGui::BeginTable("bug-report-files", 4,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                   ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("FILE", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    ImGui::TableSetupColumn("SAVED", ImGuiTableColumnFlags_WidthStretch, 0.25f);
    ImGui::TableSetupColumn("SIZE", ImGuiTableColumnFlags_WidthFixed, S(app, 90.0f));
    ImGui::TableSetupColumn("##show", ImGuiTableColumnFlags_WidthFixed, S(app, 250.0f));
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < app.bug_reports.size(); ++i) {
      const BugReportFile& report = app.bug_reports[i];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, S(app, 32.0f));
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(PathUtf8(report.file.filename()).c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(FriendlyDate(report.when).c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(FriendlyBytes(report.bytes).c_str());
      ImGui::TableSetColumnIndex(3);
      const std::string id = "Show Report##bug-report-" + std::to_string(i);
      if (LoggedButton(id.c_str(), ImVec2(S(app, 118.0f), 0.0f))) ShowReportFile(app, report.file);
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  ImGui::Spacing();
  DrawSecondaryWrapped(app, "A report holds the game's log for that session, the launcher's logs and settings, and a "
                            "short summary of your PC (graphics card, system, memory). Nothing is sent until you "
                            "attach it yourself.");
  DrawProgress(app);
}

// Shown once per crashed session: by the launcher the crash watcher opens, when a crash note arrives while the
// launcher is open, or at start for a recent crash nobody saw (the launcher was closed when it happened).
void QueueCrashNotice(AppState& app, GameSession session, std::string detail) {
  MarkCrashNoticeSeen(app.paths, session.log);
  PerfLog("crash notice: %s end=%d code=%s", PathUtf8(session.log.filename()).c_str(), static_cast<int>(session.end),
          session.crash_code.c_str());
  app.crash_notice = std::move(session);
  app.crash_notice_detail = std::move(detail);
  app.crash_notice_pending = true;
  SDL_RestoreWindow(app.window);
  SDL_RaiseWindow(app.window);
}

// Background check for a recent unseen crash: saves its report if the watcher did not, then queues the pop-up.
void StartCrashCheck(AppState& app) {
  if (app.job.active.load()) return;
  const Paths paths = app.paths;
  BeginJob(app, "Checking the last game…", [&app, paths](const ProgressFn&, CancelFlag&) {
    std::optional<GameSession> crash = UnseenRecentCrash(paths);
    if (crash && crash->report.empty()) {
      fs::path created;
      if (SaveBugReport(paths, &created, crash->log).ok) crash->report = created;
    }
    return JobOutcome{Result::Ok(), [&app, crash]() mutable {
                        if (crash) QueueCrashNotice(app, std::move(*crash));
                      }};
  });
}

void DrawCrashNotice(AppState& app) {
  constexpr const char* kTitle = "WWE 2K14 PC Recompiled##crash-notice";
  if (app.crash_notice_pending && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
    ImGui::OpenPopup(kTitle);
    app.crash_notice_pending = false;
  }
  if (!app.crash_notice) return;
  const float width = S(app, 560.0f);
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    const GameSession& session = *app.crash_notice;
    const bool crashed = session.end == SessionEnd::kCrashed;
    ImGui::PushFont(app.fonts.oswald_bold, S(app, 28.0f));
    ImGui::TextColored(kDanger, crashed ? "THE GAME CRASHED" : "THE GAME CLOSED UNEXPECTEDLY");
    ImGui::PopFont();
    ImGui::PushFont(app.fonts.barlow_regular);
    ImGui::TextWrapped("WWE 2K14 stopped because of an error (game started %s). Sorry about that.",
                       SessionStartedText(session).c_str());
    ImGui::Spacing();
    if (!session.report.empty()) {
      ImGui::TextWrapped("We saved a bug report for you: %s", PathUtf8(session.report.filename()).c_str());
      ImGui::TextWrapped("Please send it to us so we can fix it: open Bug Reports, click Report on GitHub, and drag "
                         "the report into your message.");
    } else {
      // V152 review (11-09): saving can fail (read-only folder, full disk); say so instead of promising a file.
      ImGui::TextWrapped("The bug report could not be saved automatically (the folder may be read-only or the disk "
                         "full). Open Bug Reports and click Save Report on this game, then Report on GitHub.");
    }
    if (!app.crash_notice_detail.empty()) {
      ImGui::Spacing();
      ImGui::TextWrapped("%s", app.crash_notice_detail.c_str());
    }
    ImGui::PopFont();
    ImGui::Spacing();
    const float open_width = S(app, 200.0f);
    const float close_width = S(app, 100.0f);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - open_width - close_width - ImGui::GetStyle().ItemSpacing.x) *
                         0.5f);
    if (AccentButton(app, "Open Bug Reports", ImVec2(open_width, S(app, 38.0f)))) {
      app.crash_notice.reset();
      app.tab = Tab::BugReports;
      app.bug_page_loaded = false;
      app.banner.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (LoggedButton("Close", ImVec2(close_width, S(app, 38.0f)))) {
      app.crash_notice.reset();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void DrawMain(AppState& app) {
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
  ImGui::Begin("WWE 2K14 PC Recompiled", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
  MainEventTheme::Background(ImGui::GetIO().DisplaySize);
  DrawTopBar(app);
  // Card LOG1: page changes in launcher.log.
  static Tab logged_tab = Tab::Play;
  if (app.tab != logged_tab) {
    logged_tab = app.tab;
    for (const auto& [tab, name] : kTabs) {
      if (tab == app.tab) PerfLog("page: %s", name);
    }
  }
  // B on a controller / Esc on the keyboard goes back to the home screen (not while typing or capturing a key).
  if (app.tab != Tab::Play && app.capture_index < 0 && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
      (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
    app.tab = Tab::Play;
    app.banner.clear();
  }
  ImGui::SetCursorPos(ImVec2(S(app, 48.0f), S(app, kHeaderHeight)));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, app.tab == Tab::Play ? ImVec4(0,0,0,0) : kPanel);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, app.tab == Tab::Play ? ImVec2(0, S(app, 8.0f)) : S(app, 28.0f, 22.0f));
  ImGui::BeginChild("launcher-content",
                    ImVec2(ImGui::GetIO().DisplaySize.x - S(app, 96.0f),
                           ImGui::GetIO().DisplaySize.y - S(app, kHeaderHeight) - S(app, 72.0f)),
                    ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  static const std::array<const char*, 8> page_titles{{
      "PLAY", "SETTINGS", "CONTROLS", "GAME FILES", "SAVES", "CREATION SUITE", "MODS", "BUG REPORTS"}};
  if (app.tab != Tab::Play) {
    ImGui::PushFont(app.fonts.oswald_semibold);
    if (LoggedButton("< BACK", S(app, 110.0f, 38.0f))) {
      app.tab = Tab::Play;
      app.banner.clear();
    }
    ImGui::PopFont();
    ImGui::SameLine(0, S(app, 18.0f));
    ImGui::PushFont(app.fonts.oswald_bold, S(app, 34.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(app, 4.0f));
    ImGui::TextUnformatted(page_titles[static_cast<size_t>(app.tab)]);
    ImGui::PopFont();
    ImGui::Spacing();
    static const std::array<std::pair<Tab, const char*>, 7> subpage_links{{
        {Tab::Settings, "Settings"}, {Tab::Controls, "Controls"}, {Tab::GameFiles, "Game files"},
        {Tab::Saves, "Saves"}, {Tab::CreationSuite, "Creation Suite"}, {Tab::Mods, "Mods (beta)"},
        {Tab::BugReports, "Bug Reports"},
    }};
    // Full-size buttons (owner 11-02: the small text links were too small); the current page is red. Seven buttons
    // share the row's width, so a narrow window (GitHub #115 player: 1131 px) does not cut off the last one.
    const float nav_gap = S(app, 10.0f);
    const float nav_width = std::min(S(app, 140.0f), (ImGui::GetContentRegionAvail().x -
                                                      nav_gap * static_cast<float>(subpage_links.size() - 1)) /
                                                         static_cast<float>(subpage_links.size()));
    for (size_t i = 0; i < subpage_links.size(); ++i) {
      if (i) ImGui::SameLine(0.0f, nav_gap);
      const auto [tab, label] = subpage_links[i];
      const std::string button_id = std::string(label) + "##subpage-nav-" + std::to_string(i);
      const ImVec2 size(nav_width, S(app, 40.0f));
      const bool pressed = app.tab == tab ? AccentButton(app, button_id.c_str(), size) : LoggedButton(button_id.c_str(), size);
      if (pressed) {
        if (tab == Tab::BugReports && app.tab != Tab::BugReports) app.bug_page_loaded = false;
        app.tab = tab;
        app.banner.clear();
      }
    }
  }
  ImGui::Spacing();
  DrawBanner(app);
  DrawUpdateNotes(app);
  const float footer_space = ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - S(app, 10.0f);
  // Pages scroll when their content is taller than the window (e.g. Game files with a banner and found folders on
  // a small or scaled-down window); the scrollbar only appears when needed.
  ImGui::BeginChild("active-page", ImVec2(0, std::max(footer_space, S(app, 100.0f))), false,
                    ImGuiWindowFlags_NoBackground);
  if (app.tab != Tab::Controls) app.capture_index = -1;  // leaving the page cancels a pending key capture
  if (app.tab != Tab::Mods) app.mods_loaded_for = Tab::Play;
  switch (app.tab) {
    case Tab::Play: DrawPlay(app); break;
    case Tab::Settings: DrawSettings(app); break;
    case Tab::Controls: DrawControls(app); break;
    case Tab::GameFiles: DrawGameFiles(app); break;
    case Tab::Saves: DrawSaves(app); break;
    case Tab::CreationSuite: DrawCreationSuite(app); break;
    case Tab::Mods: DrawMods(app); break;
    case Tab::BugReports: DrawBugReports(app); break;
  }
  ImGui::EndChild();
  DrawLaunchWarning(app);
  DrawCrashNotice(app);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  DrawFooter(app);
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
  const float footer_y = display_size.y - S(app, 56.0f);
  ImGui::GetForegroundDrawList()->AddRectFilled(
      ImVec2(0, footer_y), ImVec2(display_size.x, footer_y + S(app, 1.0f)),
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
  static const std::array<const char*, 8> names{{
      "play", "settings", "controls", "game-files", "saves", "creation-suite", "mods", "bug-reports"}};
  static_assert(names.size() == kTabs.size());
  app.tab = tab;
  if (tab == Tab::BugReports) {  // the page normally reads in a job; the capture needs its rows now
    app.bug_sessions = ListRecentGameSessions(app.paths, 10);
    app.bug_reports = ListBugReports(app.paths, 10);
    app.bug_page_loaded = true;
  }
  DrawFrame(app); // Settle ImGui child layout before capture.
  SaveScreenshotFrame(app, std::string(names[static_cast<size_t>(tab)]) + ".png");
}

#if !defined(WWE2K14_LAUNCHER_NO_TEST_TOOLS)
bool CapturePaintEditorState(AppState& app) {
  if (app.paint_screenshot_index >= 2) return false;
  app.tab = Tab::CreationSuite;
  g_suite_tab = kSuiteTabImages;
  if (!app.paint_editor.open) {
    ClosePaintEditor(app);
    auto& editor = app.paint_editor;
    editor.canvas = internal::MakePaintCanvas(256, 256, {0, 0, 0, 0});
    editor.open = editor.canvas.Valid();
    editor.zoom = 1.5f;
    editor.recent_colors[0] = editor.color;
    editor.recent_color_count = 1;
  }

  if (app.paint_screenshot_index == 0) {
    if (app.paint_editor.fullscreen) SetPaintEditorFullscreen(app, false);
    DrawFrame(app); // Let the popup and both columns settle before capture.
    SaveScreenshotFrame(app, "paint-editor-normal.png");
    ++app.paint_screenshot_index;
    return true;
  }

  if (!SetPaintEditorFullscreen(app, true)) return false;
  DrawFrame(app); // Let SDL resize the window and fit the canvas before capture.
  SaveScreenshotFrame(app, "paint-editor-fullscreen.png");
  SetPaintEditorFullscreen(app, false);
  ++app.paint_screenshot_index;
  return true;
}

bool CaptureModEditorScreenshot(AppState& app) {
  app.mod_wrestlers = LoadModWrestlers(app.paths);
  if (app.mod_wrestlers.empty()) return false;
  std::vector<uint32_t> ids;
  for (const ModWrestler& wrestler : app.mod_wrestlers) ids.push_back(wrestler.id);
  app.mod_themes = ReadModThemes(ModMusicPack(app), ids);
  const auto slider_wrestler = std::find_if(app.mod_wrestlers.begin(), app.mod_wrestlers.end(), [&app](const ModWrestler& w) {
    return app.mod_themes.contains(w.id);
  });
  const ModWrestler& selected = slider_wrestler == app.mod_wrestlers.end() ? app.mod_wrestlers.front() : *slider_wrestler;
  if (app.mod_themes.empty()) {
    app.mod_themes[selected.id] = {0, 0x4D4F4453, 0.0f};
    const auto partner = std::find_if(app.mod_wrestlers.begin(), app.mod_wrestlers.end(), [&selected](const ModWrestler& w) {
      return w.id != selected.id;
    });
    if (partner != app.mod_wrestlers.end()) app.mod_themes[partner->id] = {0, 0x4D4F4453, -1.5f};
  }
  app.selected_mod_wrestlers = {selected.id};
  app.edit_mod_wrestler = selected.id;
  app.mod_editor_popup_requested = true;
  app.mods_loaded_for = Tab::Mods;
  app.tab = Tab::Mods;
  DrawFrame(app);
  SDL_RenderPresent(app.renderer);
  SaveScreenshotFrame(app, "mods-edit.png");
  return true;
}
#endif

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
  const float scale = ui::ComputeUiScale(GetWindowDisplayScale(app),
                                         GetWindowPixelDensity(app));
  auto add = [scale](const unsigned char* data, unsigned int size, float pixels) {
    ImFont* font = ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(data, static_cast<int>(size), pixels * scale);
    return font ? font : ImGui::GetIO().Fonts->AddFontDefault();
  };
  app.fonts.oswald_medium = add(OswaldMedium_compressed_data, OswaldMedium_compressed_size, 19.0f);
  app.fonts.oswald_semibold = add(OswaldSemiBold_compressed_data, OswaldSemiBold_compressed_size, 27.0f);
  app.fonts.oswald_bold = add(OswaldBold_compressed_data, OswaldBold_compressed_size, 100.0f);
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

// GitHub #26: when the window cannot be created the launcher used to exit silently (no window, no log).
// Keep the reason, write it to logs/launcher.log and show it in a plain system message box.
std::string g_startup_error;

void StartupFailed(const char* what) {
  g_startup_error = std::string(what) + ": " + SDL_GetError();
  SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", g_startup_error.c_str());
}

void ReportStartupFailure() {
  if (g_startup_error.empty()) return;
  std::error_code error;
  const fs::path logs_dir = ResolvePaths(LauncherExecutablePath()).logs_dir;
  fs::create_directories(logs_dir, error);
#ifdef _WIN32
  FILE* log = _wfopen((logs_dir / "launcher.log").c_str(), L"w");
#else
  FILE* log = std::fopen((logs_dir / "launcher.log").c_str(), "w");
#endif
  if (log) {
    std::fprintf(log, "startup failed: %s\n", g_startup_error.c_str());
    std::fclose(log);
  }
  const std::string text = "The launcher could not start.\n\n" + g_startup_error +
                           "\n\nPlease attach logs\\launcher.log when you report this.";
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "WWE 2K14 PC Recompiled", text.c_str(), nullptr);
}

bool Initialize(AppState& app, int argc, char** argv) {
  if (const char* shift = std::getenv("WWE2K14_TEST_SHIFT_HELD")) {
    const std::string_view value(shift);
    app.test_shift_held = value == "1" || value == "true" || value == "TRUE";
  }
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--settings") app.bypass_auto_start = true;
    if (argument == "--after-crash") {
      app.bypass_auto_start = true;
      app.after_crash = true;
    }
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
#if !defined(WWE2K14_LAUNCHER_NO_TEST_TOOLS)
    if (argument == "--test-display-scale" && i + 1 < argc) {
      const float scale = std::strtof(argv[++i], nullptr);
      if (scale >= 0.5f && scale <= 4.0f && std::isfinite(scale)) {
        app.test_display_scale = scale;
      }
    }
#endif
    if (argument == "--test-drop-tab" && i + 1 < argc) {
      const std::string tab = argv[++i];
      if (tab == "music") app.test_drop_tab = Tab::CreationSuite;
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
#if !defined(WWE2K14_LAUNCHER_NO_TEST_TOOLS)
    if (argument == "--screenshot-paint-editor") {
      app.screenshot_mode = true;
      app.paint_screenshot_mode = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') app.screenshot_dir = Utf8Path(argv[++i]);
      else app.screenshot_dir = fs::path("out/f4b");
    }
#endif
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
    StartupFailed("Could not open the launcher window");
    return false;
  }
  app.window = SDL_CreateWindow("WWE 2K14 PC Recompiled - Native renderer (test) v" WWE2K14_LAUNCHER_VERSION, app.startup_width, app.startup_height,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!app.window) {
    StartupFailed("Could not open the launcher window");
    return false;
  }
  SDL_PumpEvents();
  app.shift_at_start = app.test_shift_held || wwe2k14::launcher::ui::ShiftDownAtLaunch(app.window);
  app.renderer = SDL_CreateRenderer(app.window, nullptr);
  if (!app.renderer) {
    StartupFailed("Could not draw the launcher window");
    return false;
  }
  SDL_SetRenderVSync(app.renderer, 1);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.ConfigNavCaptureKeyboard = true;
  MainEventTheme::ApplyStyle();
  LoadFonts(app);
  LoadTileImages(app);
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
    StartupFailed("Could not initialize the launcher controls");
    return false;
  }
  app.imgui_platform_ready = true;
  if (!ImGui_ImplSDLRenderer3_Init(app.renderer)) {
    StartupFailed("Could not initialize the launcher controls");
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
      if (app.tab == Tab::CreationSuite || app.tab == Tab::GameFiles) QueueDrop(app, event.drop.data);
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
  StopMusicPreview(app.music_editor);
  app.job.cancel.store(true);
  if (app.job.thread.joinable()) app.job.thread.join();
  if (app.update_thread.joinable()) app.update_thread.join();
  if (app.imgui_renderer_ready) ImGui_ImplSDLRenderer3_Shutdown();
  if (app.imgui_platform_ready) ImGui_ImplSDL3_Shutdown();
  if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  DestroyPaintEditorTexture(&app.paint_editor);
  for (SDL_Texture*& texture : app.tile_textures) {
    if (texture) SDL_DestroyTexture(texture);
    texture = nullptr;
  }
  if (app.renderer) SDL_DestroyRenderer(app.renderer);
  if (app.window) SDL_DestroyWindow(app.window);
  SDL_Quit();
}

}  // namespace

int main(int argc, char** argv) {
  // Crash watcher (owner 11-09): "--watch-game <pid> <log>" waits for the game without opening a window.
  if (argc == 4 && std::string_view(argv[1]) == "--watch-game") {
    const uint64_t process_id = std::strtoull(argv[2], nullptr, 10);
    return wwe2k14::launcher::RunCrashWatcher(LauncherExecutablePath(), process_id, Utf8Path(argv[3]));
  }
  AppState app;
  if (!Initialize(app, argc, argv)) {
    Shutdown(app);
    ReportStartupFailure();
    return 1;
  }
  // Vsync paces active frames. After a few frames without input (and with nothing running),
  // block until the next event instead of polling, so the UI reacts immediately but idles cheaply.
  {
    std::error_code error;
    // app.paths is filled by the background initialization job; resolve the logs folder directly.
    const fs::path logs_dir = ResolvePaths(LauncherExecutablePath()).logs_dir;
    fs::create_directories(logs_dir, error);
    // Card LOG1: keep the previous session's log for the bug report.
    fs::rename(logs_dir / "launcher.log", logs_dir / "launcher-previous.log", error);
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
            GetWindowPixelDensity(app), GetWindowDisplayScale(app), app.display_scale,
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
    if (app.initialized && !app.job.active.load() && !app.screenshot_mode) {
      // A crash watcher leaves logs/crash-watch/pending.txt when this launcher was already open; look every 2 s.
      const Uint64 now_ns = SDL_GetTicksNS();
      if (now_ns - app.crash_poll_ns > 2'000'000'000ull) {
        app.crash_poll_ns = now_ns;
        if (TakeCrashPending(app.paths)) app.crash_check_pending = true;
      }
    }
    if (app.initialized && !app.job.active.load() && app.crash_check_pending) {
      app.crash_check_pending = false;
      StartCrashCheck(app);
    } else if (app.initialized && !app.job.active.load()) {
      if (app.refresh_game_files_pending) {
        app.refresh_game_files_pending = false;
        StartRefreshGameFiles(app);
      } else if (app.first_run_scan_pending) {
        app.first_run_scan_pending = false;
        app.import_first_run = true;
        StartScanInstallationImport(app, app.first_run_candidate);
      } else if (app.first_run_search_pending) {
        StartFirstRunInstallationSearch(app);
      }
    }
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
    if (app.screenshot_mode && app.initialized && !app.screenshot_import_queued) {
      const char* search_roots = std::getenv("WWE2K14_LAUNCHER_TEST_SEARCH_ROOTS");
      if (search_roots && *search_roots) {
        app.screenshot_import_queued = true;
        StartFindInstallationImports(app);
      }
      const char* fixture = std::getenv("WWE2K14_TEST_DIALOG_PATH");
      if (!app.screenshot_import_queued && fixture && *fixture) {
        app.screenshot_import_queued = true;
        if (const char* first = std::getenv("WWE2K14_TEST_FIRST_RUN_OFFER"); first && *first == '1') {
          app.import_first_run = true;  // screenshot the Game files offer instead of the confirmation popup
        }
        StartScanInstallationImport(app, Utf8Path(fixture));
      }
    }
    if (app.screenshot_mode && app.initialized && !app.job.active.load()) {
#if !defined(WWE2K14_LAUNCHER_NO_TEST_TOOLS)
      if (app.paint_screenshot_mode) {
        if (!CapturePaintEditorState(app)) {
          app.running = false;
          break;
        }
        continue;
      }
#endif
      if (app.update_screenshot_mode) {
        if (!CaptureUpdateState(app)) {
          app.running = false;
          break;
        }
        continue;
      }
      if (app.screenshot_index >= kTabs.size()) {
#if !defined(WWE2K14_LAUNCHER_NO_TEST_TOOLS)
        const char* edit_screenshot = std::getenv("WWE2K14_TEST_MODS_EDIT_SCREENSHOT");
        if (app.screenshot_index == kTabs.size() && edit_screenshot && *edit_screenshot == '1') {
          CaptureModEditorScreenshot(app);
          ++app.screenshot_index;
          continue;
        }
        // Test-only: the Import WWE '13 arenas picker for the named WWE '13 base (folder or disc image).
        const char* arenas_base = std::getenv("WWE2K14_TEST_WWE13_ARENAS_SCREENSHOT");
        if (arenas_base && *arenas_base && app.wwe13_arena_items.empty()) {
          app.tab = Tab::Mods;
          // As in the real flow: the window opens while the list is still loading, then fills in.
          app.wwe13_arenas_open = true;
          app.wwe13_arenas_loading = true;
          DrawFrame(app);
          SDL_RenderPresent(app.renderer);
          app.wwe13_arenas_loading = false;
          std::string error;
          app.wwe13_arena_items = ListWWE13Arenas(Utf8Path(arenas_base), &error);
          app.wwe13_arena_free = FreeWWE13ArenaNumbers(ActiveGameFolder(app)).size();
          if (app.wwe13_arena_free == 0) app.wwe13_arena_free = 25;  // no game folder in the screenshot run
          for (const auto& item : app.wwe13_arena_items)
            if (item.available && item.source_id % 2) app.wwe13_arena_selected.insert(item.source_id);
          app.wwe13_arena_free = std::max<size_t>(app.wwe13_arena_free, 1);
          DrawFrame(app);
          SDL_RenderPresent(app.renderer);
          DrawFrame(app);
          SaveScreenshotFrame(app, "wwe13-arenas.png");
          continue;
        }
        // Test-only: the Import from WWE 2K15 picker after a scan of the named folder.
        const char* wwe15_folder = std::getenv("WWE2K14_TEST_WWE15_SCREENSHOT");
        if (wwe15_folder && *wwe15_folder && !app.wwe15_import_scan_done) {
          app.tab = Tab::Mods;
          app.wwe15_import_scan = ScanWWE15Import(app.paths, Utf8Path(wwe15_folder));
          for (const auto& w : app.wwe15_import_scan.wrestlers) if (w.available) app.wwe15_selected.insert(w.source_id);
          app.wwe15_import_scan_done = true;
          app.wwe15_import_open = true;
          DrawFrame(app);
          SDL_RenderPresent(app.renderer);
          DrawFrame(app);
          SaveScreenshotFrame(app, "wwe15-import.png");
          continue;
        }
#endif
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
