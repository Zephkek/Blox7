#ifndef BLOX7_SERVICES_AUTH_SERVICE_H_
#define BLOX7_SERVICES_AUTH_SERVICE_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "services/http_client.h"

namespace blox7 {
namespace services {

enum class AuthCookieStatus {
  kValid,
  kInvalid,
  kUnavailable,
};

struct AuthCookieValidation {
  AuthCookieStatus status = AuthCookieStatus::kUnavailable;
  long http_status = 0;
  std::string error;
};

enum class AuthSessionStatus {
  kAuthenticated,
  kGuest,
  kInvalid,
  kUnavailable,
};

struct AuthIdentity {
  int64_t user_id = -1;
  std::string username;
  std::string display_name;
};

struct AuthSession {
  AuthSessionStatus status = AuthSessionStatus::kUnavailable;
  AuthIdentity identity;
  long http_status = 0;
  std::string error;
};

class AuthService {
 public:
  explicit AuthService(HttpClient& http_client) : http_client_(http_client) {}

  AuthSession ResolveSession(std::string_view cookie_header,
                             bool allow_guest = false);

  AuthCookieValidation ValidateCookie(const std::string& cookie_header);
  static bool HasRoblosecurityCookie(const std::string& cookie_header);
  static std::string ExtractRoblosecurityValue(std::string_view cookie_header);

  static bool RedactRejectedRoblosecurity(std::string_view cookie_header,
                                          std::string_view rejected_value,
                                          std::string* redacted_header);

 private:
  HttpClient& http_client_;
};

}  // namespace services
}  // namespace blox7

#endif  // BLOX7_SERVICES_AUTH_SERVICE_H_
