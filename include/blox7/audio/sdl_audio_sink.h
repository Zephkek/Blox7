#ifndef BLOX7_AUDIO_SDL_AUDIO_SINK_H_
#define BLOX7_AUDIO_SDL_AUDIO_SINK_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "blox7/audio/audio_sink.h"

namespace blox7::audio {

Status InitializeSdlAudioSubsystem();
Status ShutdownSdlAudioSubsystem();

struct SdlPlaybackDevice {
  std::uint32_t id = 0;
  std::string name;
};

Status ListSdlPlaybackDevices(std::vector<SdlPlaybackDevice>* devices);

Status ResolveSdlPlaybackDevice(
    std::string_view requested,
    const std::vector<SdlPlaybackDevice>& available_devices,
    std::uint32_t* playback_device_id, std::string* resolved_name);

Status ConfigureSdlPlaybackDevice(
    std::string_view requested,
    std::vector<SdlPlaybackDevice>* available_devices,
    std::string* resolved_name);

Status GetConfiguredSdlPlaybackDevice(std::uint32_t* playback_device_id,
                                      std::string* resolved_name);

Status SwitchSdlPlaybackDevice(std::uint32_t playback_device_id,
                               std::string* resolved_name);

struct SdlAudioSinkOptions {
  PcmSpec source_spec;

  std::uint32_t playback_device_id = 0;
  bool start_paused = true;

  bool (*data_needed_callback)(void *context, const void **data,
                               std::size_t *size_bytes) = nullptr;
  void *data_needed_context = nullptr;
};

Status CreateSdlAudioSink(const SdlAudioSinkOptions& options,
                          std::unique_ptr<AudioSink>* sink);

}  // namespace blox7::audio

#endif  // BLOX7_AUDIO_SDL_AUDIO_SINK_H_
