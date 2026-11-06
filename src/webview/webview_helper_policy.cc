#include "webview/webview_helper_policy.h"

#if !defined(_WIN32)
#include <glib.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#if defined(_WIN32)
namespace {

enum GUriFlags { G_URI_FLAGS_NONE = 0 };
struct GError {};
struct GUri {
  std::string scheme, userinfo, host, path;
  bool has_userinfo = false;
  int port = -1;
};

GUri* g_uri_parse(const char* text, GUriFlags, GError**) {
  if (text == nullptr) return nullptr;
  const std::string_view uri(text);
  const std::size_t colon = uri.find(':');
  if (colon == std::string_view::npos || colon == 0) return nullptr;
  for (std::size_t i = 0; i < colon; i++) {
    const unsigned char c = static_cast<unsigned char>(uri[i]);
    const bool alpha = (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
    if (!(alpha || (i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')))) return nullptr;
  }
  auto* parsed = new GUri;
  parsed->scheme.assign(uri.substr(0, colon));
  std::string_view rest = uri.substr(colon + 1);
  const std::size_t tail_start = rest.find_first_of("?#");
  std::string_view hier = rest.substr(0, tail_start);
  if (hier.substr(0, 2) == "//") {
    hier.remove_prefix(2);
    const std::size_t slash = hier.find('/');
    std::string_view authority = hier.substr(0, slash);
    parsed->path.assign(slash == std::string_view::npos ? std::string_view() : hier.substr(slash));
    const std::size_t at = authority.rfind('@');
    if (at != std::string_view::npos) {
      parsed->has_userinfo = true;
      parsed->userinfo.assign(authority.substr(0, at));
      authority.remove_prefix(at + 1);
    }
    std::string_view host = authority, port;
    if (!authority.empty() && authority.front() == '[') {
      const std::size_t close = authority.find(']');
      if (close == std::string_view::npos) {
        delete parsed;
        return nullptr;
      }
      host = authority.substr(1, close - 1);
      if (close + 1 < authority.size()) {
        if (authority[close + 1] != ':') {
          delete parsed;
          return nullptr;
        }
        port = authority.substr(close + 2);
      }
    } else if (const std::size_t port_colon = authority.rfind(':'); port_colon != std::string_view::npos) {
      host = authority.substr(0, port_colon);
      port = authority.substr(port_colon + 1);
    }
    if (!port.empty()) {
      int value = 0;
      for (const char c : port) {
        if (c < '0' || c > '9' || value > 65535) {
          delete parsed;
          return nullptr;
        }
        value = value * 10 + (c - '0');
      }
      if (value > 65535) {
        delete parsed;
        return nullptr;
      }
      parsed->port = value;
    }
    parsed->host.assign(host);
  } else {
    parsed->path.assign(hier);
  }
  return parsed;
}

const char* g_uri_get_scheme(GUri* uri) { return uri->scheme.c_str(); }
const char* g_uri_get_host(GUri* uri) { return uri->host.empty() ? nullptr : uri->host.c_str(); }
const char* g_uri_get_path(GUri* uri) { return uri->path.c_str(); }
const char* g_uri_get_userinfo(GUri* uri) { return uri->has_userinfo ? uri->userinfo.c_str() : nullptr; }
int g_uri_get_port(GUri* uri) { return uri->port; }
void g_uri_unref(GUri* uri) { delete uri; }
void g_error_free(GError*) {}

int g_ascii_strcasecmp(const char* a, const char* b) {
  for (;; a++, b++) {
    int x = static_cast<unsigned char>(*a), y = static_cast<unsigned char>(*b);
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y || x == 0) return x - y;
  }
}

bool g_str_has_prefix(const char* text, const char* prefix) {
  return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

}  // namespace
#endif

namespace blox7 {
namespace webview {
namespace {

constexpr std::size_t kMaximumHybridIdentifierBytes = 128;
constexpr std::size_t kMaximumCallbackIdentifierBytes = 1024;
constexpr char kFallbackRobloxVersion[] = "2.725.1142";
constexpr char kUserAgentEngine[] = "AppleWebKit/605.1.15";
constexpr char kBridgeSource[] = R"JS(
(() => {
  "use strict";
  const handlers = window.webkit && window.webkit.messageHandlers;
  const handler = handlers && handlers.executeRoblox;
  if (!handler) {
    return;
  }
  const bridge = {};
  Object.defineProperty(bridge, "executeRoblox", {
    value: (query) => handler.postMessage(JSON.parse(query)),
    enumerable: true,
    writable: false,
    configurable: false
  });
  try {
    Object.defineProperty(window, "__globalRobloxAndroidBridge__", {
      value: bridge,
      enumerable: true,
      writable: false,
      configurable: false
    });
  } catch (_) {
    // A page-defined object is never allowed to replace the native bridge.
  }
})();
)JS";

bool IsSafeVersion(std::string_view version) {
  return !version.empty() && version.size() <= 32 &&
         std::all_of(version.begin(), version.end(), [](unsigned char value) {
           return (value >= '0' && value <= '9') || value == '.' ||
                  value == '-';
         });
}

bool ReadBoundedString(const nlohmann::json& object, const char* key,
                       std::size_t maximum_bytes, std::string* value,
                       bool optional = false) {
  const auto found = object.find(key);
  if (found == object.end()) {
    if (optional) {
      value->clear();
      return true;
    }
    return false;
  }
  if (!found->is_string()) {
    return false;
  }
  const auto& text = found->get_ref<const std::string&>();
  if (text.size() > maximum_bytes ||
      std::any_of(text.begin(), text.end(), [](unsigned char character) {
        return character < 0x20 || character == 0x7f;
      })) {
    return false;
  }
  *value = text;
  return true;
}

#if !defined(_WIN32)
bool ExtractBoundedJscString(JSCValue* value, std::string* text) {
  if (value == nullptr || text == nullptr || !jsc_value_is_string(value)) {
    return false;
  }
  GBytes* bytes = jsc_value_to_string_as_bytes(value);
  if (bytes == nullptr) {
    return false;
  }
  gsize size = 0;
  const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
  const bool valid =
      data != nullptr && size != 0 && size <= kMaximumHybridCommandBytes;
  if (valid) {
    text->assign(data, size);
  }
  g_bytes_unref(bytes);
  return valid;
}
#endif

bool IsRobloxHost(const char* host) {
  if (host == nullptr) {
    return false;
  }
  constexpr std::string_view kRoot = "roblox.com";
  const std::string_view value(host);
  if (value.size() < kRoot.size()) {
    return false;
  }
  const std::string_view suffix = value.substr(value.size() - kRoot.size());
  if (g_ascii_strcasecmp(std::string(suffix).c_str(),
                         std::string(kRoot).c_str()) != 0) {
    return false;
  }
  return value.size() == kRoot.size() ||
         value[value.size() - kRoot.size() - 1] == '.';
}

}  // namespace

bool ShouldDisableWebKitSandbox(std::string_view kernel_version,
                                const char* sandbox_override) {
  return sandbox_override == nullptr &&
         (kernel_version.find("FreeBSD") != std::string_view::npos ||
          kernel_version.find("freebsd.org") != std::string_view::npos);
}

bool ShouldDisableWebViewHardwareAcceleration(
    bool wayland_display, const char* compositing_override) {
  if (compositing_override != nullptr) {
    return std::string_view(compositing_override) != "0";
  }
  return wayland_display;
}

const char* AndroidBridgeSource() { return kBridgeSource; }

std::string BuildRobloxAppUserAgentTokens() {
  const char* configured_version = std::getenv("BLOX7_ROBLOX_VERSION");
  const std::string_view version =
      configured_version != nullptr && IsSafeVersion(configured_version)
          ? std::string_view(configured_version)
          : std::string_view(kFallbackRobloxVersion);
  return "ROBLOX Android App " + std::string(version) +
         " Phone Hybrid()  GooglePlayStore RobloxApp/" + std::string(version) +
         " (GlobalDist; GooglePlayStore)";
}

std::string BuildRobloxAndroidUserAgent() {
  return "Mozilla/5.0 " + std::string(kUserAgentEngine) + " (KHTML, like Gecko)  " +
         BuildRobloxAppUserAgentTokens();
}

std::string BuildRobloxAppUserAgent(const AndroidDeviceInfo& device) {
  std::string model = device.model.rfind(device.manufacturer, 0) == 0
                          ? device.model
                          : device.manufacturer + " " + device.model;
  if (!model.empty() && model[0] >= 'a' && model[0] <= 'z') model[0] = static_cast<char>(model[0] - 'a' + 'A');
  std::string clean;
  for (std::size_t i = 0; i < model.size(); i++) {
    const unsigned char c = static_cast<unsigned char>(model[i]);
    if (c >= 0x80 && c < 0xc0) continue;
    clean.push_back(c <= 31 || c >= 127 ? '_' : static_cast<char>(c));
  }
  char device_block[512];
  std::snprintf(device_block, sizeof device_block, "(%lluMB; %dx%d; %dx%d; %dx%d; %s; %s)",
                device.total_memory_mb, device.display_width, device.display_height, device.xdpi,
                device.ydpi, device.width_dp, device.height_dp, clean.c_str(), device.release.c_str());
  return "Mozilla/5.0 " + std::string(device_block) +
         " AppleWebKit/537.36 (KHTML, like Gecko)  " + BuildRobloxAppUserAgentTokens();
}

bool IsBrowserLoginUrl(std::string_view url) {
  GUri* parsed = g_uri_parse(std::string(url).c_str(), G_URI_FLAGS_NONE, nullptr);
  if (parsed == nullptr) return false;
  const char* host = g_uri_get_host(parsed);
  const char* path = g_uri_get_path(parsed);
  const bool browser_login =
      EvaluateNavigationUri(std::string(url).c_str()).allowed && host != nullptr &&
      (g_ascii_strcasecmp(host, "www.roblox.com") == 0 ||
       g_ascii_strcasecmp(host, "roblox.com") == 0) && path != nullptr &&
      (g_ascii_strcasecmp(path, "/login") == 0 ||
       g_ascii_strcasecmp(path, "/login/") == 0);
  g_uri_unref(parsed);
  return browser_login;
}

bool IsEssentialWebResource(const char* uri) {
  if (uri == nullptr) return false;
  const UriPolicyResult policy = EvaluateNavigationUri(uri);
  if (!policy.allowed) return false;
  if (policy.host == "css.rbxcdn.com" || policy.host == "js.rbxcdn.com") {
    return true;
  }
  if (policy.host != "arkoselabs.roblox.com" &&
      policy.host != "apis.rbxcdn.com") return false;
  GUri* parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, nullptr);
  if (parsed == nullptr) return false;
  const char* path = g_uri_get_path(parsed);
  const bool essential =
      path != nullptr &&
      (policy.host == "apis.rbxcdn.com"
           ? std::string_view(path) == "/captcha/v1/metadata"
           : g_str_has_prefix(path, "/v2/") || g_str_has_prefix(path, "/fc/"));
  g_uri_unref(parsed);
  return essential;
}

std::string BoundedLogToken(const char* value, std::string_view fallback) {
  if (value == nullptr || *value == '\0') {
    return std::string(fallback);
  }
  constexpr std::size_t kMaximumLogTokenBytes = 128;
  std::string token;
  for (const unsigned char character : std::string_view(value)) {
    if (token.size() == kMaximumLogTokenBytes) {
      break;
    }
    token.push_back(character >= 0x21 && character <= 0x7e
                        ? static_cast<char>(character)
                        : '?');
  }
  return token;
}

UriPolicyResult EvaluateNavigationUri(const char* uri) {
  UriPolicyResult result;
  if (uri == nullptr) {
    return result;
  }
  if (std::string_view(uri) == "about:blank") {
    result.allowed = true;
    result.scheme = "about";
    result.host = "none";
    return result;
  }

  GError* parse_error = nullptr;
  GUri* parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &parse_error);
  if (parse_error != nullptr) {
    g_error_free(parse_error);
  }
  if (parsed == nullptr) {
    return result;
  }

  const char* scheme = g_uri_get_scheme(parsed);
  const char* host = g_uri_get_host(parsed);
  result.scheme = BoundedLogToken(scheme, "invalid");
  result.host = BoundedLogToken(host, "none");
  const int port = g_uri_get_port(parsed);
  result.allowed =
      scheme != nullptr && g_ascii_strcasecmp(scheme, "https") == 0 &&
      host != nullptr && *host != '\0' &&
      g_uri_get_userinfo(parsed) == nullptr && (port == -1 || port == 443);
  result.privileged_bridge_allowed = result.allowed && IsRobloxHost(host);
  g_uri_unref(parsed);
  return result;
}

const char* CaptchaEventName(CaptchaEventType type) {
  return type == CaptchaEventType::kSuccess ? "CaptchaSuccess" : "CaptchaShown";
}

#if !defined(_WIN32)
bool ExtractExecuteRobloxCommand(JSCValue* value, std::string* command) {
  if (value == nullptr || command == nullptr || !jsc_value_is_object(value)) {
    return false;
  }
  char* serialized = jsc_value_to_json(value, 0);
  if (serialized == nullptr) {
    return false;
  }
  const std::size_t size = strnlen(serialized, kMaximumHybridCommandBytes + 1);
  const bool valid = size != 0 && size <= kMaximumHybridCommandBytes;
  if (valid) {
    command->assign(serialized, size);
  }
  g_free(serialized);
  return valid;
}

bool ExtractRobloxWkHybridCommand(JSCValue* value, std::string* command) {
  if (value == nullptr || command == nullptr || !jsc_value_is_object(value)) {
    return false;
  }
  JSCValue* property = jsc_value_object_get_property(value, "command");
  const bool extracted = ExtractBoundedJscString(property, command);
  if (property != nullptr) {
    g_object_unref(property);
  }
  return extracted;
}
#endif

bool ParseCaptchaEvent(std::string_view command, CaptchaEvent* event) {
  if (event == nullptr || command.empty() ||
      command.size() > kMaximumHybridCommandBytes) {
    return false;
  }
  const nlohmann::json message = nlohmann::json::parse(
      command.begin(), command.end(), nullptr, false, false);
  if (message.is_discarded() || !message.is_object()) {
    return false;
  }

  std::string module;
  std::string function;
  if (!ReadBoundedString(message, "moduleID", kMaximumHybridIdentifierBytes,
                         &module) ||
      !ReadBoundedString(message, "functionName", kMaximumHybridIdentifierBytes,
                         &function) ||
      module != "Navigation" || function != "navigateToFeature") {
    return false;
  }
  const auto parameters = message.find("params");
  if (parameters == message.end() || !parameters->is_object()) {
    return false;
  }
  const auto navigation = parameters->find("params");
  if (navigation == parameters->end() || !navigation->is_object()) {
    return false;
  }

  std::string feature;
  if (!ReadBoundedString(*navigation, "feature", kMaximumHybridIdentifierBytes,
                         &feature) ||
      !ReadBoundedString(message, "callbackID", kMaximumCallbackIdentifierBytes,
                         &event->callback_id, true)) {
    return false;
  }
  if (feature == "CaptchaShown") {
    event->type = CaptchaEventType::kShown;
    return true;
  }
  if (feature == "CaptchaSuccess") {
    event->type = CaptchaEventType::kSuccess;
    return true;
  }
  return false;
}

std::string BuildCallbackScript(std::string_view callback_id) {
  const std::string quoted_callback = nlohmann::json(callback_id).dump();
  return "(() => { try { if (window.Roblox.Hybrid && "
         "window.Roblox.Hybrid.Bridge.nativeCallback && typeof "
         "window.Roblox.Hybrid.Bridge.nativeCallback === \"function\") { "
         "window.Roblox.Hybrid.Bridge.nativeCallback(" +
         quoted_callback +
         ", true, {}); return true; } } catch (_) {} return false; })();";
}

}  // namespace webview
}  // namespace blox7
