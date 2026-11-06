#ifndef BLOX7_WINDOW_INPUT_PUMP_PACER_H_
#define BLOX7_WINDOW_INPUT_PUMP_PACER_H_

#include <cstdint>
#include <limits>

namespace blox7 {
namespace window {

inline constexpr uint64_t kProductionInputPumpHz = 240;
inline constexpr uint64_t kProductionInputPumpIntervalNs =
    1000000000ULL / kProductionInputPumpHz;

class InputPumpPacer final {
 public:
  explicit InputPumpPacer(
      uint64_t interval_ns = kProductionInputPumpIntervalNs)
      : interval_ns_(interval_ns) {}

  uint64_t DelayBeforeNextPump(uint64_t now_ns) {
    if (interval_ns_ == 0) {
      return 0;
    }
    if (next_deadline_ns_ == 0 || now_ns >= next_deadline_ns_) {
      next_deadline_ns_ = AddSaturating(now_ns, interval_ns_);
      return 0;
    }
    const uint64_t delay_ns = next_deadline_ns_ - now_ns;
    next_deadline_ns_ = AddSaturating(next_deadline_ns_, interval_ns_);
    return delay_ns;
  }

  void Reset() { next_deadline_ns_ = 0; }

 private:
  static uint64_t AddSaturating(uint64_t value, uint64_t increment) {
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return value > maximum - increment ? maximum : value + increment;
  }

  const uint64_t interval_ns_;
  uint64_t next_deadline_ns_ = 0;
};

}  // namespace window
}  // namespace blox7

#endif  // BLOX7_WINDOW_INPUT_PUMP_PACER_H_
