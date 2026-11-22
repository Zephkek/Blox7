#include "runtime/system_proxy.h"

namespace blox7 {
namespace runtime {

SystemProxyResult ResolveSystemProxy() {
  SystemProxyResult result;
  result.error = "system proxy resolver is unavailable";
  return result;
}

}  // namespace runtime
}  // namespace blox7
