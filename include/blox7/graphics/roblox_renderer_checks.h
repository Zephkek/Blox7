#ifndef BLOX7_GRAPHICS_ROBLOX_RENDERER_CHECKS_H_
#define BLOX7_GRAPHICS_ROBLOX_RENDERER_CHECKS_H_

namespace blox7 {
namespace graphics {

bool RobloxAcceptsGlesVersion(const char* gl_version);
bool RobloxFindsVulkanDevice();

}  // namespace graphics
}  // namespace blox7

#endif  // BLOX7_GRAPHICS_ROBLOX_RENDERER_CHECKS_H_
