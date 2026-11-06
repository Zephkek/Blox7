#ifndef BLOX7_WINDOW_WINDOW_STATE_STORE_H_
#define BLOX7_WINDOW_WINDOW_STATE_STORE_H_

#include <filesystem>

#include "blox7/status.h"

namespace blox7 {
namespace window {

inline constexpr int kMinimumWindowWidth = 160;
inline constexpr int kMinimumWindowHeight = 120;

struct PersistedWindowState {
  int x = 0;
  int y = 0;
  int width = 1280;
  int height = 720;
  bool has_position = false;
  bool fullscreen = false;
  bool maximized = false;
  int configured_width = 0;
  int configured_height = 0;
  bool has_configured_size = false;
};

inline bool RestoredSizeApplies(const PersistedWindowState& state,
                                int configured_width, int configured_height) {
  return state.has_configured_size &&
         state.configured_width == configured_width &&
         state.configured_height == configured_height;
}

struct WindowStartupPresentationPlan {
  bool fullscreen_at_creation = false;
  bool maximize_after_constraints = false;
};

WindowStartupPresentationPlan PlanWindowStartupPresentation(
    const PersistedWindowState& state);

struct WindowStateLoadResult {
  bool found = false;
  PersistedWindowState state;
  Status status;

  explicit operator bool() const { return status.ok(); }
};

WindowStateLoadResult LoadWindowState(const std::filesystem::path& path);
Status StoreWindowState(const std::filesystem::path& path,
                        const PersistedWindowState& state);

}  // namespace window
}  // namespace blox7

#endif  // BLOX7_WINDOW_WINDOW_STATE_STORE_H_
