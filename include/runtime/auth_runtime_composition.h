#ifndef BLOX7_RUNTIME_AUTH_RUNTIME_COMPOSITION_H_
#define BLOX7_RUNTIME_AUTH_RUNTIME_COMPOSITION_H_

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "jnivm/jnivm.h"

namespace blox7 {
namespace services {
class AuthService;
class HttpClient;
}  // namespace services

namespace runtime {

class Environment;
class RuntimePaths;
class RobloxSecretStore;

enum class AuthRuntimeStatus {
  kAuthenticated,
  kGuest,
  kInvalidCredentials,
  kUnavailable,
};

class SecureRobloxCredential final {
 public:
  SecureRobloxCredential() = default;
  explicit SecureRobloxCredential(std::string canonical_header);
  ~SecureRobloxCredential();

  SecureRobloxCredential(const SecureRobloxCredential&) = delete;
  SecureRobloxCredential& operator=(const SecureRobloxCredential&) = delete;
  SecureRobloxCredential(SecureRobloxCredential&& other) noexcept;
  SecureRobloxCredential& operator=(SecureRobloxCredential&& other) noexcept;

  bool empty() const { return bytes_.empty(); }
  size_t size() const { return bytes_.empty() ? 0 : bytes_.size() - 1; }
  const char* c_str() const { return bytes_.empty() ? "" : bytes_.data(); }
  std::string_view view() const { return {c_str(), size()}; }
  void Clear();

 private:
  std::vector<char> bytes_;
};

void SecurelyClearString(std::string* value);

class ScopedRobloxCredentialBinding final {
 public:
  ScopedRobloxCredentialBinding(jnivm::VM* jni_vm,
                                const SecureRobloxCredential& credential);
  ~ScopedRobloxCredentialBinding();

  ScopedRobloxCredentialBinding(const ScopedRobloxCredentialBinding&) = delete;
  ScopedRobloxCredentialBinding& operator=(
      const ScopedRobloxCredentialBinding&) = delete;
  ScopedRobloxCredentialBinding(ScopedRobloxCredentialBinding&&) = delete;
  ScopedRobloxCredentialBinding& operator=(
      ScopedRobloxCredentialBinding&&) = delete;

  bool bound() const { return jni_vm_ != nullptr; }

 private:
  static jnivm::RobloxCredentialView ProvideCredential(const void* context);

  jnivm::VM* jni_vm_ = nullptr;
};

struct AuthRuntimeComposition {
  AuthRuntimeStatus status = AuthRuntimeStatus::kUnavailable;
  std::shared_ptr<jnivm::VM> jni_vm;
  jnivm::RobloxAuthIdentity account_identity;
  SecureRobloxCredential credential;
  long http_status = 0;
  bool rejected_credential_retired = false;
  std::string error;

  explicit operator bool() const { return jni_vm != nullptr; }
};

AuthRuntimeComposition ComposeAuthRuntime(const Environment& environment,
                                          const RuntimePaths& paths,
                                          services::AuthService& auth_service);

AuthRuntimeComposition ComposeAuthRuntime(
    const Environment& environment, const RuntimePaths& paths,
    services::AuthService& auth_service,
    std::shared_ptr<services::HttpClient> live_auth_http_client,
    std::shared_ptr<RobloxSecretStore> secrets = nullptr);

bool PersistRobloxCookie(const std::filesystem::path& path,
                         std::string_view cookie_value,
                         std::shared_ptr<RobloxSecretStore> secrets = nullptr,
                         const std::filesystem::path& legacy_webview = {});

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_AUTH_RUNTIME_COMPOSITION_H_
