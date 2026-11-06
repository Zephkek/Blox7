#ifndef BLOX7_PLATFORM_SDL_GAMEPAD_MANAGER_H_
#define BLOX7_PLATFORM_SDL_GAMEPAD_MANAGER_H_

#include <SDL3/SDL_gamepad.h>

#include <map>

#include "blox7/platform/platform_runtime.h"

namespace blox7 {
namespace platform {

class SdlGamepadManager final {
 public:
  using EventFn = void (*)(void*, const PlatformEvent&);

  ~SdlGamepadManager();
  SdlGamepadManager() = default;
  SdlGamepadManager(const SdlGamepadManager&) = delete;
  SdlGamepadManager& operator=(const SdlGamepadManager&) = delete;

  Status Initialize(void* context, EventFn emit);
  void Shutdown();
  bool HandleEvent(const PlatformEvent& event);
  void ResendState();

 private:
  SDL_Gamepad* Open(SDL_JoystickID id);
  void Remove(SDL_JoystickID id);
  void Describe(SDL_JoystickID id, SDL_Gamepad* gamepad, bool remapped);
  void EmitState(SDL_JoystickID id, SDL_Gamepad* gamepad);
  void Emit(PlatformEventPayload payload);

  std::map<SDL_JoystickID, SDL_Gamepad*> gamepads_;
  void* context_ = nullptr;
  EventFn emit_ = nullptr;
  bool initialized_ = false;
};

}  // namespace platform
}  // namespace blox7

#endif  // BLOX7_PLATFORM_SDL_GAMEPAD_MANAGER_H_
