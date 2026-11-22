#ifndef BLOX7_GRAPHICS_ROBLOX_RENDERER_CHECKS_H_
#define BLOX7_GRAPHICS_ROBLOX_RENDERER_CHECKS_H_

namespace blox7 {
namespace graphics {

inline constexpr char kNoGles3Line[] =
    "[window] no OpenGL ES 3.0 from the graphics driver or from ANGLE "
    "(Direct3D 11)";

inline constexpr char kNoGles3Reason[] =
    "no OpenGL ES 3.0 from the graphics driver or from ANGLE (Direct3D 11), "
    "Roblox needs a DirectX 10 or newer graphics card";

bool RobloxAcceptsGlesVersion(const char* gl_version);
bool RobloxFindsVulkanDevice();

}  // namespace graphics
}  // namespace blox7

#endif  // BLOX7_GRAPHICS_ROBLOX_RENDERER_CHECKS_H_
