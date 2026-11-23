#include "launcher_core.h"

#include "internal/util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <numbers>
#include <string>

#include <dr_wav.h>

namespace wwe2k14::launcher {
namespace {

constexpr size_t kResamplerTaps = 32;
constexpr int kFirstResamplerTap = -15;
constexpr double kPi = std::numbers::pi_v<double>;

struct Biquad {
  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a1 = 0.0;
  double a2 = 0.0;
  double z1 = 0.0;
  double z2 = 0.0;

  float Process(float input) {
    const double output = b0 * input + z1;
    z1 = b1 * input - a1 * output + z2;
    z2 = b2 * input - a2 * output;
    return static_cast<float>(output);
  }
};

double Sinc(double value) {
  if (std::abs(value) < 1e-12) return 1.0;
  const double x = kPi * value;
  return std::sin(x) / x;
}

float SourceChannel(const DecodedMusic& decoded, uint64_t frame, uint32_t channel) {
  const float value = decoded.samples[static_cast<size_t>(frame) * decoded.channels + channel];
  return std::isfinite(value) ? value : 0.0f;
}

std::array<float, 2> SourceStereo(const DecodedMusic& decoded, uint64_t frame) {
  float left = SourceChannel(decoded, frame, 0);
  if (decoded.channels == 1) return {left, left};
  float right = SourceChannel(decoded, frame, 1);
  for (uint32_t channel = 2; channel < decoded.channels; ++channel) {
    const float extra = SourceChannel(decoded, frame, channel) * 0.5f;
    right += extra;
    // Match the game's XMP decoder's simple fold-down for channels beyond stereo.
    left += extra;
  }
  return {left, right};
}

std::vector<float> ResampleStereo(const DecodedMusic& decoded, uint64_t start_frame,
                                  uint64_t end_frame, uint32_t output_rate) {
  const uint64_t input_frames = end_frame - start_frame;
  const uint32_t input_rate = decoded.sample_rate;
  if (input_frames == 0 || output_rate == 0 || input_rate == 0) return {};
  if (input_rate == output_rate) {
    std::vector<float> output(static_cast<size_t>(input_frames) * 2);
    for (uint64_t frame = 0; frame < input_frames; ++frame) {
      const auto stereo = SourceStereo(decoded, start_frame + frame);
      output[static_cast<size_t>(frame) * 2] = stereo[0];
      output[static_cast<size_t>(frame) * 2 + 1] = stereo[1];
    }
    return output;
  }

  const long double scaled_frames = static_cast<long double>(input_frames) * output_rate / input_rate;
  if (scaled_frames < 1.0L || scaled_frames > static_cast<long double>(std::numeric_limits<size_t>::max() / 2)) {
    return {};
  }
  const size_t output_frames = static_cast<size_t>(std::llround(scaled_frames));
  if (output_frames == 0 || output_frames > std::numeric_limits<size_t>::max() / 2) return {};
  std::vector<float> output(output_frames * 2);
  const double ratio = static_cast<double>(input_rate) / output_rate;
  const double cutoff = std::min(1.0, static_cast<double>(output_rate) / input_rate);
  std::array<double, kResamplerTaps> coefficients{};

  for (size_t output_frame = 0; output_frame < output_frames; ++output_frame) {
    const double position = static_cast<double>(output_frame) * ratio;
    const int64_t center = static_cast<int64_t>(std::floor(position));
    const double fraction = position - center;
    double coefficient_sum = 0.0;
    for (size_t tap = 0; tap < kResamplerTaps; ++tap) {
      const int offset = kFirstResamplerTap + static_cast<int>(tap);
      const double distance = static_cast<double>(offset) - fraction;
      const double window_position = (static_cast<double>(tap) + 0.5) / kResamplerTaps;
      const double window = 0.42 - 0.5 * std::cos(2.0 * kPi * window_position) +
                            0.08 * std::cos(4.0 * kPi * window_position);
      const double coefficient = cutoff * Sinc(distance * cutoff) * window;
      coefficients[tap] = coefficient;
      coefficient_sum += coefficient;
    }
    if (std::abs(coefficient_sum) < 1e-15) coefficient_sum = 1.0;
    double left = 0.0;
    double right = 0.0;
    for (size_t tap = 0; tap < kResamplerTaps; ++tap) {
      const int64_t source_index = std::clamp<int64_t>(center + kFirstResamplerTap + static_cast<int>(tap),
                                                      0, static_cast<int64_t>(input_frames - 1));
      const auto stereo = SourceStereo(decoded, start_frame + static_cast<uint64_t>(source_index));
      const double weight = coefficients[tap] / coefficient_sum;
      left += stereo[0] * weight;
      right += stereo[1] * weight;
    }
    output[output_frame * 2] = static_cast<float>(left);
    output[output_frame * 2 + 1] = static_cast<float>(right);
  }
  return output;
}

Biquad MakeShelf(double sample_rate, double frequency, double gain_db, bool high_shelf) {
  Biquad result;
  if (std::abs(gain_db) < 1e-4 || sample_rate <= 0.0) return result;
  frequency = std::clamp(frequency, 10.0, sample_rate * 0.45);
  const double A = std::pow(10.0, gain_db / 40.0);
  const double omega = 2.0 * kPi * frequency / sample_rate;
  const double cosine = std::cos(omega);
  const double sine = std::sin(omega);
  constexpr double slope = 1.0;
  const double alpha = sine * 0.5 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
  const double beta = 2.0 * std::sqrt(A) * alpha;
  double b0, b1, b2, a0, a1, a2;
  if (!high_shelf) {
    b0 = A * ((A + 1.0) - (A - 1.0) * cosine + beta);
    b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosine);
    b2 = A * ((A + 1.0) - (A - 1.0) * cosine - beta);
    a0 = (A + 1.0) + (A - 1.0) * cosine + beta;
    a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosine);
    a2 = (A + 1.0) + (A - 1.0) * cosine - beta;
  } else {
    b0 = A * ((A + 1.0) + (A - 1.0) * cosine + beta);
    b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosine);
    b2 = A * ((A + 1.0) + (A - 1.0) * cosine - beta);
    a0 = (A + 1.0) - (A - 1.0) * cosine + beta;
    a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosine);
    a2 = (A + 1.0) - (A - 1.0) * cosine - beta;
  }
  result.b0 = b0 / a0;
  result.b1 = b1 / a0;
  result.b2 = b2 / a0;
  result.a1 = a1 / a0;
  result.a2 = a2 / a0;
  return result;
}

MusicTrackSettings ClampSettings(const MusicTrackSettings& input) {
  MusicTrackSettings value = input;
  auto finite_clamp = [](float number, float fallback, float low, float high) {
    return std::isfinite(number) ? std::clamp(number, low, high) : fallback;
  };
  value.volume_db = finite_clamp(value.volume_db, 0.0f, -24.0f, 12.0f);
  value.target_rms_dbfs = finite_clamp(value.target_rms_dbfs, -18.0f, -36.0f, -6.0f);
  value.trim_start_seconds = finite_clamp(value.trim_start_seconds, 0.0f, 0.0f, 86400.0f);
  value.trim_end_seconds = finite_clamp(value.trim_end_seconds, 0.0f, 0.0f, 86400.0f);
  value.fade_in_seconds = finite_clamp(value.fade_in_seconds, 0.25f, 0.0f, 10.0f);
  value.fade_out_seconds = finite_clamp(value.fade_out_seconds, 1.0f, 0.0f, 10.0f);
  value.bass_gain_db = finite_clamp(value.bass_gain_db, 0.0f, -12.0f, 12.0f);
  value.bass_frequency_hz = finite_clamp(value.bass_frequency_hz, 120.0f, 20.0f, 1000.0f);
  value.treble_gain_db = finite_clamp(value.treble_gain_db, 0.0f, -12.0f, 12.0f);
  value.treble_frequency_hz = finite_clamp(value.treble_frequency_hz, 8000.0f, 1000.0f, 18000.0f);
  return value;
}

std::string Utf8Path(const fs::path& path) { return internal::PathToUtf8(path); }

}  // namespace

std::vector<MusicWaveformPoint> BuildMusicWaveform(const DecodedMusic& decoded, size_t point_count) {
  std::vector<MusicWaveformPoint> points;
  if (decoded.channels == 0 || decoded.samples.size() < decoded.channels || point_count == 0) return points;
  const uint64_t frames = decoded.frame_count();
  if (!frames) return points;
  point_count = std::min<size_t>(point_count, static_cast<size_t>(frames));
  points.resize(point_count);
  for (size_t bin = 0; bin < point_count; ++bin) {
    const uint64_t begin = static_cast<uint64_t>(bin) * frames / point_count;
    const uint64_t end = std::max(begin + 1, static_cast<uint64_t>(bin + 1) * frames / point_count);
    MusicWaveformPoint& point = points[bin];
    point.minimum_left = point.minimum_right = std::numeric_limits<float>::infinity();
    point.maximum_left = point.maximum_right = -std::numeric_limits<float>::infinity();
    for (uint64_t frame = begin; frame < std::min(end, frames); ++frame) {
      const auto stereo = SourceStereo(decoded, frame);
      point.minimum_left = std::min(point.minimum_left, stereo[0]);
      point.maximum_left = std::max(point.maximum_left, stereo[0]);
      point.minimum_right = std::min(point.minimum_right, stereo[1]);
      point.maximum_right = std::max(point.maximum_right, stereo[1]);
    }
  }
  return points;
}

// Look-ahead peak limiter: keeps stereo peaks at kMusicLimiterCeiling instead of hard clipping when a song is
// raised (normalising a quiet song to the game's level needs several dB of gain). Gain falls over a 5 ms
// look-ahead (sliding minimum + moving average, so it reaches the needed value by the peak) and recovers over
// ~80 ms. Songs whose peaks stay under the ceiling are not changed.
static void LimitPeaks(std::vector<float>& stereo, uint32_t rate) {
  const size_t frames = stereo.size() / 2;
  if (frames == 0) return;
  std::vector<float> needed(frames);
  bool any = false;
  for (size_t i = 0; i < frames; ++i) {
    const float peak = std::max(std::abs(stereo[i * 2]), std::abs(stereo[i * 2 + 1]));
    needed[i] = peak > kMusicLimiterCeiling ? kMusicLimiterCeiling / peak : 1.0f;
    any = any || needed[i] < 1.0f;
  }
  if (!any) return;
  const size_t look = std::max<size_t>(1, rate / 200);
  // Sliding minimum of needed[i .. i + look] (monotonic deque, walking backwards).
  std::vector<float> lowest(frames);
  std::deque<size_t> window;
  for (size_t n = frames; n-- > 0;) {
    while (!window.empty() && needed[window.back()] >= needed[n]) window.pop_back();
    window.push_back(n);
    while (window.front() > n + look) window.pop_front();
    lowest[n] = needed[window.front()];
  }
  // Moving average over lowest[n - look + 1 .. n] ramps the gain down ahead of each peak.
  const double release = 1.0 - std::exp(-1.0 / (0.08 * rate));
  double sum = 0.0, gain = 1.0;
  for (size_t n = 0; n < frames; ++n) {
    sum += lowest[n];
    if (n >= look) sum -= lowest[n - look];
    // Before the first full window, pad with lowest[0] so a peak in the first 5 ms is limited too.
    const double average = n + 1 >= look ? sum / look : (sum + (look - n - 1) * lowest[0]) / look;
    gain = average < gain ? average : gain + (average - gain) * release;
    stereo[n * 2] = static_cast<float>(stereo[n * 2] * gain);
    stereo[n * 2 + 1] = static_cast<float>(stereo[n * 2 + 1] * gain);
  }
}

Result RenderMusic(const DecodedMusic& decoded, const MusicTrackSettings& raw_settings,
                   uint32_t output_rate, std::vector<float>* stereo_output) {
  if (!stereo_output) return Result::Fail("The launcher could not prepare the edited song.");
  stereo_output->clear();
  if (!decoded.sample_rate || !decoded.channels || decoded.channels > 8 ||
      decoded.samples.empty() || decoded.samples.size() % decoded.channels != 0 || output_rate < 8000 ||
      output_rate > 192000) {
    return Result::Fail("This song has an unsupported audio layout.");
  }
  const uint64_t total_frames = decoded.frame_count();
  const double duration = static_cast<double>(total_frames) / decoded.sample_rate;
  const MusicTrackSettings settings = ClampSettings(raw_settings);
  const double start_seconds = std::clamp<double>(settings.trim_start_seconds, 0.0, duration);
  const double requested_end = settings.trim_end_seconds <= 0.0f ? duration : settings.trim_end_seconds;
  const double end_seconds = std::clamp(requested_end, 0.0, duration);
  const uint64_t start_frame = std::min<uint64_t>(static_cast<uint64_t>(std::floor(start_seconds * decoded.sample_rate)), total_frames);
  const uint64_t end_frame = std::min<uint64_t>(static_cast<uint64_t>(std::floor(end_seconds * decoded.sample_rate)), total_frames);
  if (end_frame <= start_frame) return Result::Fail("The trim end must be after the trim start.");

  std::vector<float> output = ResampleStereo(decoded, start_frame, end_frame, output_rate);
  if (output.empty()) return Result::Fail("The launcher could not resample this song.");

  if (settings.normalize) {
    long double square_sum = 0.0;
    for (float sample : output) {
      const double value = std::isfinite(sample) ? sample : 0.0;
      square_sum += value * value;
    }
    const double rms = std::sqrt(static_cast<double>(square_sum / output.size()));
    if (rms > 1e-9) {
      const double target = std::pow(10.0, settings.target_rms_dbfs / 20.0);
      const float gain = static_cast<float>(target / rms);
      for (float& sample : output) sample = std::isfinite(sample) ? sample * gain : 0.0f;
    }
  }

  const float volume = static_cast<float>(std::pow(10.0, settings.volume_db / 20.0));
  Biquad bass[2] = {MakeShelf(output_rate, settings.bass_frequency_hz, settings.bass_gain_db, false),
                    MakeShelf(output_rate, settings.bass_frequency_hz, settings.bass_gain_db, false)};
  Biquad treble[2] = {MakeShelf(output_rate, settings.treble_frequency_hz, settings.treble_gain_db, true),
                      MakeShelf(output_rate, settings.treble_frequency_hz, settings.treble_gain_db, true)};
  const size_t frames = output.size() / 2;
  const size_t fade_in_frames = std::min<size_t>(frames, static_cast<size_t>(std::llround(settings.fade_in_seconds * output_rate)));
  const size_t fade_out_frames = std::min<size_t>(frames, static_cast<size_t>(std::llround(settings.fade_out_seconds * output_rate)));
  for (size_t frame = 0; frame < frames; ++frame) {
    double fade = 1.0;
    if (fade_in_frames > 1 && frame < fade_in_frames) {
      fade *= static_cast<double>(frame) / (fade_in_frames - 1);
    }
    if (fade_out_frames > 1 && frame >= frames - fade_out_frames) {
      fade *= static_cast<double>(frames - 1 - frame) / (fade_out_frames - 1);
    }
    for (size_t channel = 0; channel < 2; ++channel) {
      float sample = output[frame * 2 + channel] * volume;
      sample = bass[channel].Process(sample);
      sample = treble[channel].Process(sample);
      sample = static_cast<float>(sample * fade);
      output[frame * 2 + channel] = std::isfinite(sample) ? sample : 0.0f;
    }
  }
  LimitPeaks(output, output_rate);
  for (float& sample : output) sample = std::clamp(sample, -1.0f, 1.0f);
  *stereo_output = std::move(output);
  return Result::Ok();
}

Result WriteMusicWavFloat(const fs::path& file, const std::vector<float>& stereo_samples,
                          uint32_t sample_rate) {
  if (stereo_samples.empty() || stereo_samples.size() % 2 != 0 || sample_rate < 8000 || sample_rate > 192000 ||
      stereo_samples.size() / 2 > (std::numeric_limits<uint32_t>::max() - 44u) / 8u) {
    return Result::Fail("The edited song is too large to save as WAV.");
  }
  drwav_data_format format{};
  format.container = drwav_container_riff;
  format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
  format.channels = 2;
  format.sampleRate = sample_rate;
  format.bitsPerSample = 32;
  drwav wav{};
#ifdef _WIN32
  const std::wstring path = file.wstring();
  if (!drwav_init_file_write_w(&wav, path.c_str(), &format, nullptr)) {
#else
  const std::string path = Utf8Path(file);
  if (!drwav_init_file_write(&wav, path.c_str(), &format, nullptr)) {
#endif
    return Result::Fail("The launcher could not create the processed WAV file.");
  }
  const drwav_uint64 frames = stereo_samples.size() / 2;
  const drwav_uint64 written = drwav_write_pcm_frames(&wav, frames, stereo_samples.data());
  drwav_uninit(&wav);
  if (written != frames) return Result::Fail("The launcher could not finish writing the processed WAV file.");
  return Result::Ok();
}

namespace {
constexpr uint64_t kRmsBlock = 1024;
}  // namespace

MusicLivePlayer::MusicLivePlayer(const DecodedMusic& decoded) {
  if (!decoded.sample_rate || !decoded.channels || decoded.channels > 8) return;
  rate_ = decoded.sample_rate;
  frames_ = decoded.frame_count();
  stereo_.resize(static_cast<size_t>(frames_) * 2);
  block_squares_.assign(static_cast<size_t>((frames_ + kRmsBlock - 1) / kRmsBlock), 0.0);
  for (uint64_t frame = 0; frame < frames_; ++frame) {
    const auto value = SourceStereo(decoded, frame);
    stereo_[frame * 2] = value[0];
    stereo_[frame * 2 + 1] = value[1];
    block_squares_[frame / kRmsBlock] += double(value[0]) * value[0] + double(value[1]) * value[1];
  }
  release_ = 1.0 - std::exp(-1.0 / (0.08 * rate_));
  end_ = frames_;
}

double MusicLivePlayer::RegionRms(uint64_t start, uint64_t end) const {
  if (end <= start) return 0.0;
  auto squares = [this](uint64_t first, uint64_t last) {
    double sum = 0.0;
    for (uint64_t frame = first; frame < last; ++frame) {
      sum += double(stereo_[frame * 2]) * stereo_[frame * 2] + double(stereo_[frame * 2 + 1]) * stereo_[frame * 2 + 1];
    }
    return sum;
  };
  const uint64_t first_block = (start + kRmsBlock - 1) / kRmsBlock, last_block = end / kRmsBlock;
  double sum = 0.0;
  if (first_block >= last_block) {
    sum = squares(start, end);
  } else {
    sum = squares(start, first_block * kRmsBlock) + squares(last_block * kRmsBlock, end);
    for (uint64_t block = first_block; block < last_block; ++block) sum += block_squares_[block];
  }
  return std::sqrt(sum / (2.0 * (end - start)));
}

void MusicLivePlayer::Configure() {
  const MusicTrackSettings s = ClampSettings(settings_);
  if (original_) {
    start_ = 0;
    end_ = frames_;
    gain_ = 1.0;
  } else {
    start_ = std::min<uint64_t>(frames_, static_cast<uint64_t>(std::floor(double(s.trim_start_seconds) * rate_)));
    end_ = s.trim_end_seconds <= 0.0f
               ? frames_
               : std::min<uint64_t>(frames_, static_cast<uint64_t>(std::floor(double(s.trim_end_seconds) * rate_)));
    if (end_ < start_) end_ = start_;
    if (start_ != rms_start_ || end_ != rms_end_) {
      rms_ = RegionRms(start_, end_);
      rms_start_ = start_;
      rms_end_ = end_;
    }
    gain_ = std::pow(10.0, s.volume_db / 20.0);
    if (s.normalize && rms_ > 1e-9) gain_ *= std::pow(10.0, s.target_rms_dbfs / 20.0) / rms_;
  }
  auto retune = [this](Shelf& shelf, float& db, float& hz, float new_db, float new_hz, bool high) {
    if (db == new_db && hz == new_hz) return;
    const Biquad made = MakeShelf(rate_, new_hz, new_db, high);
    shelf.b0 = made.b0; shelf.b1 = made.b1; shelf.b2 = made.b2; shelf.a1 = made.a1; shelf.a2 = made.a2;
    db = new_db;
    hz = new_hz;
  };
  retune(bass_, bass_db_, bass_hz_, original_ ? 0.0f : s.bass_gain_db, s.bass_frequency_hz, false);
  retune(treble_, treble_db_, treble_hz_, original_ ? 0.0f : s.treble_gain_db, s.treble_frequency_hz, true);
  position_ = std::clamp(position_, start_, end_);
}

void MusicLivePlayer::SetSettings(const MusicTrackSettings& settings, bool original) {
  std::lock_guard lock(mutex_);
  settings_ = settings;
  original_ = original;
  Configure();
}

void MusicLivePlayer::Seek(double seconds) {
  std::lock_guard lock(mutex_);
  const double frame = std::isfinite(seconds) ? std::max(0.0, seconds) * rate_ : 0.0;
  position_ = std::clamp<uint64_t>(static_cast<uint64_t>(frame), start_, end_);
}

double MusicLivePlayer::PositionSeconds() const {
  std::lock_guard lock(mutex_);
  return rate_ ? double(position_) / rate_ : 0.0;
}

size_t MusicLivePlayer::Render(float* stereo, size_t frames) {
  std::lock_guard lock(mutex_);
  const MusicTrackSettings s = ClampSettings(settings_);
  const double length = double(end_ - start_);
  const double fade_in = original_ ? 0.0 : double(s.fade_in_seconds) * rate_;
  const double fade_out = original_ ? 0.0 : double(s.fade_out_seconds) * rate_;
  size_t written = 0;
  for (; written < frames && position_ < end_; ++written, ++position_) {
    const double offset = double(position_ - start_);
    double fade = 1.0;
    if (fade_in > 1.0 && offset < fade_in) fade *= offset / fade_in;
    if (fade_out > 1.0 && length - 1.0 - offset < fade_out) fade *= std::max(0.0, length - 1.0 - offset) / fade_out;
    double out[2];
    for (int channel = 0; channel < 2; ++channel) {
      double x = stereo_[position_ * 2 + channel] * gain_;
      if (!original_) {
        for (Shelf* shelf : {&bass_, &treble_}) {
          const double y = shelf->b0 * x + shelf->z1[channel];
          shelf->z1[channel] = shelf->b1 * x - shelf->a1 * y + shelf->z2[channel];
          shelf->z2[channel] = shelf->b2 * x - shelf->a2 * y;
          x = y;
        }
      }
      out[channel] = x * fade;
    }
    if (!original_) {
      const double peak = std::max(std::abs(out[0]), std::abs(out[1]));
      const double needed = peak > kMusicLimiterCeiling ? kMusicLimiterCeiling / peak : 1.0;
      limiter_ = needed < limiter_ ? needed : limiter_ + (needed - limiter_) * release_;
      out[0] *= limiter_;
      out[1] *= limiter_;
    }
    for (int channel = 0; channel < 2; ++channel) {
      const float value = static_cast<float>(out[channel]);
      stereo[written * 2 + channel] = std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
    }
  }
  return written;
}

}  // namespace wwe2k14::launcher
