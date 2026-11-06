#ifndef BLOX7_RUNTIME_WEBVIEW_HELPER_LAUNCHER_H_
#define BLOX7_RUNTIME_WEBVIEW_HELPER_LAUNCHER_H_

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace blox7 {
namespace runtime {

inline constexpr std::size_t kMaximumWebViewUrlBytes = 16 * 1024;
inline constexpr std::size_t kMaximumWebViewTitleBytes = 512;
inline constexpr std::size_t kMaximumWebViewJavaScriptBytes = 64 * 1024;
inline constexpr std::size_t kMaximumWebViewCookieBytes = 16 * 1024;
inline constexpr std::size_t kMaximumWebViewControlPacketBytes =
    kMaximumWebViewJavaScriptBytes + 12;
inline constexpr std::size_t kMaximumWebViewHybridEventBytes = 64 * 1024;
inline constexpr std::size_t kMaximumWebViewEventPacketBytes =
    kMaximumWebViewHybridEventBytes + 12;

inline constexpr int kWebViewHelperControlDescriptor = 198;

enum class WebViewHelperControlOperation : unsigned char {
  kLoadUrl = 1,
  kSetTitle = 2,
  kSetVisible = 3,
  kEvaluateJavaScript = 4,
  kClose = 5,
  kSetBackNavigationDisabled = 6,
  kSetShowDomainAsTitle = 7,
  kSetRobloxCookie = 8,
  kClearRobloxCookie = 9,
  kRetainPersistentRobloxCookie = 10,
};

struct WebViewHelperControlCommand {
  WebViewHelperControlOperation operation =
      WebViewHelperControlOperation::kClose;
  std::string payload;
  bool visible = false;
};

enum class WebViewHelperEventType : unsigned char {
  kExecuteRoblox = 1,
  kRobloxWkHybrid = 2,
  kReady = 3,
  kRobloxCookie = 4,
};

struct WebViewHelperEvent {
  WebViewHelperEventType type = WebViewHelperEventType::kExecuteRoblox;
  std::string payload;
};

bool ValidateWebViewUrl(std::string_view url, std::string *error = nullptr);
bool NormalizeWebViewUrl(std::string_view url, std::string *normalized_url,
                         std::string *error = nullptr);
bool DecodeWebViewRequest(std::string_view request, std::string *url,
                          std::string *error = nullptr);
bool DecodeWebViewHelperControlPacket(std::string_view packet,
                                      WebViewHelperControlCommand* command,
                                      std::string* error = nullptr);
bool EncodeWebViewHelperEventPacket(WebViewHelperEventType type,
                                    std::string_view payload,
                                    std::string* packet);
bool DecodeWebViewHelperEventPacket(std::string_view packet,
                                    WebViewHelperEvent* event,
                                    std::string* error = nullptr);

struct WebViewHelperExitObserver;
struct WebViewHelperLaunchResult;

class WebViewHelperProcess final {
 public:
  struct State;

  bool running() const;
  pid_t process_id() const;
  bool LoadUrl(std::string_view url) const;
  bool SetTitle(std::string_view title) const;
  bool SetVisible(bool visible) const;
  bool EvaluateJavaScript(std::string_view script) const;
  bool SetBackNavigationDisabled(bool disabled) const;
  bool SetShowDomainAsTitle(bool enabled) const;
  bool SetRobloxCookie(std::string_view value) const;
  bool ClearRobloxCookie() const;
  bool WaitUntilReady(std::chrono::milliseconds timeout) const;
  bool DrainEvents(std::vector<WebViewHelperEvent>* events) const;
  bool RequestClose() const;

 private:
  explicit WebViewHelperProcess(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;

  friend struct WebViewHelperLaunchResult;
  friend WebViewHelperLaunchResult LaunchWebViewHelper(
      const std::filesystem::path&, std::string_view,
      WebViewHelperExitObserver);
};

struct WebViewHelperLaunchResult {
  bool spawned = false;
  pid_t process_id = -1;
  std::shared_ptr<WebViewHelperProcess> process;
  std::string error;

  explicit operator bool() const { return spawned && error.empty(); }
};

struct WebViewHelperExitObserver {
  std::shared_ptr<void> context;
  void (*on_exit)(void *context) = nullptr;

  bool valid() const { return context != nullptr && on_exit != nullptr; }
};

WebViewHelperLaunchResult LaunchWebViewHelper(
    const std::filesystem::path& helper, std::string_view url,
    WebViewHelperExitObserver exit_observer = {});

} // namespace runtime
} // namespace blox7

#endif // BLOX7_RUNTIME_WEBVIEW_HELPER_LAUNCHER_H_
