#ifndef BLOX7_AUDIO_PCM_RATE_CONVERTER_H_
#define BLOX7_AUDIO_PCM_RATE_CONVERTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace blox7::audio {

class PcmRateConverter final {
 public:
  static std::unique_ptr<PcmRateConverter> Create(int source_rate_hz,
                                                  int target_rate_hz,
                                                  std::uint8_t channels);

  int source_rate_hz() const { return source_rate_hz_; }
  int target_rate_hz() const { return target_rate_hz_; }
  std::uint8_t channels() const { return channels_; }

  std::size_t MaxOutputFrames(std::size_t input_frames) const;
  std::size_t Convert(const std::int16_t* input, std::size_t input_frames,
                      float* output);
  void Reset();

 private:
  PcmRateConverter() = default;

  int source_rate_hz_ = 0;
  int target_rate_hz_ = 0;
  std::uint8_t channels_ = 0;
  std::size_t phases_ = 0;
  std::size_t step_ = 0;
  std::size_t taps_ = 0;
  std::size_t phase_ = 0;
  std::size_t index_ = 0;
  std::size_t held_frames_ = 0;
  std::vector<float> coefficients_;
  std::vector<std::vector<float>> window_;
};

}  // namespace blox7::audio

#endif  // BLOX7_AUDIO_PCM_RATE_CONVERTER_H_
