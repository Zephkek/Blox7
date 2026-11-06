#ifndef BLOX7_PLATFORM_SDL_EVENT_CONVERTER_H_
#define BLOX7_PLATFORM_SDL_EVENT_CONVERTER_H_

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include "blox7/platform/platform_runtime.h"

namespace blox7 {
namespace platform {

bool ConvertSdlEvent(SDL_Window* window, const SDL_Event& source,
                     PlatformEvent* destination,
                     bool relative_mouse_mode = false);

}  // namespace platform
}  // namespace blox7

#endif  // BLOX7_PLATFORM_SDL_EVENT_CONVERTER_H_
