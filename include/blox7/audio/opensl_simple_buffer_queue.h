#ifndef BLOX7_AUDIO_OPENSL_SIMPLE_BUFFER_QUEUE_H_
#define BLOX7_AUDIO_OPENSL_SIMPLE_BUFFER_QUEUE_H_

#include <cstdint>
#include <memory>

#include "blox7/audio/audio_sink.h"
#include "blox7/audio/opensl_abi.h"

namespace blox7::audio {

enum class OpenSlBufferQueueEvent {
  kSubmitted,
  kConsumed,
  kDiscarded,
};

using OpenSlBufferQueueEventCallback = void (*)(
    void* context, OpenSlBufferQueueEvent event, std::size_t size_bytes);

struct OpenSlSimpleBufferQueueOptions {
  std::uint32_t max_buffers = 8;
  OpenSlBufferQueueEventCallback event_callback = nullptr;
  void* event_context = nullptr;
};

struct OpenSlSimpleBufferQueueStats {
  std::uint64_t submitted_buffers = 0;
  std::uint64_t consumed_buffers = 0;
  std::uint64_t discarded_buffers = 0;
  std::uint64_t consumed_bytes = 0;
  std::size_t pending_buffers = 0;
};

class OpenSlSimpleBufferQueueAdapter final {
 public:
  static Status Create(
      std::unique_ptr<AudioSink> sink,
      const OpenSlSimpleBufferQueueOptions& options,
      std::unique_ptr<OpenSlSimpleBufferQueueAdapter>* adapter);

  ~OpenSlSimpleBufferQueueAdapter();

  OpenSlSimpleBufferQueueAdapter(const OpenSlSimpleBufferQueueAdapter&) =
      delete;
  OpenSlSimpleBufferQueueAdapter& operator=(
      const OpenSlSimpleBufferQueueAdapter&) = delete;

  opensl_abi::AndroidSimpleBufferQueue interface() const;
  Status SetPlaying(bool playing);
  Status SetGain(float linear_gain);
  OpenSlSimpleBufferQueueStats GetStats() const;
  Status last_error() const;
  void Shutdown();

 private:
  struct State;

  explicit OpenSlSimpleBufferQueueAdapter(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;
};

Status OpenSlRecorderSupportStatus();

}  // namespace blox7::audio

#endif  // BLOX7_AUDIO_OPENSL_SIMPLE_BUFFER_QUEUE_H_
