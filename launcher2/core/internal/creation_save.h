#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wwe2k14::launcher::internal {

// Reads a WWE 2K14 STFS CON creation package (.cas/.car) into its SaveData.Dat
// payload and the matching Xenia-style sidecar header required by the importer.
bool ExtractCreationConPackage(const std::filesystem::path& package,
                              bool superstar,
                              std::vector<uint8_t>* save_data,
                              std::vector<uint8_t>* header,
                              std::string* error);

// The supported ranges stop before the adjacent mapped region (119 CAW records and 50 arena records); these are
// conservative byte-map bounds, not claims about the game's total capacity.
bool IsSupportedCreationPlain(const std::vector<uint8_t>& plain);
// Names the first failing check of IsSupportedCreationPlain ("" when it passes), for the launcher log.
// Tolerant gate used by every read/write path: only the plain size is mandatory (unrecognized records are kept as
// occupied, unexpected side-table values are preserved). IsSupportedCreationPlain stays the strict full-recognition check.
bool IsTolerableCreationPlain(const std::vector<uint8_t>& plain);
// "" when everything is recognized, else "save has N unrecognized CAW records / M arena records / K side-table values;
// they are kept as they are" (for the launcher log).
std::string DescribeCreationTolerance(const std::vector<uint8_t>& plain);
std::string DescribeUnsupportedCreationPlain(const std::vector<uint8_t>& plain);
std::optional<std::vector<uint8_t>> AddKnownCreation(const std::vector<uint8_t>& main_file,
                                                     const std::vector<uint8_t>& creation_file,
                                                     bool superstar, uint32_t slot);
std::optional<std::vector<uint8_t>> RepairCawBookkeeping(const std::vector<uint8_t>& main_file,
                                                          uint32_t* repaired);
// Rewrites an arena record that an earlier launcher built from this payload (settings bytes left zero) with the
// full game-identical record. Returns the input unchanged when there is nothing to upgrade, nullopt on bad input.
std::optional<std::vector<uint8_t>> UpgradeOlderArenaRecord(const std::vector<uint8_t>& main_file,
                                                            const std::vector<uint8_t>& creation_file,
                                                            uint32_t slot, bool* upgraded);
struct CreationName {
  uint32_t slot = 0;
  std::string name;
};
// Display names of the non-empty records (CAW record +0xD1 ASCII; arena record +0x39 ASCII).
std::optional<std::vector<CreationName>> ListKnownCreationNames(const std::vector<uint8_t>& main_file,
                                                                bool superstar);
bool IsValidCreationDisplayName(std::string_view name, bool superstar) noexcept;
struct CreationRenameFiles {
  std::vector<uint8_t> main_file;
  std::vector<uint8_t> payload_file;
  std::vector<uint8_t> main_plain;
};
// Rewrites the known ASCII display-name fields and reseals the mapped creation/main checksums. Input/output files
// remain untouched; callers commit the returned pair only after a verified backup.
std::optional<CreationRenameFiles> RewriteKnownCreationName(const std::vector<uint8_t>& main_file,
                                                            const std::vector<uint8_t>& payload_file,
                                                            bool superstar, uint32_t slot,
                                                            std::string_view new_name);
std::optional<std::vector<uint8_t>> RemoveKnownCreation(const std::vector<uint8_t>& main_file,
                                                        bool superstar, uint32_t slot);
std::optional<uint32_t> FindKnownFreeCreationSlot(const std::vector<uint8_t>& main_file,
                                                  bool superstar);
std::optional<std::vector<uint32_t>> ListKnownCreationSlots(const std::vector<uint8_t>& main_file,
                                                            bool superstar);
// Confirms the mapped main-save record is valid and exactly reconstructs from this slot's creation payload.
// The main plain checksum and the creation ZLIB container are checked before comparing record bytes.
bool CreationFileMatchesMainRecord(const std::vector<uint8_t>& main_file,
                                   const std::vector<uint8_t>& creation_file,
                                   bool superstar, uint32_t slot);
// Applies the paired-creation side-table rules measured across save-before -> after -> after2.
// The three-byte field at main-plain 0x19..0x1B is deliberately preserved because its update
// function is not established by those samples.
bool ApplyGeneralCreationPairRules(std::vector<uint8_t>* main_file,
                                   std::vector<uint8_t>* main_header,
                                   uint32_t slot,
                                   const std::vector<uint8_t>& caw_file,
                                   const std::vector<uint8_t>& arena_file);
// Recreates one exact, byte-mapped owner sample transition. Disabled unless the
// WWE2K14_CREATION_SAMPLE_REPRODUCTION build option is explicitly enabled.
bool ApplyObservedGameMadePair(std::vector<uint8_t>* main_file,
                               std::vector<uint8_t>* main_header,
                               const std::vector<uint8_t>& caw_file,
                               const std::vector<uint8_t>& caw_header,
                               const std::vector<uint8_t>& arena_file,
                               const std::vector<uint8_t>& arena_header);
bool CompressWithSystemZlibBest(const std::vector<uint8_t>& plain, std::vector<uint8_t>* compressed);

}  // namespace wwe2k14::launcher::internal
