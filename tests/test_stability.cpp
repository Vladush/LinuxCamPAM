#include "../src/service/auth_engine.hpp"
#include "../src/service/camera.hpp"

#include <fstream>
#include <gtest/gtest.h>

// ============================================================================
// STABILITY TESTS
// ============================================================================

TEST(StabilityTest, ConfigResilience) {
  // 1. Create a malformed config file
  std::string config_path = "/tmp/test_malformed_config.ini";
  std::ofstream out(config_path);
  out << "[General]\n";
  out << "detection_threshold = invalid_number\n"; // Should handle bad type
  out << "log_level = DEBUG\n";
  out << "[Camera]\n";
  // Missing some keys
  out.close();

  // 2. Load it
  AuthEngine engine;

  // 3. Verify it didn't crash and set reasonable defaults
  // We use EXPECT_NO_THROW to ensure robustness.
  // We explicitly ignore the return value as we are testing for crashes, not
  // success.
  EXPECT_NO_THROW({ [[maybe_unused]] bool result = engine.init(config_path); });
}

TEST(StabilityTest, CameraOpenFailure) {
  // Directly test Camera class resilience against invalid paths
  // Constructor requires device path
  // Use a high index that parses correctly but is unlikely to exist.
  // Using "_fail" would parse to 0 (default) in current Camera implementation,
  // opening video0!
  Camera cam("/dev/video999");

  // capture() handles opening the device. If it fails, it returns empty Mat.
  // This verifies the production code's error handling.
  cv::Mat frame = cam.capture();
  EXPECT_TRUE(frame.empty());
}

TEST(StabilityTest, IrEmitterInvalidPath) {
  // Test 3: IR Emitter Robustness
  // Point to a non-existent executable.
  // We use /dev/null as camera path since we only care about emitter trigger.
  Camera cam("/dev/null", true, "/tmp/non_existent_executable");

  // Capturing stderr to avoid cluttering test output
  testing::internal::CaptureStderr();
  EXPECT_NO_THROW(cam.triggerIrEmitter()); // Should not throw/crash
  std::string output = testing::internal::GetCapturedStderr();

  // Should see error in logs
  EXPECT_NE(output.find("posix_spawn failed"), std::string::npos);
}

TEST(StabilityTest, IrEmitterNonExecutable) {
  // Create a dummy file that is NOT executable
  std::string dummy_path = "/tmp/dummy_script.sh";
  std::ofstream out(dummy_path);
  out << "#!/bin/bash\nexit 0";
  out.close();
  // chmod not applied, so not executable

  Camera cam("/dev/null", true, dummy_path);

  testing::internal::CaptureStderr();
  EXPECT_NO_THROW(cam.triggerIrEmitter());
  std::string output = testing::internal::GetCapturedStderr();

  // posix_spawn might fail with EACCES (13) or similar
  EXPECT_NE(output.find("posix_spawn failed"), std::string::npos);
}
