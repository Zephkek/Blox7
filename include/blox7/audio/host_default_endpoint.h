#ifndef BLOX7_AUDIO_HOST_DEFAULT_ENDPOINT_H_
#define BLOX7_AUDIO_HOST_DEFAULT_ENDPOINT_H_

#include <string>

namespace blox7::audio {

bool HostDefaultAudioEndpoint(bool recording, std::string* name,
                              int* mix_rate_hz);

}  // namespace blox7::audio

#endif  // BLOX7_AUDIO_HOST_DEFAULT_ENDPOINT_H_
