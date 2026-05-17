#include "../src/pam/pam_config.hpp"

#include <gtest/gtest.h>
#include <sstream>

class PamConfigTest : public ::testing::Test {
protected:
  PamConfig loadConfig(const std::string &content) {
    PamConfigState state;
    std::stringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
      process_pam_config_line(line, state);
    }
    return resolve_pam_config(state);
  }
};

TEST_F(PamConfigTest, DefaultValueWhenEmpty) {
  auto config = loadConfig("");
  EXPECT_EQ(config.min_uid, linuxcampam::DEFAULT_MIN_UID);
}

TEST_F(PamConfigTest, DefaultValueWhenKeyMissing) {
  auto config = loadConfig("[General]\nfoo=bar");
  EXPECT_EQ(config.min_uid, linuxcampam::DEFAULT_MIN_UID);
}

TEST_F(PamConfigTest, SecurityPriorityPrecedence) {
  // [Security] section should always override [General]
  auto config = loadConfig(R"(
[General]
min_uid = 500
[Security]
min_uid = 2000
    )");
  EXPECT_EQ(config.min_uid, 2000);
}

TEST_F(PamConfigTest, SecurityPriorityPrecedenceReverseOrder) {
  // Order shouldn't matter; [Security] still wins
  auto config = loadConfig(R"(
[Security]
min_uid = 2000
[General]
min_uid = 500
    )");
  EXPECT_EQ(config.min_uid, 2000);
}

TEST_F(PamConfigTest, FirstFoundFallback) {
  // If no [Security] section, take the first valid entry
  auto config = loadConfig(R"(
[General]
min_uid = 500
[Other]
min_uid = 300
    )");
  EXPECT_EQ(config.min_uid, 500);
}

TEST_F(PamConfigTest, WhitespaceHandling) {
  auto config = loadConfig("min_uid =  1234  ");
  EXPECT_EQ(config.min_uid, 1234);
}

TEST_F(PamConfigTest, InvalidNumbersIgnored) {
  auto config = loadConfig("min_uid = abc");
  EXPECT_EQ(config.min_uid, linuxcampam::DEFAULT_MIN_UID);
}

TEST_F(PamConfigTest, InvalidNumbersFallbackToFirstValid) {
  // Skip invalid 'abc', use the valid 500
  auto config = loadConfig(R"(
min_uid = abc
min_uid = 500
)");
  EXPECT_EQ(config.min_uid, 500);
}

TEST_F(PamConfigTest, CommentsIgnored) {
  auto config = loadConfig(R"(
; min_uid = 500
# min_uid = 600
min_uid = 1000
)");
  EXPECT_EQ(config.min_uid, 1000);
}

TEST_F(PamConfigTest, NegativeNumbersIgnored) {
  // Negative values are invalid for UID
  auto config = loadConfig("min_uid = -5");
  EXPECT_EQ(config.min_uid, linuxcampam::DEFAULT_MIN_UID);
}

TEST_F(PamConfigTest, ExplicitZeroAllowed) {
  // 0 explicitly disables the check
  auto config = loadConfig("min_uid = 0");
  EXPECT_EQ(config.min_uid, 0);
}

TEST_F(PamConfigTest, WelcomeDefaults) {
  auto config = loadConfig("");
  EXPECT_TRUE(config.show_welcome);
  EXPECT_EQ(config.welcome_message, "LinuxCamPAM: Welcome, %u!");
}

TEST_F(PamConfigTest, WelcomeMessageParsing) {
  auto config = loadConfig(R"(
[Security]
show_welcome = false
welcome_message = "Hello, %u!"
)");
  EXPECT_FALSE(config.show_welcome);
  EXPECT_EQ(config.welcome_message, "Hello, %u!");
}

TEST_F(PamConfigTest, WelcomeMessageUnquoted) {
  auto config = loadConfig(R"(
[Security]
welcome_message = Hello World
)");
  EXPECT_EQ(config.welcome_message, "Hello World");
}

TEST_F(PamConfigTest, WelcomeMessagePrecedence) {
  auto config = loadConfig(R"(
show_welcome = true
welcome_message = "Default"

[Security]
show_welcome = false
welcome_message = "Security"
)");
  EXPECT_FALSE(config.show_welcome);
  EXPECT_EQ(config.welcome_message, "Security");
}
