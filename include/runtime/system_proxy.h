#ifndef BLOX7_RUNTIME_SYSTEM_PROXY_H_
#define BLOX7_RUNTIME_SYSTEM_PROXY_H_

#include <optional>
#include <string>

#include "runtime/runtime_config.h"

namespace blox7 {
namespace runtime {

struct SystemProxyResult {
  std::optional<NetworkProxyConfig> proxy;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

SystemProxyResult ResolveSystemProxy();

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_SYSTEM_PROXY_H_
