#ifndef BLOX7_RUNTIME_WEBVIEW_ROBLOX_COOKIE_H_
#define BLOX7_RUNTIME_WEBVIEW_ROBLOX_COOKIE_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/auth_runtime_composition.h"

namespace blox7 {
namespace runtime {

inline constexpr std::size_t kMaximumWebViewRobloxCookieValueBytes = 16 * 1024;
inline constexpr std::string_view kWebViewRobloxCookieName = ".ROBLOSECURITY";
inline constexpr std::string_view kWebViewRobloxCookieDomain = ".roblox.com";
inline constexpr std::string_view kWebViewRobloxCookiePath = "/";

struct WebViewRobloxCookieResult;

class SecureWebViewRobloxCookie final {
 public:
  SecureWebViewRobloxCookie() = default;
  ~SecureWebViewRobloxCookie();

  SecureWebViewRobloxCookie(const SecureWebViewRobloxCookie&) = delete;
  SecureWebViewRobloxCookie& operator=(const SecureWebViewRobloxCookie&) =
      delete;
  SecureWebViewRobloxCookie(SecureWebViewRobloxCookie&& other) noexcept;
  SecureWebViewRobloxCookie& operator=(
      SecureWebViewRobloxCookie&& other) noexcept;

  bool empty() const { return bytes_.empty(); }
  std::size_t size() const { return bytes_.empty() ? 0 : bytes_.size() - 1; }
  std::string_view value() const { return {c_str(), size()}; }
  const char* c_str() const { return bytes_.empty() ? "" : bytes_.data(); }
  SecureWebViewRobloxCookie Clone() const;
  void Clear();

 private:
  explicit SecureWebViewRobloxCookie(std::string_view value);

  std::vector<char> bytes_;

  friend struct WebViewRobloxCookieResult;
  friend WebViewRobloxCookieResult PrepareWebViewRobloxCookie(
      const SecureRobloxCredential& credential);
};

struct WebViewRobloxCookieResult {
  SecureWebViewRobloxCookie cookie;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

WebViewRobloxCookieResult PrepareWebViewRobloxCookie(
    const SecureRobloxCredential& credential);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_WEBVIEW_ROBLOX_COOKIE_H_
