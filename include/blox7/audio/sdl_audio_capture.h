#ifndef BLOX7_AUDIO_SDL_AUDIO_CAPTURE_H_
#define BLOX7_AUDIO_SDL_AUDIO_CAPTURE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "blox7/audio/audio_sink.h"

namespace blox7::audio {

struct SdlRecordingDevice {
  std::uint32_t id = 0;
  std::string name;
};

Status ListSdlRecordingDevices(std::vector<SdlRecordingDevice>* devices);

Status ResolveSdlRecordingDevice(
    std::string_view requested,
    const std::vector<SdlRecordingDevice>& available_devices,
    std::uint32_t* recording_device_id, std::string* resolved_name);

Status ConfigureSdlRecordingDevice(
    std::string_view requested,
    std::vector<SdlRecordingDevice>* available_devices,
    std::string* resolved_name);

Status GetConfiguredSdlRecordingDevice(std::uint32_t* recording_device_id,
                                       std::string* resolved_name);

Status SwitchSdlRecordingDevice(std::uint32_t recording_device_id,
                                std::string *resolved_name);

using AudioCaptureDataCallback = void (*)(void* context,
                                          std::size_t size_bytes);

class AudioCapture {
 public:
  virtual ~AudioCapture() = default;

  virtual const PcmSpec& output_spec() const = 0;
  virtual void* buffer_data() = 0;
  virtual std::size_t buffer_size_bytes() const = 0;
  virtual std::size_t frames_per_buffer() const = 0;
  virtual Status Start() = 0;
  virtual Status Stop() = 0;
  virtual void Shutdown() = 0;
};

struct SdlAudioCaptureOptions {
  PcmSpec output_spec;

  std::uint32_t recording_device_id = 0;
  std::uint32_t frame_duration_ms = 10;
  AudioCaptureDataCallback data_callback = nullptr;
  void* data_context = nullptr;
};

Status CreateSdlAudioCapture(const SdlAudioCaptureOptions& options,
                             std::unique_ptr<AudioCapture>* capture);

Status PrepareSdlAudioCaptureSubsystemShutdown();

}  // namespace blox7::audio

#endif  // BLOX7_AUDIO_SDL_AUDIO_CAPTURE_H_
