// Card C3 (Xbox native build, 2026-10-24): settings for the native renderer on the Xbox UWP build.
//
// The native renderer and the title glue read ~140 WWE2K14_* environment variables (std::getenv, all inside functions,
// none at namespace scope). On PC the launcher sets them; the Xbox has no launcher and no way to set a process
// environment. This file fills the process environment (CRT copy, _putenv_s) from
//   1. <package dir>/native-env.txt  - the packaged defaults, one NAME=VALUE per line ('#' comments), generated from
//                                      the PC launcher's plan for the chosen preset (same variables as launch.cpp);
//   2. wwe2k14_scene_size (LocalState\wwe2k14.toml) - 480p / 720p / 1080p / 1440p / 4k -> WWE2K14_D3D_SCENE_SCALE
//                                      0.6 / 1.0 / 1.5 / 2.0 / 3.0 (the launcher's SceneScale table);
//   3. wwe2k14_env (LocalState\wwe2k14.toml) - "NAME=VALUE;NAME=VALUE", applied last (overrides).
// Values may use %PKG% (package folder) and %LOCAL% (LocalState); "--cvar=value" lines set SDK cvars (no argv on UWP).
// Called first thing in Wwe2k14App::OnPreSetup (after the SDK loaded the LocalState config, before any native
// renderer code reads the environment). Built only for REX_UWP (the UWP toolchain defines
// _CRT_USE_WINAPI_FAMILY_DESKTOP_APP, so getenv/_putenv_s are declared; the static CRT keeps one environment copy).
// Not compiled by the PC build (add to WWE2K14_SOURCES under if(REX_UWP)).

#if defined(REX_UWP)

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(wwe2k14_scene_size, "", "WWE 2K14",
                      "Native renderer 3D picture size: 480p, 720p, 1080p, 1440p or 4k (empty = packaged default)");
REXCVAR_DEFINE_STRING(wwe2k14_env, "", "WWE 2K14",
                      "Extra native renderer settings, NAME=VALUE;NAME=VALUE (applied after the packaged defaults)");

#ifdef WWE2K14_HAS_NR
namespace nr {
void RegisterFrontendFromEnvironment();  // src/native/nr/nr_frontend.cpp (PC registers before main instead)
}  // namespace nr
#endif

namespace wwe2k14::uwp_env {
namespace {

std::string Trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return std::string(s.substr(b, e - b));
}

std::string g_package_dir, g_local_dir;  // forward-slash UTF-8, for %PKG% / %LOCAL% in values

// %PKG% = the read-only package folder (shaders, crowd cards), %LOCAL% = LocalState (writable: logs, nr output).
std::string Expand(std::string v) {
  for (const auto& [token, dir] : {std::pair<std::string_view, const std::string*>{"%PKG%", &g_package_dir},
                                   std::pair<std::string_view, const std::string*>{"%LOCAL%", &g_local_dir}}) {
    for (size_t p; (p = v.find(token)) != std::string::npos;) v.replace(p, token.size(), *dir);
  }
  return v;
}

// Xbox 2026-11-09: the PC launcher plan also sets SDK switches (REX_*, e.g. REX_WWE2K14_ZERO_PHYSICAL_ON_ALLOC) and the
// WWE13_* names the runtime still reads; those pass too. "--name=value" lines are the plan's command-line cvars.
bool SetOne(std::string_view assignment, const char* source) {
  const std::string item = Trim(assignment);
  if (item.rfind("--", 0) == 0) {
    const size_t eq = item.find('=');
    if (eq == std::string::npos || eq <= 2) return false;
    const std::string name = item.substr(2, eq - 2), value = Expand(item.substr(eq + 1));
    rex::cvar::SetFlagByName(name, value);
    REXLOG_INFO("[uwp-env] cvar {}={} (from {})", name, value, source);
    return true;
  }
  const size_t eq = item.find('=');
  if (eq == std::string::npos) return false;
  const std::string name = Trim(std::string_view(item).substr(0, eq));
  const std::string value = Expand(Trim(std::string_view(item).substr(eq + 1)));
  if (name.empty() || !(name.rfind("WWE2K14_", 0) == 0 || name.rfind("REX_", 0) == 0 || name.rfind("WWE13_", 0) == 0)) {
    REXLOG_WARN("[uwp-env] ignored '{}' from {} (not a WWE2K14_/REX_/WWE13_ variable)", name, source);
    return false;
  }
  if (_putenv_s(name.c_str(), value.c_str()) != 0) {
    REXLOG_WARN("[uwp-env] _putenv_s failed for {}", name);
    return false;
  }
  return true;
}

std::filesystem::path PackageDir() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return n ? std::filesystem::path(std::wstring(path, n)).parent_path() : std::filesystem::path();
}

const char* SceneScale(std::string_view size) {
  if (size == "480p") return "0.6";
  if (size == "720p") return "1.0";
  if (size == "1080p") return "1.5";
  if (size == "1440p") return "2.0";
  if (size == "4k" || size == "4K" || size == "2160p") return "3.0";
  return nullptr;
}

}  // namespace

void Apply() {
  int from_file = 0, from_toml = 0;
  auto utf8 = [](const std::filesystem::path& p) {
    std::string out = rex::path_to_utf8(p);
    for (char& c : out) if (c == '\\') c = '/';
    return out;
  };
  g_package_dir = utf8(PackageDir());
  g_local_dir = utf8(rex::filesystem::GetUserFolder());
  // Xbox has no console: the native renderer reports its init/fault lines on stderr (fprintf), so keep them in
  // LocalState\nr-stderr.txt (overwritten each start; unbuffered so a crash keeps the last lines).
  if (FILE* f = std::freopen((g_local_dir + "/nr-stderr.txt").c_str(), "w", stderr)) {
    std::setvbuf(f, nullptr, _IONBF, 0);
    REXLOG_INFO("[uwp-env] native renderer messages -> {}/nr-stderr.txt", g_local_dir);
  }
  const std::filesystem::path defaults = PackageDir() / "native-env.txt";
  if (std::ifstream f(defaults); f) {
    std::string line;
    while (std::getline(f, line)) {
      const std::string t = Trim(line);
      if (!t.empty() && t[0] != '#' && SetOne(t, "native-env.txt")) ++from_file;
    }
  } else {
    REXLOG_WARN("[uwp-env] no packaged native-env.txt; native renderer runs with code defaults");
  }
  const std::string size = rex::cvar::GetFlagByName("wwe2k14_scene_size");
  if (!size.empty()) {
    if (const char* scale = SceneScale(size)) {
      _putenv_s("WWE2K14_D3D_SCENE_SCALE", scale);
      // As the PC launcher: the 480p preset also keeps a native-size set (card S3-39).
      if (size == "480p") _putenv_s("WWE2K14_D3D_SCALE_DYNAMIC", "1");
      ++from_toml;
    } else {
      REXLOG_WARN("[uwp-env] wwe2k14_scene_size '{}' unknown (480p/720p/1080p/1440p/4k)", size);
    }
  }
  const std::string extra = rex::cvar::GetFlagByName("wwe2k14_env");
  size_t pos = 0;
  while (pos <= extra.size() && !extra.empty()) {
    const size_t end = extra.find(';', pos);
    const std::string item = Trim(std::string_view(extra).substr(pos, end == std::string::npos ? end : end - pos));
    if (!item.empty() && SetOne(item, "wwe2k14_env")) ++from_toml;
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  const char* scale = std::getenv("WWE2K14_D3D_SCENE_SCALE");
#ifdef WWE2K14_HAS_NR
  // The native renderer's draw observer reads WWE2K14_NR; on PC it registers before main (launcher environment).
  ::nr::RegisterFrontendFromEnvironment();
#endif
  REXLOG_INFO("[uwp-env] {} packaged + {} LocalState settings applied; WWE2K14_NR={} scene scale {}", from_file,
              from_toml, std::getenv("WWE2K14_NR") ? std::getenv("WWE2K14_NR") : "(unset)", scale ? scale : "1.0");
}

}  // namespace wwe2k14::uwp_env

#endif  // REX_UWP
