#ifndef BLOX7_AUDIO_FMOD_JAVA_AUDIO_RUNTIME_H_
#define BLOX7_AUDIO_FMOD_JAVA_AUDIO_RUNTIME_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "blox7/audio/audio_sink.h"

namespace blox7::audio {

using FmodJavaAudioDeviceIdentity = const void*;

using FmodJavaAudioSinkFactory = Status (*)(
    void* factory_context, const PcmSpec& source_spec,
    std::unique_ptr<AudioSink>* sink);

struct FmodJavaAudioRuntimeOptions {
  FmodJavaAudioSinkFactory sink_factory = nullptr;
  void* sink_factory_context = nullptr;

  std::uint32_t max_devices = 8;
  std::uint32_t max_block_count = 256;
  std::size_t max_buffer_bytes_per_device = 64U * 1024U * 1024U;
};

struct FmodJavaAudioRuntimeStats {
  std::uint64_t init_attempts = 0;
  std::uint64_t initialized_devices = 0;
  std::uint64_t rejected_init_calls = 0;
  std::uint64_t close_calls = 0;
  std::uint64_t rejected_close_calls = 0;
  std::uint64_t write_attempts = 0;
  std::uint64_t submitted_buffers = 0;
  std::uint64_t submitted_bytes = 0;
  std::uint64_t consumed_buffers = 0;
  std::uint64_t consumed_bytes = 0;
  std::uint64_t discarded_buffers = 0;
  std::uint64_t discarded_bytes = 0;
  std::uint64_t rejected_write_calls = 0;
  std::size_t active_devices = 0;
  std::size_t pending_buffers = 0;
  std::size_t pending_bytes = 0;
};

class FmodJavaAudioRuntime final {
 public:
  explicit FmodJavaAudioRuntime(
      const FmodJavaAudioRuntimeOptions& options = {});
  ~FmodJavaAudioRuntime();

  FmodJavaAudioRuntime(const FmodJavaAudioRuntime&) = delete;
  FmodJavaAudioRuntime& operator=(const FmodJavaAudioRuntime&) = delete;

  Status Init(FmodJavaAudioDeviceIdentity device_identity, int channels,
              int sample_rate_hz, int block_size_frames, int block_count);
  Status Write(FmodJavaAudioDeviceIdentity device_identity,
               const std::uint8_t* bytes, std::size_t size_bytes);
  Status Close(FmodJavaAudioDeviceIdentity device_identity);

  FmodJavaAudioRuntimeStats GetStats() const;

  void Shutdown();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace blox7::audio

#endif  // BLOX7_AUDIO_FMOD_JAVA_AUDIO_RUNTIME_H_
