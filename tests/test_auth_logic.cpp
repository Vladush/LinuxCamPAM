
#include "service/auth_engine.hpp"
#include "service/icamera.hpp"

#include <filesystem>
#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::NiceMock;
using ::testing::Return;

// Constants for testing
constexpr int MOCK_FRAME_WIDTH = 640;
constexpr int MOCK_FRAME_HEIGHT = 480;
constexpr int MOCK_EMBEDDING_SIZE = 128;

// Mock Camera Class
class MockCamera : public ICamera {
public:
  MOCK_METHOD(cv::Mat, capture, (), (override));
  MOCK_METHOD(cv::Mat, captureHDR, (), (override));
  MOCK_METHOD(cv::Mat, captureAveraged, (int), (override));
  MOCK_METHOD(bool, supportsManualExposure, (), (const, override));
  MOCK_METHOD(void, triggerIrEmitter, (), (override));
};

class AuthEngineTest : public ::testing::Test {
public:
  void SetUp() override {
    // Create temp users dir
    fs::create_directories("/tmp/linuxcampam_test_users");

    // Create a temporary config file for testing
    std::ofstream config_file("test_auth_config.ini");
    config_file << "[Paths]\n"
                << "users_dir=/tmp/linuxcampam_test_users\n"
                << "\n"
                << "[Cameras]\n"
                << "names=mock_cam\n"
                << "\n"
                << "[Camera.mock_cam]\n"
                << "path=/dev/video0\n"
                << "type=rgb\n"
                << "\n"
                << "[Security]\n"
                << "lockout_attempts=3\n"
                << "lockout_duration_sec=1\n";
    config_file.close();

    auth_engine = std::make_unique<AuthEngine>();
  }

  void TearDown() override {
    // Clean up
    if (fs::exists("test_auth_config.ini"))
      fs::remove("test_auth_config.ini");

    if (fs::exists("/tmp/linuxcampam_test_users"))
      fs::remove_all("/tmp/linuxcampam_test_users");
  }

  std::unique_ptr<AuthEngine> auth_engine;
};

// Test 1: Verify initialization picks up the camera config
TEST_F(AuthEngineTest, InitializationLoadsCameras) {
  // We need to inject the mock factory BEFORE calling init
  bool factory_called = false;
  auth_engine->setCameraFactory(
      [&](const Configuration::CameraDefinition &def) {
        factory_called = true;
        EXPECT_EQ(def.id, "mock_cam");
        return std::make_unique<NiceMock<MockCamera>>();
      });

  // Mock file loaded?
  ASSERT_TRUE(auth_engine->init("test_auth_config.ini"));
  EXPECT_TRUE(factory_called)
      << "Camera factory should have been called during init";
}

// Test 2: Verify user validation logic fails gracefully without models or
// enrollment
TEST_F(AuthEngineTest, VerifyUserReturnsFalseWhenNotEnrolled) {
  auth_engine->setCameraFactory([&](const Configuration::CameraDefinition &) {
    return std::make_unique<NiceMock<MockCamera>>();
  });
  EXPECT_TRUE(auth_engine->init("test_auth_config.ini"));

  // "ghost_user" does not exist
  AuthResult result = auth_engine->verifyUserWithDetails("ghost_user");
  EXPECT_FALSE(result.success);
  // Exact reason string depends on implementation, but likely "User not
  // enrolled" or similar
  EXPECT_FALSE(result.reason.empty());
}

// Test 3: Rate Limiting
TEST_F(AuthEngineTest, RateLimitingLockout) {
  auth_engine->setCameraFactory([&](const Configuration::CameraDefinition &) {
    auto mock = std::make_unique<NiceMock<MockCamera>>();
    // Return a VALID but empty/black frame so we participate but fail
    // detection/matching
    cv::Mat black_frame =
        cv::Mat::zeros(MOCK_FRAME_HEIGHT, MOCK_FRAME_WIDTH, CV_8UC3);
    ON_CALL(*mock, capture()).WillByDefault(Return(black_frame));
    return mock;
  });
  EXPECT_TRUE(auth_engine->init("test_auth_config.ini"));

  // Create valid JSON user file
  std::ofstream user_file("/tmp/linuxcampam_test_users/user_limited.json");
  // Minimal valid user with one dummy embedding for 'rgb' type
  // We need MOCK_EMBEDDING_SIZE floats for the embedding vector
  std::string dummy_vec = "[";
  for (int k = 0; k < MOCK_EMBEDDING_SIZE; ++k)
    dummy_vec += (k == 0 ? "0.1" : ", 0.1");
  dummy_vec += "]";

  user_file << "{\n"
            << "  \"embeddings_rgb\": [\n"
            << "    {\"label\": \"default\", \"vector\": " << dummy_vec << "}\n"
            << "  ]\n"
            << "}";
  user_file.close();

  // Fail 3 times (configured max)
  for (int i = 0; i < 3; ++i) {
    AuthResult res = auth_engine->verifyUserWithDetails("user_limited");
    EXPECT_FALSE(res.success);
  }

  // 4th time should be locked out (engine doesn't even try camera)
  AuthResult lockoutRes = auth_engine->verifyUserWithDetails("user_limited");
  EXPECT_FALSE(lockoutRes.success);
  EXPECT_THAT(lockoutRes.reason, ::testing::HasSubstr("locked"));
}

// Test 4: Empty Frame Handling
TEST_F(AuthEngineTest, HandlesEmptyFramesGracefully) {
  auth_engine->setCameraFactory([&](const Configuration::CameraDefinition &) {
    auto mock = std::make_unique<NiceMock<MockCamera>>();
    // Explicitly return empty matrix
    EXPECT_CALL(*mock, capture()).WillOnce(Return(cv::Mat()));
    return mock;
  });
  EXPECT_TRUE(auth_engine->init("test_auth_config.ini"));

  // Create valid JSON user file
  std::ofstream user_file("/tmp/linuxcampam_test_users/user_empty_frame.json");
  std::string dummy_vec = "[";
  for (int k = 0; k < MOCK_EMBEDDING_SIZE; ++k)
    dummy_vec += (k == 0 ? "0.1" : ", 0.1");
  dummy_vec += "]";

  user_file << "{\n"
            << "  \"embeddings_rgb\": [\n"
            << "    {\"label\": \"default\", \"vector\": " << dummy_vec << "}\n"
            << "  ]\n"
            << "}";
  user_file.close();

  AuthResult res = auth_engine->verifyUserWithDetails("user_empty_frame");
  EXPECT_FALSE(res.success);
}

// Test 5: Model Loading Resilience
TEST_F(AuthEngineTest, ResilientToBadModelPaths) {
  // Overwrite config with bad model paths
  std::ofstream config_file("test_bad_models.ini");
  config_file << "[Paths]\n"
              << "users_dir=/tmp/linuxcampam_test_users\n"
              << "detection_model=/tmp/missing_det.onnx\n"
              << "recognition_model=/tmp/missing_rec.onnx\n"
              << "\n"
              << "[Cameras]\n"
              << "names=mock_cam\n"
              << "\n"
              << "[Camera.mock_cam]\n"
              << "path=/dev/video0\n"
              << "type=rgb\n";
  config_file.close();

  // We handle both eager and lazy loading scenarios here. If init() fails
  // immediately due to missing models, that's fine. If it succeeds (lazy load),
  // we verify that the actual authentication attempt fails gracefully later.

  // If implementation is lazy or soft-fail:
  bool init_ok = auth_engine->init("test_bad_models.ini");
  if (init_ok) {
    // If it initialized, verify() should fail safely
    AuthResult res = auth_engine->verifyUserWithDetails("any_user");
    EXPECT_FALSE(res.success);
  } else {
    // If it failed to init, that's also a "pass" for resilience (didn't crash)
    SUCCEED();
  }

  fs::remove("test_bad_models.ini");
}
