#ifndef BLOX7_RUNTIME_CRASH_REPORT_POLICY_H_
#define BLOX7_RUNTIME_CRASH_REPORT_POLICY_H_

#include <string>
#include <string_view>

namespace blox7 {
namespace runtime {

bool MergeCrashReportClientSettingsOverrides(std::string_view base_json,
                                             std::string* merged_json,
                                             std::string* error);

bool MergeCrashReportFastFlagsOverrides(std::string_view base_json,
                                        std::string* merged_json,
                                        std::string* error);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_CRASH_REPORT_POLICY_H_
