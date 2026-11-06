#ifndef BLOX7_WEBVIEW_WEBVIEW_HELPER_POLICY_H_
#define BLOX7_WEBVIEW_WEBVIEW_HELPER_POLICY_H_

#if !defined(_WIN32)
#include <jsc/jsc.h>
#endif

#include <cstddef>
#include <string>
#include <string_view>

namespace blox7 {
namespace webview {

inline constexpr std::size_t kMaximumHybridCommandBytes = 64 * 1024;
inline constexpr char kExecuteRobloxHandler[] = "executeRoblox";
inline constexpr char kRobloxWkHybridHandler[] = "RobloxWKHybrid";
inline constexpr char kCompatibilityHandler[] = "blox7RobloxBridge";

enum class CaptchaEventType {
  kShown,
  kSuccess,
};

struct CaptchaEvent {
  CaptchaEventType type = CaptchaEventType::kShown;
  std::string callback_id;
};

struct UriPolicyResult {
  bool allowed = false;
  bool privileged_bridge_allowed = false;
  std::string scheme = "invalid";
  std::string host = "none";
};

bool ShouldDisableWebKitSandbox(std::string_view kernel_version,
                                const char* sandbox_override);
bool ShouldDisableWebViewHardwareAcceleration(
    bool wayland_display, const char* compositing_override);
const char* AndroidBridgeSource();
std::string BuildRobloxAppUserAgentTokens();
std::string BuildRobloxAndroidUserAgent();

struct AndroidDeviceInfo {
  unsigned long long total_memory_mb = 0;
  int display_width = 0;
  int display_height = 0;
  int xdpi = 0;
  int ydpi = 0;
  int width_dp = 0;
  int height_dp = 0;
  std::string manufacturer;
  std::string model;
  std::string release;
};
std::string BuildRobloxAppUserAgent(const AndroidDeviceInfo& device);
bool IsBrowserLoginUrl(std::string_view url);
bool IsEssentialWebResource(const char* uri);
std::string BoundedLogToken(const char* value, std::string_view fallback);
UriPolicyResult EvaluateNavigationUri(const char* uri);
const char* CaptchaEventName(CaptchaEventType type);
#if !defined(_WIN32)
bool ExtractExecuteRobloxCommand(JSCValue* value, std::string* command);
bool ExtractRobloxWkHybridCommand(JSCValue* value, std::string* command);
#endif
bool ParseCaptchaEvent(std::string_view command, CaptchaEvent* event);
std::string BuildCallbackScript(std::string_view callback_id);

}  // namespace webview
}  // namespace blox7

#endif  // BLOX7_WEBVIEW_WEBVIEW_HELPER_POLICY_H_
