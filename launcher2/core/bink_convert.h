#pragma once

#include "launcher_core.h"

namespace wwe2k14::launcher {

struct WWE13TitantronResult {
  bool used_stand_in = false;
  std::string message;  // explains when the WWE 2K14 stand-in replaced a failed conversion
};

// Converts one read-only WWE '13 source movie. If conversion fails, stages the supplied
// per-wrestler WWE 2K14 stand-in and returns success with used_stand_in/message set. Cancellation
// stops the child and returns a clear failure without silently staging a stand-in.
Result ConvertWWE13TitantronOrStandIn(const fs::path& converter_executable,
                                      const fs::path& source_movie,
                                      const fs::path& stand_in_movie,
                                      const fs::path& output_movie,
                                      const std::string& wrestler_name,
                                      const ProgressFn& progress,
                                      const CancelFlag& cancel,
                                      WWE13TitantronResult* result);

}  // namespace wwe2k14::launcher
