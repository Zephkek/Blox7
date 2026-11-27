#ifndef BLOX7_RUNTIME_ROBLOX_START_INPUT_H_
#define BLOX7_RUNTIME_ROBLOX_START_INPUT_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace blox7 {
namespace runtime {

bool IsTouchStart(const std::uint8_t* code, std::size_t size, std::size_t at);
std::optional<std::size_t> FindTouchStart(const std::uint8_t* code,
                                          std::size_t size);
void StartGamesOnMouse(std::uintptr_t base,
                       const std::filesystem::path& cache_file);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_START_INPUT_H_
