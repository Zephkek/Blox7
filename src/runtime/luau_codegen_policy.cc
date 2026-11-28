#include "runtime/luau_codegen_policy.h"

#include <cpuid.h>

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

constexpr std::array<ClientSetting, 2> kLuauCodegenOffSettings = {{
    {"FFlagDebugDisableCodegen", "True"},
    {"FFlagLuauCodegen", "False"},
}};

}  // namespace

AvxState DetectAvxState() {
  AvxState avx;
  unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
    return avx;
  }
  avx.cpu = ((ecx >> 28) & 1) != 0;
  avx.osxsave = ((ecx >> 27) & 1) != 0;
  if (avx.osxsave) {
    unsigned low = 0, high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    avx.xcr0 = (static_cast<std::uint64_t>(high) << 32) | low;
  }
  return avx;
}

bool MergeLuauCodegenClientSettingsOverrides(const AvxState& avx,
                                             std::string_view base_json,
                                             std::string* merged_json,
                                             std::string* error) {
  if (merged_json == nullptr) {
    if (error != nullptr) {
      *error = "Luau codegen client-settings output is required";
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
  if (!avx.luau_codegen_off()) {
    merged_json->assign(base_json);
    return true;
  }
  for (const ClientSetting& setting : kLuauCodegenOffSettings) {
    overrides[std::string(setting.name)] = std::string(setting.value);
  }
  *merged_json = overrides.dump();
  return true;
}

}  // namespace runtime
}  // namespace blox7
