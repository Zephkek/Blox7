#ifndef BLOX7_RUNTIME_GAME_SESSION_COORDINATOR_H_
#define BLOX7_RUNTIME_GAME_SESSION_COORDINATOR_H_

#include <cstdint>
#include <mutex>
#include <string>

#include "blox7/status.h"

namespace blox7 {
namespace runtime {

enum class GameSessionPrincipalKind {
  kLocalGuest,
  kAuthenticated,
};

struct GameSessionPrincipal {
  GameSessionPrincipalKind kind = GameSessionPrincipalKind::kLocalGuest;
  uint64_t generation = 0;
  std::string principal_id;
  std::string base_url;
};

struct GameJoinRequest {
  uint64_t request_id = 0;
  int64_t place_id = 0;
  std::string launch_parameters;
};

struct GameSurface {
  uint64_t generation = 0;
  uintptr_t native_window = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  float dpi_scale = 0.0f;
};

enum class GamePresentBackend {
  kUnknown,
  kVulkan,
  kOpenGlEs,
};

struct GamePresentEvidence {
  uint64_t surface_generation = 0;
  uint64_t frame_serial = 0;
  GamePresentBackend backend = GamePresentBackend::kUnknown;
  bool present_succeeded = false;
};

using SetGameForegroundFn = Status (*)(void* context);
using StartGameFn = Status (*)(void* context,
                               const GameSessionPrincipal& principal,
                               const GameJoinRequest& request,
                               const GameSurface& surface);
using UpdateGameSurfaceFn = Status (*)(void* context,
                                       const GameSurface& surface);
using PauseGameFn = Status (*)(void* context);
using ResumeGameFn = Status (*)(void* context, const GameSurface& surface);
using LeaveGameFn = Status (*)(void* context);
using ShutdownGameRuntimeFn = Status (*)(void* context);

struct GameSessionCapabilities {
  void* context = nullptr;
  SetGameForegroundFn set_game_foreground = nullptr;
  StartGameFn start_game = nullptr;
  UpdateGameSurfaceFn update_surface = nullptr;
  PauseGameFn pause_game = nullptr;
  ResumeGameFn resume_game = nullptr;
  LeaveGameFn leave_game = nullptr;
  ShutdownGameRuntimeFn shutdown_runtime = nullptr;

  bool complete() const;
};

struct GameSessionRequirements {
  GamePresentBackend required_present_backend = GamePresentBackend::kVulkan;
};

enum class GameSessionState {
  kCreated,
  kPrincipalReady,
  kJoinRequestQueued,
  kPreparingSurface,
  kSurfaceCreated,
  kSurfaceAvailable,
  kStarting,
  kRunning,
  kUpdatingSurface,
  kPausing,
  kPaused,
  kResuming,
  kLeaving,
  kReturnedToLuaApp,
  kFailure,
  kStopping,
  kStopped,
};

enum class GameSessionUpdateStatus {
  kApplied,
  kDuplicate,
  kRejected,
};

struct GameSessionUpdateResult {
  GameSessionUpdateStatus status = GameSessionUpdateStatus::kRejected;
  GameSessionState state = GameSessionState::kCreated;
  Status cause = Status::Ok();
  std::string message;

  bool ok() const { return status != GameSessionUpdateStatus::kRejected; }
  bool changed() const { return status == GameSessionUpdateStatus::kApplied; }
};

struct GameSessionSnapshot {
  GameSessionState state = GameSessionState::kCreated;
  uint64_t revision = 0;
  bool principal_ready = false;
  GameSessionPrincipalKind principal_kind =
      GameSessionPrincipalKind::kLocalGuest;
  bool join_request_queued = false;
  bool surface_created = false;
  bool pending_surface_available = false;
  bool active_surface_available = false;
  bool initial_start_completed = false;
  bool game_running = false;
  bool game_paused = false;
  bool game_present_pending = false;
  bool game_presented = false;
  bool returned_to_lua_app = false;
  bool stopped = false;
  uint64_t principal_generation = 0;
  uint64_t request_id = 0;
  uint64_t prepared_surface_generation = 0;
  uint64_t pending_surface_generation = 0;
  uint64_t active_surface_generation = 0;
  uint32_t active_surface_width = 0;
  uint32_t active_surface_height = 0;
  uint64_t first_presented_frame = 0;
  std::string failure_reason;
};

class GameSessionCoordinator final {
 public:
  explicit GameSessionCoordinator(GameSessionCapabilities capabilities,
                                  GameSessionRequirements requirements = {});

  GameSessionCoordinator(const GameSessionCoordinator&) = delete;
  GameSessionCoordinator& operator=(const GameSessionCoordinator&) = delete;
  GameSessionCoordinator(GameSessionCoordinator&&) = delete;
  GameSessionCoordinator& operator=(GameSessionCoordinator&&) = delete;

  GameSessionUpdateResult SetPrincipal(GameSessionPrincipal principal);
  GameSessionUpdateResult QueueJoinRequest(GameJoinRequest request);
  GameSessionUpdateResult SurfaceCreated(uint64_t surface_generation);
  GameSessionUpdateResult SurfaceChanged(GameSurface surface);
  GameSessionUpdateResult RebindSurface(GameSurface surface);
  GameSessionUpdateResult SurfaceDestroyed(uint64_t surface_generation);
  GameSessionUpdateResult RecordGamePresent(GamePresentEvidence evidence);

  GameSessionUpdateResult LeaveGame();

  GameSessionUpdateResult Shutdown();

  GameSessionSnapshot Snapshot() const;

 private:
  GameSessionUpdateResult StartIfReadyLocked(std::unique_lock<std::mutex>* lock,
                                             const char* accepted_message);
  GameSessionUpdateResult UpdateSurface(GameSurface surface, bool force_rebind);
  GameSessionUpdateResult ApplyLocked(GameSessionState state,
                                      std::string message);
  GameSessionUpdateResult DuplicateLocked(std::string message) const;
  GameSessionUpdateResult FailLocked(Status cause, std::string message);
  GameSessionUpdateResult RejectLocked(Status cause, std::string message) const;
  GameSessionUpdateResult RejectTerminalLocked(std::string operation) const;
  GameSessionSnapshot SnapshotLocked() const;
  GameSessionState PreStartStateLocked() const;
  bool IsForwardTerminalLocked() const;
  bool IsPresentShapeValidLocked(const GamePresentEvidence& evidence) const;
  void PromotePendingSurfaceLocked();
  void DiscardPendingSurfaceLocked();
  void CommitPendingPresentLocked();
  void DiscardPendingPresentLocked();

  static bool SamePrincipal(const GameSessionPrincipal& left,
                            const GameSessionPrincipal& right);
  static bool SameRequest(const GameJoinRequest& left,
                          const GameJoinRequest& right);
  static bool SameSurface(const GameSurface& left, const GameSurface& right);

  mutable std::mutex operation_mutex_;
  mutable std::mutex mutex_;
  const GameSessionCapabilities capabilities_;
  const GameSessionRequirements requirements_;

  GameSessionState state_ = GameSessionState::kCreated;
  uint64_t revision_ = 0;
  bool principal_ready_ = false;
  bool join_request_queued_ = false;
  bool surface_created_ = false;
  bool pending_surface_available_ = false;
  bool active_surface_available_ = false;
  bool initial_start_completed_ = false;
  bool game_paused_ = false;
  bool pending_present_ = false;
  bool game_presented_ = false;
  bool returned_to_lua_app_ = false;
  bool shutdown_attempted_ = false;
  uint64_t prepared_surface_generation_ = 0;
  uint64_t last_destroyed_surface_generation_ = 0;
  GameSessionPrincipal principal_;
  GameJoinRequest request_;
  GameSurface pending_surface_;
  GameSurface active_surface_;
  GamePresentEvidence pending_present_evidence_;
  uint64_t first_presented_frame_ = 0;
  std::string failure_reason_;
};

const char* GameSessionStateName(GameSessionState state);
const char* GamePresentBackendName(GamePresentBackend backend);
const char* GameSessionPrincipalKindName(GameSessionPrincipalKind kind);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_GAME_SESSION_COORDINATOR_H_
