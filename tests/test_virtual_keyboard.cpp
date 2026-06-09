#include "../src/VirtualKeyboard.hpp"
#include <gtest/gtest.h>

class VirtualKeyboardTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

// In an unprivileged test environment (like standard CI), /dev/uinput will fail to open
// or setup due to permission errors. This test verifies that the class handles
// initialization failures gracefully without crashing.
TEST_F(VirtualKeyboardTest, HandlesInitializationFailureGracefully) {
  VirtualKeyboard vkb;

  // Unless the test is run as root with the uinput module loaded, this should return false.
  bool init_result = vkb.init();

  // If init failed, emit_wakeup must also fail gracefully.
  if (!init_result) {
    EXPECT_FALSE(vkb.emit_wakeup());
  } else {
    // If the test environment is privileged, it should emit without crashing.
    EXPECT_TRUE(vkb.emit_wakeup());
  }
}

TEST_F(VirtualKeyboardTest, MultipleInitCallsAreSafe) {
  VirtualKeyboard vkb;
  bool res1 = vkb.init();
  bool res2 = vkb.init();
  EXPECT_EQ(res1, res2); // Should not crash and should return same status
}
