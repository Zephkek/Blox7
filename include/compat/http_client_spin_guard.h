#ifndef BLOX7_COMPAT_HTTP_CLIENT_SPIN_GUARD_H_
#define BLOX7_COMPAT_HTTP_CLIENT_SPIN_GUARD_H_

#include <cstdint>

namespace blox7 {
namespace compat {

constexpr std::uint32_t kHttpClientSpinBatchSize = 256;
constexpr std::uint64_t kHttpClientSpinBatchIntervalNs = 1000000;

class HttpClientSpinRateLimiter {
 public:
  std::uint64_t Observe(std::uint64_t now_ns);

 private:
  std::uint32_t operation_count_ = 0;
  std::uint64_t checkpoint_ns_ = 0;
};

void ApplyHttpClientSpinGuard();

}  // namespace compat
}  // namespace blox7

#endif  // BLOX7_COMPAT_HTTP_CLIENT_SPIN_GUARD_H_
