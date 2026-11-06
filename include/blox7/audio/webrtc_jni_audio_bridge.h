#ifndef BLOX7_AUDIO_WEBRTC_JNI_AUDIO_BRIDGE_H_
#define BLOX7_AUDIO_WEBRTC_JNI_AUDIO_BRIDGE_H_

#include "blox7/status.h"

namespace jnivm {
class VM;
}

namespace blox7::audio {

Status InstallWebRtcJniAudioBridge(jnivm::VM* vm);
Status ShutdownWebRtcJniAudioBridge(jnivm::VM* vm);

}  // namespace blox7::audio

#endif  // BLOX7_AUDIO_WEBRTC_JNI_AUDIO_BRIDGE_H_
