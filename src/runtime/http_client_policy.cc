#include "runtime/http_client_policy.h"

#define JSON_NOEXCEPTION 1
#include <array>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace blox7 {
namespace runtime {
namespace {

struct ClientSetting {
  std::string_view name;
  std::string_view value;
};

constexpr std::array<ClientSetting, 3> kHttpClientSettings = {{
    {"FFlagUseRuntimeMutexRvHttpClient", "False"},
    {"DFFlagHttpClientSkipRetryForStreamingRequests", "True"},
    {"FFlagLuaAppDefaultHttpRetry", "False"},
}};

}  // namespace

bool MergeHttpClientSettingsOverrides(std::string_view base_json,
                                      std::string* merged_json,
                                      std::string* error) {
  if (merged_json == nullptr) {
    if (error != nullptr) {
      *error = "HttpClient settings output is required";
    }
    return false;
  }

  nlohmann::json overrides = nlohmann::json::parse(
      base_json.empty() ? "{}" : base_json, nullptr, false, true);
  if (overrides.is_discarded() || !overrides.is_object()) {
    if (error != nullptr) {
      *error = "client-settings overrides must be a JSON object";
    }
    return false;
  }

  for (const ClientSetting& setting : kHttpClientSettings) {
    overrides[std::string(setting.name)] = std::string(setting.value);
  }
  *merged_json = overrides.dump();
  return true;
}

}  // namespace runtime
}  // namespace blox7
