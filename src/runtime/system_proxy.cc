#include "runtime/system_proxy.h"

#ifndef BLOX7_WIN32
#include <gio/gio.h>
#endif
#include <strings.h>

#include <cstdlib>

#include <string>
#include <vector>

namespace blox7 {
namespace runtime {
namespace {

constexpr char kSystemProxyProbeUrl[] = "https://www.roblox.com/";

bool IsDirectProxy(const std::string& candidate) {
  return strcasecmp(candidate.c_str(), "direct://") == 0;
}

}  // namespace

SystemProxyResult SelectSystemProxy(
    const std::vector<std::string>& candidates) {
  SystemProxyResult result;
  if (candidates.empty()) {
    result.error = "system proxy resolver returned no connection method";
    return result;
  }

  const std::string& candidate = candidates.front();
  if (IsDirectProxy(candidate)) {
    return result;
  }

#ifdef BLOX7_WIN32
  const std::string::size_type scheme_end = candidate.find("://");
  if (scheme_end == std::string::npos) {
    result.error = "system proxy resolver returned an invalid proxy URI";
    return result;
  }
  const std::string scheme_text = candidate.substr(0, scheme_end);
  std::string authority = candidate.substr(scheme_end + 3);
  authority = authority.substr(0, authority.find('/'));
  const std::string::size_type at = authority.rfind('@');
  const std::string user_text = at == std::string::npos ? "" : authority.substr(0, at);
  if (at != std::string::npos) authority = authority.substr(at + 1);
  const std::string::size_type colon = authority.rfind(':');
  const std::string host_text = colon == std::string::npos ? authority : authority.substr(0, colon);
  const char* scheme = scheme_text.c_str();
  const char* host = host_text.empty() ? nullptr : host_text.c_str();
  const char* user_info = user_text.c_str();
  const int port = colon == std::string::npos ? -1 : std::atoi(authority.c_str() + colon + 1);
#else
  GError* parse_error = nullptr;
  GUri* uri = g_uri_parse(candidate.c_str(), G_URI_FLAGS_SCHEME_NORMALIZE,
                          &parse_error);
  if (uri == nullptr) {
    if (parse_error != nullptr) g_error_free(parse_error);
    result.error = "system proxy resolver returned an invalid proxy URI";
    return result;
  }

  const char* scheme = g_uri_get_scheme(uri);
  const char* host = g_uri_get_host(uri);
  const char* user_info = g_uri_get_userinfo(uri);
  const int port = g_uri_get_port(uri);
#endif
  if (user_info != nullptr && user_info[0] != '\0') {
    result.error = "authenticated system proxies are not supported";
  } else if (host == nullptr || port <= 0 || port > 65535) {
    result.error = "system proxy has no valid host and port";
  } else {
    result.proxy = ParseNetworkProxyConfig(
        host, std::to_string(port), scheme != nullptr ? scheme : "");
    if (!result.proxy.has_value()) {
      result.error = "system proxy must use HTTP, HTTPS, or SOCKS5";
    }
  }
#ifndef BLOX7_WIN32
  g_uri_unref(uri);
#endif
  return result;
}

SystemProxyResult ResolveSystemProxy() {
  SystemProxyResult result;
#ifdef BLOX7_WIN32
  result.error = "system proxy resolver is unavailable";
  return result;
#else
  GProxyResolver* resolver = g_proxy_resolver_get_default();
  if (resolver == nullptr || !g_proxy_resolver_is_supported(resolver)) {
    result.error = "system proxy resolver is unavailable";
    return result;
  }

  GError* lookup_error = nullptr;
  gchar** resolved = g_proxy_resolver_lookup(
      resolver, kSystemProxyProbeUrl, nullptr, &lookup_error);
  if (resolved == nullptr) {
    result.error = lookup_error != nullptr && lookup_error->message != nullptr
                       ? std::string("could not resolve system proxy: ") +
                             lookup_error->message
                       : "could not resolve system proxy";
    if (lookup_error != nullptr) g_error_free(lookup_error);
    return result;
  }

  std::vector<std::string> candidates;
  for (gchar** candidate = resolved; *candidate != nullptr; ++candidate) {
    candidates.emplace_back(*candidate);
  }
  g_strfreev(resolved);
  return SelectSystemProxy(candidates);
#endif
}

}  // namespace runtime
}  // namespace blox7
