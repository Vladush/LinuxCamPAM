#include "../src/service/config.hpp"
#include "../src/service/utils.hpp"

#include <fstream>
#include <gtest/gtest.h>

class AuthConfigTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override { (void)remove("config_test.ini"); }

  void createConfig(const std::string &content) {
    std::ofstream out("config_test.ini");
    out << content;
    out.close();
  }
};

TEST_F(AuthConfigTest, UsesDefaultsWhenConfigMissing) {
  Configuration config;
  // Load from non-existent file should result in success (using defaults)
  // unless strict checking added
  bool result = config.load("non_existent_file.ini");

  EXPECT_TRUE(result);

  // Verify Defaults
  EXPECT_FLOAT_EQ(config.threshold, Configuration::DEFAULT_THRESHOLD);
  EXPECT_EQ(config.timeout_ms, Configuration::DEFAULT_TIMEOUT_MS);
  EXPECT_EQ(config.lockout_attempts, Configuration::DEFAULT_LOCKOUT_ATTEMPTS);
  EXPECT_EQ(config.gpu_throttle_ms, Configuration::DEFAULT_GPU_THROTTLE_MS);

  // Verify Provider Priority Default (Should contain OpenCL)
  ASSERT_FALSE(config.provider_priority.empty());
  EXPECT_EQ(config.provider_priority[0], "OpenCL");
}

TEST_F(AuthConfigTest, ParsesValidValues) {
  createConfig(R"(
[Auth]
; Legacy section name compatibility isn't strict, config.cpp looks for "General.threshold" 
; Wait, config.cpp looks for [General] section for threshold.
; The test helper checks logic mapping "General.threshold"
[General]
threshold = 0.75
detection_threshold = 0.85
timeout_ms = 5000
max_embeddings = 10

[Hardware]
provider_priority = CUDA,OpenVINO,OpenCL,CPU

[Security]
lockout_attempts = 3
lockout_duration_sec = 600

[Performance]
gpu_flush = off
gpu_throttle_ms = 50
model_keep_alive_sec = 120
)");

  Configuration config;
  ASSERT_TRUE(config.load("config_test.ini"));

  // General
  EXPECT_FLOAT_EQ(config.threshold, 0.75f);
  EXPECT_FLOAT_EQ(config.detection_threshold, 0.85f);
  EXPECT_EQ(config.timeout_ms, 5000);
  EXPECT_EQ(config.max_embeddings, 10);

  // Hardware
  ASSERT_EQ(config.provider_priority.size(), 4);
  EXPECT_EQ(config.provider_priority[0], "CUDA");
  EXPECT_EQ(config.provider_priority[1], "OpenVINO");

  // Security
  EXPECT_EQ(config.lockout_attempts, 3);
  EXPECT_EQ(config.lockout_duration_sec, 600);

  // Performance
  EXPECT_FALSE(config.gpu_flush);
  EXPECT_EQ(config.gpu_throttle_ms, 50);
  EXPECT_EQ(config.model_keep_alive_sec, 120);
}

TEST_F(AuthConfigTest, ParsesProximityConfig) {
  createConfig(R"(
[Hardware]
proximity_sensor_id = MY_SENSOR

[Proximity]
wake_enabled = true
wake_confidence_threshold = 70
lock_enabled = true
lock_confidence_threshold = 10
lock_timeout_seconds = 30
lock_command = custom_lock_cmd
)");

  Configuration config;
  ASSERT_TRUE(config.load("config_test.ini"));

  EXPECT_EQ(config.proximity_sensor_id, "MY_SENSOR");
  EXPECT_TRUE(config.wake_enabled);
  EXPECT_EQ(config.wake_confidence_threshold, 70);
  EXPECT_TRUE(config.lock_enabled);
  EXPECT_EQ(config.lock_confidence_threshold, 10);
  EXPECT_EQ(config.lock_timeout_seconds, 30);
  EXPECT_EQ(config.lock_command, "custom_lock_cmd");
}

TEST_F(AuthConfigTest, HandlesPartialConfig) {
  createConfig(R"(
[General]
threshold = 0.65
)");
  Configuration config;
  ASSERT_TRUE(config.load("config_test.ini"));
  EXPECT_FLOAT_EQ(config.threshold, 0.65f);
  // Default preserved
  EXPECT_EQ(config.lockout_attempts, Configuration::DEFAULT_LOCKOUT_ATTEMPTS);
}

TEST_F(AuthConfigTest, ParsesLoggingConfig) {
  createConfig(R"(
[General]
log_level = debug
log_file = /tmp/linuxcampam.log
)");
  Configuration config;
  ASSERT_TRUE(config.load("config_test.ini"));
  EXPECT_EQ(config.log_level, "debug");
  EXPECT_EQ(config.log_file, "/tmp/linuxcampam.log");
}

TEST_F(AuthConfigTest, FallbackOnInvalidData) {
  createConfig(R"(
[General]
threshold = invalid_float
timeout_ms = invalid_int
[Security]
lockout_attempts = bad_number
)");
  Configuration config;
  ASSERT_TRUE(config.load("config_test.ini"));

  // Should use defaults/ignore invalid
  EXPECT_FLOAT_EQ(config.threshold, Configuration::DEFAULT_THRESHOLD);
  EXPECT_EQ(config.lockout_attempts, Configuration::DEFAULT_LOCKOUT_ATTEMPTS);
}

// ----------------------------------------------------
// Security Helper Tests
// ----------------------------------------------------

// isValidUsername is tested locally here as a sanity check for the algorithm.
// The actual implementation resides in the security utilities.

TEST(SecurityTest, UsernameSanitizationAlgorithm) {
  EXPECT_TRUE(linuxcampam::isValidUsername("vlad"));
  EXPECT_TRUE(linuxcampam::isValidUsername("user.name"));
  EXPECT_FALSE(linuxcampam::isValidUsername("../../etc/passwd"));
  EXPECT_FALSE(linuxcampam::isValidUsername("user; rm -rf /"));
}
