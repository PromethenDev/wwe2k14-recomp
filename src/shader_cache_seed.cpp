#include "shader_cache_seed.h"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <cstring>
#include <string>
#include <system_error>

#include <rex/filesystem.h>
#include <rex/logging.h>

namespace wwe2k14 {
namespace {

// Same names the runtime opens (vulkan pipeline_cache.cpp: <title>.xsh, <title>.fbo.vk.xpso).
constexpr const char* kSeedFiles[] = {"545408B2.xsh", "545408B2.fbo.vk.xpso"};

}  // namespace

void SeedShaderCache(const std::filesystem::path& user_data_root) {
  const char* env = std::getenv("WWE2K14_SHADER_CACHE_SEED");
  if (env != nullptr && std::strcmp(env, "0") == 0) {
    REXLOG_INFO("[wwe2k14] shader cache seed: off (WWE2K14_SHADER_CACHE_SEED=0)");
    return;
  }
  if (user_data_root.empty()) {
    return;
  }
  std::error_code ec;
  const std::filesystem::path seed_dir = rex::filesystem::GetExecutableFolder() / "shader-cache";
  const std::filesystem::path cache_dir = user_data_root / "cache" / "shaders" / "shareable";
  for (const char* name : kSeedFiles) {
    if (!std::filesystem::is_regular_file(seed_dir / name, ec)) {
      return;  // No (complete) seed shipped with this build.
    }
  }
  // Never touch a player's own storage: seed only when neither file exists yet.
  for (const char* name : kSeedFiles) {
    if (std::filesystem::exists(cache_dir / name, ec) || ec) {
      return;
    }
  }
  std::filesystem::create_directories(cache_dir, ec);
  if (ec) {
    REXLOG_WARN("[wwe2k14] shader cache seed: cannot create the cache folder: {}", ec.message());
    return;
  }
  // On any failure remove what this run installed, so the next start tries again from a clean state.
  auto undo = [&](size_t installed) {
    for (size_t i = 0; i < installed; ++i) {
      std::filesystem::remove(cache_dir / kSeedFiles[i], ec);
    }
  };
  for (size_t i = 0; i < std::size(kSeedFiles); ++i) {
    const char* name = kSeedFiles[i];
    // Copy to a temporary name and rename, so an interrupted copy never leaves a half file under the real name
    // (the runtime would keep only its valid prefix anyway; this just keeps the cache folder tidy).
    const std::filesystem::path target = cache_dir / name;
    std::filesystem::path partial = target;
    partial += ".seed-tmp-" + std::to_string(static_cast<unsigned long long>(
                                   std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove(partial, ec);
    if (!std::filesystem::copy_file(seed_dir / name, partial, ec) || ec) {
      REXLOG_WARN("[wwe2k14] shader cache seed: copy of {} failed: {}", name, ec.message());
      std::filesystem::remove(partial, ec);
      undo(i);
      return;
    }
    if (std::filesystem::exists(target, ec)) {  // Another instance seeded meanwhile: keep its file.
      std::filesystem::remove(partial, ec);
      continue;
    }
    std::filesystem::rename(partial, target, ec);
    if (ec) {
      REXLOG_WARN("[wwe2k14] shader cache seed: rename of {} failed: {}", name, ec.message());
      std::filesystem::remove(partial, ec);
      undo(i);
      return;
    }
  }
  REXLOG_INFO("[wwe2k14] shader cache seed: installed the shipped shader cache (first run)");
}

}  // namespace wwe2k14
