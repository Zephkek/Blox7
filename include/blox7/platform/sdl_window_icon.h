#ifndef BLOX7_PLATFORM_SDL_WINDOW_ICON_H_
#define BLOX7_PLATFORM_SDL_WINDOW_ICON_H_

#include "blox7/status.h"

struct SDL_Window;

namespace blox7 {
namespace platform {

Status ApplySdlWindowIcon(SDL_Window* window);

}  // namespace platform
}  // namespace blox7

#endif  // BLOX7_PLATFORM_SDL_WINDOW_ICON_H_
