#ifdef NDEBUG
#undef NDEBUG
#endif

#include "../launcher_core.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <vector>

namespace fs = std::filesystem;
using namespace wwe2k14::launcher;

namespace {

DecodedMusic Sine(uint32_t rate, uint32_t channels, double hz, double seconds, double amplitude) {
  DecodedMusic music;
  music.sample_rate = rate;
  music.channels = channels;
  const size_t frames = static_cast<size_t>(rate * seconds);
  music.samples.resize(frames * channels);
  for (size_t i = 0; i < frames; ++i) {
    const float v = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * i / rate));
    for (uint32_t c = 0; c < channels; ++c) music.samples[i * channels + c] = v;
  }
  return music;
}

double Rms(const std::vector<float>& s, size_t first = 0, size_t last = SIZE_MAX) {
  last = std::min(last, s.size());
  double sum = 0;
  for (size_t i = first; i < last; ++i) sum += double(s[i]) * s[i];
  return std::sqrt(sum / std::max<size_t>(1, last - first));
}

MusicTrackSettings Flat() {
  MusicTrackSettings s;
  s.normalize = false;
  s.fade_in_seconds = 0;
  s.fade_out_seconds = 0;
  return s;
}

double Db(double ratio) { return 20.0 * std::log10(ratio); }

}  // namespace

int main() {
  std::vector<float> out;

  // Rate conversion: 44.1 kHz -> 48 kHz keeps duration and tone level.
  {
    const DecodedMusic in = Sine(44100, 1, 1000.0, 1.0, 0.5);
    assert(RenderMusic(in, Flat(), kGameMusicSampleRate, &out).ok);
    assert(out.size() == 2 * 48000);
    const double rms = Rms(out, 4000, out.size() - 4000);
    assert(std::abs(rms - 0.5 / std::sqrt(2.0)) < 0.01);
  }
  // Downsampling removes content above the new Nyquist (anti-alias filter).
  {
    const DecodedMusic in = Sine(96000, 2, 30000.0, 0.5, 0.5);
    assert(RenderMusic(in, Flat(), 48000, &out).ok);
    assert(Rms(out, 4000, out.size() - 4000) < 0.05);
  }
  // Volume in dB.
  {
    const DecodedMusic in = Sine(48000, 2, 440.0, 0.5, 0.25);
    MusicTrackSettings s = Flat();
    assert(RenderMusic(in, s, 48000, &out).ok);
    const double base = Rms(out);
    s.volume_db = -6.0f;
    assert(RenderMusic(in, s, 48000, &out).ok);
    assert(std::abs(Db(Rms(out) / base) + 6.0) < 0.05);
  }
  // Raising a quiet song with sharp peaks to the game's level limits the peaks instead of clipping them.
  {
    DecodedMusic in = Sine(48000, 2, 440.0, 2.0, 0.03);
    for (size_t i = 0; i < in.samples.size(); i += 24000 * 2) in.samples[i] = in.samples[i + 1] = 0.5f;
    MusicTrackSettings s = Flat();
    s.normalize = true;
    s.target_rms_dbfs = kGameMusicRmsDbfs;
    assert(RenderMusic(in, s, 48000, &out).ok);
    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::abs(v));
    assert(peak <= kMusicLimiterCeiling + 1e-4f);
    assert(std::abs(Db(Rms(out)) - kGameMusicRmsDbfs) < 1.5);  // the tone stays near the target
  }
  // Live player: same loudness as RenderMusic, settings changes apply to the next block, seeking and the end.
  {
    const DecodedMusic in = Sine(48000, 2, 440.0, 2.0, 0.05);
    MusicTrackSettings s = Flat();
    s.normalize = true;
    s.target_rms_dbfs = -20.0f;
    MusicLivePlayer player(in);
    player.SetSettings(s, false);
    std::vector<float> block(48000 * 2);
    assert(player.Render(block.data(), 48000) == 48000);
    assert(std::abs(Db(Rms(block)) + 20.0) < 0.1);
    player.SetSettings(s, true);  // original: untouched
    assert(player.Render(block.data(), 24000) == 24000);
    assert(std::abs(Rms(block, 0, 48000) - 0.05 / std::sqrt(2.0)) < 0.002);
    player.Seek(1.9);
    assert(std::abs(player.PositionSeconds() - 1.9) < 0.001);
    assert(player.Render(block.data(), 48000) == 4800);  // stops at the end
    s.trim_start_seconds = 0.5f;
    s.trim_end_seconds = 1.0f;
    player.SetSettings(s, false);
    player.Seek(0.0);
    assert(std::abs(player.PositionSeconds() - 0.5) < 0.001);
    assert(player.Render(block.data(), 48000) == 24000);
    // Peaks stay under the limiter ceiling when raised far.
    s = Flat();
    s.volume_db = 12.0f;
    MusicLivePlayer loud(Sine(48000, 2, 440.0, 0.5, 0.9));
    loud.SetSettings(s, false);
    assert(loud.Render(block.data(), 24000) == 24000);
    for (size_t i = 0; i < 48000; ++i) assert(std::abs(block[i]) <= kMusicLimiterCeiling + 1e-4f);
  }
  // Normalisation hits the RMS target.
  {
    const DecodedMusic in = Sine(48000, 2, 440.0, 0.5, 0.2);
    MusicTrackSettings s = Flat();
    s.normalize = true;
    s.target_rms_dbfs = -20.0f;
    assert(RenderMusic(in, s, 48000, &out).ok);
    assert(std::abs(Db(Rms(out)) + 20.0) < 0.1);
  }
  // Trim.
  {
    const DecodedMusic in = Sine(48000, 2, 440.0, 2.0, 0.5);
    MusicTrackSettings s = Flat();
    s.trim_start_seconds = 0.5f;
    s.trim_end_seconds = 1.5f;
    assert(RenderMusic(in, s, 48000, &out).ok);
    assert(out.size() == 2 * 48000);
    s.trim_start_seconds = 1.5f;
    s.trim_end_seconds = 0.5f;
    assert(!RenderMusic(in, s, 48000, &out).ok);
  }
  // Fades.
  {
    const DecodedMusic in = Sine(48000, 2, 440.0, 2.0, 0.5);
    MusicTrackSettings s = Flat();
    s.fade_in_seconds = 0.5f;
    s.fade_out_seconds = 0.5f;
    assert(RenderMusic(in, s, 48000, &out).ok);
    assert(std::abs(out[0]) < 1e-4f);
    assert(std::abs(out[out.size() - 1]) < 1e-3f);
    assert(Rms(out, 2 * 24000, 2 * 24480) > 0.3);  // steady state afterwards
    assert(Rms(out, 0, 2 * 2400) < Rms(out, 2 * 40000, 2 * 42400));
  }
  // EQ: bass shelf boosts low tone, leaves high tone.
  {
    MusicTrackSettings s = Flat();
    s.bass_gain_db = 9.0f;
    s.bass_frequency_hz = 200.0f;
    const DecodedMusic low = Sine(48000, 2, 40.0, 1.0, 0.1);
    assert(RenderMusic(low, Flat(), 48000, &out).ok);
    const double low_base = Rms(out, 20000);
    assert(RenderMusic(low, s, 48000, &out).ok);
    assert(std::abs(Db(Rms(out, 20000) / low_base) - 9.0) < 0.7);
    const DecodedMusic high = Sine(48000, 2, 5000.0, 1.0, 0.1);
    assert(RenderMusic(high, Flat(), 48000, &out).ok);
    const double high_base = Rms(out, 20000);
    assert(RenderMusic(high, s, 48000, &out).ok);
    assert(std::abs(Db(Rms(out, 20000) / high_base)) < 0.5);
  }
  // Treble shelf cut.
  {
    MusicTrackSettings s = Flat();
    s.treble_gain_db = -9.0f;
    s.treble_frequency_hz = 4000.0f;
    const DecodedMusic high = Sine(48000, 2, 12000.0, 1.0, 0.1);
    assert(RenderMusic(high, Flat(), 48000, &out).ok);
    const double base = Rms(out, 20000);
    assert(RenderMusic(high, s, 48000, &out).ok);
    assert(std::abs(Db(Rms(out, 20000) / base) + 9.0) < 0.7);
  }
  // Clipping is bounded; invalid layouts are rejected.
  {
    const DecodedMusic in = Sine(48000, 2, 440.0, 0.2, 0.9);
    MusicTrackSettings s = Flat();
    s.volume_db = 12.0f;
    assert(RenderMusic(in, s, 48000, &out).ok);
    for (float v : out) assert(v >= -1.0f && v <= 1.0f);
    DecodedMusic bad;
    assert(!RenderMusic(bad, s, 48000, &out).ok);
  }
  // Waveform.
  {
    const DecodedMusic in = Sine(48000, 2, 100.0, 1.0, 0.5);
    const auto wave = BuildMusicWaveform(in, 64);
    assert(wave.size() == 64);
    assert(wave[10].maximum_left > 0.45f && wave[10].minimum_left < -0.45f);
  }
  // Write + decode round trip, settings persistence, edit/save/remove flow.
  {
    const fs::path root = fs::path(WWE2K14_LAUNCHER_TESTDATA) / "music-mixer";
    std::error_code ec;
    fs::remove_all(root, ec);
    Paths paths;
    paths.exe_dir = root;
    paths.settings_file = root / "wwe2k14-enhanced.ini";
    paths.music_dir = root / "music";
    fs::create_directories(paths.music_dir);

    const DecodedMusic in = Sine(44100, 2, 440.0, 1.0, 0.3);
    std::vector<float> flat;
    assert(RenderMusic(in, Flat(), kGameMusicSampleRate, &flat).ok);
    const fs::path original = paths.music_dir / "song.wav";
    assert(WriteMusicWavFloat(original, flat, 44100).ok);  // 44.1 kHz source file

    DecodedMusic decoded;
    assert(DecodeMusicFile(original, &decoded).ok);
    assert(decoded.sample_rate == 44100 && decoded.channels == 2);
    const auto original_bytes = fs::file_size(original);

    Settings settings = LoadSettings(paths);
    settings.music_preset.target_rms_dbfs = -20.0f;
    settings.music_preset.bass_gain_db = 3.0f;
    assert(SaveSettings(paths, settings).ok);
    settings = LoadSettings(paths);
    assert(std::abs(settings.music_preset.target_rms_dbfs + 20.0f) < 1e-3f);
    assert(std::abs(settings.music_preset.bass_gain_db - 3.0f) < 1e-3f);
    // Per-song defaults come from the preset until edited.
    assert(std::abs(MusicSettingsForSong(settings, original).bass_gain_db - 3.0f) < 1e-3f);

    MusicTrackSettings edit = MusicSettingsForSong(settings, original);
    edit.volume_db = -3.0f;
    edit.trim_end_seconds = 0.8f;
    fs::path saved;
    assert(SaveMusicEdit(paths, &settings, original, decoded, edit, &saved).ok);
    assert(fs::exists(saved));
    assert(settings.music_tracks.size() == 1);
    // Original kept untouched in .sources, not visible in the game's top-level scan.
    const fs::path source = MusicSourcePath(paths, settings, saved);
    assert(source != saved && fs::exists(source));
    assert(fs::file_size(source) == original_bytes);
    DecodedMusic processed;
    assert(DecodeMusicFile(saved, &processed).ok);
    assert(processed.sample_rate == kGameMusicSampleRate && processed.channels == 2);
    assert(std::abs(double(processed.frame_count()) - 0.8 * 48000) < 2);
    auto songs = ListSongs(paths);
    assert(songs.size() == 1 && songs[0].processed);

    // Persisted across reload; re-edit from the original keeps one file.
    Settings reloaded = LoadSettings(paths);
    assert(reloaded.music_tracks.size() == 1);
    assert(std::abs(MusicSettingsForSong(reloaded, saved).volume_db + 3.0f) < 1e-3f);
    DecodedMusic from_source;
    assert(DecodeMusicFile(MusicSourcePath(paths, reloaded, saved), &from_source).ok);
    assert(from_source.sample_rate == 44100);
    edit.volume_db = -6.0f;
    fs::path saved2;
    assert(SaveMusicEdit(paths, &reloaded, saved, from_source, edit, &saved2).ok);
    assert(saved2 == saved && ListSongs(paths).size() == 1);

    assert(RemoveSong(paths, saved2, &reloaded).ok);
    assert(ListSongs(paths).empty() && reloaded.music_tracks.empty());
    assert(!fs::exists(source));
    fs::remove_all(root, ec);
  }
  std::cout << "music_mixer_test OK\n";
  return 0;
}
