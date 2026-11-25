#include "blox7/audio/host_audio_routes.h"

#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <string_view>
#include <utility>

#include "blox7/audio/host_default_endpoint.h"
#include "blox7/audio/sdl_audio_capture.h"
#include "blox7/audio/sdl_audio_sink.h"

namespace blox7::audio {
namespace {

constexpr std::size_t kMaximumRoutes = 128;
constexpr std::size_t kMaximumNameBytes = 512;
constexpr std::size_t kMaximumLost = 32;
constexpr int kLowestOpenRateHz = 44100;

std::size_t Slot(HostAudioDirection direction) {
  return static_cast<std::size_t>(direction);
}

const char* DirectionName(HostAudioDirection direction) {
  return direction == HostAudioDirection::kOutput ? "output" : "input";
}

std::string DefaultGuid(HostAudioDirection direction) {
  return direction == HostAudioDirection::kOutput ? "blox7:default"
                                                  : "blox7:input:default";
}

std::string RouteGuid(HostAudioDirection direction, std::string_view name,
                      std::uint32_t ordinal) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : name) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  for (std::size_t shift = 0; shift < sizeof(ordinal) * CHAR_BIT;
       shift += CHAR_BIT) {
    hash ^= (ordinal >> shift) & 0xffU;
    hash *= 1099511628211ULL;
  }
  char encoded[24];
  std::snprintf(encoded, sizeof(encoded), "mt:%016" PRIx64, hash);
  return direction == HostAudioDirection::kOutput
             ? std::string(encoded)
             : "input:" + std::string(encoded);
}

template <typename Device>
std::vector<HostAudioRoute> BuildRoutes(HostAudioDirection direction,
                                        std::vector<Device> devices) {
  std::sort(devices.begin(), devices.end(),
            [](const Device& a, const Device& b) { return a.id < b.id; });
  std::vector<HostAudioRoute> routes;
  routes.reserve(std::min(devices.size() + 1, kMaximumRoutes));
  routes.push_back({0, "System Default", DefaultGuid(direction)});
  for (const Device& device : devices) {
    if (routes.size() == kMaximumRoutes) break;
    if (device.id == 0 || device.name.empty() ||
        device.name.size() > kMaximumNameBytes) {
      continue;
    }
    std::uint32_t ordinal = 0;
    for (std::size_t i = 1; i < routes.size(); ++i) {
      if (routes[i].name == device.name) ++ordinal;
    }
    routes.push_back(
        {device.id, device.name, RouteGuid(direction, device.name, ordinal)});
  }
  return routes;
}

Status ListRoutes(HostAudioDirection direction,
                  std::vector<HostAudioRoute>* routes) {
  if (direction == HostAudioDirection::kOutput) {
    std::vector<SdlPlaybackDevice> devices;
    Status status = ListSdlPlaybackDevices(&devices);
    if (!status.ok()) return status;
    *routes = BuildRoutes(direction, std::move(devices));
    return Status::Ok();
  }
  std::vector<SdlRecordingDevice> devices;
  Status status = ListSdlRecordingDevices(&devices);
  if (!status.ok()) return status;
  *routes = BuildRoutes(direction, std::move(devices));
  return Status::Ok();
}

bool SameRoutes(const std::vector<HostAudioRoute>& a,
                const std::vector<HostAudioRoute>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].device_id != b[i].device_id || a[i].guid != b[i].guid) {
      return false;
    }
  }
  return true;
}

const HostAudioRoute* FindGuid(const std::vector<HostAudioRoute>& routes,
                               const std::string& guid) {
  for (const HostAudioRoute& route : routes) {
    if (route.guid == guid) return &route;
  }
  return nullptr;
}

const HostAudioRoute* FindDevice(const std::vector<HostAudioRoute>& routes,
                                 std::uint32_t device_id) {
  for (const HostAudioRoute& route : routes) {
    if (route.device_id == device_id) return &route;
  }
  return nullptr;
}

void NoteChanges(HostAudioDirection direction,
                 const std::vector<HostAudioRoute>& before,
                 const std::vector<HostAudioRoute>& after,
                 std::vector<std::uint32_t>* lost) {
  for (const HostAudioRoute& route : before) {
    if (route.device_id == 0 ||
        FindDevice(after, route.device_id) != nullptr) {
      continue;
    }
    std::fprintf(stderr, "  [audio-menu] host %s removed: %s\n",
                 DirectionName(direction), route.name.c_str());
    if (lost->size() == kMaximumLost) lost->erase(lost->begin());
    lost->push_back(route.device_id);
  }
  for (const HostAudioRoute& route : after) {
    if (route.device_id == 0 ||
        FindDevice(before, route.device_id) != nullptr) {
      continue;
    }
    std::fprintf(stderr, "  [audio-menu] host %s added: %s\n",
                 DirectionName(direction), route.name.c_str());
  }
}

std::uint32_t ConfiguredDevice(HostAudioDirection direction) {
  std::uint32_t device_id = 0;
  std::string name;
  const Status status =
      direction == HostAudioDirection::kOutput
          ? GetConfiguredSdlPlaybackDevice(&device_id, &name)
          : GetConfiguredSdlRecordingDevice(&device_id, &name);
  return status.ok() ? device_id : 0;
}

bool HostAgrees(HostAudioDirection direction, bool asked,
                const std::string& target_name) {
  const bool recording = direction == HostAudioDirection::kInput;
  const std::string sdl_default = SdlDefaultAudioDeviceName(recording);
  std::string host_default;
  int host_rate = 0;
  if (!HostDefaultAudioEndpoint(recording, &host_default, &host_rate)) {
    return asked || sdl_default == target_name;
  }
  return !host_default.empty() && host_default == sdl_default;
}

}  // namespace

HostAudioRoutes::~HostAudioRoutes() { Stop(); }

std::uint32_t HostAudioRoutes::Resolve(Side* side) {
  const HostAudioRoute* route = side->wanted_id != 0
                                    ? FindDevice(side->latest, side->wanted_id)
                                    : nullptr;
  if (route == nullptr) {
    route = FindGuid(side->latest, side->wanted_guid);
  }
  if (route == nullptr) {
    return 0;
  }
  side->wanted_id = route->device_id;
  side->wanted_guid = route->guid;
  side->wanted_name = route->name;
  return route->device_id;
}

Status HostAudioRoutes::Start(bool with_input) {
  std::array<std::vector<HostAudioRoute>, 2> lists;
  std::array<std::uint32_t, 2> configured{};
  std::array<std::string, 2> names;
  Status status = ListRoutes(HostAudioDirection::kOutput, &lists[0]);
  if (status.ok()) {
    status = GetConfiguredSdlPlaybackDevice(&configured[0], &names[0]);
  }
  if (status.ok() && with_input) {
    status = ListRoutes(HostAudioDirection::kInput, &lists[1]);
  }
  if (status.ok() && with_input) {
    status = GetConfiguredSdlRecordingDevice(&configured[1], &names[1]);
  }
  if (!status.ok()) return status;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t slot = 0; slot < sides_.size(); ++slot) {
      const auto direction = static_cast<HostAudioDirection>(slot);
      Side& side = sides_[slot];
      side = Side{};
      side.enabled = direction == HostAudioDirection::kOutput || with_input;
      if (!side.enabled) continue;
      side.shown = lists[slot];
      side.latest = std::move(lists[slot]);
      side.active = configured[slot];
      side.wanted_id = configured[slot];
      side.wanted_name = names[slot];
      side.wanted_guid = names[slot].empty() || names[slot] == "default"
                             ? DefaultGuid(direction)
                             : RouteGuid(direction, names[slot], 0);
    }
    rate_try_ = {};
    started_ = true;
  }
  SetSdlAudioDeviceListener(&OnDevicesChanged, this);
  RequestSdlAudioDeviceRefresh();
  return Status::Ok();
}

void HostAudioRoutes::Stop() {
  SetSdlAudioDeviceListener(nullptr, nullptr);
  std::lock_guard<std::mutex> switching(switch_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  started_ = false;
  sides_ = {};
}

bool HostAudioRoutes::OnDevicesChanged(void* context, bool last_try) {
  return static_cast<HostAudioRoutes*>(context)->Refresh(last_try);
}

std::size_t HostAudioRoutes::Count(HostAudioDirection direction) {
  std::lock_guard<std::mutex> lock(mutex_);
  Side& side = sides_[Slot(direction)];
  if (!started_ || !side.enabled) return 0;
  if (side.fresh) {
    side.shown = side.latest;
    side.fresh = false;
  }
  return side.shown.size();
}

bool HostAudioRoutes::Route(HostAudioDirection direction, std::size_t index,
                            HostAudioRoute* route) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Side& side = sides_[Slot(direction)];
  if (!started_ || !side.enabled || index >= side.shown.size()) return false;
  *route = side.shown[index];
  return true;
}

std::size_t HostAudioRoutes::Current(HostAudioDirection direction) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Side& side = sides_[Slot(direction)];
  if (!started_ || !side.enabled) return 0;
  for (std::size_t i = 0; i < side.shown.size(); ++i) {
    if (side.shown[i].guid == side.wanted_guid) return i;
  }
  return 0;
}

Status HostAudioRoutes::Apply(HostAudioDirection direction,
                              std::uint32_t target) {
  std::string resolved_name;
  const Status status =
      direction == HostAudioDirection::kOutput
          ? SwitchSdlPlaybackDevice(target, &resolved_name)
          : SwitchSdlRecordingDevice(target, &resolved_name);
  if (status.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    sides_[Slot(direction)].active = target;
  }
  return status;
}

void HostAudioRoutes::Settle(HostAudioDirection direction,
                             const std::string& guid, bool keep_wish) {
  std::lock_guard<std::mutex> lock(mutex_);
  Side& side = sides_[Slot(direction)];
  if (side.wanted_guid != guid) return;
  side.asked = false;
  if (keep_wish || side.latest.empty()) return;
  const HostAudioRoute* route =
      side.active != 0 ? FindDevice(side.latest, side.active) : nullptr;
  if (route == nullptr) {
    route = &side.latest.front();
  }
  side.wanted_id = route->device_id;
  side.wanted_guid = route->guid;
  side.wanted_name = route->name;
}

void HostAudioRoutes::Select(HostAudioDirection direction,
                             std::size_t index) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Side& side = sides_[Slot(direction)];
    if (!started_ || !side.enabled || index >= side.shown.size()) return;
    const HostAudioRoute& chosen = side.shown[index];
    side.wanted_id = chosen.device_id;
    side.wanted_guid = chosen.guid;
    side.wanted_name = chosen.name;
    side.asked = true;
  }
  RequestSdlAudioDeviceRefresh();
}

bool HostAudioRoutes::Refresh(bool last_try) {
  std::lock_guard<std::mutex> switching(switch_mutex_);
  std::array<bool, 2> enabled{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return true;
    enabled = {sides_[0].enabled, sides_[1].enabled};
  }
  std::array<std::vector<HostAudioRoute>, 2> lists;
  std::array<bool, 2> listed{};
  std::array<std::uint32_t, 2> configured{};
  for (std::size_t slot = 0; slot < lists.size(); ++slot) {
    if (!enabled[slot]) continue;
    const auto direction = static_cast<HostAudioDirection>(slot);
    listed[slot] = ListRoutes(direction, &lists[slot]).ok();
    configured[slot] = ConfiguredDevice(direction);
  }

  struct Move {
    bool due = false;
    bool waits = false;
    bool asked = false;
    bool again = false;
    bool wants_default = false;
    std::uint32_t target = 0;
    std::string name;
    std::string guid;
  };
  std::array<Move, 2> moves;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return true;
    for (std::size_t slot = 0; slot < sides_.size(); ++slot) {
      Side& side = sides_[slot];
      if (!side.enabled) continue;
      const auto direction = static_cast<HostAudioDirection>(slot);
      side.active = configured[slot];
      if (listed[slot] && !SameRoutes(side.latest, lists[slot])) {
        NoteChanges(direction, side.latest, lists[slot], &side.lost);
        side.latest = std::move(lists[slot]);
        side.fresh = true;
      }
      side.lost.erase(std::remove_if(side.lost.begin(), side.lost.end(),
                                     &SdlAudioDeviceGone),
                      side.lost.end());
      Move& move = moves[slot];
      move.target = Resolve(&side);
      move.name = side.wanted_name;
      move.guid = side.wanted_guid;
      move.asked = side.asked;
      move.wants_default = side.wanted_guid == DefaultGuid(direction);
      move.waits = !side.lost.empty();
      if (move.target != 0 && side.active != 0 &&
          side.active != move.target &&
          FindDevice(side.latest, side.active) == nullptr) {
        move.target = 0;
        move.again = true;
      }
      move.due = move.target != side.active;
      if (!move.due) side.asked = false;
    }
  }

  bool settled = true;
  for (std::size_t slot = 0; slot < moves.size(); ++slot) {
    const Move& move = moves[slot];
    if (!move.due) continue;
    const auto direction = static_cast<HostAudioDirection>(slot);
    const bool ready =
        move.target != 0
            ? !move.waits && HostAgrees(direction, move.asked, move.name)
            : SdlDefaultOpenIsSafe(direction == HostAudioDirection::kInput);
    if (!ready && !last_try) {
      settled = false;
      continue;
    }
    const Status status = Apply(direction, move.target);
    if (!status.ok()) {
      settled = false;
      std::fprintf(stderr, "  [audio-menu] cannot move host %s to '%s': %s\n",
                   DirectionName(direction),
                   move.target == 0 ? "System Default" : move.name.c_str(),
                   status.message().c_str());
      if (last_try) Settle(direction, move.guid, !move.asked);
      continue;
    }
    if (move.target != 0 && move.asked) {
      std::fprintf(stderr, "  [audio-menu] selected host %s=%s\n",
                   DirectionName(direction), move.name.c_str());
    } else if (move.target != 0) {
      std::fprintf(stderr, "  [audio-menu] host %s '%s' is back\n",
                   DirectionName(direction), move.name.c_str());
    } else if (move.wants_default) {
      std::fprintf(stderr, "  [audio-menu] selected host %s=default\n",
                   DirectionName(direction));
    } else {
      std::fprintf(
          stderr, "  [audio-menu] host %s '%s' is gone, using System Default\n",
          DirectionName(direction), move.name.c_str());
    }
    if (move.again) {
      settled = false;
    } else {
      Settle(direction, move.guid, true);
    }
  }
  return settled && MatchOutputRate();
}

bool HostAudioRoutes::MatchOutputRate() {
  std::string host_default;
  int host_rate = 0;
  if (!HostDefaultAudioEndpoint(false, &host_default, &host_rate) ||
      host_default.empty() || host_rate <= 0) {
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const Side& side = sides_[Slot(HostAudioDirection::kOutput)];
    if (!started_ || !side.enabled) return true;
    if (!side.lost.empty()) return false;
    const HostAudioRoute* route =
        side.active != 0 ? FindDevice(side.latest, side.active) : nullptr;
    if (side.active != 0 && (route == nullptr || route->name != host_default)) {
      return true;
    }
  }
  if (host_default != SdlDefaultAudioDeviceName(false)) return false;
  const int opened = SdlPlaybackOpenRate();
  const int native = std::max(kLowestOpenRateHz, host_rate);
  if (opened <= 0 || opened == native ||
      (rate_try_.opened_hz == opened && rate_try_.native_hz == native)) {
    return true;
  }
  const Status status = ReopenSdlPlaybackDevice();
  rate_try_ = {SdlPlaybackOpenRate(), native};
  if (!status.ok()) {
    std::fprintf(stderr, "  [audio-menu] cannot reopen host output: %s\n",
                 status.message().c_str());
    return false;
  }
  std::fprintf(stderr,
               "  [audio-menu] host output reopened at %d Hz, was %d Hz\n",
               rate_try_.opened_hz, opened);
  return true;
}

}  // namespace blox7::audio
