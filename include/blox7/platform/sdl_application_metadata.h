#ifndef BLOX7_PLATFORM_SDL_APPLICATION_METADATA_H_
#define BLOX7_PLATFORM_SDL_APPLICATION_METADATA_H_

#include "blox7/status.h"

namespace blox7 {
namespace platform {

inline constexpr char kBlox7ApplicationName[] = "Blox7";
inline constexpr char kBlox7ApplicationIdentifier[] =
    "space.bigrat.blox7";

Status ConfigureSdlApplicationMetadata();

}  // namespace platform
}  // namespace blox7

#endif  // BLOX7_PLATFORM_SDL_APPLICATION_METADATA_H_
