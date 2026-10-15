#pragma once

#include <filesystem>

namespace wwe2k14 {

// First run only: copy the shader/pipeline storage shipped next to the executable (shader-cache/) into the
// player's cache folder, so the GPU pipelines the game uses are built at boot instead of mid-match.
void SeedShaderCache(const std::filesystem::path& user_data_root);

}  // namespace wwe2k14
