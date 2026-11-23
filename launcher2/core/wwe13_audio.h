#pragma once

#include "launcher_core.h"
#include "wwe13_pipeline.h"

#include <set>
#include <utility>

namespace wwe2k14::launcher {

struct WWE13ImportAudioResult {
  std::filesystem::path staging_directory;
  std::vector<std::string> files;
  uint32_t songs = 0;
  uint32_t announcer_names = 0;
  std::vector<uint32_t> songs_without_audio;
  std::vector<uint32_t> names_without_audio;
  std::vector<std::pair<uint32_t, float>> theme_levels;  // target id, gain that puts the song at WWE 2K14's level
};

// Builds entrance music and announcer-name files under <fresh_staging_directory>/files.
// The WWE '13, WWE 2K14, and userdata trees are read-only. No game files are installed.
bool RunWWE13ImportAudio(const WWE13ImportPaths& sources,
                         const std::vector<WWE13ImportPort>& chosen_wrestlers,
                         const std::filesystem::path& fresh_staging_directory,
                         const ProgressFn& progress, const CancelFlag& cancel,
                         WWE13ImportAudioResult* result, std::string* error);

// One WWE 2K15 wrestler whose own entrance theme is wanted in WWE 2K14.
struct WWE15MusicRequest {
  uint32_t source_id = 0;  // WWE 2K15 character ID
  uint32_t target_id = 0;  // WWE 2K14 slot (100..499)
  uint32_t selector = 0;   // his 2K15 PRO music selector (row bytes 576..577, e.g. 10410 = Play_MUS_0104_1_0); 0 = none
};

struct WWE15ImportMusicResult {
  std::vector<std::string> files;                         // written under <files_root>, relative ("game/sound/DLC_01.pck")
  std::set<uint32_t> targets_with_song;                   // target IDs that now have their own 2K15 theme
  std::vector<std::pair<uint32_t, float>> theme_levels;   // target ID, gain that puts the song at WWE 2K14's level
  std::vector<std::string> notes;                         // plain-language notes (who keeps a borrowed theme and why)
};

// Adds each wrestler's own WWE 2K15 entrance theme (the event his 2K15 music selector names, else Play_MUS_<source ID>;
// events/containers in <wwe15_root>/sound/Game_Sound_E_S.pck, songs in Game_Sound_M.pck) to the WWE 2K14 entrance music
// pack as Play_MUS_<target>. Builds on <wwe14_root>/sound/DLC_01.pck when an earlier import made one (the game reads that
// file first), otherwise on the add-on's own sound/DLC_01.pck. Writes <files_root>/game/sound/DLC_01.pck only when at least
// one song was added. Input trees are read-only.
bool RunWWE15ImportMusic(const std::filesystem::path& wwe15_root, const std::filesystem::path& wwe14_root,
                         const std::filesystem::path& userdata_root, const std::vector<WWE15MusicRequest>& requests,
                         const std::filesystem::path& files_root, const CancelFlag& cancel,
                         WWE15ImportMusicResult* result, std::string* error);

// Ring-announcer name lines for WWE 2K15 imports (owner 11-16, "call names"). The 2K15 RASS key of each wrestler (pac/sound.pac)
// goes into the 2K14 RASS table for his slot. When WWE 2K14 already speaks that key in every name event (46 of the 74, e.g.
// Chris Jericho), the table row is all. Otherwise his 2K15 name clips are added to the 2K14 'jr' name events (2K15 has no
// second announcer, 'tc' stays silent for him) with 2K15 media from Game_Sound_M.pck. Builds on the given current files.
struct WWE15NameRequest {
  uint32_t source_id = 0;
  uint32_t target_id = 0;
};
struct WWE15AnnouncerResult {
  std::vector<uint8_t> es_pack;    // new Game_Sound_E_S.pck, empty when no clip was added
  std::vector<uint8_t> sound_pac;  // new pac/sound.pac, empty when no table row was added
  std::set<uint32_t> named;        // target IDs that got a name (row, and clips where needed)
  std::vector<std::string> notes;
};
bool BuildWWE15AnnouncerNames(const std::filesystem::path& wwe15_root, const std::filesystem::path& current_es_pack,
                              const std::vector<uint8_t>& current_sound_pac, const std::vector<WWE15NameRequest>& requests,
                              WWE15AnnouncerResult* result, std::string* error);

}  // namespace wwe2k14::launcher
