#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION

#include "launcher_core.h"

#include "internal/util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <system_error>

#include <dr_flac.h>
#include <dr_mp3.h>
#include <dr_wav.h>
#include <stb_vorbis.c>

namespace wwe2k14::launcher {
namespace {

struct AudioType {
  const char* extension;
  const char* display;
};

constexpr AudioType kAudioTypes[] = {{".mp3", "MP3"}, {".ogg", "OGG"},
                                     {".flac", "FLAC"}, {".wav", "WAV"}};

const AudioType* TypeForPath(const fs::path& file) {
  const std::string extension = internal::LowerAscii(internal::PathToUtf8(file.extension()));
  for (const auto& type : kAudioTypes) if (extension == type.extension) return &type;
  return nullptr;
}

double SongDuration(const fs::path& file, const AudioType& type) {
  std::vector<uint8_t> data;
  if (!internal::ReadBinaryFile(file, data) || data.empty()) return 0.0;
  double duration = 0.0;
  if (type.display == std::string_view("MP3")) {
    drmp3 decoder{};
    if (drmp3_init_memory(&decoder, data.data(), data.size(), nullptr)) {
      const uint64_t frames = drmp3_get_pcm_frame_count(&decoder);
      if (decoder.sampleRate != 0) duration = static_cast<double>(frames) / decoder.sampleRate;
      drmp3_uninit(&decoder);
    }
  } else if (type.display == std::string_view("FLAC")) {
    drflac* decoder = drflac_open_memory(data.data(), data.size(), nullptr);
    if (decoder) {
      if (decoder->sampleRate != 0) {
        duration = static_cast<double>(decoder->totalPCMFrameCount) / decoder->sampleRate;
      }
      drflac_close(decoder);
    }
  } else if (type.display == std::string_view("WAV")) {
    drwav decoder{};
    if (drwav_init_memory(&decoder, data.data(), data.size(), nullptr)) {
      drwav_uint64 frames = 0;
      if (decoder.sampleRate != 0 && drwav_get_length_in_pcm_frames(&decoder, &frames) == DRWAV_SUCCESS) {
        duration = static_cast<double>(frames) / decoder.sampleRate;
      }
      drwav_uninit(&decoder);
    }
  } else if (data.size() <= static_cast<size_t>(std::numeric_limits<int>::max())) {
    int error = 0;
    stb_vorbis* decoder = stb_vorbis_open_memory(data.data(), static_cast<int>(data.size()), &error, nullptr);
    if (decoder) {
      const stb_vorbis_info info = stb_vorbis_get_info(decoder);
      if (info.sample_rate != 0) {
        duration = static_cast<double>(stb_vorbis_stream_length_in_samples(decoder)) / info.sample_rate;
      }
      stb_vorbis_close(decoder);
    }
  }
  return std::isfinite(duration) && duration > 0.0 ? duration : 0.0;
}

bool IsSamePathOrFile(const fs::path& left, const fs::path& right) {
  if (left.lexically_normal() == right.lexically_normal()) return true;
  std::error_code error;
  const bool same = fs::equivalent(left, right, error);
  return !error && same;
}

std::string UniqueStamp() {
  return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

fs::path ChooseProcessedPath(const fs::path& music_dir, const fs::path& source_file) {
  std::set<std::string> existing;
  std::error_code error;
  for (fs::directory_iterator it(music_dir, fs::directory_options::skip_permission_denied, error), end;
       !error && it != end; it.increment(error)) {
    existing.insert(internal::LowerAscii(internal::PathToUtf8(it->path().filename())));
  }
  const fs::path stem = source_file.stem();
  for (unsigned number = 1; number < 100000; ++number) {
    fs::path filename = stem;
    if (number > 1) {
#ifdef _WIN32
      filename += std::wstring(L" (") + std::to_wstring(number) + L")";
#else
      filename += std::string(" (") + std::to_string(number) + ")";
#endif
    }
    filename += ".wav";
    const fs::path candidate = music_dir / filename;
    if (!existing.contains(internal::LowerAscii(internal::PathToUtf8(filename))) ||
        IsSamePathOrFile(candidate, source_file)) {
      return candidate;
    }
  }
  return {};
}

struct ReplaceState {
  fs::path backup;
  bool had_target = false;
};

bool CommitStagedFile(const fs::path& staged, const fs::path& target, ReplaceState* state) {
  std::error_code error;
  state->had_target = fs::exists(target, error) && !error;
  if (state->had_target) {
    state->backup = target;
    state->backup += ".replace-backup-" + UniqueStamp();
    fs::rename(target, state->backup, error);
    if (error) return false;
  }
  error.clear();
  fs::rename(staged, target, error);
  if (!error) return true;
  if (state->had_target) {
    std::error_code restore_error;
    fs::rename(state->backup, target, restore_error);
  }
  return false;
}

void RollbackStagedFile(const fs::path& target, const ReplaceState& state) {
  std::error_code error;
  fs::remove(target, error);
  if (state.had_target) {
    error.clear();
    fs::rename(state.backup, target, error);
  }
}

void FinishStagedFile(const ReplaceState& state) {
  if (!state.had_target) return;
  std::error_code error;
  fs::remove(state.backup, error);
}

bool IsSafeMusicRelative(const fs::path& relative) {
  if (relative.empty() || relative.is_absolute() || relative.has_root_name()) return false;
  for (const auto& component : relative) {
    if (component == "..") return false;
  }
  return true;
}

fs::path UniqueSongName(const fs::path& music_dir, const fs::path& source) {
  std::error_code error;
  std::set<std::string> existing;
  fs::directory_iterator iterator(music_dir, fs::directory_options::skip_permission_denied, error);
  if (!error) {
    for (const auto& entry : iterator) existing.insert(internal::LowerAscii(internal::PathToUtf8(entry.path().filename())));
  }
  if (!existing.contains(internal::LowerAscii(internal::PathToUtf8(source.filename())))) return music_dir / source.filename();
  const fs::path stem = source.stem();
  const fs::path extension = source.extension();
  for (unsigned number = 2; number < 100000; ++number) {
    const fs::path name = fs::path(stem.native() +
#ifdef _WIN32
                                   std::wstring(L" (") + std::to_wstring(number) + L")" + extension.native()
#else
                                   std::string(" (") + std::to_string(number) + ")" + extension.native()
#endif
                                   );
    if (!existing.contains(internal::LowerAscii(internal::PathToUtf8(name)))) return music_dir / name;
  }
  return {};
}

}  // namespace

Result DecodeMusicFile(const fs::path& file, DecodedMusic* decoded) {
  try {
    if (!decoded) return Result::Fail("The launcher could not open that song.");
    const AudioType* type = TypeForPath(file);
    if (!type) return Result::Fail("Only MP3, OGG, FLAC, and WAV files can be edited.");
    std::vector<uint8_t> bytes;
    if (!internal::ReadBinaryFile(file, bytes) || bytes.empty()) {
      return Result::Fail("The launcher could not read that song.");
    }
    DecodedMusic result;
    if (type->display == std::string_view("MP3")) {
      drmp3 decoder{};
      if (!drmp3_init_memory(&decoder, bytes.data(), bytes.size(), nullptr)) {
        return Result::Fail("The launcher could not decode that MP3 file.");
      }
      result.sample_rate = decoder.sampleRate;
      result.channels = decoder.channels;
      const uint64_t frames = drmp3_get_pcm_frame_count(&decoder);
      if (result.channels && frames <= std::numeric_limits<size_t>::max() / result.channels) {
        result.samples.resize(static_cast<size_t>(frames) * result.channels);
        const drmp3_uint64 read = drmp3_read_pcm_frames_f32(&decoder, frames, result.samples.data());
        result.samples.resize(static_cast<size_t>(read) * result.channels);
      }
      drmp3_uninit(&decoder);
    } else if (type->display == std::string_view("FLAC")) {
      drflac* decoder = drflac_open_memory(bytes.data(), bytes.size(), nullptr);
      if (!decoder) return Result::Fail("The launcher could not decode that FLAC file.");
      result.sample_rate = decoder->sampleRate;
      result.channels = decoder->channels;
      const uint64_t frames = decoder->totalPCMFrameCount;
      if (result.channels && frames <= std::numeric_limits<size_t>::max() / result.channels) {
        result.samples.resize(static_cast<size_t>(frames) * result.channels);
        const drflac_uint64 read = drflac_read_pcm_frames_f32(decoder, frames, result.samples.data());
        result.samples.resize(static_cast<size_t>(read) * result.channels);
      }
      drflac_close(decoder);
    } else if (type->display == std::string_view("WAV")) {
      drwav decoder{};
      if (!drwav_init_memory(&decoder, bytes.data(), bytes.size(), nullptr)) {
        return Result::Fail("The launcher could not decode that WAV file.");
      }
      result.sample_rate = decoder.sampleRate;
      result.channels = decoder.channels;
      drwav_uint64 frames = 0;
      if (result.channels && drwav_get_length_in_pcm_frames(&decoder, &frames) == DRWAV_SUCCESS &&
          frames <= std::numeric_limits<size_t>::max() / result.channels) {
        result.samples.resize(static_cast<size_t>(frames) * result.channels);
        const drwav_uint64 read = drwav_read_pcm_frames_f32(&decoder, frames, result.samples.data());
        result.samples.resize(static_cast<size_t>(read) * result.channels);
      }
      drwav_uninit(&decoder);
    } else {
      if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return Result::Fail("That OGG file is too large to edit.");
      }
      int error = 0;
      stb_vorbis* decoder = stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()), &error, nullptr);
      if (!decoder) return Result::Fail("The launcher could not decode that OGG file.");
      const stb_vorbis_info info = stb_vorbis_get_info(decoder);
      result.sample_rate = info.sample_rate;
      result.channels = static_cast<uint32_t>(std::max(info.channels, 0));
      if (result.channels && result.channels <= 8) {
        constexpr int kChunkFloatCount = 1 << 15;
        std::vector<float> chunk(kChunkFloatCount);
        for (;;) {
          const int frames = stb_vorbis_get_samples_float_interleaved(
              decoder, static_cast<int>(result.channels), chunk.data(), kChunkFloatCount);
          if (frames <= 0) break;
          const size_t sample_count = static_cast<size_t>(frames) * result.channels;
          result.samples.insert(result.samples.end(), chunk.begin(), chunk.begin() + sample_count);
        }
      }
      stb_vorbis_close(decoder);
    }
    if (!result.sample_rate || !result.channels || result.channels > 8 || result.samples.empty() ||
        result.samples.size() % result.channels != 0) {
      return Result::Fail("That song has no supported audio samples.");
    }
    *decoded = std::move(result);
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The launcher could not decode that song.");
  }
}

fs::path MusicSourcePath(const Paths& paths, const Settings& settings, const fs::path& song_file) {
  const std::string filename = internal::PathToUtf8(song_file.filename());
  const auto record = settings.music_tracks.find(filename);
  if (record == settings.music_tracks.end()) return song_file;
  try {
    const fs::path relative = internal::PathFromUtf8(record->second.source_relative_path);
    if (!IsSafeMusicRelative(relative)) return song_file;
    const fs::path source = (paths.music_dir / relative).lexically_normal();
    if (!internal::IsWithin(source, paths.music_dir)) return song_file;
    std::error_code error;
    return fs::is_regular_file(source, error) && !error ? source : song_file;
  } catch (...) {
    return song_file;
  }
}

MusicTrackSettings MusicSettingsForSong(const Settings& settings, const fs::path& song_file) {
  const auto record = settings.music_tracks.find(internal::PathToUtf8(song_file.filename()));
  if (record != settings.music_tracks.end()) return record->second.settings;
  MusicTrackSettings result;
  result.target_rms_dbfs = settings.music_preset.target_rms_dbfs;
  result.fade_in_seconds = settings.music_preset.fade_in_seconds;
  result.fade_out_seconds = settings.music_preset.fade_out_seconds;
  result.bass_gain_db = settings.music_preset.bass_gain_db;
  result.bass_frequency_hz = settings.music_preset.bass_frequency_hz;
  result.treble_gain_db = settings.music_preset.treble_gain_db;
  result.treble_frequency_hz = settings.music_preset.treble_frequency_hz;
  return result;
}

std::vector<Song> ListSongs(const Paths& paths) {
  std::vector<Song> songs;
  const Settings settings = LoadSettings(paths);
  std::error_code error;
  fs::directory_iterator iterator(paths.music_dir, fs::directory_options::skip_permission_denied, error);
  if (error) return songs;
  for (const auto& entry : iterator) {
    error.clear();
    if (!entry.is_regular_file(error) || error) continue;
    const AudioType* type = TypeForPath(entry.path());
    if (!type) continue;
    Song song;
    song.file = entry.path();
    song.title = internal::PathToUtf8(entry.path().stem());
    song.format = type->display;
    song.seconds = SongDuration(entry.path(), *type);
    song.processed = settings.music_tracks.contains(internal::PathToUtf8(entry.path().filename()));
    songs.push_back(std::move(song));
  }
  std::sort(songs.begin(), songs.end(), [](const Song& left, const Song& right) {
    const std::string left_title = internal::LowerAscii(left.title);
    const std::string right_title = internal::LowerAscii(right.title);
    if (left_title != right_title) return left_title < right_title;
    return internal::PathToUtf8(left.file) < internal::PathToUtf8(right.file);
  });
  return songs;
}

Result AddSongs(const Paths& paths, const std::vector<fs::path>& files, std::vector<std::string>* skipped) {
  try {
    if (skipped) skipped->clear();
    if (!internal::EnsureDirectory(paths.music_dir)) return Result::Fail("The launcher could not create the music folder.");
    for (const fs::path& file : files) {
      const AudioType* type = TypeForPath(file);
      if (!type) {
        if (skipped) skipped->push_back("Only MP3, OGG, FLAC, and WAV files can be added: " + internal::PathToUtf8(file.filename()));
        continue;
      }
      std::error_code error;
      if (!fs::is_regular_file(file, error) || error) {
        if (skipped) skipped->push_back("This song could not be opened: " + internal::PathToUtf8(file.filename()));
        continue;
      }
      if (internal::IsWithin(file, paths.music_dir)) continue;
      const fs::path destination = UniqueSongName(paths.music_dir, file);
      if (destination.empty()) return Result::Fail("The launcher could not choose a name for a song.");
      error.clear();
      if (fs::equivalent(file, destination, error) && !error) continue;
      error.clear();
      if (!fs::copy_file(file, destination, fs::copy_options::none, error) || error) {
        return Result::Fail("The launcher could not add " + internal::PathToUtf8(file.filename()) + ".");
      }
    }
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The launcher could not add the selected songs.");
  }
}

Result SaveMusicEdit(const Paths& paths, Settings* settings, const fs::path& song_file,
                     const DecodedMusic& decoded, const MusicTrackSettings& edit,
                     fs::path* saved_song_file) {
  try {
    if (!settings || !saved_song_file || !internal::IsWithin(song_file, paths.music_dir) ||
        !TypeForPath(song_file)) {
      return Result::Fail("Choose a song from the music folder to edit it.");
    }
    std::error_code error;
    if (!fs::is_regular_file(song_file, error) || error) return Result::Fail("That song is no longer available.");

    const std::string song_key = internal::PathToUtf8(song_file.filename());
    const auto old_record = settings->music_tracks.find(song_key);
    const bool already_processed = old_record != settings->music_tracks.end();
    const fs::path source_file = MusicSourcePath(paths, *settings, song_file);
    if (!internal::IsWithin(source_file, paths.music_dir) || !fs::is_regular_file(source_file, error) || error) {
      return Result::Fail("The original song file is missing from the music folder.");
    }
    const fs::path output_file = already_processed ? song_file : ChooseProcessedPath(paths.music_dir, song_file);
    if (output_file.empty()) return Result::Fail("The launcher could not choose a name for the processed song.");

    std::vector<float> rendered;
    const Result render_result = RenderMusic(decoded, edit, kGameMusicSampleRate, &rendered);
    if (!render_result.ok) return render_result;
    if (!internal::EnsureDirectory(paths.music_dir)) return Result::Fail("The launcher could not create the music folder.");

    fs::path source_backup;
    std::string source_relative;
    if (already_processed) {
      source_backup = source_file;
      source_relative = old_record->second.source_relative_path;
    } else {
      const fs::path source_directory = paths.music_dir / ".sources";
      if (!internal::EnsureDirectory(source_directory)) {
        return Result::Fail("The launcher could not save a copy of the original song.");
      }
      fs::path desired = output_file.stem();
      desired += ".original";
      desired += song_file.extension();
      source_backup = UniqueSongName(source_directory, desired);
      if (source_backup.empty() ||
          !fs::copy_file(source_file, source_backup, fs::copy_options::none, error) || error) {
        return Result::Fail("The launcher could not save a copy of the original song.");
      }
      source_relative = ".sources/" + internal::PathToUtf8(source_backup.filename());
    }

    fs::path staged_file = output_file;
    staged_file += ".stage-" + UniqueStamp();
    const Result write_result = WriteMusicWavFloat(staged_file, rendered, kGameMusicSampleRate);
    if (!write_result.ok) {
      if (!already_processed) fs::remove(source_backup, error);
      return write_result;
    }

    Settings updated = *settings;
    MusicTrackRecord record;
    record.source_relative_path = source_relative;
    record.settings = edit;
    if (already_processed && song_key != internal::PathToUtf8(output_file.filename())) {
      updated.music_tracks.erase(song_key);
    }
    updated.music_tracks.insert_or_assign(internal::PathToUtf8(output_file.filename()), std::move(record));
    const Result saved_settings = SaveSettings(paths, updated);
    if (!saved_settings.ok) {
      fs::remove(staged_file, error);
      if (!already_processed) fs::remove(source_backup, error);
      return saved_settings;
    }

    ReplaceState replace_state;
    if (!CommitStagedFile(staged_file, output_file, &replace_state)) {
      (void)SaveSettings(paths, *settings);
      fs::remove(staged_file, error);
      if (!already_processed) fs::remove(source_backup, error);
      return Result::Fail("The launcher could not replace the song with its processed copy.");
    }

    if (!already_processed && !IsSamePathOrFile(source_file, output_file)) {
      error.clear();
      if (!fs::remove(source_file, error) || error) {
        RollbackStagedFile(output_file, replace_state);
        (void)SaveSettings(paths, *settings);
        if (!already_processed) fs::remove(source_backup, error);
        return Result::Fail("The launcher could not hide the original song from the game playlist.");
      }
    }
    FinishStagedFile(replace_state);
    *settings = std::move(updated);
    *saved_song_file = output_file;
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The launcher could not save the processed song.");
  }
}

Result RemoveSong(const Paths& paths, const fs::path& song_file, Settings* settings) {
  try {
    const AudioType* type = TypeForPath(song_file);
    if (!type || !internal::IsWithin(song_file, paths.music_dir)) {
      return Result::Fail("Choose a song from the music folder to remove it.");
    }
    std::error_code error;
    if (!fs::is_regular_file(song_file, error) || error) {
      return Result::Fail("The launcher could not remove that song.");
    }

    Settings updated = settings ? *settings : LoadSettings(paths);
    const std::string key = internal::PathToUtf8(song_file.filename());
    const auto record = updated.music_tracks.find(key);
    if (record == updated.music_tracks.end()) {
      if (!fs::remove(song_file, error) || error) return Result::Fail("The launcher could not remove that song.");
      return Result::Ok();
    }

    const fs::path source_file = MusicSourcePath(paths, updated, song_file);
    fs::path removed_file = song_file;
    removed_file += ".remove-" + UniqueStamp();
    fs::rename(song_file, removed_file, error);
    if (error) return Result::Fail("The launcher could not remove that song.");
    updated.music_tracks.erase(key);
    const Result saved = SaveSettings(paths, updated);
    if (!saved.ok) {
      error.clear();
      fs::rename(removed_file, song_file, error);
      return saved;
    }
    error.clear();
    fs::remove(removed_file, error);
    if (source_file != song_file && internal::IsWithin(source_file, paths.music_dir)) {
      error.clear();
      fs::remove(source_file, error);
    }
    if (settings) *settings = std::move(updated);
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The launcher could not remove that song.");
  }
}

}  // namespace wwe2k14::launcher
