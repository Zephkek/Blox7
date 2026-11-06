#include "blox7/platform/sdl_window_icon.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <string>
#include <utility>

#include "blox7/platform/sdl_window_icon_data.h"

namespace blox7 {
namespace platform {
namespace {

Status IconError(const char* operation) {
  std::string message = operation;
  message += " failed: ";
  const char* error = SDL_GetError();
  message += error != nullptr && error[0] != '\0' ? error : "unknown SDL error";
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

}  // namespace

Status ApplySdlWindowIcon(SDL_Window* window) {
  if (window == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "SDL window must not be null");
  }

  SDL_IOStream* stream = nullptr;
#ifdef BLOX7_WIN32
  if (const char* file = std::getenv("BLOX7_WINDOW_ICON_FILE");
      file != nullptr && file[0] != '\0') {
    stream = SDL_IOFromFile(file, "rb");
  }
#endif
  if (stream == nullptr) {
    stream = SDL_IOFromConstMem(internal::kBlox7WindowIconPng,
                                internal::kBlox7WindowIconPngSize);
  }
  if (stream == nullptr) {
    return IconError("SDL_IOFromConstMem(window icon)");
  }

  SDL_Surface* icon = SDL_LoadPNG_IO(stream, true);
  if (icon == nullptr) {
    return IconError("SDL_LoadPNG_IO(window icon)");
  }

  if (!SDL_SetWindowIcon(window, icon)) {
    Status status = IconError("SDL_SetWindowIcon");
    SDL_DestroySurface(icon);
    return status;
  }
  SDL_DestroySurface(icon);
  return Status::Ok();
}

}  // namespace platform
}  // namespace blox7
