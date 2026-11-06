#ifndef BLOX7_AUDIO_AUDIO_SINK_H_
#define BLOX7_AUDIO_AUDIO_SINK_H_

#include <cstddef>
#include <cstdint>

#include "blox7/status.h"

namespace blox7::audio {

enum class PcmSampleFormat {
  kUnsigned8,
  kSigned16LittleEndian,
  kSigned32LittleEndian,
  kFloat32LittleEndian,
};

struct PcmSpec {
  int sample_rate_hz = 48000;
  std::uint8_t channels = 2;
  PcmSampleFormat format = PcmSampleFormat::kSigned16LittleEndian;
};

using AudioBufferReleaseCallback = void (*)(void* context, const void* data,
                                            std::size_t size_bytes);

struct PcmBuffer {
  const void* data = nullptr;
  std::size_t size_bytes = 0;
  AudioBufferReleaseCallback release_callback = nullptr;
  void* release_context = nullptr;
};

Status ValidatePcmSpec(const PcmSpec& spec);
std::size_t BytesPerSample(PcmSampleFormat format);
std::size_t BytesPerFrame(const PcmSpec& spec);

class AudioSink {
 public:
  virtual ~AudioSink() = default;

  virtual const PcmSpec& source_spec() const = 0;
  virtual Status Enqueue(const PcmBuffer& buffer) = 0;
  virtual Status Pause() = 0;
  virtual Status Resume() = 0;
  virtual Status SetGain(float linear_gain) = 0;
  virtual Status Flush() = 0;
  virtual Status Clear() = 0;
  virtual Status GetQueuedBytes(std::size_t* size_bytes) const = 0;

  virtual void Shutdown() = 0;
};

}  // namespace blox7::audio

#endif  // BLOX7_AUDIO_AUDIO_SINK_H_
