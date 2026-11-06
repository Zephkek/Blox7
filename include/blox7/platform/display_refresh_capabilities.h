#ifndef BLOX7_PLATFORM_DISPLAY_REFRESH_CAPABILITIES_H_
#define BLOX7_PLATFORM_DISPLAY_REFRESH_CAPABILITIES_H_

#include <vector>

struct SDL_Window;

namespace blox7 {
namespace platform {

struct DisplayRefreshCapabilities {
  float current_hz = 0.0f;
  std::vector<float> supported_hz;

  bool valid() const { return current_hz > 0.0f; }
};

DisplayRefreshCapabilities
QuerySdlDisplayRefreshCapabilities(SDL_Window *window);

DisplayRefreshCapabilities
NormalizeDisplayRefreshCapabilities(float current_hz,
                                    std::vector<float> supported_hz);

} // namespace platform
} // namespace blox7

#endif // BLOX7_PLATFORM_DISPLAY_REFRESH_CAPABILITIES_H_
