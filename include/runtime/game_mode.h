#ifndef BLOX7_RUNTIME_GAME_MODE_H_
#define BLOX7_RUNTIME_GAME_MODE_H_

#include <string>
#include <string_view>

#include "blox7/status.h"

namespace blox7 {
namespace runtime {

enum class GameModePolicy {
  kAuto,
  kOn,
  kOff,
};

bool ParseGameModePolicy(std::string_view value, GameModePolicy* policy);
const char* GameModePolicyName(GameModePolicy policy);

enum class GameModeSessionState {
  kDisabled,
  kUnavailable,
  kRequestFailed,
  kAlreadyActive,
  kActive,
  kStopped,
  kStopFailed,
};

const char* GameModeSessionStateName(GameModeSessionState state);

struct GameModeClientApi {
  using RequestFn = int (*)();
  using QueryFn = int (*)();
  using ErrorFn = const char* (*)();

  RequestFn request_start = nullptr;
  RequestFn request_end = nullptr;
  QueryFn query_status = nullptr;
  ErrorFn error_string = nullptr;
};

class GameModeSession final {
 public:
  GameModeSession() = default;
  ~GameModeSession();

  GameModeSession(const GameModeSession&) = delete;
  GameModeSession& operator=(const GameModeSession&) = delete;
  GameModeSession(GameModeSession&& other) noexcept;
  GameModeSession& operator=(GameModeSession&& other) = delete;

  static GameModeSession Start(GameModePolicy policy);
  static GameModeSession StartWithClientForTesting(
      GameModePolicy policy, GameModeClientApi api);

  GameModeSessionState state() const { return state_; }
  const std::string& detail() const { return detail_; }
  bool active() const {
    return state_ == GameModeSessionState::kActive ||
           state_ == GameModeSessionState::kAlreadyActive;
  }
  bool owns_request() const { return owns_request_; }

  Status Stop();

 private:
  static GameModeSession StartBound(GameModePolicy policy,
                                    GameModeClientApi api,
                                    void* library_handle);
  void CloseLibrary();

  GameModeClientApi api_;
  void* library_handle_ = nullptr;
  GameModeSessionState state_ = GameModeSessionState::kDisabled;
  bool owns_request_ = false;
  std::string detail_;
};

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_GAME_MODE_H_
