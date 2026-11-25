#ifndef BLOX7_AUDIO_HOST_AUDIO_ROUTES_H_
#define BLOX7_AUDIO_HOST_AUDIO_ROUTES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "blox7/status.h"

namespace blox7::audio {

enum class HostAudioDirection { kOutput = 0, kInput = 1 };

struct HostAudioRoute {
  std::uint32_t device_id = 0;
  std::string name;
  std::string guid;
};

class HostAudioRoutes final {
 public:
  HostAudioRoutes() = default;
  ~HostAudioRoutes();

  HostAudioRoutes(const HostAudioRoutes&) = delete;
  HostAudioRoutes& operator=(const HostAudioRoutes&) = delete;

  Status Start(bool with_input);
  void Stop();

  std::size_t Count(HostAudioDirection direction);
  bool Route(HostAudioDirection direction, std::size_t index,
             HostAudioRoute* route) const;
  std::size_t Current(HostAudioDirection direction) const;
  void Select(HostAudioDirection direction, std::size_t index);
  bool Refresh(bool last_try);

 private:
  struct Side {
    bool enabled = false;
    bool fresh = false;
    bool asked = false;
    std::vector<HostAudioRoute> shown;
    std::vector<HostAudioRoute> latest;
    std::vector<std::uint32_t> lost;
    std::string wanted_guid;
    std::string wanted_name;
    std::uint32_t wanted_id = 0;
    std::uint32_t active = 0;
  };

  struct RateTry {
    int opened_hz = 0;
    int native_hz = 0;
  };

  static bool OnDevicesChanged(void* context, bool last_try);
  static std::uint32_t Resolve(Side* side);
  Status Apply(HostAudioDirection direction, std::uint32_t target);
  void Settle(HostAudioDirection direction, const std::string& guid,
              bool keep_wish);
  bool MatchOutputRate();

  mutable std::mutex mutex_;
  std::mutex switch_mutex_;
  std::array<Side, 2> sides_;
  RateTry rate_try_;
  bool started_ = false;
};

}  // namespace blox7::audio

#endif  // BLOX7_AUDIO_HOST_AUDIO_ROUTES_H_
