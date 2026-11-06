#ifndef BLOX7_AUDIO_OPENSL_PLAYBACK_RUNTIME_H_
#define BLOX7_AUDIO_OPENSL_PLAYBACK_RUNTIME_H_

#include <cstdint>

#include "blox7/audio/opensl_runtime_abi.h"

#if defined(__GNUC__) || defined(__clang__)
#define BLOX7_OPENSL_EXPORT __attribute__((visibility("default")))
#else
#define BLOX7_OPENSL_EXPORT
#endif

struct Blox7OpenSlRuntimeStats {
  std::uint64_t submitted_buffers;
  std::uint64_t consumed_buffers;
  std::uint64_t clean_player_shutdowns;
};

extern "C" {

#if !defined(BLOX7_USE_SYSTEM_OPENSL_HEADERS)
extern BLOX7_OPENSL_EXPORT const
    blox7::audio::opensl_abi::InterfaceId SL_IID_ANDROIDCONFIGURATION;
extern BLOX7_OPENSL_EXPORT const
    blox7::audio::opensl_abi::InterfaceId SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
extern BLOX7_OPENSL_EXPORT const
    blox7::audio::opensl_abi::InterfaceId SL_IID_BUFFERQUEUE;
extern BLOX7_OPENSL_EXPORT const
    blox7::audio::opensl_abi::InterfaceId SL_IID_ENGINE;
extern BLOX7_OPENSL_EXPORT const
    blox7::audio::opensl_abi::InterfaceId SL_IID_PLAY;
extern BLOX7_OPENSL_EXPORT const
    blox7::audio::opensl_abi::InterfaceId SL_IID_RECORD;
extern BLOX7_OPENSL_EXPORT const
    blox7::audio::opensl_abi::InterfaceId SL_IID_VOLUME;

BLOX7_OPENSL_EXPORT blox7::audio::opensl_abi::Result
BLOX7_OPENSL_API_ENTRY slCreateEngine(
    blox7::audio::opensl_abi::Object* engine,
    blox7::audio::opensl_abi::Uint32 num_options,
    const blox7::audio::opensl_abi::EngineOption* options,
    blox7::audio::opensl_abi::Uint32 num_interfaces,
    const blox7::audio::opensl_abi::InterfaceId* interface_ids,
    const blox7::audio::opensl_abi::Boolean* interface_required);
#endif

BLOX7_OPENSL_EXPORT blox7::audio::opensl_abi::Result
blox7OpenSlGetRuntimeStats(Blox7OpenSlRuntimeStats* stats,
                              std::uint32_t stats_size);

}  // extern "C"

#endif  // BLOX7_AUDIO_OPENSL_PLAYBACK_RUNTIME_H_
