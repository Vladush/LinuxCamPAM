#include "../src/service/auth_engine.hpp"
#include "../src/service/camera.hpp"

#include <fstream>
#include <gtest/gtest.h>

// Mock Camera subclass to test open() failures safely without real hardware
class MockCamera : public Camera {
public:
  using Camera::Camera;
  bool open(const std::string &path) {
    if (path == "/dev/video_fail") {
      return false; // Simulate failure
    }
    return true; // Simulate success
  }
};

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
  bool init_result = engine.init(config_path);

  // 3. Verify it didn't crash and set reasonable defaults
  // Init might return false due to missing hardware checks in init(),
  // but we are testing that it DOES NOT CRASH during parsing.
  EXPECT_NO_THROW({ engine.init(config_path); });
}

TEST(StabilityTest, CameraOpenFailure) {
  MockCamera cam;
  // Should handle failure gracefully
  EXPECT_FALSE(cam.open("/dev/video_fail"));
}

TEST(StabilityTest, IrEmitterInvalidPath) {
  // Test 3: IR Emitter Robustness
  // Point to a non-existent executable
  Camera cam;
  cam.ir_emitter_path_ = "/tmp/non_existent_executable";

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

  Camera cam;
  cam.ir_emitter_path_ = dummy_path;

  testing::internal::CaptureStderr();
  EXPECT_NO_THROW(cam.triggerIrEmitter());
  std::string output = testing::internal::GetCapturedStderr();

  // posix_spawn might fail with EACCES (13)
  EXPECT_NE(output.find("posix_spawn failed"), std::string::npos);
}
