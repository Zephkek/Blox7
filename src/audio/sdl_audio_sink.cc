#include "blox7/audio/sdl_audio_sink.h"

#include "blox7/audio/host_default_endpoint.h"
#include "blox7/audio/pcm_rate_converter.h"
#include "blox7/audio/sdl_audio_capture.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if !SDL_VERSION_ATLEAST(3, 4, 0)
#error "Blox7 borrowed audio buffers require SDL 3.4 or newer"
#endif

namespace blox7::audio {
namespace {

class SdlAudioSink;

struct SdlAudioSubsystemState {
  std::mutex mutex;
  std::vector<SdlAudioSink*> live_sinks;
  std::uint32_t configured_playback_device_id = 0;
  std::string configured_playback_device_name = "default";
  bool initialized = false;
  std::mutex held_mutex;
  SDL_AudioDeviceID held_device = 0;
  SDL_AudioStream* held_stream = nullptr;
  std::uint32_t held_playback_device_id = 0;
  std::atomic<bool> preparing{false};
  bool discarding = false;
};

constexpr int kEngineSampleRateHz = 48000;
constexpr std::chrono::milliseconds kDiscardPeriod{10};

SdlAudioSubsystemState& AudioSubsystemState() {
  static SdlAudioSubsystemState state;
  return state;
}

Status SdlError(const char* operation) {
  std::string message(operation);
  message.append(": ");
  const char* detail = SDL_GetError();
  message.append(detail != nullptr && detail[0] != '\0' ? detail
                                                        : "unknown SDL error");
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

Status InvalidArgument(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

Status ResolveSdlAudioSinkDevice(std::uint32_t requested_device_id,
                                 std::uint32_t* resolved_device_id) {
  if (resolved_device_id == nullptr) {
    return InvalidArgument("resolved SDL playback device pointer is null");
  }
  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.initialized) {
    return FailedPrecondition(
        "SDL audio subsystem must be initialized by the process owner");
  }
  *resolved_device_id = requested_device_id == 0
                            ? state.configured_playback_device_id
                            : requested_device_id;
  return Status::Ok();
}

Status RegisterSdlAudioSink(SdlAudioSink* sink) {
  if (sink == nullptr) {
    return InvalidArgument("SDL audio sink registration is null");
  }
  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.initialized) {
    return FailedPrecondition(
        "SDL audio subsystem stopped while opening a sink");
  }
  if (std::find(state.live_sinks.begin(), state.live_sinks.end(), sink) !=
      state.live_sinks.end()) {
    return FailedPrecondition("SDL audio sink is already registered");
  }
  state.live_sinks.push_back(sink);
  return Status::Ok();
}

void ReleaseSdlAudioSink(SdlAudioSink* sink) {
  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto found =
      std::find(state.live_sinks.begin(), state.live_sinks.end(), sink);
  if (found != state.live_sinks.end()) {
    state.live_sinks.erase(found);
  }
}

SDL_AudioFormat ToSdlFormat(PcmSampleFormat format) {
  switch (format) {
    case PcmSampleFormat::kUnsigned8:
      return SDL_AUDIO_U8;
    case PcmSampleFormat::kSigned16LittleEndian:
      return SDL_AUDIO_S16LE;
    case PcmSampleFormat::kSigned32LittleEndian:
      return SDL_AUDIO_S32LE;
    case PcmSampleFormat::kFloat32LittleEndian:
      return SDL_AUDIO_F32LE;
  }
  return SDL_AUDIO_UNKNOWN;
}

SDL_AudioDeviceID ToSdlPlaybackDevice(std::uint32_t playback_device_id) {
  return playback_device_id == 0
             ? SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK
             : static_cast<SDL_AudioDeviceID>(playback_device_id);
}

SDL_AudioDeviceID OpenPlaybackDevice(std::uint32_t playback_device_id) {
  return SDL_OpenAudioDevice(ToSdlPlaybackDevice(playback_device_id), nullptr);
}

void HoldPlaybackDeviceOpen(std::uint32_t playback_device_id) {
  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.held_mutex);
  if (state.held_device != 0 &&
      state.held_playback_device_id == playback_device_id) {
    return;
  }
  SDL_AudioDeviceID held = OpenPlaybackDevice(playback_device_id);
  if (held != 0 && !SDL_PauseAudioDevice(held)) {
    SDL_CloseAudioDevice(held);
    held = 0;
  }
  if (state.held_device != 0) {
    SDL_CloseAudioDevice(state.held_device);
  }
  state.held_device = held;
  state.held_playback_device_id = playback_device_id;
  if (held == 0) {
    return;
  }
  SDL_AudioSpec opened{};
  SDL_AudioSpec engine{};
  engine.format = SDL_AUDIO_S16LE;
  engine.channels = 2;
  engine.freq = SDL_GetAudioDeviceFormat(held, &opened, nullptr) &&
                        opened.freq > 0
                    ? opened.freq
                    : kEngineSampleRateHz;
  if (state.held_stream == nullptr) {
    state.held_stream = SDL_CreateAudioStream(&engine, nullptr);
  } else {
    (void)SDL_SetAudioStreamFormat(state.held_stream, &engine, nullptr);
  }
  if (state.held_stream != nullptr) {
    (void)SDL_BindAudioStream(held, state.held_stream);
  }
}

void ReleaseHeldPlaybackDevice() {
  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.held_mutex);
  if (state.held_device != 0) {
    SDL_CloseAudioDevice(state.held_device);
  }
  if (state.held_stream != nullptr) {
    SDL_DestroyAudioStream(state.held_stream);
  }
  state.held_stream = nullptr;
  state.held_device = 0;
  state.held_playback_device_id = 0;
}

struct SdlReleaseContext {
  AudioBufferReleaseCallback callback = nullptr;
  void* callback_context = nullptr;
};

void SDLCALL OnSdlBufferReleased(void* userdata, const void* data, int size) {
  auto* release = static_cast<SdlReleaseContext*>(userdata);
  if (release == nullptr) {
    return;
  }
  if (release->callback != nullptr) {
    release->callback(release->callback_context, data,
                      size > 0 ? static_cast<std::size_t>(size) : 0);
  }
  delete release;
}

struct ConvertedBlock {
  SdlAudioSink* sink = nullptr;
  std::vector<float> samples;
  PcmBuffer source;
};

void SDLCALL OnConvertedBlockPlayed(void* userdata, const void*, int);

class SdlAudioSink final : public AudioSink {
 public:
  SdlAudioSink(PcmSpec spec, SDL_AudioStream* stream,
               SDL_AudioDeviceID logical_device_id,
               std::uint32_t physical_device_id, bool paused,
               bool (*data_needed_callback)(void*, const void**,
                                            std::size_t*),
               void* data_needed_context)
      : source_spec_(spec),
        stream_(stream),
        logical_device_id_(logical_device_id),
        physical_device_id_(physical_device_id),
        data_needed_callback_(data_needed_callback),
        data_needed_context_(data_needed_context),
        paused_(paused),
        detached_(logical_device_id == 0) {}

  ~SdlAudioSink() override { Shutdown(); }

  const PcmSpec& source_spec() const override { return source_spec_; }

  Status Enqueue(const PcmBuffer& buffer) override {
    const std::size_t frame_size = BytesPerFrame(source_spec_);
    if (buffer.data == nullptr || buffer.size_bytes == 0) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "PCM buffer must contain data");
    }
    if (frame_size == 0 || buffer.size_bytes % frame_size != 0) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "PCM buffer size must contain complete frames");
    }
    if (buffer.size_bytes >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "PCM buffer exceeds SDL's per-call byte limit");
    }

    SDL_AudioStream* stream = BeginCall();
    if (stream == nullptr) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "audio sink is shut down");
    }

    if (converting_.load(std::memory_order_acquire)) {
      const Status status = EnqueueConverted(stream, buffer);
      EndCall();
      return status;
    }

    bool queued = false;
    if (buffer.release_callback == nullptr) {
      queued = SDL_PutAudioStreamData(stream, buffer.data,
                                      static_cast<int>(buffer.size_bytes));
    } else {
      auto* release = new (std::nothrow)
          SdlReleaseContext{buffer.release_callback, buffer.release_context};
      if (release == nullptr) {
        EndCall();
        return Status::Error(StatusCode::kUnavailable,
                             "unable to allocate SDL buffer release context");
      }
      queued = SDL_PutAudioStreamDataNoCopy(stream, buffer.data,
                                            static_cast<int>(buffer.size_bytes),
                                            OnSdlBufferReleased, release);
      if (!queued) {
        delete release;
      }
    }
    const Status status = queued
                              ? Status::Ok()
                              : SdlError(buffer.release_callback == nullptr
                                             ? "SDL_PutAudioStreamData"
                                             : "SDL_PutAudioStreamDataNoCopy");
    EndCall();
    return status;
  }

  Status Pause() override {
    return WithStream("SDL_PauseAudioDevice", [this](SDL_AudioStream*) {
      paused_.store(true, std::memory_order_release);
      return logical_device_id_ == 0 ||
             SDL_PauseAudioDevice(logical_device_id_);
    });
  }

  Status Resume() override {
    return WithStream("SDL_ResumeAudioDevice", [this](SDL_AudioStream*) {
      paused_.store(false, std::memory_order_release);
      return logical_device_id_ == 0 ||
             SDL_ResumeAudioDevice(logical_device_id_);
    });
  }

  Status SetGain(float linear_gain) override {
    if (!std::isfinite(linear_gain) || linear_gain < 0.0F) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "audio gain must be finite and non-negative");
    }
    return WithStream("SDL_SetAudioStreamGain",
                      [this, linear_gain](SDL_AudioStream* stream) {
                        gain_.store(linear_gain, std::memory_order_release);
                        return SDL_SetAudioStreamGain(stream, linear_gain);
                      });
  }

  Status Flush() override {
    return WithStream("SDL_FlushAudioStream", [](SDL_AudioStream* stream) {
      return SDL_FlushAudioStream(stream);
    });
  }

  Status Clear() override {
    return WithStream("SDL_ClearAudioStream", [this](SDL_AudioStream* stream) {
      {
        std::lock_guard<std::mutex> lock(convert_mutex_);
        if (converter_ != nullptr) {
          converter_->Reset();
        }
      }
      return SDL_ClearAudioStream(stream);
    });
  }

  Status GetQueuedBytes(std::size_t* size_bytes) const override {
    if (size_bytes == nullptr) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "queued-byte output pointer is null");
    }
    *size_bytes = 0;
    SDL_AudioStream* stream = BeginCall();
    if (stream == nullptr) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "audio sink is shut down");
    }
    const int queued = SDL_GetAudioStreamQueued(stream);
    const Status status =
        queued >= 0 ? Status::Ok() : SdlError("SDL_GetAudioStreamQueued");
    if (queued >= 0) {
      *size_bytes = SourceBytes(static_cast<std::size_t>(queued));
    }
    EndCall();
    return status;
  }

  Status SwitchPlaybackDevice(std::uint32_t physical_device_id) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      shutdown_cv_.wait(lock, [this] { return !moving_ || shutting_down_; });
      if (shutting_down_ || stream_ == nullptr) {
        return FailedPrecondition("audio sink is shut down");
      }
      if (physical_device_id_ == physical_device_id &&
          logical_device_id_ != 0) {
        return Status::Ok();
      }
      moving_ = true;
    }
    SDL_AudioDeviceID replacement = OpenPlaybackDevice(physical_device_id);
    const Status status =
        replacement != 0
            ? MoveTo(&replacement, physical_device_id)
            : SdlError("SDL_OpenAudioDevice during playback migration");
    if (replacement != 0) {
      SDL_CloseAudioDevice(replacement);
    }
    FinishMove();
    return status;
  }

  bool detached() const {
    return detached_.load(std::memory_order_acquire);
  }

  void Detach() {
    std::uint32_t physical_device_id = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      shutdown_cv_.wait(lock, [this] { return !moving_ || shutting_down_; });
      if (shutting_down_ || stream_ == nullptr) {
        return;
      }
      moving_ = true;
      physical_device_id = physical_device_id_;
    }
    SDL_AudioDeviceID none = 0;
    (void)MoveTo(&none, physical_device_id);
    FinishMove();
  }

  bool Discard(std::chrono::milliseconds span) {
    SDL_AudioStream* stream = BeginCall();
    if (stream == nullptr) {
      return false;
    }
    const bool discarding = detached_.load(std::memory_order_acquire);
    if (discarding && !paused_.load(std::memory_order_acquire)) {
      const std::size_t frames = static_cast<std::size_t>(
          static_cast<std::int64_t>(source_spec_.sample_rate_hz) *
          span.count() / 1000);
      discarded_.resize(frames * BytesPerFrame(source_spec_));
      (void)SDL_GetAudioStreamData(stream, discarded_.data(),
                                   static_cast<int>(discarded_.size()));
    }
    EndCall();
    return discarding;
  }

  std::uint32_t playback_device_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return physical_device_id_;
  }

  void RefreshDeviceRate() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (shutting_down_ || stream_ == nullptr) {
      return;
    }
    switching_device_ = true;
    shutdown_cv_.wait(lock,
                      [this] { return active_calls_ == 0 || shutting_down_; });
    if (!shutting_down_ && stream_ != nullptr) {
      MatchDeviceRate();
    }
    switching_device_ = false;
    lock.unlock();
    shutdown_cv_.notify_all();
  }

  void Shutdown() override {
    SDL_AudioStream* stream = nullptr;
    SDL_AudioDeviceID logical_device_id = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (shutting_down_) {
        shutdown_cv_.wait(lock, [this] { return shutdown_complete_; });
        return;
      }
      shutting_down_ = true;
      shutdown_cv_.notify_all();
      shutdown_cv_.wait(lock, [this] {
        return active_calls_ == 0 && !switching_device_ && !moving_;
      });
      stream = std::exchange(stream_, nullptr);
      logical_device_id = std::exchange(logical_device_id_, 0);
    }

    if (stream != nullptr) {
      if (logical_device_id != 0) {
        SDL_CloseAudioDevice(logical_device_id);
      }
      SDL_DestroyAudioStream(stream);
      ReleaseSdlAudioSink(this);
    }
    {
      std::lock_guard<std::mutex> lock(spare_mutex_);
      for (ConvertedBlock* block : spare_blocks_) {
        delete block;
      }
      spare_blocks_.clear();
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      shutdown_complete_ = true;
    }
    shutdown_cv_.notify_all();
  }

  static void SDLCALL OnDataNeeded(void* userdata, SDL_AudioStream* stream,
                                   int additional_amount, int) {
    auto* sink = static_cast<SdlAudioSink*>(userdata);
    if (sink == nullptr || sink->data_needed_callback_ == nullptr ||
        additional_amount <= 0) {
      return;
    }
    constexpr std::size_t kMaximumBuffersPerCallback = 32;
    std::size_t supplied = 0;
    for (std::size_t index = 0;
         supplied < static_cast<std::size_t>(additional_amount) &&
         index < kMaximumBuffersPerCallback;
         ++index) {
      const void* data = nullptr;
      std::size_t size_bytes = 0;
      if (!sink->data_needed_callback_(sink->data_needed_context_, &data,
                                       &size_bytes) ||
          data == nullptr || size_bytes == 0 ||
          size_bytes >
              static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        break;
      }
      const std::size_t put = sink->PutPulled(stream, data, size_bytes);
      if (put == 0) {
        break;
      }
      supplied += put;
    }
  }

  void MatchDeviceRate() {
    if (!SDL_LockAudioStream(stream_)) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(convert_mutex_);
      int device_rate_hz = source_spec_.sample_rate_hz;
      SDL_AudioSpec device{};
      if (source_spec_.format == PcmSampleFormat::kSigned16LittleEndian &&
          SDL_GetAudioStreamFormat(stream_, nullptr, &device) &&
          device.freq > 0) {
        device_rate_hz = device.freq;
      }
      const int current_rate_hz = converter_ != nullptr
                                      ? converter_->target_rate_hz()
                                      : source_spec_.sample_rate_hz;
      if (current_rate_hz != device_rate_hz) {
        std::unique_ptr<PcmRateConverter> converter =
            PcmRateConverter::Create(source_spec_.sample_rate_hz,
                                     device_rate_hz, source_spec_.channels);
        SDL_AudioSpec input{};
        input.format = converter != nullptr ? SDL_AUDIO_F32LE
                                            : ToSdlFormat(source_spec_.format);
        input.channels = source_spec_.channels;
        input.freq = converter != nullptr ? device_rate_hz
                                          : source_spec_.sample_rate_hz;
        if (SDL_SetAudioStreamFormat(stream_, &input, nullptr)) {
          converter_ = std::move(converter);
          converting_.store(converter_ != nullptr, std::memory_order_release);
        }
      }
    }
    SDL_UnlockAudioStream(stream_);
  }

  void Recycle(ConvertedBlock* block) {
    if (block->source.release_callback != nullptr) {
      block->source.release_callback(block->source.release_context,
                                     block->source.data,
                                     block->source.size_bytes);
    }
    block->source = {};
    GiveBack(block);
  }

 private:
  Status MoveTo(SDL_AudioDeviceID* replacement,
                std::uint32_t physical_device_id) {
    SDL_AudioSpec source{};
    source.format = ToSdlFormat(source_spec_.format);
    source.channels = source_spec_.channels;
    source.freq = source_spec_.sample_rate_hz;
    SDL_AudioStream* fresh =
        SDL_CreateAudioStream(&source, *replacement != 0 ? nullptr : &source);
    if (fresh == nullptr) {
      return SdlError("SDL_CreateAudioStream during playback migration");
    }
    if (data_needed_callback_ != nullptr &&
        !SDL_SetAudioStreamGetCallback(fresh, &SdlAudioSink::OnDataNeeded,
                                       this)) {
      const Status status = SdlError("SDL_SetAudioStreamGetCallback");
      SDL_DestroyAudioStream(fresh);
      return status;
    }
    (void)SDL_SetAudioStreamGain(fresh,
                                 gain_.load(std::memory_order_acquire));

    Status status = Status::Ok();
    SDL_AudioStream* retired = nullptr;
    SDL_AudioDeviceID retired_device = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      switching_device_ = true;
      shutdown_cv_.wait(
          lock, [this] { return active_calls_ == 0 || shutting_down_; });
      if (shutting_down_) {
        status = FailedPrecondition("audio sink is shut down");
      } else if (!SDL_LockAudioStream(fresh)) {
        status = SdlError("SDL_LockAudioStream during playback migration");
      } else {
        SDL_AudioStream* const locked = fresh;
        if (*replacement == 0 ||
            ((!paused_.load(std::memory_order_acquire) ||
              SDL_PauseAudioDevice(*replacement)) &&
             SDL_BindAudioStream(*replacement, fresh))) {
          if (data_needed_callback_ != nullptr) {
            (void)SDL_SetAudioStreamGetCallback(stream_, nullptr, nullptr);
          }
          {
            std::lock_guard<std::mutex> convert(convert_mutex_);
            converter_.reset();
            converting_.store(false, std::memory_order_release);
          }
          retired = std::exchange(stream_, std::exchange(fresh, nullptr));
          retired_device = std::exchange(logical_device_id_,
                                         std::exchange(*replacement, 0));
          physical_device_id_ = physical_device_id;
          detached_.store(logical_device_id_ == 0, std::memory_order_release);
          MatchDeviceRate();
        } else {
          status = SdlError("SDL_BindAudioStream during playback migration");
        }
        SDL_UnlockAudioStream(locked);
      }
      switching_device_ = false;
    }
    shutdown_cv_.notify_all();
    if (fresh != nullptr) {
      SDL_DestroyAudioStream(fresh);
    }
    if (retired != nullptr) {
      (void)SDL_ClearAudioStream(retired);
      if (retired_device != 0) {
        SDL_CloseAudioDevice(retired_device);
      }
      SDL_DestroyAudioStream(retired);
    }
    return status;
  }

  void FinishMove() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      moving_ = false;
    }
    shutdown_cv_.notify_all();
  }

  ConvertedBlock* TakeBlock() {
    {
      std::lock_guard<std::mutex> lock(spare_mutex_);
      if (!spare_blocks_.empty()) {
        ConvertedBlock* block = spare_blocks_.back();
        spare_blocks_.pop_back();
        return block;
      }
    }
    auto* block = new (std::nothrow) ConvertedBlock();
    if (block != nullptr) {
      block->sink = this;
    }
    return block;
  }

  Status EnqueueConverted(SDL_AudioStream* stream, const PcmBuffer& buffer) {
    ConvertedBlock* block = TakeBlock();
    if (block == nullptr) {
      return Status::Error(StatusCode::kUnavailable,
                           "unable to allocate a converted audio block");
    }
    const std::size_t channels = source_spec_.channels;
    const std::size_t frames = buffer.size_bytes / BytesPerFrame(source_spec_);
    std::size_t produced = 0;
    bool converted = false;
    {
      std::lock_guard<std::mutex> lock(convert_mutex_);
      if (converter_ != nullptr) {
        const std::size_t wanted =
            converter_->MaxOutputFrames(frames) * channels;
        if (block->samples.size() < wanted) {
          block->samples.resize(wanted);
        }
        produced = converter_->Convert(
            static_cast<const std::int16_t*>(buffer.data), frames,
            block->samples.data());
        converted = true;
      }
    }
    if (!converted) {
      GiveBack(block);
      if (!SDL_PutAudioStreamData(stream, buffer.data,
                                  static_cast<int>(buffer.size_bytes))) {
        return SdlError("SDL_PutAudioStreamData");
      }
      if (buffer.release_callback != nullptr) {
        buffer.release_callback(buffer.release_context, buffer.data,
                                buffer.size_bytes);
      }
      return Status::Ok();
    }
    block->source = buffer;
    if (produced == 0) {
      Recycle(block);
      return Status::Ok();
    }
    if (!SDL_PutAudioStreamDataNoCopy(
            stream, block->samples.data(),
            static_cast<int>(produced * channels * sizeof(float)),
            OnConvertedBlockPlayed, block)) {
      block->source = {};
      GiveBack(block);
      return SdlError("SDL_PutAudioStreamDataNoCopy");
    }
    return Status::Ok();
  }

  void GiveBack(ConvertedBlock* block) {
    std::lock_guard<std::mutex> lock(spare_mutex_);
    spare_blocks_.push_back(block);
  }

  std::size_t PutPulled(SDL_AudioStream* stream, const void* data,
                        std::size_t size_bytes) {
    if (!converting_.load(std::memory_order_acquire)) {
      return SDL_PutAudioStreamData(stream, data, static_cast<int>(size_bytes))
                 ? size_bytes
                 : 0;
    }
    std::size_t bytes = 0;
    {
      std::lock_guard<std::mutex> lock(convert_mutex_);
      if (converter_ == nullptr) {
        return SDL_PutAudioStreamData(stream, data,
                                      static_cast<int>(size_bytes))
                   ? size_bytes
                   : 0;
      }
      const std::size_t channels = source_spec_.channels;
      const std::size_t frames = size_bytes / BytesPerFrame(source_spec_);
      const std::size_t wanted = converter_->MaxOutputFrames(frames) * channels;
      if (pulled_.size() < wanted) {
        pulled_.resize(wanted);
      }
      const std::size_t produced = converter_->Convert(
          static_cast<const std::int16_t*>(data), frames, pulled_.data());
      bytes = produced * channels * sizeof(float);
    }
    if (bytes != 0 &&
        !SDL_PutAudioStreamData(stream, pulled_.data(),
                                static_cast<int>(bytes))) {
      return 0;
    }
    return std::max<std::size_t>(bytes, 1);
  }

  std::size_t SourceBytes(std::size_t queued_bytes) const {
    std::lock_guard<std::mutex> lock(convert_mutex_);
    if (converter_ == nullptr) {
      return queued_bytes;
    }
    const std::uint64_t frames =
        queued_bytes / (source_spec_.channels * sizeof(float));
    const std::uint64_t source_frames =
        frames * static_cast<std::uint64_t>(converter_->source_rate_hz()) /
        static_cast<std::uint64_t>(converter_->target_rate_hz());
    return static_cast<std::size_t>(source_frames) *
           BytesPerFrame(source_spec_);
  }

  template <typename Operation>
  Status WithStream(const char* name, Operation operation) {
    SDL_AudioStream* stream = BeginCall();
    if (stream == nullptr) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "audio sink is shut down");
    }
    const bool succeeded = operation(stream);
    const Status status = succeeded ? Status::Ok() : SdlError(name);
    EndCall();
    return status;
  }

  SDL_AudioStream* BeginCall() const {
    std::unique_lock<std::mutex> lock(mutex_);
    shutdown_cv_.wait(lock,
                      [this] { return !switching_device_ || shutting_down_; });
    if (shutting_down_ || stream_ == nullptr) {
      return nullptr;
    }
    ++active_calls_;
    return stream_;
  }

  void EndCall() const {
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --active_calls_;
      if (__builtin_expect(shutting_down_ || switching_device_, 0)) {
        notify = true;
      }
    }
    if (notify) {
      shutdown_cv_.notify_all();
    }
  }

  const PcmSpec source_spec_;
  mutable std::mutex mutex_;
  mutable std::condition_variable shutdown_cv_;
  mutable std::size_t active_calls_ = 0;
  SDL_AudioStream* stream_ = nullptr;
  SDL_AudioDeviceID logical_device_id_ = 0;
  std::uint32_t physical_device_id_ = 0;
  bool (*data_needed_callback_)(void*, const void**, std::size_t*) = nullptr;
  void* data_needed_context_ = nullptr;
  mutable bool switching_device_ = false;
  bool moving_ = false;
  bool shutting_down_ = false;
  bool shutdown_complete_ = false;
  std::mutex spare_mutex_;
  mutable std::mutex convert_mutex_;
  std::atomic<bool> converting_{false};
  std::unique_ptr<PcmRateConverter> converter_;
  std::vector<ConvertedBlock*> spare_blocks_;
  std::vector<float> pulled_;
  std::atomic<bool> paused_{false};
  std::atomic<float> gain_{1.0F};
  std::atomic<bool> detached_{false};
  std::vector<std::uint8_t> discarded_;
};

void SDLCALL OnConvertedBlockPlayed(void* userdata, const void*, int) {
  auto* block = static_cast<ConvertedBlock*>(userdata);
  if (block != nullptr && block->sink != nullptr) {
    block->sink->Recycle(block);
  }
}

struct SdlAudioDeviceWatch {
  std::mutex mutex;
  std::condition_variable wake;
  std::thread thread;
  bool running = false;
  bool stop = false;
  bool changed = false;
  std::mutex listener_mutex;
  SdlAudioDeviceListener listener = nullptr;
  void* listener_context = nullptr;
};

constexpr std::chrono::milliseconds kDeviceSettleDelay{50};
constexpr std::chrono::milliseconds kDeviceRetryDelay{250};
constexpr int kDeviceRetries = 5;

SdlAudioDeviceWatch& DeviceWatch() {
  static SdlAudioDeviceWatch watch;
  return watch;
}

bool SDLCALL OnSdlAudioDeviceEvent(void*, SDL_Event* event) {
  if (event->type - SDL_EVENT_AUDIO_DEVICE_ADDED >
      SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED - SDL_EVENT_AUDIO_DEVICE_ADDED) {
    return true;
  }
  SdlAudioDeviceWatch& watch = DeviceWatch();
  {
    std::lock_guard<std::mutex> lock(watch.mutex);
    watch.changed = true;
  }
  watch.wake.notify_one();
  return true;
}

void DiscardWithoutDevice() {
  SdlAudioSubsystemState& state = AudioSubsystemState();
  auto next = std::chrono::steady_clock::now();
  for (;;) {
    next += kDiscardPeriod;
    std::this_thread::sleep_until(next);
    std::lock_guard<std::mutex> lock(state.mutex);
    bool discarding = false;
    for (SdlAudioSink* sink : state.live_sinks) {
      discarding = sink->Discard(kDiscardPeriod) || discarding;
    }
    if (!state.initialized || !discarding) {
      state.discarding = false;
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - next > kDiscardPeriod * 10) {
      next = now;
    }
  }
}

void StartDiscardingLocked(SdlAudioSubsystemState* state) {
  if (state->initialized && !std::exchange(state->discarding, true)) {
    std::thread(&DiscardWithoutDevice).detach();
  }
}

bool RefreshPlayback() {
  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.initialized) {
    return true;
  }
  bool attached = true;
  const bool default_is_safe = SdlDefaultOpenIsSafe(false);
  for (SdlAudioSink* sink : state.live_sinks) {
    if (sink->detached()) {
      if (default_is_safe && sink->SwitchPlaybackDevice(0).ok()) {
        state.configured_playback_device_id = 0;
        state.configured_playback_device_name = "default";
        std::fprintf(stderr, "  [blox7][audio] output device attached\n");
      } else {
        attached = false;
        StartDiscardingLocked(&state);
      }
    }
    sink->RefreshDeviceRate();
  }
  if (state.configured_playback_device_id != 0 || default_is_safe) {
    HoldPlaybackDeviceOpen(state.configured_playback_device_id);
  }
  return attached;
}

void WatchAudioDevices() {
  SdlAudioDeviceWatch& watch = DeviceWatch();
  std::unique_lock<std::mutex> lock(watch.mutex);
  int retry = 0;
  for (;;) {
    const auto woken = [&watch] { return watch.stop || watch.changed; };
    if (retry == 0) {
      watch.wake.wait(lock, woken);
    } else {
      watch.wake.wait_for(lock, kDeviceRetryDelay * (1 << (retry - 1)), woken);
    }
    if (watch.stop ||
        watch.wake.wait_for(lock, kDeviceSettleDelay,
                            [&watch] { return watch.stop; })) {
      return;
    }
    if (std::exchange(watch.changed, false)) {
      retry = 0;
    }
    lock.unlock();
    const bool last_try = retry == kDeviceRetries;
    bool settled = true;
    {
      std::lock_guard<std::mutex> listener_lock(watch.listener_mutex);
      if (watch.listener != nullptr) {
        settled = watch.listener(watch.listener_context, last_try);
      }
    }
    settled = RefreshPlayback() && settled;
    lock.lock();
    retry = settled || last_try ? 0 : retry + 1;
  }
}

void StartDiscarding() {
  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  StartDiscardingLocked(&state);
}

void StartDeviceWatch() {
  SdlAudioDeviceWatch& watch = DeviceWatch();
  {
    std::lock_guard<std::mutex> lock(watch.mutex);
    if (watch.running) {
      return;
    }
    watch.stop = false;
    watch.changed = false;
    watch.thread = std::thread(&WatchAudioDevices);
    watch.running = true;
  }
  SDL_AddEventWatch(&OnSdlAudioDeviceEvent, nullptr);
}

void StopDeviceWatch() {
  SdlAudioDeviceWatch& watch = DeviceWatch();
  SDL_RemoveEventWatch(&OnSdlAudioDeviceEvent, nullptr);
  std::thread thread;
  {
    std::lock_guard<std::mutex> lock(watch.mutex);
    if (!watch.running) {
      return;
    }
    watch.stop = true;
    watch.running = false;
    thread = std::move(watch.thread);
  }
  watch.wake.notify_all();
  if (thread.joinable()) {
    thread.join();
  }
}

}  // namespace

Status InitializeSdlAudioSubsystem() {
  if (!SDL_IsMainThread()) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "SDL audio initialization requires the main thread");
  }
  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.initialized) {
    return Status::Ok();
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return SdlError("SDL_InitSubSystem(SDL_INIT_AUDIO)");
  }
  state.initialized = true;
  StartDeviceWatch();
  return Status::Ok();
}

void SetSdlAudioDeviceListener(SdlAudioDeviceListener listener,
                               void* context) {
  SdlAudioDeviceWatch& watch = DeviceWatch();
  std::lock_guard<std::mutex> lock(watch.listener_mutex);
  watch.listener = listener;
  watch.listener_context = context;
}

void RequestSdlAudioDeviceRefresh() {
  SdlAudioDeviceWatch& watch = DeviceWatch();
  {
    std::lock_guard<std::mutex> lock(watch.mutex);
    watch.changed = true;
  }
  watch.wake.notify_one();
}

std::string SdlDefaultAudioDeviceName(bool recording) {
  const char* name = SDL_GetAudioDeviceName(
      recording ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
                : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
  return name != nullptr ? name : "";
}

bool SdlAudioDeviceGone(std::uint32_t device_id) {
  return SDL_GetAudioDeviceName(static_cast<SDL_AudioDeviceID>(device_id)) ==
         nullptr;
}

bool SdlDefaultOpenIsSafe(bool recording) {
  std::string host_default;
  int host_rate = 0;
  if (!HostDefaultAudioEndpoint(recording, &host_default, &host_rate) ||
      host_default.empty()) {
    return true;
  }
  const std::string sdl_default = SdlDefaultAudioDeviceName(recording);
  if (host_default == sdl_default) {
    return true;
  }
  int count = 0;
  SDL_AudioDeviceID* ids = recording ? SDL_GetAudioRecordingDevices(&count)
                                     : SDL_GetAudioPlaybackDevices(&count);
  bool live = false;
  for (int index = 0; ids != nullptr && index < count && !live; ++index) {
    const char* name = SDL_GetAudioDeviceName(ids[index]);
    live = name != nullptr && sdl_default == name;
  }
  SDL_free(ids);
  return !live;
}

void PrepareSdlPlaybackDevice() {
  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::uint32_t playback_device_id = 0;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.initialized || state.preparing.exchange(true)) {
      return;
    }
    playback_device_id = state.configured_playback_device_id;
  }
  std::thread([playback_device_id] {
    HoldPlaybackDeviceOpen(playback_device_id);
    AudioSubsystemState().preparing.store(false);
  }).detach();
}

Status ListSdlPlaybackDevices(std::vector<SdlPlaybackDevice>* devices) {
  if (devices == nullptr) {
    return InvalidArgument("SDL playback device output pointer is null");
  }
  devices->clear();
  {
    SdlAudioSubsystemState& state = AudioSubsystemState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.initialized) {
      return FailedPrecondition(
          "SDL audio subsystem must be initialized before device enumeration");
    }
  }

  int count = 0;
  SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
  if (ids == nullptr) {
    return SdlError("SDL_GetAudioPlaybackDevices");
  }
  devices->reserve(count > 0 ? static_cast<std::size_t>(count) : 0U);
  for (int index = 0; index < count; ++index) {
    const char* name = SDL_GetAudioDeviceName(ids[index]);
    if (name == nullptr || name[0] == '\0') {
      continue;
    }
    devices->push_back(
        {static_cast<std::uint32_t>(ids[index]), std::string(name)});
  }
  SDL_free(ids);
  return Status::Ok();
}

Status ResolveSdlPlaybackDevice(
    std::string_view requested,
    const std::vector<SdlPlaybackDevice>& available_devices,
    std::uint32_t* playback_device_id, std::string* resolved_name) {
  if (playback_device_id == nullptr || resolved_name == nullptr) {
    return InvalidArgument("SDL playback selection output pointer is null");
  }
  *playback_device_id = 0;
  resolved_name->clear();
  if (requested.empty() || requested.size() > 512U) {
    return InvalidArgument(
        "audio output device must be `default` or a bounded device name");
  }
  for (const unsigned char character : requested) {
    if (character < 0x20U || character == 0x7fU) {
      return InvalidArgument(
          "audio output device name must not contain control bytes");
    }
  }
  if (requested == "default") {
    *resolved_name = "default";
    return Status::Ok();
  }

  const SdlPlaybackDevice* match = nullptr;
  for (const SdlPlaybackDevice& device : available_devices) {
    if (device.name != requested) {
      continue;
    }
    if (match != nullptr) {
      return InvalidArgument(
          "audio output device name is ambiguous; use `default`");
    }
    match = &device;
  }
  if (match == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "configured audio output device is unavailable: " +
                             std::string(requested));
  }
  *playback_device_id = match->id;
  *resolved_name = match->name;
  return Status::Ok();
}

Status ConfigureSdlPlaybackDevice(
    std::string_view requested,
    std::vector<SdlPlaybackDevice>* available_devices,
    std::string* resolved_name) {
  if (available_devices == nullptr || resolved_name == nullptr) {
    return InvalidArgument("SDL playback configuration output pointer is null");
  }
  const std::string requested_copy(requested);
  Status status = ListSdlPlaybackDevices(available_devices);
  if (!status.ok() && requested_copy != "default") {
    return status;
  }
  if (!status.ok()) {
    available_devices->clear();
  }
  std::uint32_t playback_device_id = 0;
  status = ResolveSdlPlaybackDevice(requested_copy, *available_devices,
                                    &playback_device_id, resolved_name);
  std::string wanted_name = *resolved_name;
  if (status.code() == StatusCode::kUnavailable) {
    wanted_name = requested_copy;
    *resolved_name = "default";
  } else if (!status.ok()) {
    return status;
  }

  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.initialized) {
    return FailedPrecondition(
        "SDL audio subsystem stopped during playback configuration");
  }
  if (!state.live_sinks.empty()) {
    return FailedPrecondition(
        "SDL playback device must be configured before opening a sink");
  }
  state.configured_playback_device_id = playback_device_id;
  state.configured_playback_device_name = wanted_name;
  return Status::Ok();
}

Status GetConfiguredSdlPlaybackDevice(std::uint32_t* playback_device_id,
                                      std::string* resolved_name) {
  if (playback_device_id == nullptr || resolved_name == nullptr) {
    return InvalidArgument(
        "configured SDL playback device output pointer is null");
  }
  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.initialized) {
    return FailedPrecondition(
        "SDL audio subsystem must be initialized before reading its route");
  }
  *playback_device_id = state.configured_playback_device_id;
  *resolved_name = state.configured_playback_device_name;
  return Status::Ok();
}

Status SwitchSdlPlaybackDevice(std::uint32_t playback_device_id,
                               std::string* resolved_name) {
  if (resolved_name == nullptr) {
    return InvalidArgument("SDL playback switch output pointer is null");
  }
  resolved_name->clear();

  std::string target_name = "default";
  if (playback_device_id != 0) {
    std::vector<SdlPlaybackDevice> devices;
    Status status = ListSdlPlaybackDevices(&devices);
    if (!status.ok()) {
      return status;
    }
    const auto found =
        std::find_if(devices.begin(), devices.end(),
                     [playback_device_id](const SdlPlaybackDevice& device) {
                       return device.id == playback_device_id;
                     });
    if (found == devices.end()) {
      return Status::Error(StatusCode::kUnavailable,
                           "selected SDL playback device is unavailable");
    }
    target_name = found->name;
  }

  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.initialized) {
    return FailedPrecondition(
        "SDL audio subsystem stopped during playback migration");
  }
  struct MigratedSink {
    SdlAudioSink* sink = nullptr;
    std::uint32_t previous_device_id = 0;
  };
  std::vector<MigratedSink> migrated;
  migrated.reserve(state.live_sinks.size());
  for (SdlAudioSink* sink : state.live_sinks) {
    const std::uint32_t previous_device_id = sink->playback_device_id();
    Status status = sink->SwitchPlaybackDevice(playback_device_id);
    if (!status.ok()) {
      for (auto rollback = migrated.rbegin(); rollback != migrated.rend();
           ++rollback) {
        (void)rollback->sink->SwitchPlaybackDevice(
            rollback->previous_device_id);
      }
      for (SdlAudioSink* live : state.live_sinks) {
        if (live->detached()) {
          StartDiscardingLocked(&state);
          break;
        }
      }
      return status;
    }
    migrated.push_back({sink, previous_device_id});
  }

  state.configured_playback_device_id = playback_device_id;
  state.configured_playback_device_name = target_name;
  *resolved_name = target_name;
  HoldPlaybackDeviceOpen(playback_device_id);
  return Status::Ok();
}

int SdlPlaybackOpenRate() {
  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.held_mutex);
  SDL_AudioSpec opened{};
  return state.held_device != 0 &&
                 SDL_GetAudioDeviceFormat(state.held_device, &opened, nullptr)
             ? opened.freq
             : 0;
}

Status ReopenSdlPlaybackDevice() {
  SdlAudioSubsystemState& state = AudioSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.initialized) {
    return FailedPrecondition(
        "SDL audio subsystem stopped during playback reopen");
  }
  ReleaseHeldPlaybackDevice();
  for (SdlAudioSink* sink : state.live_sinks) {
    sink->Detach();
  }
  Status status = Status::Ok();
  for (SdlAudioSink* sink : state.live_sinks) {
    const Status attached =
        sink->SwitchPlaybackDevice(state.configured_playback_device_id);
    if (!attached.ok()) {
      status = attached;
      StartDiscardingLocked(&state);
    }
  }
  HoldPlaybackDeviceOpen(state.configured_playback_device_id);
  return status;
}

Status ShutdownSdlAudioSubsystem() {
  if (!SDL_IsMainThread()) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "SDL audio shutdown requires the main thread");
  }
  SdlAudioSubsystemState& state = AudioSubsystemState();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.initialized) {
      return Status::Ok();
    }
    if (!state.live_sinks.empty()) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "SDL audio shutdown requires every sink to close");
    }
  }
  StopDeviceWatch();
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      if (!state.discarding) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.initialized) {
    return Status::Ok();
  }
  if (!state.live_sinks.empty()) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "SDL audio shutdown requires every sink to close");
  }
  Status capture_status = PrepareSdlAudioCaptureSubsystemShutdown();
  if (!capture_status.ok()) {
    return capture_status;
  }
  while (state.preparing.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ReleaseHeldPlaybackDevice();
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  state.configured_playback_device_id = 0;
  state.configured_playback_device_name = "default";
  state.initialized = false;
  return Status::Ok();
}

Status CreateSdlAudioSink(const SdlAudioSinkOptions& options,
                          std::unique_ptr<AudioSink>* sink) {
  if (sink == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "audio sink output pointer is null");
  }
  sink->reset();
  Status status = ValidatePcmSpec(options.source_spec);
  if (!status.ok()) {
    return status;
  }

  std::uint32_t playback_device_id = 0;
  status = ResolveSdlAudioSinkDevice(options.playback_device_id,
                                     &playback_device_id);
  if (!status.ok()) {
    return status;
  }

  SDL_AudioSpec source{};
  source.format = ToSdlFormat(options.source_spec.format);
  source.channels = options.source_spec.channels;
  source.freq = options.source_spec.sample_rate_hz;
  if (options.playback_device_id == 0) {
    HoldPlaybackDeviceOpen(playback_device_id);
  }
  const SDL_AudioDeviceID logical_device =
      OpenPlaybackDevice(playback_device_id);
  if (logical_device != 0 && options.start_paused &&
      !SDL_PauseAudioDevice(logical_device)) {
    status = SdlError("SDL_PauseAudioDevice");
    SDL_CloseAudioDevice(logical_device);
    return status;
  }

  SDL_AudioStream* stream = SDL_CreateAudioStream(
      &source, logical_device != 0 ? nullptr : &source);
  if (stream == nullptr) {
    status = SdlError("SDL_CreateAudioStream");
    if (logical_device != 0) {
      SDL_CloseAudioDevice(logical_device);
    }
    return status;
  }
  if (logical_device != 0 && !SDL_BindAudioStream(logical_device, stream)) {
    status = SdlError("SDL_BindAudioStream");
    SDL_DestroyAudioStream(stream);
    SDL_CloseAudioDevice(logical_device);
    return status;
  }

  auto* implementation = new (std::nothrow) SdlAudioSink(
      options.source_spec, stream, logical_device, playback_device_id,
      options.start_paused, options.data_needed_callback,
      options.data_needed_context);
  if (implementation == nullptr) {
    if (logical_device != 0) {
      SDL_CloseAudioDevice(logical_device);
    }
    SDL_DestroyAudioStream(stream);
    return Status::Error(StatusCode::kUnavailable,
                         "unable to allocate SDL audio sink");
  }
  implementation->MatchDeviceRate();
  if (options.data_needed_callback != nullptr &&
      !SDL_SetAudioStreamGetCallback(stream, &SdlAudioSink::OnDataNeeded,
                                     implementation)) {
    status = SdlError("SDL_SetAudioStreamGetCallback");
    delete implementation;
    return status;
  }
  status = RegisterSdlAudioSink(implementation);
  if (!status.ok()) {
    delete implementation;
    return status;
  }
  if (logical_device == 0) {
    std::fprintf(stderr,
                 "  [blox7][audio] no output device, waiting for one\n");
    StartDiscarding();
  }
  sink->reset(implementation);
  return Status::Ok();
}

}  // namespace blox7::audio
