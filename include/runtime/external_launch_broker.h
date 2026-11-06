#ifndef BLOX7_RUNTIME_EXTERNAL_LAUNCH_BROKER_H_
#define BLOX7_RUNTIME_EXTERNAL_LAUNCH_BROKER_H_

#include <cstddef>
#include <filesystem>
#include <memory>

#include "blox7/status.h"
#include "runtime/roblox_experience_launch_bridge.h"

namespace blox7 {
namespace runtime {

inline constexpr std::size_t kMaximumPendingExternalLaunches = 8;

struct ExternalLaunchBrokerOptions {
  std::filesystem::path socket_path;
  std::size_t maximum_pending_launches = kMaximumPendingExternalLaunches;
  int forwarding_timeout_ms = 30000;
};

using DispatchExternalLaunchFn =
    Status (*)(void* context, const RobloxExperienceLaunchRequest& request);

struct ExternalLaunchSink {
  void* context = nullptr;
  DispatchExternalLaunchFn dispatch = nullptr;

  bool valid() const { return dispatch != nullptr; }
};

Status ResolveExternalLaunchSocketPath(bool owner,
                                       std::filesystem::path* socket_path);

class ExternalLaunchBroker final {
 public:
  ~ExternalLaunchBroker();

  ExternalLaunchBroker(const ExternalLaunchBroker&) = delete;
  ExternalLaunchBroker& operator=(const ExternalLaunchBroker&) = delete;
  ExternalLaunchBroker(ExternalLaunchBroker&&) = delete;
  ExternalLaunchBroker& operator=(ExternalLaunchBroker&&) = delete;

  static Status StartOwnerAfterLockAcquired(
      ExternalLaunchBrokerOptions options,
      std::shared_ptr<ExternalLaunchBroker>* broker,
      const RobloxExperienceLaunchRequest* initial_request = nullptr);

  static Status ForwardToOwner(const ExternalLaunchBrokerOptions& options,
                               const RobloxExperienceLaunchRequest& request);

  Status QueueInitialRequest(RobloxExperienceLaunchRequest request);

  Status Drain(const ExternalLaunchSink& sink, std::size_t maximum_requests);

  Status Shutdown();
  std::size_t pending_launch_count() const;
  const std::filesystem::path& socket_path() const;

 private:
  class Impl;
  explicit ExternalLaunchBroker(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

Status InstallActiveExternalLaunchBroker(
    const std::shared_ptr<ExternalLaunchBroker>& broker);
void ClearActiveExternalLaunchBroker(const ExternalLaunchBroker* broker);
std::shared_ptr<ExternalLaunchBroker> GetActiveExternalLaunchBroker();
Status DrainActiveExternalLaunchRequests(const ExternalLaunchSink& sink,
                                         std::size_t maximum_requests);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_EXTERNAL_LAUNCH_BROKER_H_
