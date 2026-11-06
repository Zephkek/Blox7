#ifndef BLOX7_RUNTIME_ROBLOX_USER_AGENT_H_
#define BLOX7_RUNTIME_ROBLOX_USER_AGENT_H_

#include <cstdlib>
#include <string>
#include <string_view>

namespace blox7::runtime {

inline constexpr std::string_view kRobloxVersionPlaceholder = "{roblox_version}";

inline std::string ActiveRobloxVersion() {
  const char* version = std::getenv("BLOX7_ROBLOX_VERSION");
  return version != nullptr && *version != '\0' ? version : "";
}

inline std::string FillRobloxVersion(std::string text, std::string_view version) {
  for (std::size_t at = text.find(kRobloxVersionPlaceholder); at != std::string::npos;
       at = text.find(kRobloxVersionPlaceholder, at + version.size()))
    text.replace(at, kRobloxVersionPlaceholder.size(), version);
  return text;
}

inline std::string RobloxFallbackUserAgent() {
  const std::string version = ActiveRobloxVersion();
  return "Roblox/" + (version.empty() ? std::string("unknown") : version) + " (Linux; Android 16; SM-S908B)";
}

}  // namespace blox7::runtime

#endif
