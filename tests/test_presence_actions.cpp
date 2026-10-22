#include "service/presence_actions.hpp"
#include <gtest/gtest.h>
#include <future>

TEST(PresenceActionsTest, LocksWithoutWaitingForClientWorkOrWakeEnabled) {
  Configuration config;
  config.lock_enabled = true;
  config.wake_enabled = false;
  config.lock_timeout_seconds = 0;
  std::promise<void> locked;
  auto future = locked.get_future();
  PresenceActions actions(config, [] { FAIL() << "Wake is disabled"; }, [&] { locked.set_value(); });
  actions.observe(true, 100);
  actions.observe(false, 0);
  // No socket-loop iteration or authentication completion is needed to lock.
  EXPECT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
}

TEST(PresenceActionsTest, ReturnCancelsPendingLockAndNextAbsenceRestartsIt) {
  Configuration config;
  config.lock_enabled = true;
  config.lock_timeout_seconds = 1;
  std::promise<void> locked;
  auto future = locked.get_future();
  PresenceActions actions(config, [] {}, [&] { locked.set_value(); });
  actions.observe(true, 100);
  actions.observe(false, 0);
  actions.observe(true, 100);
  EXPECT_EQ(future.wait_for(std::chrono::milliseconds(1100)), std::future_status::timeout);
  actions.observe(false, 0);
  EXPECT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
}
