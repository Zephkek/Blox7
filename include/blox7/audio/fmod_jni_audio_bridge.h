#ifndef BLOX7_AUDIO_FMOD_JNI_AUDIO_BRIDGE_H_
#define BLOX7_AUDIO_FMOD_JNI_AUDIO_BRIDGE_H_

#include "blox7/status.h"

namespace jnivm {
class VM;
}  // namespace jnivm

namespace blox7::audio {

Status InstallFmodJniAudioBridge(jnivm::VM* vm);

Status ShutdownFmodJniAudioBridge(jnivm::VM* vm);

}  // namespace blox7::audio

#endif  // BLOX7_AUDIO_FMOD_JNI_AUDIO_BRIDGE_H_
