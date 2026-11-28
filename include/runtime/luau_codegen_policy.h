#ifndef BLOX7_RUNTIME_LUAU_CODEGEN_POLICY_H_
#define BLOX7_RUNTIME_LUAU_CODEGEN_POLICY_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace blox7 {
namespace runtime {

struct AvxState {
  bool cpu = false;
  bool osxsave = false;
  std::uint64_t xcr0 = 0;

  bool os() const { return osxsave && (xcr0 & 6) == 6; }
  bool luau_codegen_off() const { return cpu && !os(); }
};

AvxState DetectAvxState();

bool MergeLuauCodegenClientSettingsOverrides(const AvxState& avx,
                                             std::string_view base_json,
                                             std::string* merged_json,
                                             std::string* error);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_LUAU_CODEGEN_POLICY_H_
