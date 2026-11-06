#ifndef BLOX7_WINDOW_WINDOW_CREATION_POLICY_H_
#define BLOX7_WINDOW_WINDOW_CREATION_POLICY_H_

#include <SDL3/SDL_video.h>

#include <string_view>

namespace blox7 {
namespace window {

SDL_WindowFlags ApplyHighPixelDensityWindowFlag(SDL_WindowFlags flags,
                                                bool high_dpi);

bool DirectVulkanWindowRequiresInitialSync(std::string_view video_driver);

}  // namespace window
}  // namespace blox7

#endif  // BLOX7_WINDOW_WINDOW_CREATION_POLICY_H_
