#include "blox7/audio/pcm_rate_converter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <numeric>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace blox7::audio {
namespace {

constexpr std::size_t kMaximumPhases = 640;
constexpr std::size_t kBaseTaps = 64;
constexpr std::size_t kMaximumTaps = 384;
constexpr double kStopbandDecibels = 100.0;
constexpr double kAudibleHz = 20000.0;
constexpr double kPi = 3.14159265358979323846;

double BesselI0(double x) {
  double sum = 1.0;
  double term = 1.0;
  const double half = x * 0.5;
  for (int k = 1; k < 64; ++k) {
    term *= (half / k) * (half / k);
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

double Sinc(double x) {
  if (std::fabs(x) < 1e-12) return 1.0;
  const double a = kPi * x;
  return std::sin(a) / a;
}

float Dot(const float* a, const float* b, std::size_t count) {
#if defined(__SSE__)
  __m128 sum0 = _mm_setzero_ps();
  __m128 sum1 = _mm_setzero_ps();
  std::size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    sum0 = _mm_add_ps(
        sum0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
    sum1 = _mm_add_ps(
        sum1, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
  }
  for (; i + 4 <= count; i += 4) {
    sum0 = _mm_add_ps(
        sum0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
  }
  sum0 = _mm_add_ps(sum0, sum1);
  sum0 = _mm_add_ps(sum0, _mm_movehl_ps(sum0, sum0));
  sum0 = _mm_add_ss(sum0, _mm_shuffle_ps(sum0, sum0, 1));
  float total = _mm_cvtss_f32(sum0);
  for (; i < count; ++i) total += a[i] * b[i];
  return total;
#else
  float total = 0.0F;
  for (std::size_t i = 0; i < count; ++i) total += a[i] * b[i];
  return total;
#endif
}

}  // namespace

std::unique_ptr<PcmRateConverter> PcmRateConverter::Create(
    int source_rate_hz, int target_rate_hz, std::uint8_t channels) {
  if (source_rate_hz <= 0 || target_rate_hz <= 0 || channels == 0 ||
      channels > 8 || source_rate_hz == target_rate_hz) {
    return nullptr;
  }
  const int divisor = std::gcd(source_rate_hz, target_rate_hz);
  const std::size_t phases = static_cast<std::size_t>(target_rate_hz / divisor);
  const std::size_t step = static_cast<std::size_t>(source_rate_hz / divisor);
  if (phases > kMaximumPhases) {
    return nullptr;
  }

  const double source = source_rate_hz;
  const double target = target_rate_hz;
  const double narrow = std::min(source, target);
  std::size_t taps = kBaseTaps;
  if (target < source) {
    taps = static_cast<std::size_t>(std::ceil(kBaseTaps * source / target));
  }
  taps = std::min(kMaximumTaps, (taps + 3) / 4 * 4);

  const double transition_hz =
      (kStopbandDecibels - 7.95) / (14.36 * static_cast<double>(taps - 1)) *
      source;
  const double stop_hz = std::max(narrow * 0.5, narrow - kAudibleHz);
  const double cutoff_hz =
      std::min(source * 0.5, stop_hz - transition_hz * 0.5);
  if (cutoff_hz <= 0.0) {
    return nullptr;
  }
  const double cutoff = cutoff_hz / source;
  const double beta = 0.1102 * (kStopbandDecibels - 8.7);
  const double window_scale = 1.0 / BesselI0(beta);
  const double centre = static_cast<double>(taps / 2 - 1);
  const double reach = static_cast<double>(taps) * 0.5;

  std::unique_ptr<PcmRateConverter> converter(new (std::nothrow)
                                                  PcmRateConverter());
  if (converter == nullptr) {
    return nullptr;
  }
  converter->source_rate_hz_ = source_rate_hz;
  converter->target_rate_hz_ = target_rate_hz;
  converter->channels_ = channels;
  converter->phases_ = phases;
  converter->step_ = step;
  converter->taps_ = taps;
  converter->coefficients_.resize(phases * taps);
  for (std::size_t phase = 0; phase < phases; ++phase) {
    const double fraction =
        static_cast<double>(phase) / static_cast<double>(phases);
    double sum = 0.0;
    std::vector<double> row(taps);
    for (std::size_t tap = 0; tap < taps; ++tap) {
      const double distance = static_cast<double>(tap) - centre - fraction;
      const double position = distance / reach;
      double window = 0.0;
      if (std::fabs(position) < 1.0) {
        window = BesselI0(beta * std::sqrt(1.0 - position * position)) *
                 window_scale;
      }
      row[tap] = 2.0 * cutoff * Sinc(2.0 * cutoff * distance) * window;
      sum += row[tap];
    }
    for (std::size_t tap = 0; tap < taps; ++tap) {
      converter->coefficients_[phase * taps + tap] =
          static_cast<float>(row[tap] / sum);
    }
  }
  converter->window_.resize(channels);
  converter->Reset();
  return converter;
}

void PcmRateConverter::Reset() {
  phase_ = 0;
  index_ = 0;
  held_frames_ = taps_ - 1;
  for (auto& channel : window_) {
    channel.assign(held_frames_, 0.0F);
  }
}

std::size_t PcmRateConverter::MaxOutputFrames(std::size_t input_frames) const {
  return (input_frames * phases_ + phases_ - 1) / step_ + 1;
}

std::size_t PcmRateConverter::Convert(const std::int16_t* input,
                                      std::size_t input_frames,
                                      float* output) {
  const std::size_t channels = channels_;
  const std::size_t available = held_frames_ + input_frames;
  constexpr float kScale = 1.0F / 32768.0F;
  for (std::size_t channel = 0; channel < channels; ++channel) {
    std::vector<float>& window = window_[channel];
    if (window.size() < available) window.resize(available);
    float* destination = window.data() + held_frames_;
    const std::int16_t* source = input + channel;
    for (std::size_t frame = 0; frame < input_frames; ++frame) {
      destination[frame] =
          static_cast<float>(source[frame * channels]) * kScale;
    }
  }

  std::size_t produced = 0;
  std::size_t index = index_;
  std::size_t phase = phase_;
  while (index + taps_ <= available) {
    const float* row = coefficients_.data() + phase * taps_;
    for (std::size_t channel = 0; channel < channels; ++channel) {
      output[produced * channels + channel] =
          Dot(row, window_[channel].data() + index, taps_);
    }
    ++produced;
    phase += step_;
    index += phase / phases_;
    phase %= phases_;
  }
  phase_ = phase;

  const std::size_t consumed = std::min(index, available);
  index_ = index - consumed;
  held_frames_ = available - consumed;
  for (std::size_t channel = 0; channel < channels; ++channel) {
    float* window = window_[channel].data();
    std::memmove(window, window + consumed, held_frames_ * sizeof(float));
  }
  return produced;
}

}  // namespace blox7::audio
