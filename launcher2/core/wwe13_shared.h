#pragma once

// A few WWE '13 import internals shared with the WWE 2K15 import (wwe15_pipeline.cpp). Thin
// wrappers only; the WWE '13 code paths are unchanged.

#include "wwe13_convert.h"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wwe2k14::launcher {

// chEtc rows replaced in CHAR/DAT, DSBD and PRO (build_test.patch_chetc).
bool WWE13SharedPatchChEtc(const WWE13ConvertBytes& source, const std::vector<WWE13ConvertedRows>& rows,
                           WWE13ConvertBytes* output, std::string* error);
// A packed EVP's own key (u16 at +32) changed; only the first BPE block is re-encoded (evp_retarget.py).
bool WWE13SharedRetargetEvp(const WWE13ConvertBytes& raw, uint32_t new_key, uint32_t old_key,
                            WWE13ConvertBytes* output, std::string* error);
// DLC registry ENEX/LSD rows of one leaf, and the alphabetical rank rule (porttool auto_rank).
bool WWE13SharedRegistryRows(const WWE13ConvertBytes& pac, const std::string& file_name, uint32_t leaf_id,
                             std::vector<std::array<uint16_t, 4>>* rows, std::string* error);
uint16_t WWE13SharedAutoRank(const std::vector<std::array<uint16_t, 4>>& rows,
                             const std::map<uint32_t, std::string>& names, const std::string& wrestler_name,
                             const std::set<uint16_t>& taken);

}  // namespace wwe2k14::launcher
