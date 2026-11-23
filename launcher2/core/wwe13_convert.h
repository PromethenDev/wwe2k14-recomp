#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace wwe2k14::launcher {

using WWE13ConvertBytes = std::vector<uint8_t>;
using WWE13RawRowMap = std::map<uint32_t, WWE13ConvertBytes>;

// Small archive primitives shared by the staged WWE '13 importer. Entry names retain their
// original 4/8 bytes, including padding, so rebuilding an existing EPAC can preserve its keys.
struct WWE13PacEntryData {
  std::string name;
  WWE13ConvertBytes data;
};
struct WWE13PacGroupData {
  std::string name;
  bool wide = false;
  uint32_t reserved = 0;
  std::vector<WWE13PacEntryData> entries;
};
struct WWE13PachLeafData {
  uint32_t id = 0;
  WWE13ConvertBytes data;
};
bool ReadWWE13PacGroups(const WWE13ConvertBytes& pac,
                        std::vector<WWE13PacGroupData>* groups, std::string* error);
bool RebuildWWE13PacGroups(const WWE13ConvertBytes& source_pac,
                           const std::vector<WWE13PacGroupData>& groups,
                           WWE13ConvertBytes* output, std::string* error);
bool BuildWWE13PacGroups(const std::vector<WWE13PacGroupData>& groups,
                         WWE13ConvertBytes* output, std::string* error);
bool ReadWWE13Pach(const WWE13ConvertBytes& raw, WWE13RawRowMap* leaves,
                   std::string* error);
bool ReadWWE13PachLeaves(const WWE13ConvertBytes& raw,
                         std::vector<WWE13PachLeafData>* leaves,
                         std::string* error);
bool BuildWWE13Pach(const WWE13RawRowMap& leaves, WWE13ConvertBytes* output,
                    bool pad_to_256, std::string* error);
bool BuildWWE13PachLeaves(const std::vector<WWE13PachLeafData>& leaves,
                          WWE13ConvertBytes* output, bool pad_to_256,
                          std::string* error);
bool DecodeWWE13Bpe(const WWE13ConvertBytes& raw, WWE13ConvertBytes* output,
                    std::string* error);
WWE13ConvertBytes StoreWWE13BpeLiteral(const WWE13ConvertBytes& decoded);

// Stage 2 of the private port-tool recipe. This is deliberately not connected to the Mods
// page or an install/apply path. Optional source IDs select only the documented size-test
// substitutions; normal ports leave them empty.
struct WWE13CharacterPacRecipe {
  uint32_t source_id = 0;
  uint32_t target_id = 0;
  std::optional<uint32_t> leaf_10000_source_id;
  std::optional<uint32_t> stock_model_source_id;
  std::optional<uint32_t> stock_body_source_id;
  bool add_shared_leaf50 = false;
  bool waive_missing_20002 = false;
  // Decoded stock WWE 2K14 cloth leaves (Havok hk_2012.2: leaf id -> packfile), the layout the WWE '13 (hk_2011.3) cloth
  // leaves 9000-9099 are converted to (owner 11-16, Ministry Undertaker's cape). Null or a missing id: the leaf is kept as is.
  const std::map<uint32_t, std::vector<uint8_t>>* cloth_templates = nullptr;
};

// Each source file is read-only. Optional PACs are required only when the matching recipe
// field is set. shared_leaf50 is the raw, verified 303-byte 2K14 leaf 50.
bool ConvertWWE13CharacterPac(const WWE13CharacterPacRecipe& recipe,
                              const WWE13ConvertBytes& source_wwe13_pac,
                              const WWE13ConvertBytes* leaf_10000_source_wwe13_pac,
                              const WWE13ConvertBytes* stock_model_pac,
                              const WWE13ConvertBytes* stock_body_pac,
                              const WWE13ConvertBytes* shared_leaf50,
                              WWE13ConvertBytes* output, std::string* error);

// Extract one raw leaf from a named EMD entry; used by the small parity probe and synthetic
// host test. The PAC bytes are never modified.
bool ReadWWE13CharacterLeaf(const WWE13ConvertBytes& pac, const std::string& emd_name,
                            uint32_t leaf_id, WWE13ConvertBytes* output, std::string* error);
bool ReadWWE13PacLeaf(const WWE13ConvertBytes& pac, const std::string& group,
                      const std::string& entry, uint32_t leaf_id,
                      WWE13ConvertBytes* output, std::string* error);
bool ReadWWE13AttireColorRecords(const WWE13ConvertBytes& pac,
                                 std::map<uint32_t, WWE13ConvertBytes>* records,
                                 std::string* error);

// Stage 4: clone each selected donor's sorted 664-byte attire-color record and replace its ID.
struct WWE13AttireColorPort {
  uint32_t target_id = 0;
  uint32_t donor_id = 0;
};
bool ConvertWWE13AttireColors(const WWE13ConvertBytes& source_pac,
                              const std::vector<WWE13AttireColorPort>& ports,
                              WWE13ConvertBytes* output, std::string* error);

// Stage 5: insert the four per-wrestler name/handle keys into each SDB table that carries
// the stock WWE 2K14 sentinel key. Keys map directly to their UTF-8 string bytes.
using WWE13StringKeyMap = std::map<uint32_t, WWE13ConvertBytes>;
bool ConvertWWE13StringKeys(const WWE13ConvertBytes& source_pac,
                            const WWE13StringKeyMap& keyed_strings,
                            WWE13ConvertBytes* output, size_t* inserted_keys,
                            std::string* error);
// The same insertion, into every SDB table that carries `sentinel` (arena names: 40002).
bool ConvertWWE13StringKeysAt(const WWE13ConvertBytes& source_pac,
                              const WWE13StringKeyMap& keyed_strings, uint32_t sentinel,
                              WWE13ConvertBytes* output, size_t* inserted_keys,
                              std::string* error);
// Plain EPAC/EPK8 bytes of a PAC (XCompress unwrapped when present).
bool DecodeWWE13PacPlain(const WWE13ConvertBytes& pac, WWE13ConvertBytes* plain, std::string* error);
// 'ZLIB' leaves (16-byte header + zlib stream). Encoding keeps the original header's second word.
bool DecodeWWE13ZlibLeaf(const WWE13ConvertBytes& leaf, WWE13ConvertBytes* plain, std::string* error);
bool EncodeWWE13ZlibLeaf(const WWE13ConvertBytes& original_leaf, const WWE13ConvertBytes& plain,
                         WWE13ConvertBytes* leaf, std::string* error);
bool ReadWWE13StringKeyValues(const WWE13ConvertBytes& pac, WWE13StringKeyMap* values,
                              std::string* error);

// Stage 3: donor-based DAT/DSBD rows and the learned WWE '13 to 2K14 PRO row conversion.
struct WWE13RowsPort {
  uint32_t source_id = 0;
  uint32_t target_id = 0;
  uint32_t donor_id = 0;
  std::string attrs = "13";   // "13" or a 2K14 donor row ID
  std::string index = "free"; // "free", a fixed numeric index, or "donor:<id>"
  bool own_pro = true;
};
struct WWE13RowsInput {
  WWE13RawRowMap training13_pro; // base-game shared IDs used to learn field mappings
  WWE13RawRowMap source13_dat;   // base plus each source wrestler's own DLC overlay row
  WWE13RawRowMap source13_pro;
  WWE13RawRowMap game14_dat;
  WWE13RawRowMap game14_dsbd;
  WWE13RawRowMap game14_pro;
  std::set<uint16_t> occupied_dat_indices; // base and every installed 2K14 overlay
};
struct WWE13RowsOptions {
  bool own_attire_codes = true; // PORT_OWN_CODES; false preserves the selected 2K14 donor's codes
};
struct WWE13ConvertedRows {
  uint32_t target_id = 0;
  uint16_t dat_index = 0;
  WWE13ConvertBytes dat;
  WWE13ConvertBytes dsbd;
  WWE13ConvertBytes pro;
};
bool ConvertWWE13Rows(const std::vector<WWE13RowsPort>& ports, const WWE13RowsInput& input,
                      const WWE13RowsOptions& options,
                      std::vector<WWE13ConvertedRows>* output, std::string* error);

// Decode a CHAR table from an EPAC/EPK8 chEtc PAC, including each row's BPE wrapper.
bool ReadWWE13PacRows(const WWE13ConvertBytes& pac, const std::string& group,
                      const std::string& table, WWE13RawRowMap* rows, std::string* error);

}  // namespace wwe2k14::launcher
