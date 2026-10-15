#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION

#include "launcher_core.h"

#include "internal/util.h"

#include <algorithm>
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

std::vector<Song> ListSongs(const Paths& paths) {
  std::vector<Song> songs;
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

Result RemoveSong(const Paths& paths, const fs::path& song_file) {
  try {
    const AudioType* type = TypeForPath(song_file);
    if (!type || !internal::IsWithin(song_file, paths.music_dir)) {
      return Result::Fail("Choose a song from the music folder to remove it.");
    }
    std::error_code error;
    if (!fs::is_regular_file(song_file, error) || error || !fs::remove(song_file, error) || error) {
      return Result::Fail("The launcher could not remove that song.");
    }
    return Result::Ok();
  } catch (...) {
    return Result::Fail("The launcher could not remove that song.");
  }
}

}  // namespace wwe2k14::launcher
