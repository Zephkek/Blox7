#include "window/window_pointer_capture_owner.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace blox7 {
namespace window {
namespace {

class FakeBackend final : public PointerCaptureBackend {
 public:
  bool Apply(const PointerCaptureRequest& request) override {
    calls.push_back(request);
    return succeeds;
  }
  std::optional<PointerPosition> TakeWarp() override {
    std::optional<PointerPosition> taken = warp;
    warp.reset();
    return taken;
  }

  using Call = PointerCaptureRequest;
  std::vector<Call> calls;
  bool succeeds = true;
  std::optional<PointerPosition> warp;
};

struct QueryState {
  bool succeeds = true;
  bool locked_center = false;
};

bool Query(void* context, bool* locked_center) {
  auto* state = static_cast<QueryState*>(context);
  if (state == nullptr || locked_center == nullptr || !state->succeeds) {
    return false;
  }
  *locked_center = state->locked_center;
  return true;
}

struct ReentrantQueryState {
  WindowPointerCaptureOwner* owner = nullptr;
};

bool ClearQueryReentrantly(void* context, bool* locked_center) {
  auto* state = static_cast<ReentrantQueryState*>(context);
  if (state == nullptr || state->owner == nullptr || locked_center == nullptr) {
    return false;
  }
  *locked_center = false;
  state->owner->ClearQuery();
  return true;
}

TEST(WindowPointerCaptureOwnerTest, FollowsNativeMouseLockState) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  query.locked_center = true;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  query.locked_center = false;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
}

TEST(WindowPointerCaptureOwnerTest, TextInputPreservesNativeShiftLock) {
  FakeBackend backend;
  QueryState query{true, true};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.Pump(true));
  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());

  query.locked_center = false;
  EXPECT_TRUE(owner.Pump(true));
  EXPECT_FALSE(owner.captured());
  query.locked_center = true;
  EXPECT_TRUE(owner.Pump(true));
  EXPECT_TRUE(owner.captured());

  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_TRUE(owner.OnFocusGained(true));
  EXPECT_TRUE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, QueryOrCaptureFailureStaysReleased) {
  FakeBackend backend;
  backend.succeeds = false;
  QueryState query{true, true};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  EXPECT_FALSE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_EQ(backend.calls.size(), 2U);
  query.succeeds = false;
  backend.succeeds = true;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  ASSERT_EQ(backend.calls.size(), 3U);
  EXPECT_FALSE(backend.calls.back().relative_mode);
}

TEST(WindowPointerCaptureOwnerTest, QueryCanClearItselfWithoutDeadlock) {
  FakeBackend backend;
  WindowPointerCaptureOwner owner(&backend);
  ReentrantQueryState query{&owner};
  ASSERT_TRUE(owner.RegisterQuery(ClearQueryReentrantly, &query));

  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());

  QueryState replacement;
  EXPECT_TRUE(owner.RegisterQuery(Query, &replacement));
}

TEST(WindowPointerCaptureOwnerTest, RightDragCapturesUntilButtonRelease) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());

  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
}

TEST(WindowPointerCaptureOwnerTest, RightDragRetainsItsEventAnchorAcrossTextFocus) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.OnRightButton(true, false, PointerPosition{25.0f, 40.0f}));
  ASSERT_TRUE(backend.calls.back().anchor.has_value());
  EXPECT_FLOAT_EQ(backend.calls.back().anchor->x, 25.0f);
  EXPECT_FLOAT_EQ(backend.calls.back().anchor->y, 40.0f);

  ASSERT_TRUE(owner.Pump(true));
  EXPECT_FALSE(owner.captured());
  ASSERT_TRUE(owner.Pump(false));
  ASSERT_TRUE(backend.calls.back().anchor.has_value());
  EXPECT_FLOAT_EQ(backend.calls.back().anchor->x, 25.0f);
  EXPECT_FLOAT_EQ(backend.calls.back().anchor->y, 40.0f);

  ASSERT_TRUE(owner.OnRightButton(false, false));
  query.locked_center = true;
  ASSERT_TRUE(owner.Pump(false));
  EXPECT_FALSE(backend.calls.back().anchor.has_value());
}

TEST(WindowPointerCaptureOwnerTest, RightDragDoesNotReplaceANativeLockAnchor) {
  FakeBackend backend;
  QueryState query{true, true};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));
  ASSERT_TRUE(owner.OnRightButton(true, false, PointerPosition{900.0f, 800.0f}));
  ASSERT_TRUE(owner.OnRightButton(false, false));
  EXPECT_TRUE(owner.captured());
  query.locked_center = false;
  ASSERT_TRUE(owner.Pump(false));
  EXPECT_FALSE(backend.calls.back().anchor.has_value());
}

TEST(WindowPointerCaptureOwnerTest, FocusLossAndShutdownCancelCursorRestoration) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.OnRightButton(true, false, PointerPosition{25.0f, 40.0f}));
  ASSERT_TRUE(owner.OnFocusLost());
  EXPECT_FALSE(backend.calls.back().restore_position);
  EXPECT_FALSE(backend.calls.back().anchor.has_value());

  query.locked_center = true;
  ASSERT_TRUE(owner.OnFocusGained(false));
  EXPECT_FALSE(backend.calls.back().anchor.has_value());
  ASSERT_TRUE(owner.Shutdown());
  EXPECT_FALSE(backend.calls.back().restore_position);
}

TEST(WindowPointerCaptureOwnerTest, FailedReleaseIsRetriedWhileUnfocused) {
  FakeBackend backend;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.OnRightButton(true, false, PointerPosition{25.0f, 40.0f}));
  backend.succeeds = false;
  EXPECT_FALSE(owner.OnFocusLost());
  EXPECT_FALSE(backend.calls.back().restore_position);
  const auto calls_before_retry = backend.calls.size();
  backend.succeeds = true;
  EXPECT_TRUE(owner.Pump(false));
  ASSERT_EQ(backend.calls.size(), calls_before_retry + 1);
  EXPECT_FALSE(backend.calls.back().relative_mode);
  EXPECT_FALSE(backend.calls.back().restore_position);
}

TEST(WindowPointerCaptureOwnerTest,
     RightReleaseRestoresUnlockedStateWhenNoNativeLock) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  query.locked_center = true;
  ASSERT_TRUE(owner.OnRightButton(true, false));
  constexpr int kTenSecondsAt240Hz = 2400;
  for (int pump = 0; pump < kTenSecondsAt240Hz; ++pump) {
    ASSERT_TRUE(owner.Pump(false));
  }
  ASSERT_TRUE(owner.captured());

  query.locked_center = false;
  ASSERT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
}

TEST(WindowPointerCaptureOwnerTest,
     RightReleasePreservesNativeLockThatPredatedDrag) {
  FakeBackend backend;
  QueryState query{true, true};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  ASSERT_TRUE(owner.OnRightButton(true, false));
  ASSERT_TRUE(owner.OnRightButton(false, false));

  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
}

TEST(WindowPointerCaptureOwnerTest,
     ShiftLockActivatedDuringRightDragSurvivesButtonRelease) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  ASSERT_TRUE(owner.OnRightButton(true, false));
  query.locked_center = true;
  ASSERT_TRUE(owner.OnShiftKeyPressed(false));
  ASSERT_TRUE(owner.OnRightButton(false, false));

  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
}

TEST(WindowPointerCaptureOwnerTest,
     ShiftDuringRightDragDoesNotInventNativeMouseLock) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  ASSERT_TRUE(owner.OnRightButton(true, false));
  ASSERT_TRUE(owner.OnShiftKeyPressed(false));
  ASSERT_TRUE(owner.OnRightButton(false, false));

  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
}

TEST(WindowPointerCaptureOwnerTest, RightDragDoesNotDependOnNativeLockQuery) {
  FakeBackend backend;
  QueryState query{false, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
}

TEST(WindowPointerCaptureOwnerTest, TextAndFocusCancelRightDragCapture) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  EXPECT_TRUE(owner.OnRightButton(true, true));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_TRUE(owner.OnFocusGained(false));
  EXPECT_FALSE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, FocusGainHidesTheSystemCursorImmediately) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  ASSERT_TRUE(owner.Pump(false));
  ASSERT_FALSE(owner.cursor_visible());

  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_TRUE(owner.cursor_visible());

  const std::size_t calls_before = backend.calls.size();
  EXPECT_TRUE(owner.OnFocusGained(false));
  EXPECT_FALSE(owner.cursor_visible());
  ASSERT_GT(backend.calls.size(), calls_before);
  EXPECT_FALSE(backend.calls.back().cursor_visible);

  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_TRUE(owner.OnFocusGained(true));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
}

TEST(WindowPointerCaptureOwnerTest, NativeLockAsksForTheCenter) {
  FakeBackend backend;
  QueryState query{true, true};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));
  ASSERT_TRUE(owner.captured());
  EXPECT_TRUE(backend.calls.back().relative_mode);
  EXPECT_TRUE(backend.calls.back().center);

  const std::size_t calls = backend.calls.size();
  ASSERT_TRUE(owner.Pump(false));
  EXPECT_EQ(backend.calls.size(), calls);

  query.locked_center = false;
  ASSERT_TRUE(owner.Pump(false));
  EXPECT_FALSE(backend.calls.back().relative_mode);
  EXPECT_FALSE(backend.calls.back().center);
  EXPECT_TRUE(backend.calls.back().restore_position);
}

TEST(WindowPointerCaptureOwnerTest, RightDragKeepsItsPressPosition) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.OnRightButton(true, false, PointerPosition{25.0f, 40.0f}));
  ASSERT_TRUE(owner.captured());
  EXPECT_FALSE(backend.calls.back().center);
  ASSERT_TRUE(backend.calls.back().anchor.has_value());
  EXPECT_FLOAT_EQ(backend.calls.back().anchor->x, 25.0f);
}

TEST(WindowPointerCaptureOwnerTest, ShiftLockDuringRightDragMovesToTheCenterOnce) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.OnRightButton(true, false, PointerPosition{25.0f, 40.0f}));
  EXPECT_FALSE(backend.calls.back().center);

  query.locked_center = true;
  ASSERT_TRUE(owner.Pump(false));
  EXPECT_TRUE(backend.calls.back().relative_mode);
  EXPECT_TRUE(backend.calls.back().center);

  const std::size_t calls = backend.calls.size();
  ASSERT_TRUE(owner.OnRightButton(false, false));
  ASSERT_TRUE(owner.Pump(false));
  EXPECT_EQ(backend.calls.size(), calls);
  EXPECT_TRUE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, WarpsComeFromTheBackendOnce) {
  FakeBackend backend;
  WindowPointerCaptureOwner owner(&backend);
  EXPECT_FALSE(owner.TakeWarp().has_value());
  backend.warp = PointerPosition{640.0f, 360.0f};
  const std::optional<PointerPosition> warp = owner.TakeWarp();
  ASSERT_TRUE(warp.has_value());
  EXPECT_FLOAT_EQ(warp->x, 640.0f);
  EXPECT_FLOAT_EQ(warp->y, 360.0f);
  EXPECT_FALSE(owner.TakeWarp().has_value());
}

TEST(WindowPointerCaptureOwnerTest, TextInputWithoutNativeCursorShowsSystemCursor) {
  FakeBackend backend;
  QueryState query{false, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  EXPECT_TRUE(owner.Pump(true));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());

  EXPECT_TRUE(owner.OnRightButton(true, true));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());

  query.succeeds = true;
  EXPECT_TRUE(owner.Pump(true));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
}

}  // namespace
}  // namespace window
}  // namespace blox7
