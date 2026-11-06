#ifndef BLOX7_RUNTIME_ROBLOX_FULLSCREEN_RUNTIME_BRIDGE_INTERNAL_H_
#define BLOX7_RUNTIME_ROBLOX_FULLSCREEN_RUNTIME_BRIDGE_INTERNAL_H_

#include <cstddef>
#include <cstdint>

namespace blox7 {
namespace runtime {
namespace internal {

bool HasExpectedFullscreenSetterContract(const std::uint8_t* code,
                                         std::size_t size);

}  // namespace internal
}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_FULLSCREEN_RUNTIME_BRIDGE_INTERNAL_H_
