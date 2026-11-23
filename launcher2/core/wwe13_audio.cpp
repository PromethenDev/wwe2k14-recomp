#include "wwe13_audio.h"

#include "internal/util.h"
#include "path_case.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "internal/miniz_api.h"

namespace wwe2k14::launcher {
namespace {

using Bytes = WWE13ConvertBytes;
namespace fs = std::filesystem;

constexpr uint32_t kMusicBank = 0x3017f079;
constexpr uint32_t kMusicTemplateBank = 0xe85dda21;
constexpr uint32_t kMusicMixer = 0x078ce9f2;
constexpr uint32_t kMusicSongBank13 = 0xf4757ccf;
constexpr uint32_t kMusicPrefetchBank13 = 0x9a4847cb;
constexpr uint32_t kAnnouncerArgumentGroup = 0x367cc164;
constexpr uint32_t kAnnouncerActor = 0x1ab691a1;
constexpr uint32_t kAnnouncerPlugin = 0x00040001;
constexpr uint32_t kAkpkBlockSize = 2048;
constexpr uint32_t kFreshAnnouncerKeyPrefixA = 0x8d7b6600;
constexpr uint32_t kFreshAnnouncerKeyPrefixB = 0x8c7b6400;

#include "wwe13_audio_levels.inc"
#include "wwe15_audio_levels.inc"

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}

uint16_t Be16(const Bytes& bytes, size_t at) {
  return static_cast<uint16_t>((uint16_t(bytes[at]) << 8) | bytes[at + 1]);
}
uint32_t Be32(const Bytes& bytes, size_t at) {
  return (uint32_t(bytes[at]) << 24) | (uint32_t(bytes[at + 1]) << 16) |
         (uint32_t(bytes[at + 2]) << 8) | bytes[at + 3];
}
uint32_t Le32(const Bytes& bytes, size_t at) {
  return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) |
         (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24);
}
void PutBe16(Bytes* bytes, size_t at, uint16_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value >> 8);
  (*bytes)[at + 1] = static_cast<uint8_t>(value);
}
void PutBe32(Bytes* bytes, size_t at, uint32_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value >> 24);
  (*bytes)[at + 1] = static_cast<uint8_t>(value >> 16);
  (*bytes)[at + 2] = static_cast<uint8_t>(value >> 8);
  (*bytes)[at + 3] = static_cast<uint8_t>(value);
}
void PutLe32(Bytes* bytes, size_t at, uint32_t value) {
  (*bytes)[at] = static_cast<uint8_t>(value);
  (*bytes)[at + 1] = static_cast<uint8_t>(value >> 8);
  (*bytes)[at + 2] = static_cast<uint8_t>(value >> 16);
  (*bytes)[at + 3] = static_cast<uint8_t>(value >> 24);
}

bool ReadFile(const fs::path& path, Bytes* bytes, std::string* error) {
  std::error_code ec;
  const uintmax_t size = fs::file_size(path, ec);
  if (ec || size > std::numeric_limits<size_t>::max())
    return Fail(error, "An audio source file could not be read: " + internal::PathToUtf8(path));
  std::ifstream input(path, std::ios::binary);
  if (!input) return Fail(error, "An audio source file could not be opened: " + internal::PathToUtf8(path));
  bytes->resize(static_cast<size_t>(size));
  if (size) input.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(size));
  if (!input && size) return Fail(error, "An audio source file could not be read: " + internal::PathToUtf8(path));
  return true;
}

bool Cancelled(const CancelFlag& cancel, std::string* error);

bool WriteStagedFile(const fs::path& path, const Bytes& bytes, const CancelFlag& cancel,
                     const ProgressFn& progress, uint64_t already, uint64_t total,
                     const std::string& label, std::string* error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return Fail(error, "The audio staging folder could not be created: " + ec.message());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return Fail(error, "The audio staging file could not be created: " + internal::PathToUtf8(path));
  constexpr size_t chunk = 1024 * 1024;
  size_t offset = 0;
  while (offset < bytes.size()) {
    if (Cancelled(cancel, error)) return false;
    const size_t count = std::min(chunk, bytes.size() - offset);
    output.write(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::streamsize>(count));
    if (!output) return Fail(error, "The audio staging file could not be written: " + internal::PathToUtf8(path));
    offset += count;
    if (progress) progress(already + offset, total, label);
  }
  output.flush();
  if (!output) return Fail(error, "The audio staging file could not be written: " + internal::PathToUtf8(path));
  return true;
}

bool Cancelled(const CancelFlag& cancel, std::string* error) {
  return cancel.load(std::memory_order_relaxed) && Fail(error, "The import was cancelled.");
}

bool SafeToStage(const fs::path& staging, const WWE13ImportPaths& paths,
                 std::string* error) {
  std::error_code ec;
  const fs::path target = fs::absolute(staging, ec).lexically_normal();
  if (ec) return Fail(error, "The audio staging folder path could not be resolved: " + ec.message());
  std::vector<fs::path> inputs{paths.wwe13_root, paths.wwe14_root, paths.userdata_root};
  inputs.insert(inputs.end(), paths.wwe13_pack_roots.begin(), paths.wwe13_pack_roots.end());
  for (const fs::path& root : inputs) {
    if (root.empty()) continue;
    const fs::path input = fs::absolute(root, ec).lexically_normal();
    if (ec) return Fail(error, "An audio input folder path could not be resolved: " + ec.message());
    auto a = input.begin();
    auto b = target.begin();
    for (; a != input.end() && b != target.end() && *a == *b; ++a, ++b) {}
    if (a == input.end()) return Fail(error, "The audio staging folder may not be inside a source tree.");
  }
  return true;
}

uint32_t Fnv1(std::string_view text) {
  uint32_t hash = 2166136261u;
  for (unsigned char byte : text) {
    if (byte >= 'A' && byte <= 'Z') byte = static_cast<unsigned char>(byte + ('a' - 'A'));
    hash = hash * 16777619u;
    hash ^= byte;
  }
  return hash;
}

uint32_t EventId(const char* format_prefix, uint32_t id) {
  char text[48]{};
  std::snprintf(text, sizeof(text), "%s%04u_0_0", format_prefix, id);
  return Fnv1(text);
}

struct AkRow {
  uint32_t id = 0;
  uint32_t block_size = 0;
  uint32_t byte_size = 0;
  uint32_t start_block = 0;
  uint32_t language = 0;
};

struct Akpk {
  fs::path path;
  Bytes data;
  uint32_t strings_size = 0;
  uint32_t banks_size = 0;
  uint32_t sounds_size = 0;
  uint32_t ext_size = 0;
  size_t banks_at = 0;
  size_t sounds_at = 0;
  size_t ext_at = 0;
  Bytes strings;
  Bytes ext;
  std::vector<AkRow> banks;
  std::vector<AkRow> sounds;
};

bool ReadAkRows(const Bytes& data, size_t at, uint32_t bytes, std::vector<AkRow>* rows,
                std::string* error, const char* what) {
  if (bytes < 4 || at > data.size() || bytes > data.size() - at)
    return Fail(error, std::string("The ") + what + " table is truncated.");
  const uint32_t count = Be32(data, at);
  if (count > (bytes - 4) / 20 || bytes != 4ull + 20ull * count)
    return Fail(error, std::string("The ") + what + " table has invalid rows.");
  std::vector<AkRow> result;
  result.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t row = at + 4 + size_t(i) * 20;
    result.push_back({Be32(data, row), Be32(data, row + 4), Be32(data, row + 8),
                      Be32(data, row + 12), Be32(data, row + 16)});
  }
  *rows = std::move(result);
  return true;
}

bool ParseAkpk(fs::path path, Bytes data, Akpk* output, std::string* error) {
  if (!output || data.size() < 28 || std::memcmp(data.data(), "AKPK", 4) != 0 ||
      Be32(data, 8) != 1)
    return Fail(error, "An audio pack is not a supported AKPK version 1 file: " + internal::PathToUtf8(path));
  Akpk p;
  p.path = std::move(path);
  p.data = std::move(data);
  p.strings_size = Be32(p.data, 12);
  p.banks_size = Be32(p.data, 16);
  p.sounds_size = Be32(p.data, 20);
  p.ext_size = Be32(p.data, 24);
  const uint64_t strings_end = 28ull + p.strings_size;
  const uint64_t banks_end = strings_end + p.banks_size;
  const uint64_t sounds_end = banks_end + p.sounds_size;
  const uint64_t ext_end = sounds_end + p.ext_size;
  if (ext_end > p.data.size() || Be32(p.data, 4) != 20ull + p.strings_size + p.banks_size +
                                                    p.sounds_size + p.ext_size)
    return Fail(error, "An audio pack has invalid AKPK table lengths: " + internal::PathToUtf8(p.path));
  p.strings.assign(p.data.begin() + 28, p.data.begin() + static_cast<std::ptrdiff_t>(strings_end));
  p.banks_at = static_cast<size_t>(strings_end);
  p.sounds_at = static_cast<size_t>(banks_end);
  p.ext_at = static_cast<size_t>(sounds_end);
  p.ext.assign(p.data.begin() + static_cast<std::ptrdiff_t>(p.ext_at),
               p.data.begin() + static_cast<std::ptrdiff_t>(ext_end));
  if (!ReadAkRows(p.data, p.banks_at, p.banks_size, &p.banks, error, "bank") ||
      !ReadAkRows(p.data, p.sounds_at, p.sounds_size, &p.sounds, error, "sound")) return false;
  for (const AkRow& row : p.banks) {
    if (!row.block_size || uint64_t(row.start_block) * row.block_size > p.data.size() ||
        row.byte_size > p.data.size() - uint64_t(row.start_block) * row.block_size)
      return Fail(error, "An audio bank points outside its AKPK file: " + internal::PathToUtf8(p.path));
  }
  for (const AkRow& row : p.sounds) {
    if (!row.block_size || uint64_t(row.start_block) * row.block_size > p.data.size() ||
        row.byte_size > p.data.size() - uint64_t(row.start_block) * row.block_size)
      return Fail(error, "An audio clip points outside its AKPK file: " + internal::PathToUtf8(p.path));
  }
  *output = std::move(p);
  return true;
}

std::optional<AkRow> FindAkRow(const std::vector<AkRow>& rows, uint32_t id) {
  const auto found = std::find_if(rows.begin(), rows.end(), [id](const AkRow& row) { return row.id == id; });
  return found == rows.end() ? std::nullopt : std::optional<AkRow>(*found);
}

bool ReadAkpk(const fs::path& path, Akpk* output, std::string* error) {
  Bytes data;
  return ReadFile(path, &data, error) && ParseAkpk(path, std::move(data), output, error);
}

bool BankBytes(const Akpk& pck, uint32_t id, Bytes* bank, std::string* error) {
  const auto row = FindAkRow(pck.banks, id);
  if (!row) return Fail(error, "The required audio bank is missing from " + internal::PathToUtf8(pck.path) + ".");
  const size_t at = static_cast<size_t>(uint64_t(row->start_block) * row->block_size);
  bank->assign(pck.data.begin() + static_cast<std::ptrdiff_t>(at),
               pck.data.begin() + static_cast<std::ptrdiff_t>(at + row->byte_size));
  return true;
}

bool MediaBytes(const Akpk& pck, uint32_t id, Bytes* media, std::string* error) {
  const auto row = FindAkRow(pck.sounds, id);
  if (!row) return Fail(error, "The selected WWE '13 audio clip is not in " + internal::PathToUtf8(pck.path) + ".");
  const size_t at = static_cast<size_t>(uint64_t(row->start_block) * row->block_size);
  media->assign(pck.data.begin() + static_cast<std::ptrdiff_t>(at),
                pck.data.begin() + static_cast<std::ptrdiff_t>(at + row->byte_size));
  return true;
}

struct BankSection {
  std::string tag;
  size_t payload = 0;
  uint32_t size = 0;
};
struct HircObject {
  uint8_t type = 0;
  uint32_t id = 0;
  size_t at = 0;
  uint32_t length = 0;
  size_t body = 0;
  size_t body_size = 0;
};

bool BankSections(const Bytes& bank, std::vector<BankSection>* sections, std::string* error) {
  std::vector<BankSection> result;
  size_t at = 0;
  while (at + 8 <= bank.size()) {
    const uint32_t size = Be32(bank, at + 4);
    if (size > bank.size() - at - 8)
      return Fail(error, "A Wwise bank has a section that extends past the bank.");
    result.push_back({std::string(reinterpret_cast<const char*>(bank.data() + at), 4), at + 8, size});
    at += 8 + size;
  }
  if (at != bank.size()) return Fail(error, "A Wwise bank has trailing bytes after its sections.");
  *sections = std::move(result);
  return true;
}

bool HircObjects(const Bytes& bank, const BankSection& section,
                 std::vector<HircObject>* objects, std::string* error) {
  if (section.size < 4) return Fail(error, "A Wwise HIRC section is too short.");
  const uint32_t count = Be32(bank, section.payload);
  size_t at = section.payload + 4;
  const size_t end = section.payload + section.size;
  std::vector<HircObject> result;
  result.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (at + 9 > end) return Fail(error, "A Wwise HIRC object is truncated.");
    const uint32_t length = Be32(bank, at + 1);
    if (length < 4 || length > end - at - 5)
      return Fail(error, "A Wwise HIRC object has an invalid size.");
    result.push_back({bank[at], Be32(bank, at + 5), at, length, at + 9, length - 4});
    at += 5 + length;
  }
  if (at != end) return Fail(error, "The Wwise HIRC count does not reach the section end.");
  *objects = std::move(result);
  return true;
}

bool ReadHirc(const Bytes& bank, std::vector<BankSection>* sections,
              std::vector<HircObject>* objects, std::string* error) {
  if (!BankSections(bank, sections, error)) return false;
  const auto hirc = std::find_if(sections->begin(), sections->end(),
                                [](const BankSection& section) { return section.tag == "HIRC"; });
  if (hirc == sections->end()) return Fail(error, "A Wwise bank is missing its HIRC section.");
  return HircObjects(bank, *hirc, objects, error);
}

Bytes Body(const Bytes& bank, const HircObject& object) {
  return Bytes(bank.begin() + static_cast<std::ptrdiff_t>(object.body),
               bank.begin() + static_cast<std::ptrdiff_t>(object.body + object.body_size));
}

const HircObject* FindObject(const std::vector<HircObject>& objects, uint32_t id) {
  const auto found = std::find_if(objects.begin(), objects.end(), [id](const HircObject& o) { return o.id == id; });
  return found == objects.end() ? nullptr : &*found;
}

bool ReplaceWord(Bytes* bytes, uint32_t old_value, uint32_t new_value, std::string* error) {
  bool changed = false;
  for (size_t at = 0; at + 4 <= bytes->size(); ++at) {
    if (Be32(*bytes, at) != old_value) continue;
    PutBe32(bytes, at, new_value);
    changed = true;
    at += 3;
  }
  if (!changed) return Fail(error, "A Wwise template does not contain the expected object ID.");
  return true;
}

Bytes MakeObject(uint8_t type, uint32_t id, const Bytes& body) {
  Bytes result(9 + body.size());
  result[0] = type;
  PutBe32(&result, 1, static_cast<uint32_t>(4 + body.size()));
  PutBe32(&result, 5, id);
  std::copy(body.begin(), body.end(), result.begin() + 9);
  return result;
}

Bytes MakeSection(const std::string& tag, const Bytes& payload) {
  Bytes result(8 + payload.size());
  std::copy(tag.begin(), tag.end(), result.begin());
  PutBe32(&result, 4, static_cast<uint32_t>(payload.size()));
  std::copy(payload.begin(), payload.end(), result.begin() + 8);
  return result;
}

struct MusicSource {
  Akpk pck;
  uint32_t event_bank = 0;
  uint32_t container_bank = 0;
  std::vector<uint32_t> prefetch_banks;
  // WWE 2K15 keeps events/containers in Game_Sound_E_S.pck and the songs (and their prefetch bank) in Game_Sound_M.pck.
  bool separate_media = false;
  Akpk media_pck;
};

const Akpk& MediaPck(const MusicSource& source) { return source.separate_media ? source.media_pck : source.pck; }

bool HasDidx(const Akpk& pck, uint32_t bank_id) {
  Bytes bank;
  std::string ignored;
  std::vector<BankSection> sections;
  if (!BankBytes(pck, bank_id, &bank, &ignored) || !BankSections(bank, &sections, &ignored)) return false;
  return std::any_of(sections.begin(), sections.end(), [](const BankSection& section) { return section.tag == "DIDX"; });
}

bool LoadMusicSources(const WWE13ImportPaths& paths, std::vector<MusicSource>* sources,
                      std::string* error) {
  std::vector<MusicSource> result;
  const fs::path base = paths.wwe13_root / "sound/Music.pck";
  MusicSource main;
  if (!ReadAkpk(base, &main.pck, error)) return false;
  main.event_bank = kMusicSongBank13;
  main.container_bank = kMusicPrefetchBank13;  // Music.pck splits them; a pack keeps both in one bank
  main.prefetch_banks = {kMusicPrefetchBank13};
  result.push_back(std::move(main));

  for (const auto& [number, bank_id] : {std::pair<uint32_t, uint32_t>{2, 0x3017f07a}, {3, 0x3017f07b}}) {
    fs::path found;
    for (const fs::path& root : paths.wwe13_pack_roots) {
      const fs::path candidate = root / "sound" / ("DLC_0" + std::to_string(number) + ".pck");
      std::error_code ec;
      if (fs::is_regular_file(candidate, ec) && !ec) { found = candidate; break; }
    }
    if (found.empty()) continue;
    MusicSource addon;
    if (!ReadAkpk(found, &addon.pck, error)) return false;
    if (!FindAkRow(addon.pck.banks, bank_id)) continue;
    addon.event_bank = bank_id;
    addon.container_bank = bank_id;
    for (const AkRow& row : addon.pck.banks)
      if (HasDidx(addon.pck, row.id)) addon.prefetch_banks.push_back(row.id);
    result.push_back(std::move(addon));
  }
  *sources = std::move(result);
  return true;
}

struct MusicHit {
  uint32_t event_id = 0;
  uint32_t action_id = 0;
  uint32_t container_id = 0;
  uint32_t sound_id = 0;
  uint32_t media_id = 0;
  Bytes clip;
  Bytes prefetch;
  float gain = 0.0f;  // Wwise volume (dB) that puts the song at WWE 2K14's level
};

bool FindPrefetch(const MusicSource& source, uint32_t media_id, Bytes* prefetch) {
  for (uint32_t bank_id : source.prefetch_banks) {
    Bytes bank;
    std::string ignored;
    std::vector<BankSection> sections;
    if (!BankBytes(MediaPck(source), bank_id, &bank, &ignored) || !BankSections(bank, &sections, &ignored)) continue;
    const auto didx = std::find_if(sections.begin(), sections.end(),
                                   [](const BankSection& section) { return section.tag == "DIDX"; });
    if (didx == sections.end() || didx->size % 12) continue;
    for (size_t at = didx->payload; at + 12 <= didx->payload + didx->size; at += 12) {
      if (Be32(bank, at) != media_id) continue;
      const uint32_t offset = Be32(bank, at + 4), size = Be32(bank, at + 8);
      const auto data = std::find_if(sections.begin(), sections.end(),
                                     [](const BankSection& section) { return section.tag == "DATA"; });
      if (data == sections.end() || offset > data->size || size > data->size - offset) continue;
      prefetch->assign(bank.begin() + static_cast<std::ptrdiff_t>(data->payload + offset),
                       bank.begin() + static_cast<std::ptrdiff_t>(data->payload + offset + size));
      return true;
    }
  }
  return false;
}

bool LookupMusicEvent(const MusicSource& source, uint32_t event_hash, uint32_t requested_event, MusicHit* hit) {
  Bytes event_bank, container_bank;
  std::string ignored;
  if (!BankBytes(source.pck, source.event_bank, &event_bank, &ignored) ||
      !BankBytes(source.pck, source.container_bank, &container_bank, &ignored)) return false;
  std::vector<BankSection> es, cs;
  std::vector<HircObject> events, containers;
  if (!ReadHirc(event_bank, &es, &events, &ignored) || !ReadHirc(container_bank, &cs, &containers, &ignored)) return false;
  const HircObject* event = FindObject(events, event_hash);
  if (!event || event->type != 4 || event->body_size < 4) return false;
  const Bytes event_body = Body(event_bank, *event);
  const uint32_t count = Be32(event_body, 0);
  if (count > (event_body.size() - 4) / 4) return false;
  std::vector<const HircObject*> plays;
  for (uint32_t i = 0; i < count; ++i) {
    const HircObject* action = FindObject(events, Be32(event_body, 4 + i * 4));
    if (!action || action->type != 3 || action->body_size < 6) continue;
    const Bytes action_body = Body(event_bank, *action);
    if (action_body[0] == 0x04 && action_body[1] == 0x03) plays.push_back(action);
  }
  if (plays.empty()) return false;
  uint32_t container_id = 0;
  for (const HircObject* play : plays) {
    const Bytes action_body = Body(event_bank, *play);
    const uint32_t current = Be32(action_body, 2);
    if (container_id && container_id != current) return false;
    container_id = current;
  }
  const HircObject* container = FindObject(containers, container_id);
  if (!container || container->type != 5) return false;
  const Bytes container_body = Body(container_bank, *container);
  std::vector<const HircObject*> sounds;
  for (const HircObject& object : containers) {
    if (object.type != 2 || object.id == 0) continue;
    const std::array<uint8_t, 4> needle{uint8_t(object.id >> 24), uint8_t(object.id >> 16),
                                        uint8_t(object.id >> 8), uint8_t(object.id)};
    if (std::search(container_body.begin(), container_body.end(), needle.begin(), needle.end()) != container_body.end())
      sounds.push_back(&object);
  }
  if (sounds.size() != 1 || sounds.front()->body_size < 12) return false;
  const Bytes sound_body = Body(container_bank, *sounds.front());
  if (Be32(sound_body, 0) != kAnnouncerPlugin) return false;
  const uint32_t media_id = Be32(sound_body, 8);
  Bytes clip, prefetch;
  if (!MediaBytes(MediaPck(source), media_id, &clip, &ignored) || !FindPrefetch(source, media_id, &prefetch) ||
      clip.size() < 4 || std::memcmp(clip.data(), "RIFX", 4) != 0 || prefetch.size() > clip.size() ||
      !std::equal(prefetch.begin(), prefetch.end(), clip.begin())) return false;
  *hit = {requested_event, plays.front()->id, container_id, sounds.front()->id, media_id,
          std::move(clip), std::move(prefetch)};
  return true;
}

bool LookupMusic(const MusicSource& source, uint32_t requested_event, MusicHit* hit) {
  return LookupMusicEvent(source, EventId("Play_MUS_", requested_event), requested_event, hit);
}

bool SourceProSlot288(const WWE13ImportPaths& paths, uint32_t source_id, uint16_t* slot,
                      std::string* error) {
  std::vector<fs::path> packs = paths.wwe13_pack_roots;
  std::sort(packs.begin(), packs.end());
  fs::path source_root = paths.wwe13_root;
  std::string character_name;
  if (source_id < 100) {
    for (const fs::path& pack : packs) {
      const fs::path directory = pack / "pac/ch";
      std::error_code ec;
      if (!fs::is_directory(directory, ec) || ec) continue;
      const std::string prefix = "ch" + std::to_string(source_id) + "_dlc";
      for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
           !ec && it != end; it.increment(ec)) {
        const std::string name = internal::PathToUtf8(it->path().filename());
        if (name.rfind(prefix, 0) == 0 && it->is_regular_file(ec)) {
          source_root = pack;
          character_name = name;
          break;
        }
      }
      if (!character_name.empty()) break;
    }
  }
  if (character_name.empty()) character_name = "ch" + std::to_string(source_id) + ".pac";
  fs::path etc = source_root / "pac/ch/chEtc.pac";
  if (character_name.find("_dlc") != std::string::npos) {
    const size_t at = character_name.find("_dlc");
    if (at + 6 <= character_name.size()) etc = source_root / "pac/ch" / ("chEtc" + character_name.substr(at, 6) + ".pac");
  }
  WWE13RawRowMap rows;
  Bytes etc_bytes;
  if (!ReadFile(etc, &etc_bytes, error) ||
      !ReadWWE13PacRows(etc_bytes, "CHAR", "PRO", &rows, error)) return false;
  const auto found = rows.find(source_id);
  if (found == rows.end() || found->second.size() < (288 + 1) * 2)
    return Fail(error, "The source wrestler has no music slot in its WWE '13 PRO row.");
  *slot = static_cast<uint16_t>(found->second[576] | (uint16_t(found->second[577]) << 8));
  return true;
}

bool FindMusicForPort(const WWE13ImportPaths& paths, const std::vector<MusicSource>& sources,
                      const WWE13ImportPort& port, MusicHit* hit, std::string* error) {
  std::vector<uint32_t> requested{port.source_id};
  uint16_t slot = 0;
  if (!SourceProSlot288(paths, port.source_id, &slot, error)) return false;
  if (slot != port.source_id) requested.push_back(slot);
  for (uint32_t event_id : requested)
    for (const MusicSource& source : sources)
      if (LookupMusic(source, event_id, hit)) return true;
  return Fail(error, "No supported WWE '13 entrance theme was found for source " + std::to_string(port.source_id) + ".");
}

std::optional<float> MusicGain(uint32_t source_id) {
  const auto found = std::lower_bound(kMeasuredMusicGains.begin(), kMeasuredMusicGains.end(), source_id,
                                      [](const auto& row, uint32_t id) { return row.first < id; });
  if (found == kMeasuredMusicGains.end() || found->first != source_id) return std::nullopt;
  return found->second;
}

struct MusicTemplate {
  uint32_t action_id = 0;
  uint32_t container_id = 0;
  uint32_t sound_id = 0;
  Bytes action_body;
  Bytes container_body;
  Bytes sound_body;
};

bool ReadMusicTemplate(const WWE13ImportPaths& paths, MusicTemplate* result,
                       std::string* error) {
  Akpk pck;
  if (!ReadAkpk(paths.wwe14_root / "sound/Game_Sound_E_S.pck", &pck, error)) return false;
  Bytes bank;
  if (!BankBytes(pck, kMusicTemplateBank, &bank, error)) return false;
  std::vector<BankSection> sections;
  std::vector<HircObject> objects;
  if (!ReadHirc(bank, &sections, &objects, error)) return false;
  const HircObject* event = FindObject(objects, EventId("Play_MUS_", 318));
  if (!event || event->type != 4 || event->body_size != 8)
    return Fail(error, "The WWE 2K14 music template event is not the expected one-action event.");
  const Bytes event_body = Body(bank, *event);
  const HircObject* action = FindObject(objects, Be32(event_body, 4));
  if (!action || action->type != 3 || action->body_size < 6)
    return Fail(error, "The WWE 2K14 music template action is missing.");
  const Bytes action_body = Body(bank, *action);
  if (action_body[0] != 0x04 || action_body[1] != 0x03)
    return Fail(error, "The WWE 2K14 music template action has an unsupported layout.");
  const uint32_t container_id = Be32(action_body, 2);
  const HircObject* container = FindObject(objects, container_id);
  if (!container || container->type != 5)
    return Fail(error, "The WWE 2K14 music template container is missing.");
  const Bytes container_body = Body(bank, *container);
  const HircObject* sound = nullptr;
  for (const HircObject& object : objects) {
    if (object.type != 2) continue;
    const std::array<uint8_t, 4> needle{uint8_t(object.id >> 24), uint8_t(object.id >> 16),
                                        uint8_t(object.id >> 8), uint8_t(object.id)};
    if (std::search(container_body.begin(), container_body.end(), needle.begin(), needle.end()) != container_body.end()) {
      if (sound) return Fail(error, "The WWE 2K14 music template container has multiple Sounds.");
      sound = &object;
    }
  }
  if (!sound || sound->body_size < 44)
    return Fail(error, "The WWE 2K14 music template Sound is missing or too short.");
  *result = {action->id, container_id, sound->id, action_body, container_body, Body(bank, *sound)};
  return true;
}

bool SoundPropsAt(const Bytes& body, size_t* props) {
  if (body.size() < 37) return false;
  size_t at = 26;
  const uint8_t fx_count = body[at];
  if (fx_count > 0) at += 2 + 7 * fx_count;
  else at += 1;
  at += 4 + 4 + 2;
  if (at >= body.size()) return false;
  *props = at;
  return true;
}

bool AddMusicVolume(Bytes* body, float db, std::string* error) {
  size_t props = 0;
  if (!SoundPropsAt(*body, &props) || props + 6 > body->size() || (*body)[props] != 1 ||
      (*body)[props + 1] != 6 || Be32(*body, props + 2) != 0)
    return Fail(error, "A WWE 2K14 music Sound template has unexpected volume properties.");
  Bytes result(body->begin(), body->begin() + static_cast<std::ptrdiff_t>(props));
  result.push_back(2);
  result.push_back(0);
  result.push_back(6);
  uint32_t bits = 0;
  std::memcpy(&bits, &db, sizeof(bits));
  const size_t at = result.size();
  result.resize(at + 4);
  PutBe32(&result, at, bits);
  result.insert(result.end(), body->begin() + static_cast<std::ptrdiff_t>(props + 2), body->end());
  *body = std::move(result);
  return true;
}

struct MusicBuild {
  Bytes pack;
  uint32_t songs = 0;
  std::vector<std::pair<uint32_t, MusicHit>> hits;
};

bool BuildMusicBank(const Bytes& old_bank, const MusicTemplate& templ,
                    const std::vector<std::pair<uint32_t, MusicHit>>& hits,
                    Bytes* new_bank, std::string* error) {
  std::vector<BankSection> sections;
  std::vector<HircObject> objects;
  if (!ReadHirc(old_bank, &sections, &objects, error)) return false;
  const HircObject* mixer = FindObject(objects, kMusicMixer);
  if (!mixer || mixer->type != 7)
    return Fail(error, "The 2K14 DLC music bank is missing its stock music mixer.");
  Bytes mixer_body = Body(old_bank, *mixer);
  size_t child_at = 0;
  bool found_children = false;
  for (size_t at = 0; at < std::min<size_t>(400, mixer_body.size() >= 4 ? mixer_body.size() - 4 : 0); ++at) {
    const uint32_t count = Be32(mixer_body, at);
    if (!count || at + 4ull + 4ull * count != mixer_body.size()) continue;
    std::vector<uint32_t> ids(count);
    for (uint32_t i = 0; i < count; ++i) ids[i] = Be32(mixer_body, at + 4 + i * 4);
    // Stock lists are sorted; an earlier import appends its containers at the end (the game takes that), so require
    // only distinct IDs here (a WWE 2K15 import builds on the WWE '13 import's pack).
    std::vector<uint32_t> sorted_ids = ids;
    std::sort(sorted_ids.begin(), sorted_ids.end());
    if (std::adjacent_find(sorted_ids.begin(), sorted_ids.end()) != sorted_ids.end()) continue;
    child_at = at;
    found_children = true;
    break;
  }
  if (!found_children) return Fail(error, "The 2K14 DLC music mixer child list is not recognized.");

  std::set<uint32_t> containers, actions;
  std::vector<uint32_t> ordered_containers;
  std::vector<Bytes> front;
  std::vector<Bytes> back;
  std::set<uint32_t> new_ids;
  for (const auto& [target_id, hit] : hits) {
    if (containers.insert(hit.container_id).second) {
      if (FindObject(objects, hit.container_id) || FindObject(objects, hit.sound_id) ||
          new_ids.count(hit.container_id) || new_ids.count(hit.sound_id))
        return Fail(error, "A WWE '13 music object ID is already in use by the 2K14 DLC bank.");
      ordered_containers.push_back(hit.container_id);
      Bytes sound_body = templ.sound_body;
      const uint32_t old_source = Be32(sound_body, 8);
      if (!ReplaceWord(&sound_body, old_source, hit.media_id, error) ||
          !ReplaceWord(&sound_body, templ.container_id, hit.container_id, error)) return false;
      if (sound_body.size() < 24) return Fail(error, "A music Sound template is too short.");
      PutBe32(&sound_body, 4, 2);
      PutBe32(&sound_body, 20, static_cast<uint32_t>(hit.prefetch.size()));
      if (!AddMusicVolume(&sound_body, hit.gain, error)) return false;
      Bytes container_body = templ.container_body;
      if (!ReplaceWord(&container_body, templ.sound_id, hit.sound_id, error)) return false;
      front.push_back(MakeObject(2, hit.sound_id, sound_body));
      front.push_back(MakeObject(5, hit.container_id, container_body));
      new_ids.insert(hit.sound_id);
      new_ids.insert(hit.container_id);
    }
    if (actions.insert(hit.action_id).second) {
      if (FindObject(objects, hit.action_id) || new_ids.count(hit.action_id))
        return Fail(error, "A WWE '13 music action ID is already in use by the 2K14 DLC bank.");
      Bytes action_body = templ.action_body;
      if (!ReplaceWord(&action_body, templ.container_id, hit.container_id, error) ||
          !ReplaceWord(&action_body, kMusicTemplateBank, kMusicBank, error)) return false;
      back.push_back(MakeObject(3, hit.action_id, action_body));
      new_ids.insert(hit.action_id);
    }
    const uint32_t event_id = EventId("Play_MUS_", target_id);
    if (FindObject(objects, event_id) || !new_ids.insert(event_id).second)
      return Fail(error, "The selected 2K14 music event ID is already in use.");
    Bytes event_body(8);
    PutBe32(&event_body, 0, 1);
    PutBe32(&event_body, 4, hit.action_id);
    back.push_back(MakeObject(4, event_id, event_body));
  }

  Bytes updated_mixer = mixer_body;
  std::vector<uint32_t> children;
  for (size_t at = child_at + 4; at < mixer_body.size(); at += 4) children.push_back(Be32(mixer_body, at));
  children.insert(children.end(), ordered_containers.begin(), ordered_containers.end());
  updated_mixer.resize(child_at);
  const size_t count_at = updated_mixer.size();
  updated_mixer.resize(count_at + 4 + children.size() * 4);
  PutBe32(&updated_mixer, count_at, static_cast<uint32_t>(children.size()));
  for (size_t i = 0; i < children.size(); ++i) PutBe32(&updated_mixer, count_at + 4 + i * 4, children[i]);

  Bytes hirc;
  const auto hirc_section = std::find_if(sections.begin(), sections.end(),
                                         [](const BankSection& section) { return section.tag == "HIRC"; });
  const uint32_t old_count = Be32(old_bank, hirc_section->payload);
  const uint32_t new_count = old_count + static_cast<uint32_t>(new_ids.size());
  hirc.resize(4);
  PutBe32(&hirc, 0, new_count);
  bool mixer_done = false;
  for (const HircObject& object : objects) {
    if (object.id == mixer->id) {
      if (mixer_done) return Fail(error, "The 2K14 DLC music bank has duplicate mixer objects.");
      for (const Bytes& item : front) hirc.insert(hirc.end(), item.begin(), item.end());
      const Bytes encoded = MakeObject(object.type, object.id, updated_mixer);
      hirc.insert(hirc.end(), encoded.begin(), encoded.end());
      mixer_done = true;
    } else {
      const size_t end = object.at + 5 + object.length;
      hirc.insert(hirc.end(), old_bank.begin() + static_cast<std::ptrdiff_t>(object.at),
                  old_bank.begin() + static_cast<std::ptrdiff_t>(end));
    }
  }
  if (!mixer_done) return Fail(error, "The 2K14 DLC music mixer object could not be rebuilt.");
  for (const Bytes& item : back) hirc.insert(hirc.end(), item.begin(), item.end());

  Bytes rebuilt;
  bool wrote_bkhd = false, wrote_hirc = false, wrote_stid = false;
  for (const BankSection& section : sections) {
    if (section.tag == "BKHD") {
      const Bytes encoded = MakeSection("BKHD", Bytes(old_bank.begin() + static_cast<std::ptrdiff_t>(section.payload),
                                                       old_bank.begin() + static_cast<std::ptrdiff_t>(section.payload + section.size)));
      rebuilt.insert(rebuilt.end(), encoded.begin(), encoded.end());
      wrote_bkhd = true;
    } else if (section.tag == "HIRC") {
      const Bytes encoded = MakeSection("HIRC", hirc);
      rebuilt.insert(rebuilt.end(), encoded.begin(), encoded.end());
      wrote_hirc = true;
    } else if (section.tag == "STID") {
      const Bytes encoded = MakeSection("STID", Bytes(old_bank.begin() + static_cast<std::ptrdiff_t>(section.payload),
                                                         old_bank.begin() + static_cast<std::ptrdiff_t>(section.payload + section.size)));
      rebuilt.insert(rebuilt.end(), encoded.begin(), encoded.end());
      wrote_stid = true;
    }
  }
  if (!wrote_bkhd || !wrote_hirc || !wrote_stid)
    return Fail(error, "The 2K14 DLC music bank is missing BKHD or STID.");
  *new_bank = std::move(rebuilt);
  return true;
}

struct AkBlob {
  bool bank = false;
  size_t row = 0;
  Bytes bytes;
  uint64_t original_order = 0;
};

void WriteAkRow(Bytes* output, size_t at, const AkRow& row) {
  PutBe32(output, at, row.id);
  PutBe32(output, at + 4, row.block_size);
  PutBe32(output, at + 8, row.byte_size);
  PutBe32(output, at + 12, row.start_block);
  PutBe32(output, at + 16, row.language);
}

bool RebuildMusicPck(const Akpk& target, const Bytes& new_bank,
                     const std::vector<std::pair<uint32_t, Bytes>>& new_media,
                     Bytes* output, std::string* error) {
  std::vector<AkRow> banks = target.banks;
  std::vector<AkRow> sounds = target.sounds;
  const auto bank_row = std::find_if(banks.begin(), banks.end(), [](const AkRow& row) { return row.id == kMusicBank; });
  if (bank_row == banks.end()) return Fail(error, "The selected WWE 2K14 DLC pack is missing its entrance music bank.");
  for (const auto& [id, media] : new_media) {
    if (FindAkRow(sounds, id)) return Fail(error, "A WWE '13 music media ID is already present in the 2K14 DLC pack.");
    sounds.push_back({id, kAkpkBlockSize, static_cast<uint32_t>(media.size()), 0, 0});
  }
  std::sort(sounds.begin(), sounds.end(), [](const AkRow& a, const AkRow& b) { return a.id < b.id; });

  std::vector<AkBlob> blobs;
  blobs.reserve(banks.size() + sounds.size());
  for (size_t i = 0; i < banks.size(); ++i) {
    const AkRow& row = banks[i];
    const size_t at = static_cast<size_t>(uint64_t(row.start_block) * row.block_size);
    Bytes bytes;
    if (row.id == kMusicBank) bytes = new_bank;
    else bytes.assign(target.data.begin() + static_cast<std::ptrdiff_t>(at),
                      target.data.begin() + static_cast<std::ptrdiff_t>(at + row.byte_size));
    blobs.push_back({true, i, std::move(bytes), row.start_block});
  }
  for (size_t i = 0; i < target.sounds.size(); ++i) {
    const AkRow& row = target.sounds[i];
    const size_t at = static_cast<size_t>(uint64_t(row.start_block) * row.block_size);
    Bytes bytes(target.data.begin() + static_cast<std::ptrdiff_t>(at),
                target.data.begin() + static_cast<std::ptrdiff_t>(at + row.byte_size));
    const auto sorted = std::lower_bound(sounds.begin(), sounds.end(), row.id,
                                        [](const AkRow& a, uint32_t id) { return a.id < id; });
    blobs.push_back({false, static_cast<size_t>(sorted - sounds.begin()), std::move(bytes), row.start_block});
  }
  for (size_t i = 0; i < new_media.size(); ++i) {
    const uint32_t id = new_media[i].first;
    const auto sorted = std::lower_bound(sounds.begin(), sounds.end(), id,
                                        [](const AkRow& a, uint32_t value) { return a.id < value; });
    blobs.push_back({false, static_cast<size_t>(sorted - sounds.begin()), new_media[i].second,
                     std::numeric_limits<uint64_t>::max() - new_media.size() + i});
  }
  std::stable_sort(blobs.begin(), blobs.end(), [](const AkBlob& a, const AkBlob& b) {
    return a.original_order < b.original_order;
  });
  const uint64_t bank_table_bytes = 4 + 20ull * banks.size();
  const uint64_t sound_table_bytes = 4 + 20ull * sounds.size();
  const uint64_t header_size = 28ull + target.strings.size() + bank_table_bytes + sound_table_bytes + target.ext.size();
  if (header_size > std::numeric_limits<size_t>::max() - kAkpkBlockSize)
    return Fail(error, "The music pack header is too large to rebuild.");
  size_t position = static_cast<size_t>((header_size + kAkpkBlockSize - 1) / kAkpkBlockSize * kAkpkBlockSize);
  Bytes body(position, 0);
  for (AkBlob& blob : blobs) {
    position = (position + kAkpkBlockSize - 1) / kAkpkBlockSize * kAkpkBlockSize;
    if (body.size() < position) body.resize(position, 0);
    AkRow& row = blob.bank ? banks[blob.row] : sounds[blob.row];
    row.start_block = static_cast<uint32_t>(position / kAkpkBlockSize);
    row.block_size = kAkpkBlockSize;
    row.byte_size = static_cast<uint32_t>(blob.bytes.size());
    body.insert(body.end(), blob.bytes.begin(), blob.bytes.end());
    position = body.size();
  }

  Bytes header(28 + target.strings.size() + static_cast<size_t>(bank_table_bytes + sound_table_bytes) + target.ext.size(), 0);
  std::memcpy(header.data(), "AKPK", 4);
  PutBe32(&header, 4, static_cast<uint32_t>(header.size() - 8));
  PutBe32(&header, 8, 1);
  PutBe32(&header, 12, static_cast<uint32_t>(target.strings.size()));
  PutBe32(&header, 16, static_cast<uint32_t>(bank_table_bytes));
  PutBe32(&header, 20, static_cast<uint32_t>(sound_table_bytes));
  PutBe32(&header, 24, static_cast<uint32_t>(target.ext.size()));
  std::copy(target.strings.begin(), target.strings.end(), header.begin() + 28);
  size_t at = 28 + target.strings.size();
  PutBe32(&header, at, static_cast<uint32_t>(banks.size()));
  at += 4;
  for (const AkRow& row : banks) { WriteAkRow(&header, at, row); at += 20; }
  PutBe32(&header, at, static_cast<uint32_t>(sounds.size()));
  at += 4;
  for (const AkRow& row : sounds) { WriteAkRow(&header, at, row); at += 20; }
  std::copy(target.ext.begin(), target.ext.end(), header.begin() + static_cast<std::ptrdiff_t>(at));
  if (header.size() > body.size()) return Fail(error, "The rebuilt music pack header overlaps its data.");
  std::copy(header.begin(), header.end(), body.begin());
  *output = std::move(body);
  return true;
}

struct AnnouncerRassRow {
  uint32_t id = 0;
  uint32_t key = 0;
};

// strict = sorted, unique rows (2K14 and WWE '13); WWE 2K15's own table lists id 443 twice, read with strict = false
// (first row wins).
bool ParseRass(const Bytes& bytes, std::map<uint32_t, uint32_t>* rows,
               std::string* error, bool strict = true) {
  if (bytes.size() < 16 || std::memcmp(bytes.data(), "RASS1001", 8) != 0)
    return Fail(error, "A ring-announcer RASS table has an invalid header.");
  const uint32_t size = Le32(bytes, 8), count = Le32(bytes, 12);
  if (size != uint64_t(count) * 8 || bytes.size() != 16ull + size)
    return Fail(error, "A ring-announcer RASS table has invalid row counts.");
  std::map<uint32_t, uint32_t> result;
  uint32_t previous = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id = Le32(bytes, 16 + size_t(i) * 8);
    const uint32_t key = Le32(bytes, 20 + size_t(i) * 8);
    const bool fresh = result.emplace(id, key).second;
    if (strict && ((i && id <= previous) || !fresh))
      return Fail(error, "The ring-announcer RASS rows are not sorted and unique.");
    previous = id;
  }
  *rows = std::move(result);
  return true;
}

Bytes EncodeRass(const std::map<uint32_t, uint32_t>& rows) {
  Bytes result(16 + rows.size() * 8, 0);
  std::memcpy(result.data(), "RASS1001", 8);
  PutLe32(&result, 8, static_cast<uint32_t>(rows.size() * 8));
  PutLe32(&result, 12, static_cast<uint32_t>(rows.size()));
  size_t at = 16;
  for (const auto& [id, key] : rows) {
    PutLe32(&result, at, id);
    PutLe32(&result, at + 4, key);
    at += 8;
  }
  return result;
}

bool FindPacGroup(std::vector<WWE13PacGroupData>* groups, const std::string& name,
                  WWE13PacGroupData** found) {
  for (WWE13PacGroupData& group : *groups)
    if (group.name.substr(0, 4) == name) { *found = &group; return true; }
  return false;
}
bool FindPacEntry(WWE13PacGroupData* group, const std::string& name,
                  WWE13PacEntryData** found) {
  if (!group) return false;
  for (WWE13PacEntryData& entry : group->entries)
    if (entry.name.substr(0, name.size()) == name) { *found = &entry; return true; }
  return false;
}

bool ReadRassFromSoundPac(const Bytes& sound_pac, std::map<uint32_t, uint32_t>* rows,
                          std::string* error, bool strict = true) {
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(sound_pac, &groups, error)) return false;
  WWE13PacGroupData* sond = nullptr;
  WWE13PacEntryData* dupl = nullptr;
  if (!FindPacGroup(&groups, "SOND", &sond) || !FindPacEntry(sond, "DUPL", &dupl))
    return Fail(error, "The sound PAC is missing SOND/DUPL.");
  Bytes outer_bytes;
  if (!DecodeWWE13Bpe(dupl->data, &outer_bytes, error)) return false;
  std::vector<WWE13PachLeafData> outer;
  if (!ReadWWE13PachLeaves(outer_bytes, &outer, error)) return false;
  const auto compressed_outer = std::find_if(outer.begin(), outer.end(), [](const auto& leaf) { return leaf.id == 200; });
  if (compressed_outer == outer.end() || compressed_outer->data.size() < 16 ||
      std::memcmp(compressed_outer->data.data(), "ZLIB", 4) != 0)
    return Fail(error, "The sound PAC has no compressed RASS data.");
  const uint32_t packed = Le32(compressed_outer->data, 8);
  const uint32_t plain_size = Le32(compressed_outer->data, 12);
  if (packed > compressed_outer->data.size() - 16 || plain_size > 32 * 1024 * 1024)
    return Fail(error, "The ring-announcer RASS compression lengths are invalid.");
  Bytes inner_bytes(plain_size);
  wwe_mz_ulong actual = plain_size;
  if (mz_uncompress(inner_bytes.data(), &actual, compressed_outer->data.data() + 16, packed) != 0 || actual != plain_size)
    return Fail(error, "The ring-announcer RASS data could not be decompressed.");
  std::vector<WWE13PachLeafData> inner;
  if (!ReadWWE13PachLeaves(inner_bytes, &inner, error)) return false;
  const auto rass = std::find_if(inner.begin(), inner.end(), [](const auto& leaf) { return leaf.id == 21; });
  if (rass == inner.end()) return Fail(error, "The ring-announcer RASS leaf is missing.");
  return ParseRass(rass->data, rows, error, strict);
}

bool PatchSoundPacRass(const Bytes& source, const std::map<uint32_t, uint32_t>& additions,
                       Bytes* output, std::string* error) {
  std::vector<WWE13PacGroupData> groups;
  if (!ReadWWE13PacGroups(source, &groups, error)) return false;
  WWE13PacGroupData* sond = nullptr;
  WWE13PacEntryData* dupl = nullptr;
  if (!FindPacGroup(&groups, "SOND", &sond) || !FindPacEntry(sond, "DUPL", &dupl))
    return Fail(error, "The 2K14 sound PAC is missing SOND/DUPL.");
  Bytes outer_bytes;
  if (!DecodeWWE13Bpe(dupl->data, &outer_bytes, error)) return false;
  std::vector<WWE13PachLeafData> outer;
  if (!ReadWWE13PachLeaves(outer_bytes, &outer, error)) return false;
  auto compressed_outer = std::find_if(outer.begin(), outer.end(), [](const auto& leaf) { return leaf.id == 200; });
  if (compressed_outer == outer.end() || compressed_outer->data.size() < 16 ||
      std::memcmp(compressed_outer->data.data(), "ZLIB", 4) != 0)
    return Fail(error, "The 2K14 sound PAC has no compressed RASS data.");
  const uint32_t packed = Le32(compressed_outer->data, 8);
  const uint32_t plain_size = Le32(compressed_outer->data, 12);
  if (packed > compressed_outer->data.size() - 16 || plain_size > 32 * 1024 * 1024)
    return Fail(error, "The ring-announcer RASS compression lengths are invalid.");
  Bytes inner_bytes(plain_size);
  wwe_mz_ulong actual = plain_size;
  if (mz_uncompress(inner_bytes.data(), &actual, compressed_outer->data.data() + 16, packed) != 0 || actual != plain_size)
    return Fail(error, "The ring-announcer RASS data could not be decompressed.");
  std::vector<WWE13PachLeafData> inner;
  if (!ReadWWE13PachLeaves(inner_bytes, &inner, error)) return false;
  auto rass = std::find_if(inner.begin(), inner.end(), [](const auto& leaf) { return leaf.id == 21; });
  if (rass == inner.end()) return Fail(error, "The ring-announcer RASS leaf is missing.");
  std::map<uint32_t, uint32_t> rows;
  if (!ParseRass(rass->data, &rows, error)) return false;
  for (const auto& [id, key] : additions) {
    const auto existing = rows.find(id);
    if (existing != rows.end() && existing->second != key)
      return Fail(error, "The target already has a different ring-announcer name key.");
    if (!key) return Fail(error, "A ring-announcer name key cannot be zero.");
    rows[id] = key;
  }
  rass->data = EncodeRass(rows);
  Bytes next_inner;
  if (!BuildWWE13PachLeaves(inner, &next_inner, false, error)) return false;
  next_inner.resize((next_inner.size() + 3) & ~size_t(3), 0);
  Bytes packed_data(mz_compressBound(static_cast<wwe_mz_ulong>(next_inner.size())));
  wwe_mz_ulong packed_size = static_cast<wwe_mz_ulong>(packed_data.size());
  if (mz_compress2(packed_data.data(), &packed_size, next_inner.data(), static_cast<wwe_mz_ulong>(next_inner.size()), 9) != 0)
    return Fail(error, "The ring-announcer name table could not be compressed.");
  Bytes wrapper(16, 0);
  std::memcpy(wrapper.data(), "ZLIB", 4);
  std::copy_n(compressed_outer->data.begin() + 4, 4, wrapper.begin() + 4);
  PutLe32(&wrapper, 8, static_cast<uint32_t>(packed_size));
  PutLe32(&wrapper, 12, static_cast<uint32_t>(next_inner.size()));
  wrapper.insert(wrapper.end(), packed_data.begin(), packed_data.begin() + static_cast<std::ptrdiff_t>(packed_size));
  compressed_outer->data = std::move(wrapper);
  Bytes next_outer;
  if (!BuildWWE13PachLeaves(outer, &next_outer, false, error)) return false;
  next_outer.resize((next_outer.size() + 0xff) & ~size_t(0xff), 0);
  dupl->data = std::move(next_outer);
  return RebuildWWE13PacGroups(source, groups, output, error);
}

struct TreeNode {
  uint32_t key = 0;
  uint16_t weight = 0;
  uint16_t probability = 0;
  uint32_t audio_id = 0;
  std::vector<TreeNode> children;
};

struct DialogueTree {
  uint32_t argument_count = 0;
  std::vector<uint32_t> arguments;
  Bytes header;
  size_t size_at = 0;
  TreeNode root;
};

bool ParseTree(const Bytes& body, DialogueTree* tree, std::string* error) {
  if (body.size() < 12) return Fail(error, "A ring-announcer event tree is too short.");
  DialogueTree result;
  result.argument_count = Be32(body, 0);
  if (!result.argument_count || result.argument_count > 16 || body.size() < 8 + 4ull * result.argument_count)
    return Fail(error, "A ring-announcer event has an invalid argument list.");
  for (uint32_t i = 0; i < result.argument_count; ++i) result.arguments.push_back(Be32(body, 4 + 4 * i));
  result.size_at = 4 + 4 * result.argument_count;
  const uint32_t node_bytes = Be32(body, result.size_at);
  if (!node_bytes || node_bytes % 12 || node_bytes > body.size() - result.size_at - 4)
    return Fail(error, "A ring-announcer event tree has an invalid node table.");
  const size_t start = body.size() - node_bytes;
  if (start <= result.size_at) return Fail(error, "A ring-announcer event tree has a truncated header.");
  result.header.assign(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(start));
  const size_t count = node_bytes / 12;
  struct RawNode { uint32_t key; uint16_t first; uint16_t children; uint16_t weight; uint16_t probability; };
  std::vector<RawNode> raw;
  raw.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const size_t at = start + i * 12;
    raw.push_back({Be32(body, at), Be16(body, at + 4), Be16(body, at + 6),
                   Be16(body, at + 8), Be16(body, at + 10)});
  }
  std::vector<bool> visited(count, false);
  std::function<bool(size_t, uint32_t, TreeNode*)> read_node = [&](size_t index, uint32_t depth, TreeNode* out) {
    if (index >= raw.size() || visited[index]) return false;
    visited[index] = true;
    const RawNode& node = raw[index];
    out->key = node.key;
    out->weight = node.weight;
    out->probability = node.probability;
    if (depth == result.argument_count) {
      out->audio_id = (uint32_t(node.first) << 16) | node.children;
      return true;
    }
    if (!node.children || node.first > count || node.children > count - node.first) return false;
    out->children.resize(node.children);
    for (uint32_t i = 0; i < node.children; ++i)
      if (!read_node(node.first + i, depth + 1, &out->children[i])) return false;
    for (size_t i = 1; i < out->children.size(); ++i)
      if (out->children[i - 1].key >= out->children[i].key) return false;
    return true;
  };
  if (!read_node(0, 0, &result.root) || std::find(visited.begin(), visited.end(), false) != visited.end())
    return Fail(error, "A ring-announcer event tree has shared, unsorted, or invalid nodes.");
  *tree = std::move(result);
  return true;
}

Bytes EncodeTree(const DialogueTree& tree) {
  std::vector<const TreeNode*> level{&tree.root};
  Bytes nodes;
  uint32_t next_child = 1;
  uint32_t depth = 0;
  while (!level.empty()) {
    std::vector<const TreeNode*> children;
    for (const TreeNode* node : level) {
      const size_t at = nodes.size();
      nodes.resize(at + 12);
      PutBe32(&nodes, at, node->key);
      uint16_t first = 0, count = 0;
      if (depth == tree.argument_count) {
        first = static_cast<uint16_t>(node->audio_id >> 16);
        count = static_cast<uint16_t>(node->audio_id);
      } else {
        first = static_cast<uint16_t>(next_child);
        count = static_cast<uint16_t>(node->children.size());
        next_child += count;
        for (const TreeNode& child : node->children) children.push_back(&child);
      }
      PutBe16(&nodes, at + 4, first);
      PutBe16(&nodes, at + 6, count);
      PutBe16(&nodes, at + 8, node->weight);
      PutBe16(&nodes, at + 10, node->probability);
    }
    level = std::move(children);
    ++depth;
  }
  Bytes result = tree.header;
  PutBe32(&result, tree.size_at, static_cast<uint32_t>(nodes.size()));
  result.insert(result.end(), nodes.begin(), nodes.end());
  return result;
}

using TreePath = std::vector<uint32_t>;
void CollectTreeLeaves(const TreeNode& node, uint32_t depth, uint32_t argument_count,
                       TreePath* path, std::vector<std::pair<TreePath, uint32_t>>* leaves) {
  if (depth == argument_count) {
    leaves->emplace_back(*path, node.audio_id);
    return;
  }
  for (const TreeNode& child : node.children) {
    path->push_back(child.key);
    CollectTreeLeaves(child, depth + 1, argument_count, path, leaves);
    path->pop_back();
  }
}

bool AddTreeLeaf(DialogueTree* tree, const TreePath& path, uint32_t audio_id) {
  if (path.size() != tree->argument_count || path.empty()) return false;
  TreeNode* node = &tree->root;
  for (size_t depth = 0; depth < path.size(); ++depth) {
    auto child = std::lower_bound(node->children.begin(), node->children.end(), path[depth],
                                  [](const TreeNode& item, uint32_t key) { return item.key < key; });
    if (child == node->children.end() || child->key != path[depth]) {
      TreeNode fresh;
      fresh.key = path[depth];
      fresh.weight = 50;
      fresh.probability = 100;
      child = node->children.insert(child, std::move(fresh));
    } else if (depth + 1 == path.size()) {
      return false;
    }
    node = &*child;
  }
  node->audio_id = audio_id;
  return true;
}

bool RebuildAnnouncerBank(const Bytes& source,
                          const std::map<uint32_t, Bytes>& changed_events,
                          const std::map<uint32_t, Bytes>& added_sounds,
                          Bytes* output, std::string* error) {
  std::vector<BankSection> sections;
  std::vector<HircObject> objects;
  if (!ReadHirc(source, &sections, &objects, error)) return false;
  Bytes hirc;
  const auto hirc_section = std::find_if(sections.begin(), sections.end(),
                                         [](const BankSection& section) { return section.tag == "HIRC"; });
  if (hirc_section == sections.end()) return Fail(error, "An announcer bank has no HIRC section.");
  uint32_t count = Be32(source, hirc_section->payload);
  count += static_cast<uint32_t>(added_sounds.size());
  hirc.resize(4);
  PutBe32(&hirc, 0, count);
  std::set<uint32_t> written_events;
  const HircObject* actor = nullptr;
  if (!added_sounds.empty()) {
    actor = FindObject(objects, kAnnouncerActor);
    if (!actor || actor->type != 7)
      return Fail(error, "An announcer sound bank is missing its stock actor mixer.");
  }
  Bytes actor_body;
  if (actor) {
    actor_body = Body(source, *actor);
    size_t children_at = 0;
    bool found = false;
    for (size_t at = 0; at < std::min<size_t>(400, actor_body.size() >= 4 ? actor_body.size() - 4 : 0); ++at) {
      const uint32_t child_count = Be32(actor_body, at);
      if (!child_count || at + 4ull + 4ull * child_count != actor_body.size()) continue;
      std::vector<uint32_t> ids(child_count);
      for (uint32_t i = 0; i < child_count; ++i) ids[i] = Be32(actor_body, at + 4 + i * 4);
      if (!std::is_sorted(ids.begin(), ids.end()) ||
          std::adjacent_find(ids.begin(), ids.end()) != ids.end()) continue;
      children_at = at;
      found = true;
      break;
    }
    if (!found) return Fail(error, "An announcer actor's child list is not recognized.");
    std::set<uint32_t> children;
    for (size_t at = children_at + 4; at < actor_body.size(); at += 4) children.insert(Be32(actor_body, at));
    for (const auto& [id, body] : added_sounds) { (void)body; children.insert(id); }
    actor_body.resize(children_at);
    const size_t count_at = actor_body.size();
    actor_body.resize(count_at + 4 + 4 * children.size());
    PutBe32(&actor_body, count_at, static_cast<uint32_t>(children.size()));
    size_t child_at = count_at + 4;
    for (uint32_t id : children) { PutBe32(&actor_body, child_at, id); child_at += 4; }
  }

  bool actor_written = false;
  for (const HircObject& object : objects) {
    if (actor && object.id == actor->id) {
      for (const auto& [id, body] : added_sounds) {
        if (FindObject(objects, id)) return Fail(error, "An announcer Sound ID is already in use.");
        const Bytes encoded = MakeObject(2, id, body);
        hirc.insert(hirc.end(), encoded.begin(), encoded.end());
      }
      const Bytes encoded = MakeObject(object.type, object.id, actor_body);
      hirc.insert(hirc.end(), encoded.begin(), encoded.end());
      actor_written = true;
      continue;
    }
    const auto replacement = changed_events.find(object.id);
    if (replacement != changed_events.end()) {
      const Bytes encoded = MakeObject(object.type, object.id, replacement->second);
      hirc.insert(hirc.end(), encoded.begin(), encoded.end());
      written_events.insert(object.id);
    } else {
      const size_t end = object.at + 5 + object.length;
      hirc.insert(hirc.end(), source.begin() + static_cast<std::ptrdiff_t>(object.at),
                  source.begin() + static_cast<std::ptrdiff_t>(end));
    }
  }
  if (!added_sounds.empty() && !actor_written)
    return Fail(error, "The announcer actor mixer could not be rebuilt.");
  if (written_events.size() != changed_events.size())
    return Fail(error, "An announcer event selected for update is missing from its bank.");
  Bytes rebuilt;
  for (const BankSection& section : sections) {
    const Bytes encoded = section.tag == "HIRC"
        ? MakeSection("HIRC", hirc)
        : MakeSection(section.tag, Bytes(source.begin() + static_cast<std::ptrdiff_t>(section.payload),
                                         source.begin() + static_cast<std::ptrdiff_t>(section.payload + section.size)));
    rebuilt.insert(rebuilt.end(), encoded.begin(), encoded.end());
  }
  *output = std::move(rebuilt);
  return true;
}

bool RebuildAnnouncerPck(const Akpk& target,
                         const std::map<uint32_t, Bytes>& replacements,
                         const std::map<uint32_t, Bytes>& media,
                         Bytes* output, std::string* error) {
  if (target.data.size() < 28) return Fail(error, "The 2K14 announcer pack header is invalid.");
  std::vector<AkRow> banks = target.banks;
  std::vector<AkRow> sounds = target.sounds;
  for (const auto& [id, bytes] : media) {
    if (FindAkRow(sounds, id)) return Fail(error, "A WWE '13 announcer clip ID is already used in 2K14.");
    sounds.push_back({id, kAkpkBlockSize, static_cast<uint32_t>(bytes.size()), 0, 1});
  }
  std::sort(sounds.begin(), sounds.end(), [](const AkRow& a, const AkRow& b) { return a.id < b.id; });

  struct Blob { bool bank; size_t row; Bytes bytes; uint64_t order; };
  std::vector<Blob> blobs;
  for (size_t i = 0; i < banks.size(); ++i) {
    const AkRow& row = banks[i];
    const size_t start = static_cast<size_t>(uint64_t(row.start_block) * row.block_size);
    Bytes bytes;
    const auto replacement = replacements.find(row.id);
    if (replacement != replacements.end()) bytes = replacement->second;
    else bytes.assign(target.data.begin() + static_cast<std::ptrdiff_t>(start),
                      target.data.begin() + static_cast<std::ptrdiff_t>(start + row.byte_size));
    blobs.push_back({true, i, std::move(bytes), row.start_block});
  }
  for (size_t i = 0; i < target.sounds.size(); ++i) {
    const AkRow& row = target.sounds[i];
    const size_t start = static_cast<size_t>(uint64_t(row.start_block) * row.block_size);
    const auto sorted = std::lower_bound(sounds.begin(), sounds.end(), row.id,
                                        [](const AkRow& a, uint32_t id) { return a.id < id; });
    blobs.push_back({false, static_cast<size_t>(sorted - sounds.begin()),
                     Bytes(target.data.begin() + static_cast<std::ptrdiff_t>(start),
                           target.data.begin() + static_cast<std::ptrdiff_t>(start + row.byte_size)),
                     row.start_block});
  }
  uint64_t insertion_order = std::numeric_limits<uint32_t>::max() + uint64_t(1);
  for (const auto& [id, bytes] : media) {
    const auto sorted = std::lower_bound(sounds.begin(), sounds.end(), id,
                                        [](const AkRow& a, uint32_t value) { return a.id < value; });
    blobs.push_back({false, static_cast<size_t>(sorted - sounds.begin()), bytes, insertion_order++});
  }
  std::stable_sort(blobs.begin(), blobs.end(), [](const Blob& a, const Blob& b) { return a.order < b.order; });

  const uint64_t bank_bytes = 4 + 20ull * banks.size();
  const uint64_t sound_bytes = 4 + 20ull * sounds.size();
  const uint64_t header_bytes = 28ull + target.strings.size() + bank_bytes + sound_bytes + target.ext.size();
  if (header_bytes > std::numeric_limits<size_t>::max() - kAkpkBlockSize)
    return Fail(error, "The announcer pack header is too large.");
  size_t position = static_cast<size_t>((header_bytes + kAkpkBlockSize - 1) / kAkpkBlockSize * kAkpkBlockSize);
  Bytes result(position, 0);
  for (Blob& blob : blobs) {
    position = (position + kAkpkBlockSize - 1) / kAkpkBlockSize * kAkpkBlockSize;
    if (result.size() < position) result.resize(position, 0);
    AkRow& row = blob.bank ? banks[blob.row] : sounds[blob.row];
    row.start_block = static_cast<uint32_t>(position / kAkpkBlockSize);
    row.block_size = kAkpkBlockSize;
    row.byte_size = static_cast<uint32_t>(blob.bytes.size());
    result.insert(result.end(), blob.bytes.begin(), blob.bytes.end());
    position = result.size();
  }

  Bytes header(28 + target.strings.size() + static_cast<size_t>(bank_bytes + sound_bytes) + target.ext.size(), 0);
  std::memcpy(header.data(), "AKPK", 4);
  PutBe32(&header, 4, static_cast<uint32_t>(header.size() - 8));
  PutBe32(&header, 8, 1);
  PutBe32(&header, 12, static_cast<uint32_t>(target.strings.size()));
  PutBe32(&header, 16, static_cast<uint32_t>(bank_bytes));
  PutBe32(&header, 20, static_cast<uint32_t>(sound_bytes));
  PutBe32(&header, 24, static_cast<uint32_t>(target.ext.size()));
  std::copy(target.strings.begin(), target.strings.end(), header.begin() + 28);
  size_t at = 28 + target.strings.size();
  PutBe32(&header, at, static_cast<uint32_t>(banks.size()));
  at += 4;
  for (const AkRow& row : banks) { WriteAkRow(&header, at, row); at += 20; }
  PutBe32(&header, at, static_cast<uint32_t>(sounds.size()));
  at += 4;
  for (const AkRow& row : sounds) { WriteAkRow(&header, at, row); at += 20; }
  std::copy(target.ext.begin(), target.ext.end(), header.begin() + static_cast<std::ptrdiff_t>(at));
  if (header.size() > result.size()) return Fail(error, "The announcer pack header overlaps its media.");
  std::copy(header.begin(), header.end(), result.begin());
  *output = std::move(result);
  return true;
}

struct NameEventBank {
  const char* announcer;
  const char* segment;
  uint32_t source_bank;
  uint32_t event_bank;
  uint32_t sound_bank;
};

std::array<NameEventBank, 4> AnnouncerBanks() {
  const auto bank_id = [](const std::string& name) { return Fnv1(name); };
  return {{{"jr", "intro", 0x4237190a, bank_id("ra_jr_intro_E"), bank_id("ra_jr_intro_S")},
           {"tc", "intro", 0xe30cc625, bank_id("ra_tc_intro_E"), bank_id("ra_tc_intro_S")},
           {"jr", "global", 0x770542af, bank_id("ra_jr_global_E"), bank_id("ra_jr_global_S")},
           {"tc", "global", 0xebb30a24, bank_id("ra_tc_global_E"), bank_id("ra_tc_global_S")}}};
}

struct SourceAnnouncerBank {
  NameEventBank ids{};
  Bytes bytes;
  std::vector<BankSection> sections;
  std::vector<HircObject> objects;
};

struct AnnouncerMediaSource {
  std::vector<Akpk> packs;
  std::map<uint32_t, Bytes> clips;
};

bool ReadSourceAnnouncerMedia(const WWE13ImportPaths& paths, AnnouncerMediaSource* media,
                              std::string* error) {
  AnnouncerMediaSource result;
  Akpk base;
  if (!ReadAkpk(paths.wwe13_root / "sound/RA.pck", &base, error)) return false;
  result.packs.push_back(std::move(base));
  for (uint32_t number : {2u, 3u}) {
    for (const fs::path& root : paths.wwe13_pack_roots) {
      const fs::path path = root / "sound" / ("DLC_0" + std::to_string(number) + ".pck");
      std::error_code ec;
      if (!fs::is_regular_file(path, ec) || ec) continue;
      Akpk addon;
      if (!ReadAkpk(path, &addon, error)) return false;
      result.packs.push_back(std::move(addon));
      break;
    }
  }
  *media = std::move(result);
  return true;
}

bool AnnouncerClip(const AnnouncerMediaSource& sources, uint32_t id, Bytes* clip) {
  const auto cached = sources.clips.find(id);
  if (cached != sources.clips.end()) { *clip = cached->second; return true; }
  for (const Akpk& pck : sources.packs) {
    if (!FindAkRow(pck.sounds, id)) continue;
    std::string ignored;
    if (MediaBytes(pck, id, clip, &ignored)) return true;
  }
  return false;
}

bool SupportedAnnouncerFormat(const Bytes& clip) {
  if (clip.size() < 20 || std::memcmp(clip.data(), "RIFX", 4) != 0) return false;
  size_t at = 12;
  while (at + 8 <= clip.size()) {
    const uint32_t length = Be32(clip, at + 4);
    if (length > clip.size() - at - 8) return false;
    if (std::memcmp(clip.data() + at, "fmt ", 4) == 0) {
      if (length != 66 || at + 8 + length < 12) return false;
      const uint16_t codec = Be16(clip, at + 8);
      const uint16_t channels = Be16(clip, at + 10);
      const uint32_t rate = Be32(clip, at + 12);
      static constexpr std::array<uint8_t, 12> tail{0x47,0x04,0x00,0x00,0x48,0xcc,0x38,0x8c,0x8e,0xc2,0x08,0x0b};
      return codec == 0xffff && channels == 1 && rate == 32000 &&
             std::equal(tail.begin(), tail.end(), clip.begin() + static_cast<std::ptrdiff_t>(at + 8 + length - 12));
    }
    at += 8 + length + (length & 1);
  }
  return false;
}

bool ParseDecimalId(const std::string& text, uint32_t* value) {
  if (text.empty()) return false;
  uint64_t number = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    number = number * 10 + static_cast<unsigned>(c - '0');
    if (number > std::numeric_limits<uint32_t>::max()) return false;
  }
  *value = static_cast<uint32_t>(number);
  return true;
}

uint32_t FreshAnnouncerKey(uint32_t source_id) {
  char name[64]{};
  std::snprintf(name, sizeof(name), "wwe13_ra_name_%u", source_id);
  return Fnv1(name);
}
bool IsSlotKey(uint32_t key) {
  return (key & 0xffffff00u) == kFreshAnnouncerKeyPrefixA ||
         (key & 0xffffff00u) == kFreshAnnouncerKeyPrefixB;
}

struct NameCandidate {
  const WWE13ImportPort* port = nullptr;
  uint32_t source_key = 0;
  uint32_t target_key = 0;
  bool available = false;
  std::string reason;
};

struct PlannedNameEvent {
  uint32_t event_bank = 0;
  uint32_t sound_bank = 0;
  uint32_t event_id = 0;
  TreePath target_path;
  uint32_t audio_id = 0;
  Bytes sound_body;
};

bool PlanAnnouncerNames(const std::vector<WWE13ImportPort>& ports,
                        const std::map<uint32_t, uint32_t>& source_rass,
                        const std::map<uint32_t, uint32_t>& target_rass,
                        const Akpk& source_ra,
                        const Akpk& target_es,
                        const AnnouncerMediaSource& media_sources,
                        std::vector<NameCandidate>* candidates,
                        std::vector<PlannedNameEvent>* events,
                        std::map<uint32_t, uint32_t>* rass_additions,
                        std::map<uint32_t, Bytes>* new_media,
                        std::vector<uint32_t>* without_audio,
                        std::string* error) {
  std::vector<NameCandidate> candidates_result;
  std::map<uint32_t, uint32_t> additions;
  std::set<uint32_t> used_keys;
  for (const WWE13ImportPort& port : ports) {
    NameCandidate candidate;
    candidate.port = &port;
    if (!port.own_announcer_name && !port.ra_name.empty()) {
      uint32_t stock_id = 0;
      if (!ParseDecimalId(port.ra_name, &stock_id) || !target_rass.count(stock_id))
        return Fail(error, "A ring-announcer alias does not name an existing 2K14 RASS row.");
      const uint32_t key = target_rass.at(stock_id);
      const auto existing = target_rass.find(port.target_id);
      if (existing != target_rass.end() && existing->second != key)
        return Fail(error, "The target already has a different ring-announcer name key.");
      additions[port.target_id] = key;
      continue;
    }
    if (!port.own_announcer_name) continue;
    const auto found = source_rass.find(port.source_id);
    if (found == source_rass.end() || !found->second) {
      candidate.reason = "no WWE '13 ring-announcer table row";
      without_audio->push_back(port.source_id);
      candidates_result.push_back(std::move(candidate));
      continue;
    }
    candidate.source_key = found->second;
    candidate.target_key = candidate.source_key;
    if (IsSlotKey(candidate.target_key))
      candidate.target_key = FreshAnnouncerKey(port.source_id);
    if (!candidate.target_key || used_keys.count(candidate.target_key)) {
      candidate.reason = "the WWE '13 name key is already used";
      without_audio->push_back(port.source_id);
      candidates_result.push_back(std::move(candidate));
      continue;
    }
    const auto existing = target_rass.find(port.target_id);
    if (existing != target_rass.end() && existing->second != candidate.target_key) {
      candidate.reason = "the target already has a different ring-announcer name";
      without_audio->push_back(port.source_id);
      candidates_result.push_back(std::move(candidate));
      continue;
    }
    used_keys.insert(candidate.target_key);
    candidates_result.push_back(std::move(candidate));
  }

  std::vector<SourceAnnouncerBank> source_banks;
  const std::array<NameEventBank, 4> bank_ids = AnnouncerBanks();
  for (const NameEventBank& ids : bank_ids) {
    SourceAnnouncerBank source;
    source.ids = ids;
    if (!BankBytes(source_ra, ids.source_bank, &source.bytes, error) ||
        !ReadHirc(source.bytes, &source.sections, &source.objects, error)) return false;
    source_banks.push_back(std::move(source));
    if (!FindAkRow(target_es.banks, ids.event_bank) || !FindAkRow(target_es.banks, ids.sound_bank))
      return Fail(error, "The 2K14 announcer event or sound bank is missing.");
  }

  std::map<uint32_t, Bytes> media_result;
  std::map<uint32_t, Bytes> media_cache;
  std::vector<PlannedNameEvent> planned;
  for (NameCandidate& candidate : candidates_result) {
    if (!candidate.port || !candidate.source_key || !candidate.target_key) continue;
    // 2K14 already speaks this key in every name event (Bellas, Kevin Nash): only the table row is needed.
    size_t name_events = 0, events_with_key = 0;
    for (const SourceAnnouncerBank& source : source_banks) {
      Bytes target_bank;
      if (!BankBytes(target_es, source.ids.event_bank, &target_bank, error)) return false;
      std::vector<BankSection> target_sections;
      std::vector<HircObject> target_objects;
      if (!ReadHirc(target_bank, &target_sections, &target_objects, error)) return false;
      for (const HircObject& target_object : target_objects) {
        if (target_object.type != 15) continue;
        DialogueTree target_tree;
        std::string tree_error;
        if (!ParseTree(Body(target_bank, target_object), &target_tree, &tree_error)) continue;
        if (std::find(target_tree.arguments.begin(), target_tree.arguments.end(), kAnnouncerArgumentGroup) == target_tree.arguments.end()) continue;
        ++name_events;
        std::vector<std::pair<TreePath, uint32_t>> target_leaves;
        TreePath path;
        CollectTreeLeaves(target_tree.root, 0, target_tree.argument_count, &path, &target_leaves);
        if (std::any_of(target_leaves.begin(), target_leaves.end(), [&](const auto& leaf) {
              return !leaf.first.empty() && leaf.first.front() == candidate.target_key; })) ++events_with_key;
      }
    }
    if (name_events && events_with_key == name_events) {
      candidate.available = true;
      additions[candidate.port->target_id] = candidate.target_key;
      continue;
    }
    std::vector<PlannedNameEvent> per_candidate;
    std::map<uint32_t, Bytes> candidate_media;
    std::string reason;
    bool valid = true;
    size_t event_count = 0;
    for (const SourceAnnouncerBank& source : source_banks) {
      Bytes target_bank;
      if (!BankBytes(target_es, source.ids.event_bank, &target_bank, error)) return false;
      std::vector<BankSection> target_sections;
      std::vector<HircObject> target_objects;
      if (!ReadHirc(target_bank, &target_sections, &target_objects, error)) return false;
      std::map<uint32_t, const HircObject*> source_objects;
      for (const HircObject& object : source.objects) source_objects[object.id] = &object;
      for (const HircObject& target_object : target_objects) {
        if (target_object.type != 15) continue;
        const Bytes target_body = Body(target_bank, target_object);
        DialogueTree target_tree;
        std::string tree_error;
        if (!ParseTree(target_body, &target_tree, &tree_error)) continue;
        if (std::find(target_tree.arguments.begin(), target_tree.arguments.end(), kAnnouncerArgumentGroup) == target_tree.arguments.end()) continue;
        ++event_count;
        const auto source_object = source_objects.find(target_object.id);
        if (source_object == source_objects.end() || source_object->second->type != 15) {
          valid = false; reason = "a matching WWE '13 name event is missing"; break;
        }
        DialogueTree source_tree;
        if (!ParseTree(Body(source.bytes, *source_object->second), &source_tree, &tree_error) ||
            source_tree.arguments != target_tree.arguments) {
          valid = false; reason = "the WWE '13 and 2K14 name events have different layouts"; break;
        }
        std::vector<std::pair<TreePath, uint32_t>> leaves;
        TreePath path;
        CollectTreeLeaves(source_tree.root, 0, source_tree.argument_count, &path, &leaves);
        std::vector<std::pair<TreePath, uint32_t>> matches;
        for (const auto& leaf : leaves)
          if (!leaf.first.empty() && leaf.first.front() == candidate.source_key) matches.push_back(leaf);
        if (matches.size() != 1) {
          valid = false; reason = "the WWE '13 name event has no unique leaf for the selected wrestler"; break;
        }
        TreePath target_path = matches.front().first;
        target_path.front() = candidate.target_key;
        std::vector<std::pair<TreePath, uint32_t>> target_leaves;
        CollectTreeLeaves(target_tree.root, 0, target_tree.argument_count, &path, &target_leaves);
        if (std::any_of(target_leaves.begin(), target_leaves.end(), [&](const auto& leaf) { return leaf.first == target_path; })) {
          valid = false; reason = "the target already has this announcer key in an event"; break;
        }
        const HircObject* sound = FindObject(source.objects, matches.front().second);
        if (!sound || sound->type != 2 || sound->body_size < 24) {
          valid = false; reason = "the WWE '13 announcer clip object is missing"; break;
        }
        const Bytes sound_body = Body(source.bytes, *sound);
        const std::array<uint8_t, 4> actor_word{
            uint8_t(kAnnouncerActor >> 24), uint8_t(kAnnouncerActor >> 16),
            uint8_t(kAnnouncerActor >> 8), uint8_t(kAnnouncerActor)};
        if (Be32(sound_body, 0) != kAnnouncerPlugin || Be32(sound_body, 4) != 1 ||
            Be32(sound_body, 8) != Be32(sound_body, 12) ||
            std::search(sound_body.begin(), sound_body.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(40, sound_body.size())),
                        actor_word.begin(), actor_word.end()) == sound_body.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(40, sound_body.size()))) {
          valid = false; reason = "the WWE '13 announcer Sound uses an unsupported format"; break;
        }
        const uint32_t media_id = Be32(sound_body, 8);
        Bytes clip;
        const auto cached_clip = media_cache.find(media_id);
        if (cached_clip != media_cache.end()) clip = cached_clip->second;
        else if (AnnouncerClip(media_sources, media_id, &clip)) media_cache.emplace(media_id, clip);
        if (clip.empty() || !SupportedAnnouncerFormat(clip)) {
          valid = false; reason = "the WWE '13 announcer audio clip is missing or has an unsupported format"; break;
        }
        candidate_media.emplace(media_id, std::move(clip));
        per_candidate.push_back({source.ids.event_bank, source.ids.sound_bank, target_object.id,
                                 std::move(target_path), matches.front().second, sound_body});
      }
      if (!valid) break;
    }
    if (!valid || event_count != 26 || per_candidate.size() != 26) {
      candidate.reason = reason.empty() ? "not all 26 WWE '13 announcer events are available" : reason;
      if (candidate.port) without_audio->push_back(candidate.port->source_id);
      continue;
    }
    candidate.available = true;
    additions[candidate.port->target_id] = candidate.target_key;
    media_result.insert(candidate_media.begin(), candidate_media.end());
    planned.insert(planned.end(), per_candidate.begin(), per_candidate.end());
  }
  *candidates = std::move(candidates_result);
  *events = std::move(planned);
  *rass_additions = std::move(additions);
  *new_media = std::move(media_result);
  return true;
}

// The wrestler stages may already have rewritten this game file: build on their copy, not the stock one.
fs::path StagedOrStock(const WWE13ImportPaths& paths, const fs::path& relative) {
  std::error_code ec;
  const fs::path staged = paths.staged_game_root / relative;
  if (!paths.staged_game_root.empty() && fs::is_regular_file(staged, ec) && !ec) return staged;
  return paths.wwe14_root / relative;
}

bool BuildAnnouncerFiles(const WWE13ImportPaths& paths,
                         const std::vector<WWE13ImportPort>& ports,
                         Bytes* output_es, Bytes* output_sound_pac,
                         std::vector<Bytes>* output_arcs,
                         uint32_t* name_count,
                         std::vector<uint32_t>* without_audio,
                         std::string* error) {
  Bytes source_sound_pac, target_sound_pac;
  Akpk source_ra, target_es;
  if (!ReadFile(paths.wwe13_root / "pac/sound.pac", &source_sound_pac, error) ||
      !ReadFile(StagedOrStock(paths, "pac/sound.pac"), &target_sound_pac, error) ||
      !ReadAkpk(paths.wwe13_root / "sound/RA.pck", &source_ra, error) ||
      !ReadAkpk(paths.wwe14_root / "sound/Game_Sound_E_S.pck", &target_es, error)) return false;
  if (!target_es.sounds.empty())
    return Fail(error, "The 2K14 announcer sound pack already contains media rows; this importer only supports the stock empty table.");
  std::map<uint32_t, uint32_t> source_rass, target_rass;
  if (!ReadRassFromSoundPac(source_sound_pac, &source_rass, error) ||
      !ReadRassFromSoundPac(target_sound_pac, &target_rass, error)) return false;
  AnnouncerMediaSource media_sources;
  if (!ReadSourceAnnouncerMedia(paths, &media_sources, error)) return false;
  std::vector<NameCandidate> candidates;
  std::vector<PlannedNameEvent> events;
  std::map<uint32_t, uint32_t> rass_additions;
  std::map<uint32_t, Bytes> media;
  if (!PlanAnnouncerNames(ports, source_rass, target_rass, source_ra, target_es,
                          media_sources, &candidates, &events, &rass_additions,
                          &media, without_audio, error)) return false;

  std::map<uint32_t, std::map<uint32_t, Bytes>> event_replacements;
  std::map<uint32_t, std::map<uint32_t, Bytes>> sound_additions;
  for (const NameEventBank& ids : AnnouncerBanks()) {
    Bytes event_bank;
    if (!BankBytes(target_es, ids.event_bank, &event_bank, error)) return false;
    std::vector<BankSection> sections;
    std::vector<HircObject> objects;
    if (!ReadHirc(event_bank, &sections, &objects, error)) return false;
    std::set<uint32_t> changed_ids;
    for (const PlannedNameEvent& event : events)
      if (event.event_bank == ids.event_bank) changed_ids.insert(event.event_id);
    for (uint32_t event_id : changed_ids) {
      const HircObject* object = FindObject(objects, event_id);
      if (!object || object->type != 15) return Fail(error, "A target announcer event disappeared during conversion.");
      DialogueTree tree;
      if (!ParseTree(Body(event_bank, *object), &tree, error)) return false;
      const Bytes original = Body(event_bank, *object);
      if (EncodeTree(tree) != original)
        return Fail(error, "A stock announcer event does not round-trip byte-for-byte.");
      for (const PlannedNameEvent& event : events) {
        if (event.event_bank != ids.event_bank || event.event_id != event_id) continue;
        if (!AddTreeLeaf(&tree, event.target_path, event.audio_id))
          return Fail(error, "The selected announcer leaf already exists in the target event.");
        sound_additions[event.sound_bank][event.audio_id] = event.sound_body;
      }
      event_replacements[ids.event_bank][event_id] = EncodeTree(tree);
    }
  }
  std::map<uint32_t, Bytes> bank_replacements;
  for (const auto& [bank_id, changes] : event_replacements) {
    Bytes source_bank;
    if (!BankBytes(target_es, bank_id, &source_bank, error)) return false;
    Bytes rebuilt;
    if (!RebuildAnnouncerBank(source_bank, changes, {}, &rebuilt, error)) return false;
    bank_replacements.emplace(bank_id, std::move(rebuilt));
  }
  for (const auto& [bank_id, sounds] : sound_additions) {
    Bytes source_bank;
    if (!BankBytes(target_es, bank_id, &source_bank, error)) return false;
    Bytes rebuilt;
    if (!RebuildAnnouncerBank(source_bank, {}, sounds, &rebuilt, error)) return false;
    bank_replacements.emplace(bank_id, std::move(rebuilt));
  }
  if (!events.empty() && !RebuildAnnouncerPck(target_es, bank_replacements, media, output_es, error)) return false;
  if (events.empty()) output_es->clear();
  if (!rass_additions.empty()) {
    if (!PatchSoundPacRass(target_sound_pac, rass_additions, output_sound_pac, error)) return false;
    output_arcs->resize(2);
    const std::array<const char*, 2> arc_names{"plist360.arc", "plist360_4x3.arc"};
    for (size_t i = 0; i < 2; ++i) {
      Bytes old_arc;
      if (!ReadFile(StagedOrStock(paths, arc_names[i]), &old_arc, error)) return false;
      (*output_arcs)[i] = std::move(old_arc);
    }
    if (!SyncWWE13PacArchives(target_sound_pac, *output_sound_pac, output_arcs, error)) return false;
  }
  *name_count = static_cast<uint32_t>(std::count_if(candidates.begin(), candidates.end(),
                                                    [](const NameCandidate& c) { return c.available; }));
  return true;
}

fs::path UserdataTitleRoot(const fs::path& root) {
  return root / "0000000000000000/545408B2/00000002";
}

bool FindTargetDlc01(const WWE13ImportPaths& paths, fs::path* package,
                     fs::path* pack_path, std::string* error) {
  std::vector<fs::path> candidates;
  std::error_code ec;
  const fs::path title = UserdataTitleRoot(paths.userdata_root);
  if (fs::is_directory(title, ec) && !ec) {
    for (fs::directory_iterator it(title, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
      std::error_code item_error;
      if (!it->is_directory(item_error) || item_error) continue;
      const fs::path pck = it->path() / "sound/DLC_01.pck";
      if (fs::is_regular_file(pck, item_error) && !item_error) candidates.push_back(it->path());
    }
  }
  if (ec) return Fail(error, "The 2K14 DLC folders could not be read: " + ec.message());
  std::sort(candidates.begin(), candidates.end());
  if (candidates.empty())
    return Fail(error, "The WWE 2K14 user data is missing the DLC_01 music pack.");
  *package = candidates.front();
  *pack_path = *package / "sound/DLC_01.pck";
  return true;
}

constexpr uint32_t kMusicPrefetchBank15 = 0x9a4847cb;  // WWE 2K15: in Game_Sound_M.pck (events in E_S bank 0xe85dda21)

std::optional<float> MusicGain15(uint32_t media_id) {
  const auto found = std::lower_bound(kMeasuredMusicGains15.begin(), kMeasuredMusicGains15.end(), media_id,
                                      [](const auto& row, uint32_t id) { return row.first < id; });
  if (found == kMeasuredMusicGains15.end() || found->first != media_id) return std::nullopt;
  return found->second;
}

// 2K15 music selector 10410 -> "Play_MUS_0104_1_0" (theme 104, variant 1/0), the event the 2K15 game posts.
uint32_t SelectorEventId(uint32_t selector) {
  char text[48]{};
  std::snprintf(text, sizeof(text), "Play_MUS_%04u_%u_%u", selector / 100, (selector / 10) % 10, selector % 10);
  return Fnv1(text);
}

// An ID that is in none of the given sets, derived from a name so the same import gives the same IDs.
uint32_t FreshObjectId(const std::string& name, const std::set<uint32_t>& taken_a, const std::set<uint32_t>& taken_b) {
  uint32_t id = Fnv1(name);
  for (uint32_t salt = 1; id == 0 || taken_a.count(id) || taken_b.count(id); ++salt) id = Fnv1(name + "#" + std::to_string(salt));
  return id;
}

// WWE 2K15 name events: one extra leading byte and one byte per argument before the node table, otherwise the 2K14 layout
// (tools/port/p15roster/ra.py). Rewritten into the 2K14 layout so ParseTree reads it.
bool ParseTree15(const Bytes& body, DialogueTree* tree, std::string* error) {
  if (body.size() < 13) return Fail(error, "A WWE 2K15 announcer event is too short.");
  const uint32_t n = Be32(body, 1);
  if (!n || n > 16 || body.size() < 5 + 5ull * n + 4) return Fail(error, "A WWE 2K15 announcer event has an invalid argument list.");
  const uint32_t node_bytes = Be32(body, 5 + 5 * n);
  if (!node_bytes || node_bytes % 12 || node_bytes > body.size()) return Fail(error, "A WWE 2K15 announcer event has an invalid node table.");
  Bytes plain(8 + 4 * n, 0);
  PutBe32(&plain, 0, n);
  for (uint32_t i = 0; i < n; ++i) PutBe32(&plain, 4 + 4 * i, Be32(body, 5 + 4 * i));
  PutBe32(&plain, 4 + 4 * n, node_bytes);
  plain.insert(plain.end(), body.end() - node_bytes, body.end());
  return ParseTree(plain, tree, error);
}

}  // namespace

bool RunWWE13ImportAudio(const WWE13ImportPaths& sources,
                         const std::vector<WWE13ImportPort>& chosen_wrestlers,
                         const std::filesystem::path& fresh_staging_directory,
                         const ProgressFn& progress, const CancelFlag& cancel,
                         WWE13ImportAudioResult* result, std::string* error) {
  if (error) error->clear();
  if (!result) return Fail(error, "The audio import result output is missing.");
  *result = {};
  if (chosen_wrestlers.empty()) return Fail(error, "Choose at least one wrestler to import audio for.");
  if (sources.wwe13_root.empty() || sources.wwe14_root.empty() || sources.userdata_root.empty())
    return Fail(error, "The WWE '13, WWE 2K14, and userdata input folders are required.");
  if (fresh_staging_directory.empty()) return Fail(error, "The audio staging folder path is empty.");
  std::error_code ec;
  if (fs::exists(fresh_staging_directory, ec) || ec)
    return Fail(error, "The audio staging folder must be new and empty.");
  if (!SafeToStage(fresh_staging_directory, sources, error)) return false;
  for (const fs::path& root : {sources.wwe13_root, sources.wwe14_root, sources.userdata_root}) {
    if (!fs::is_directory(root, ec) || ec)
      return Fail(error, "An audio input folder is not accessible: " + internal::PathToUtf8(root));
  }

  std::set<uint32_t> targets;
  for (const WWE13ImportPort& port : chosen_wrestlers) {
    if (port.target_id < 100 || port.target_id > 499 || port.source_id > 499)
      return Fail(error, "A selected wrestler has an ID outside the supported roster range.");
    if (!targets.insert(port.target_id).second)
      return Fail(error, "The selected wrestler list contains a duplicate target ID.");
  }
  if (Cancelled(cancel, error)) return false;

  std::vector<MusicSource> music_sources;
  if (progress) progress(0, chosen_wrestlers.size(), "Reading WWE '13 entrance music");
  if (!LoadMusicSources(sources, &music_sources, error)) return false;
  fs::path music_package, music_source_path;
  if (!FindTargetDlc01(sources, &music_package, &music_source_path, error)) return false;
  Akpk target_music;
  if (!ReadAkpk(music_source_path, &target_music, error)) return false;
  if (std::any_of(target_music.banks.begin(), target_music.banks.end(),
                  [](const AkRow& row) { return row.block_size != kAkpkBlockSize; }) ||
      std::any_of(target_music.sounds.begin(), target_music.sounds.end(),
                  [](const AkRow& row) { return row.block_size != kAkpkBlockSize; }))
    return Fail(error, "The 2K14 DLC music pack uses an unsupported block size.");
  Bytes old_music_bank;
  if (!BankBytes(target_music, kMusicBank, &old_music_bank, error)) return false;
  MusicTemplate music_template;
  if (!ReadMusicTemplate(sources, &music_template, error)) return false;

  std::vector<std::pair<uint32_t, MusicHit>> music_hits;
  std::vector<uint32_t> songs_without_audio;
  std::set<uint32_t> seen_media;
  std::vector<std::pair<uint32_t, Bytes>> song_media;
  std::map<uint32_t, Bytes> media_for_check;
  for (size_t i = 0; i < chosen_wrestlers.size(); ++i) {
    if (Cancelled(cancel, error)) return false;
    const WWE13ImportPort& port = chosen_wrestlers[i];
    if (progress) progress(i, chosen_wrestlers.size(), "Finding theme for " + port.name);
    MusicHit hit;
    std::string ignored;
    if (!FindMusicForPort(sources, music_sources, port, &hit, &ignored) || !MusicGain(hit.media_id)) {
      songs_without_audio.push_back(port.source_id);
      continue;
    }
    hit.gain = *MusicGain(hit.media_id);
    const auto prior = media_for_check.find(hit.media_id);
    if (prior != media_for_check.end() && prior->second != hit.clip)
      return Fail(error, "Two WWE '13 music packs use the same media ID for different clips.");
    media_for_check.emplace(hit.media_id, hit.clip);
    if (seen_media.insert(hit.media_id).second) song_media.emplace_back(hit.media_id, hit.clip);
    music_hits.emplace_back(port.target_id, std::move(hit));
  }
  if (music_hits.size()) {
    Bytes new_bank, new_pack;
    if (!BuildMusicBank(old_music_bank, music_template, music_hits, &new_bank, error) ||
        !RebuildMusicPck(target_music, new_bank, song_media, &new_pack, error)) return false;
    media_for_check.clear();
    // Keep output bytes in a separate slot until the staging directory is created.
    target_music.data = std::move(new_pack);
  }

  const bool needs_announcer = std::any_of(chosen_wrestlers.begin(), chosen_wrestlers.end(),
      [](const WWE13ImportPort& port) { return port.own_announcer_name || !port.ra_name.empty(); });
  Bytes announcer_pack, sound_pac;
  std::vector<Bytes> sound_arcs;
  uint32_t announcer_names = 0;
  std::vector<uint32_t> names_without_audio;
  if (needs_announcer) {
    if (Cancelled(cancel, error)) return false;
    if (progress) progress(chosen_wrestlers.size(), chosen_wrestlers.size(), "Preparing announcer names");
    if (!BuildAnnouncerFiles(sources, chosen_wrestlers, &announcer_pack, &sound_pac,
                             &sound_arcs, &announcer_names, &names_without_audio, error)) return false;
  }

  if (Cancelled(cancel, error)) return false;
  if (!fs::create_directories(fresh_staging_directory, ec) || ec)
    return Fail(error, "The audio staging folder could not be created: " + ec.message());
  bool keep = false;
  struct Cleanup {
    fs::path path;
    bool* keep;
    ~Cleanup() { if (!*keep) { std::error_code ec; fs::remove_all(path, ec); } }
  } cleanup{fresh_staging_directory, &keep};
  const fs::path files = fresh_staging_directory / "files";
  if (!fs::create_directories(files, ec) || ec)
    return Fail(error, "The audio staging output folder could not be created: " + ec.message());

  struct PendingFile { std::string relative; Bytes bytes; };
  std::vector<PendingFile> pending;
  if (music_hits.size()) {
    pending.push_back({"game/sound/DLC_01.pck", std::move(target_music.data)});
  }
  if (!announcer_pack.empty())
    pending.push_back({"game/sound/Game_Sound_E_S.pck", std::move(announcer_pack)});
  if (!sound_pac.empty()) {
    pending.push_back({"game/pac/sound.pac", std::move(sound_pac)});
    pending.push_back({"game/plist360.arc", std::move(sound_arcs[0])});
    pending.push_back({"game/plist360_4x3.arc", std::move(sound_arcs[1])});
  }
  uint64_t total_bytes = 0;
  for (const PendingFile& file : pending) total_bytes += file.bytes.size();
  uint64_t written_bytes = 0;
  std::vector<std::string> output_files;
  for (PendingFile& file : pending) {
    const fs::path relative(file.relative);
    if (relative.empty() || relative.is_absolute() || file.relative.find("..") != std::string::npos)
      return Fail(error, "The audio staging output path is unsafe.");
    if (progress) progress(written_bytes, total_bytes, "Writing " + file.relative);
    if (!WriteStagedFile(files / relative, file.bytes, cancel, progress, written_bytes,
                         total_bytes, "Writing " + file.relative, error)) return false;
    written_bytes += file.bytes.size();
    output_files.push_back(file.relative);
  }
  std::sort(output_files.begin(), output_files.end());
  result->staging_directory = fresh_staging_directory;
  result->files = std::move(output_files);
  result->songs = static_cast<uint32_t>(music_hits.size());
  for (const auto& [target, hit] : music_hits) result->theme_levels.emplace_back(target, hit.gain);
  result->announcer_names = announcer_names;
  result->songs_without_audio = std::move(songs_without_audio);
  result->names_without_audio = std::move(names_without_audio);
  if (progress) progress(total_bytes, total_bytes, "Audio import ready");
  keep = true;
  return true;
}

bool RunWWE15ImportMusic(const fs::path& wwe15_root, const fs::path& wwe14_root, const fs::path& userdata_root,
                         const std::vector<WWE15MusicRequest>& requests, const fs::path& files_root,
                         const CancelFlag& cancel, WWE15ImportMusicResult* result, std::string* error) {
  if (error) error->clear();
  if (!result) return Fail(error, "The WWE 2K15 music result output is missing.");
  *result = {};
  if (requests.empty()) return true;
  std::set<uint32_t> targets;
  for (const WWE15MusicRequest& r : requests) {
    if (r.target_id < 100 || r.target_id > 499) return Fail(error, "A WWE 2K15 wrestler has a slot outside the roster range.");
    if (!targets.insert(r.target_id).second) return Fail(error, "Two WWE 2K15 wrestlers share one slot.");
  }

  MusicSource source;
  source.event_bank = kMusicTemplateBank;
  source.container_bank = kMusicTemplateBank;
  source.separate_media = true;
  source.prefetch_banks = {kMusicPrefetchBank15};
  if (!ReadAkpk(ResolvePathCase(wwe15_root, "sound/Game_Sound_E_S.pck"), &source.pck, error) ||
      !ReadAkpk(ResolvePathCase(wwe15_root, "sound/Game_Sound_M.pck"), &source.media_pck, error)) return false;
  if (Cancelled(cancel, error)) return false;

  WWE13ImportPaths p14;
  p14.wwe14_root = wwe14_root;
  p14.userdata_root = userdata_root;
  // The game reads GAME:\Sound\DLC_01.pck before the add-on copies: build on the WWE '13 import's pack when there is one.
  fs::path target_path = wwe14_root / "sound" / "DLC_01.pck";
  std::error_code ec;
  if (!fs::is_regular_file(target_path, ec) || ec) {
    fs::path package;
    if (!FindTargetDlc01(p14, &package, &target_path, error)) return false;
  }
  Akpk target;
  if (!ReadAkpk(target_path, &target, error)) return false;
  if (std::any_of(target.banks.begin(), target.banks.end(), [](const AkRow& row) { return row.block_size != kAkpkBlockSize; }) ||
      std::any_of(target.sounds.begin(), target.sounds.end(), [](const AkRow& row) { return row.block_size != kAkpkBlockSize; }))
    return Fail(error, "The WWE 2K14 entrance music pack uses an unsupported block size.");
  Bytes old_bank;
  if (!BankBytes(target, kMusicBank, &old_bank, error)) return false;
  MusicTemplate templ;
  if (!ReadMusicTemplate(p14, &templ, error)) return false;

  // IDs already loaded by the game: the entrance music bank itself and WWE 2K14's own theme bank (the template bank).
  std::set<uint32_t> taken;
  {
    std::vector<BankSection> sections;
    std::vector<HircObject> objects;
    if (!ReadHirc(old_bank, &sections, &objects, error)) return false;
    for (const HircObject& o : objects) taken.insert(o.id);
    Akpk stock;
    Bytes stock_bank;
    std::string ignored;
    if (ReadAkpk(wwe14_root / "sound" / "Game_Sound_E_S.pck", &stock, &ignored) &&
        BankBytes(stock, kMusicTemplateBank, &stock_bank, &ignored) && ReadHirc(stock_bank, &sections, &objects, &ignored))
      for (const HircObject& o : objects) taken.insert(o.id);
  }

  std::vector<std::pair<uint32_t, MusicHit>> hits;
  std::vector<std::pair<uint32_t, Bytes>> media;
  std::map<uint32_t, Bytes> media_seen;
  std::map<uint32_t, uint32_t> remap;  // 2K15 object ID -> ID used in WWE 2K14 (only when the 2K15 one is taken)
  std::set<uint32_t> fresh;
  auto own_id = [&](uint32_t id, const char* kind) {
    if (!taken.count(id) && !fresh.count(id)) return id;
    auto& slot = remap[id];
    if (!slot) {
      slot = FreshObjectId(std::string("wwe2k14-p15-") + kind + "-" + std::to_string(id), taken, fresh);
      fresh.insert(slot);
    }
    return slot;
  };
  for (const WWE15MusicRequest& r : requests) {
    if (Cancelled(cancel, error)) return false;
    MusicHit hit;
    bool found = r.selector && LookupMusicEvent(source, SelectorEventId(r.selector), r.source_id, &hit);
    if (!found) found = LookupMusic(source, r.source_id, &hit);
    const std::optional<float> gain = found ? MusicGain15(hit.media_id) : std::nullopt;
    if (!found || !gain) {
      result->notes.push_back("WWE 2K15 " + std::to_string(r.source_id) + ": no own theme in WWE 2K15, keeps a WWE 2K14 theme.");
      continue;
    }
    if (taken.count(EventId("Play_MUS_", r.target_id))) {
      result->notes.push_back("Slot " + std::to_string(r.target_id) + " already has a theme in the music pack; it is kept.");
      continue;
    }
    if (FindAkRow(target.sounds, hit.media_id))
      return Fail(error, "A WWE 2K15 song has the same ID as a song already in the WWE 2K14 music pack.");
    const auto prior = media_seen.find(hit.media_id);
    if (prior != media_seen.end() && prior->second != hit.clip)
      return Fail(error, "Two WWE 2K15 songs use the same ID for different audio.");
    if (prior == media_seen.end()) {
      media_seen.emplace(hit.media_id, hit.clip);
      media.emplace_back(hit.media_id, hit.clip);
    }
    hit.gain = *gain;
    // Same song for several wrestlers (the Shield, the Usos): one container/sound/action, one event per wrestler.
    hit.container_id = own_id(hit.container_id, "container");
    hit.sound_id = own_id(hit.sound_id, "sound");
    hit.action_id = own_id(hit.action_id, "action");
    result->targets_with_song.insert(r.target_id);
    result->theme_levels.emplace_back(r.target_id, hit.gain);
    hits.emplace_back(r.target_id, std::move(hit));
  }
  if (hits.empty()) return true;
  // BuildMusicBank refuses IDs that the bank already holds; the shared song objects above are added once.
  Bytes new_bank, new_pack;
  if (!BuildMusicBank(old_bank, templ, hits, &new_bank, error) ||
      !RebuildMusicPck(target, new_bank, media, &new_pack, error)) return false;
  media_seen.clear();
  const std::string relative = "game/sound/DLC_01.pck";
  if (!WriteStagedFile(files_root / fs::u8path(relative), new_pack, cancel, {}, 0, new_pack.size(), "", error)) return false;
  result->files.push_back(relative);
  return true;
}

bool BuildWWE15AnnouncerNames(const fs::path& wwe15_root, const fs::path& current_es_pack, const Bytes& current_sound_pac,
                              const std::vector<WWE15NameRequest>& requests, WWE15AnnouncerResult* result, std::string* error) {
  if (error) error->clear();
  if (!result) return Fail(error, "The WWE 2K15 announcer result output is missing.");
  *result = {};
  if (requests.empty()) return true;
  Bytes source_sound_pac;
  Akpk source_es, source_media, target_es;
  if (!ReadFile(ResolvePathCase(wwe15_root, "pac/sound.pac"), &source_sound_pac, error) ||
      !ReadAkpk(ResolvePathCase(wwe15_root, "sound/Game_Sound_E_S.pck"), &source_es, error) ||
      !ReadAkpk(ResolvePathCase(wwe15_root, "sound/Game_Sound_M.pck"), &source_media, error) ||
      !ReadAkpk(current_es_pack, &target_es, error)) return false;
  std::map<uint32_t, uint32_t> source_rass, target_rass;
  if (!ReadRassFromSoundPac(source_sound_pac, &source_rass, error, false) ||
      !ReadRassFromSoundPac(current_sound_pac, &target_rass, error)) return false;

  // the 2K14 name events (jr + tc, intro + global) and which keys each one already speaks
  struct TargetEvent { uint32_t bank = 0; uint32_t id = 0; DialogueTree tree; size_t group = 0; std::set<uint32_t> keys; };
  std::vector<TargetEvent> target_events;
  std::map<uint32_t, Bytes> target_banks;
  for (const NameEventBank& ids : AnnouncerBanks()) {
    Bytes bank;
    if (!BankBytes(target_es, ids.event_bank, &bank, error)) return false;
    std::vector<BankSection> sections;
    std::vector<HircObject> objects;
    if (!ReadHirc(bank, &sections, &objects, error)) return false;
    for (const HircObject& o : objects) {
      if (o.type != 15) continue;
      TargetEvent e;
      std::string ignored;
      if (!ParseTree(Body(bank, o), &e.tree, &ignored)) continue;
      const auto g = std::find(e.tree.arguments.begin(), e.tree.arguments.end(), kAnnouncerArgumentGroup);
      if (g == e.tree.arguments.end()) continue;
      e.bank = ids.event_bank; e.id = o.id; e.group = static_cast<size_t>(g - e.tree.arguments.begin());
      std::vector<std::pair<TreePath, uint32_t>> leaves;
      TreePath path;
      CollectTreeLeaves(e.tree.root, 0, e.tree.argument_count, &path, &leaves);
      for (const auto& leaf : leaves) if (leaf.first.size() > e.group) e.keys.insert(leaf.first[e.group]);
      target_events.push_back(std::move(e));
    }
    target_banks[ids.event_bank] = std::move(bank);
  }
  if (target_events.empty()) return Fail(error, "The WWE 2K14 announcer name events were not found.");

  // 2K15 has only the 'jr' banks (same bank and event IDs as 2K14)
  std::map<uint32_t, std::pair<Bytes, std::vector<HircObject>>> source_banks;  // bank id -> bytes, objects
  for (const NameEventBank& ids : AnnouncerBanks()) {
    if (std::string(ids.announcer) != "jr") continue;
    for (uint32_t bank_id : {ids.event_bank, ids.sound_bank}) {
      Bytes bank;
      std::vector<BankSection> sections;
      std::vector<HircObject> objects;
      if (!BankBytes(source_es, bank_id, &bank, error) || !ReadHirc(bank, &sections, &objects, error)) return false;
      source_banks[bank_id] = {std::move(bank), std::move(objects)};
    }
  }
  // the stock 2K14 Sound body every added name clip uses (plugin 0x00040001, stream type 1, actor 0x1ab691a1)
  std::map<uint32_t, Bytes> sound_templates;  // 2K14 sound bank -> body
  std::map<uint32_t, std::set<uint32_t>> target_sound_ids;
  for (const NameEventBank& ids : AnnouncerBanks()) {
    Bytes bank;
    std::vector<BankSection> sections;
    std::vector<HircObject> objects;
    if (!BankBytes(target_es, ids.sound_bank, &bank, error) || !ReadHirc(bank, &sections, &objects, error)) return false;
    const std::array<uint8_t, 4> actor{uint8_t(kAnnouncerActor >> 24), uint8_t(kAnnouncerActor >> 16), uint8_t(kAnnouncerActor >> 8), uint8_t(kAnnouncerActor)};
    for (const HircObject& o : objects) {
      target_sound_ids[ids.sound_bank].insert(o.id);
      if (o.type != 2 || o.body_size < 40 || sound_templates.count(ids.sound_bank)) continue;
      const Bytes b = Body(bank, o);
      if (Be32(b, 0) == kAnnouncerPlugin && Be32(b, 4) == 1 && Be32(b, 8) == Be32(b, 12) &&
          std::search(b.begin(), b.begin() + 40, actor.begin(), actor.end()) != b.begin() + 40)
        sound_templates[ids.sound_bank] = b;
    }
  }

  std::map<uint32_t, uint32_t> additions;
  std::map<std::pair<uint32_t, uint32_t>, std::vector<std::pair<TreePath, uint32_t>>> leaves_to_add;  // (bank, event) -> leaves
  std::map<uint32_t, std::map<uint32_t, Bytes>> sounds_to_add;  // sound bank -> audio id -> body
  std::map<uint32_t, Bytes> media;
  for (const WWE15NameRequest& r : requests) {
    const auto key_row = source_rass.find(r.source_id);
    if (key_row == source_rass.end() || !key_row->second) {
      result->notes.push_back("WWE 2K15 " + std::to_string(r.source_id) + ": no ring-announcer name in WWE 2K15.");
      continue;
    }
    const uint32_t key = key_row->second;
    const auto existing = target_rass.find(r.target_id);
    if (existing != target_rass.end() && existing->second != key) {
      result->notes.push_back("Slot " + std::to_string(r.target_id) + " already has another ring-announcer name; it is kept.");
      continue;
    }
    const bool spoken = std::all_of(target_events.begin(), target_events.end(), [&](const TargetEvent& e) { return e.keys.count(key) > 0; });
    if (spoken) { additions[r.target_id] = key; result->named.insert(r.target_id); continue; }
    if (std::any_of(target_events.begin(), target_events.end(), [&](const TargetEvent& e) { return e.keys.count(key) > 0; })) {
      result->notes.push_back("WWE 2K15 " + std::to_string(r.source_id) + ": his name key is partly used in WWE 2K14; left out.");
      continue;
    }
    // his own 2K15 clips, event by event ('jr' only)
    std::vector<std::tuple<uint32_t, uint32_t, TreePath, uint32_t, uint32_t, Bytes, Bytes>> planned;  // bank, event, path, audio, sbank, sound, clip
    bool ok = true;
    for (const TargetEvent& e : target_events) {
      const NameEventBank* ids = nullptr;
      for (const NameEventBank& b : AnnouncerBanks()) if (b.event_bank == e.bank) ids = &b;
      if (!ids || std::string(ids->announcer) != "jr") continue;
      const auto& [sbytes, sobjects] = source_banks[e.bank];
      const HircObject* so = FindObject(sobjects, e.id);
      if (!so || so->type != 15) continue;
      DialogueTree st;
      std::string ignored;
      if (!ParseTree15(Body(sbytes, *so), &st, &ignored) || st.arguments != e.tree.arguments) { ok = false; break; }
      std::vector<std::pair<TreePath, uint32_t>> leaves;
      TreePath path;
      CollectTreeLeaves(st.root, 0, st.argument_count, &path, &leaves);
      std::vector<std::pair<TreePath, uint32_t>> hits;
      for (const auto& leaf : leaves) if (leaf.first.size() > e.group && leaf.first[e.group] == key) hits.push_back(leaf);
      if (hits.empty()) continue;
      if (hits.size() > 1) { ok = false; break; }
      const auto& [sound_bank_bytes, sound_objects] = source_banks[ids->sound_bank];
      const HircObject* sound = FindObject(sound_objects, hits.front().second);
      if (!sound || sound->type != 2 || sound->body_size < 16) { ok = false; break; }
      const Bytes sb = Body(sound_bank_bytes, *sound);
      if (Be32(sb, 0) != kAnnouncerPlugin || Be32(sb, 4) != 1 || Be32(sb, 8) != Be32(sb, 12)) { ok = false; break; }
      const uint32_t clip_id = Be32(sb, 8);
      Bytes clip;
      if (!MediaBytes(source_media, clip_id, &clip, &ignored) || !SupportedAnnouncerFormat(clip)) { ok = false; break; }
      const auto templ = sound_templates.find(ids->sound_bank);
      if (templ == sound_templates.end()) { ok = false; break; }
      Bytes body = templ->second;
      PutBe32(&body, 8, clip_id);
      PutBe32(&body, 12, clip_id);
      planned.emplace_back(e.bank, e.id, hits.front().first, hits.front().second, ids->sound_bank, std::move(body), std::move(clip));
    }
    if (!ok || planned.empty()) {
      result->notes.push_back("WWE 2K15 " + std::to_string(r.source_id) + ": his name clips could not be used; no ring-announcer name.");
      continue;
    }
    for (auto& [bank, event, path, audio, sbank, body, clip] : planned) {
      leaves_to_add[{bank, event}].emplace_back(path, audio);
      if (!target_sound_ids[sbank].count(audio)) sounds_to_add[sbank][audio] = body;
      const uint32_t clip_id = Be32(body, 8);
      if (FindAkRow(target_es.sounds, clip_id)) continue;  // already in the pack (shared clip)
      media[clip_id] = std::move(clip);
    }
    additions[r.target_id] = key;
    result->named.insert(r.target_id);
  }

  if (!leaves_to_add.empty()) {
    std::map<uint32_t, std::map<uint32_t, Bytes>> changed_events;
    for (const TargetEvent& e : target_events) {
      const auto it = leaves_to_add.find({e.bank, e.id});
      if (it == leaves_to_add.end()) continue;
      DialogueTree tree = e.tree;
      for (const auto& [path, audio] : it->second)
        if (!AddTreeLeaf(&tree, path, audio)) return Fail(error, "A WWE 2K15 announcer line is already in the 2K14 event.");
      changed_events[e.bank][e.id] = EncodeTree(tree);
    }
    std::map<uint32_t, Bytes> replacements;
    for (const auto& [bank, events] : changed_events) {
      Bytes rebuilt;
      if (!RebuildAnnouncerBank(target_banks.at(bank), events, {}, &rebuilt, error)) return false;
      replacements[bank] = std::move(rebuilt);
    }
    for (const auto& [bank, sounds] : sounds_to_add) {
      Bytes source_bank, rebuilt;
      if (!BankBytes(target_es, bank, &source_bank, error) || !RebuildAnnouncerBank(source_bank, {}, sounds, &rebuilt, error)) return false;
      replacements[bank] = std::move(rebuilt);
    }
    if (!RebuildAnnouncerPck(target_es, replacements, media, &result->es_pack, error)) return false;
  }
  if (!additions.empty() && !PatchSoundPacRass(current_sound_pac, additions, &result->sound_pac, error)) return false;
  return true;
}

}  // namespace wwe2k14::launcher



