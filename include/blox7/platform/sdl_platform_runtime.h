#ifndef BLOX7_PLATFORM_SDL_PLATFORM_RUNTIME_H_
#define BLOX7_PLATFORM_SDL_PLATFORM_RUNTIME_H_

#include <memory>

#include "blox7/platform/platform_runtime.h"

namespace blox7 {
namespace platform {

std::unique_ptr<PlatformRuntime> CreateSdlPlatformRuntime();

}  // namespace platform
}  // namespace blox7

#endif  // BLOX7_PLATFORM_SDL_PLATFORM_RUNTIME_H_
