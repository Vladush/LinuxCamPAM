
#include "service/auth_engine.hpp"
#include "service/icamera.hpp"

#include <filesystem>
#include <fstream>
#include "json.hpp"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::NiceMock;
using ::testing::Return;

constexpr int MOCK_FRAME_WIDTH = 640;
constexpr int MOCK_FRAME_HEIGHT = 480;
constexpr int MOCK_EMBEDDING_SIZE = 128;

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
    fs::create_directories("/tmp/linuxcampam_test_users");

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
                << "lockout_duration_sec=10\n";
    config_file.close();

    auth_engine = std::make_unique<AuthEngine>();
  }

  void TearDown() override {
    if (fs::exists("test_auth_config.ini"))
      fs::remove("test_auth_config.ini");

    if (fs::exists("/tmp/linuxcampam_test_users"))
      fs::remove_all("/tmp/linuxcampam_test_users");
  }

  // Returns a JSON-formatted string like "[0.1, 0.1, ...]" with
  // MOCK_EMBEDDING_SIZE elements, suitable for embedding in test JSON files.
  static std::string makeDummyVec() {
    std::string vec = "[";
    for (int k = 0; k < MOCK_EMBEDDING_SIZE; ++k)
      vec += (k == 0 ? "0.1" : ", 0.1");
    vec += "]";
    return vec;
  }

  // Writes a user JSON file with a single embedding entry under embeddings_rgb.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - local test helper; parameters are self-documenting.
  void writeUserWithEmbedding(const std::string &username,
                              const std::string &label) {
    std::string vec = makeDummyVec();
    std::ofstream f("/tmp/linuxcampam_test_users/" + username + ".json");
    f << "{\n"
      << "  \"embeddings_rgb\": [\n"
      << "    {\"label\": \"" << label << "\", \"data\": " << vec << "}\n"
      << "  ]\n"
      << "}";
    f.close();
  }

  // Initializes the engine with a NiceMock camera that returns nothing.
  void initWithMockCamera() {
    auth_engine->setCameraFactory([&](const Configuration::CameraDefinition &) {
      return std::make_unique<NiceMock<MockCamera>>();
    });
    ASSERT_TRUE(auth_engine->init("test_auth_config.ini"));
  }

  std::unique_ptr<AuthEngine> auth_engine;
};

TEST_F(AuthEngineTest, InitializationLoadsCameras) {
  bool factory_called = false;
  auth_engine->setCameraFactory(
      [&](const Configuration::CameraDefinition &def) {
        factory_called = true;
        EXPECT_EQ(def.id, "mock_cam");
        return std::make_unique<NiceMock<MockCamera>>();
      });

  ASSERT_TRUE(auth_engine->init("test_auth_config.ini"));
  EXPECT_TRUE(factory_called)
      << "Camera factory should have been called during init";
}

TEST_F(AuthEngineTest, VerifyUserReturnsFalseWhenNotEnrolled) {
  initWithMockCamera();

  AuthResult result = auth_engine->verifyUserWithDetails("ghost_user");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.reason.empty());
}

TEST_F(AuthEngineTest, RateLimitingLockout) {
  auth_engine->setCameraFactory([&](const Configuration::CameraDefinition &) {
    auto mock = std::make_unique<NiceMock<MockCamera>>();
    cv::Mat black_frame =
        cv::Mat::zeros(MOCK_FRAME_HEIGHT, MOCK_FRAME_WIDTH, CV_8UC3);
    ON_CALL(*mock, capture()).WillByDefault(Return(black_frame));
    return mock;
  });
  EXPECT_TRUE(auth_engine->init("test_auth_config.ini"));

  writeUserWithEmbedding("user_limited", "default");

  for (int i = 0; i < 3; ++i) {
    AuthResult res = auth_engine->verifyUserWithDetails("user_limited");
    EXPECT_FALSE(res.success);
  }

  AuthResult lockoutRes = auth_engine->verifyUserWithDetails("user_limited");
  EXPECT_FALSE(lockoutRes.success);
  EXPECT_THAT(lockoutRes.reason, ::testing::HasSubstr("locked"));
}

TEST_F(AuthEngineTest, HandlesEmptyFramesGracefully) {
  auth_engine->setCameraFactory([&](const Configuration::CameraDefinition &) {
    auto mock = std::make_unique<NiceMock<MockCamera>>();
    EXPECT_CALL(*mock, capture()).WillOnce(Return(cv::Mat()));
    return mock;
  });
  EXPECT_TRUE(auth_engine->init("test_auth_config.ini"));

  writeUserWithEmbedding("user_empty_frame", "default");

  AuthResult res = auth_engine->verifyUserWithDetails("user_empty_frame");
  EXPECT_FALSE(res.success);
}

TEST_F(AuthEngineTest, ResilientToBadModelPaths) {
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
  bool init_ok = auth_engine->init("test_bad_models.ini");
  if (init_ok) {
    AuthResult res = auth_engine->verifyUserWithDetails("any_user");
    EXPECT_FALSE(res.success);
  } else {
    SUCCEED();
  }

  fs::remove("test_bad_models.ini");
}

// --- setLabel tests ---

TEST_F(AuthEngineTest, SetLabelPromotesPendingEmbedding) {
  initWithMockCamera();

  std::string vec = makeDummyVec();
  std::ofstream user_file("/tmp/linuxcampam_test_users/test_user.json");
  user_file << "{\n"
            << "  \"_pending_rgb\": " << vec << "\n"
            << "}";
  user_file.close();

  EXPECT_TRUE(auth_engine->setLabel("test_user", "my_label"));

  std::ifstream verify_file("/tmp/linuxcampam_test_users/test_user.json");
  nlohmann::json j;
  verify_file >> j;
  EXPECT_FALSE(j.contains("_pending_rgb"));
  ASSERT_TRUE(j.contains("embeddings_rgb"));
  ASSERT_EQ(j["embeddings_rgb"].size(), 1);
  EXPECT_EQ(j["embeddings_rgb"][0]["label"], "my_label");

  // Verify embedding data survived the round-trip
  auto stored = j["embeddings_rgb"][0]["data"].get<std::vector<float>>();
  ASSERT_EQ(stored.size(), MOCK_EMBEDDING_SIZE);
  for (int k = 0; k < MOCK_EMBEDDING_SIZE; ++k) {
    EXPECT_FLOAT_EQ(stored[k], 0.1f);
  }
}

TEST_F(AuthEngineTest, SetLabelReturnsFalseWithoutPendingData) {
  initWithMockCamera();

  // User file exists but has no _pending_* keys
  writeUserWithEmbedding("test_user", "existing");

  EXPECT_FALSE(auth_engine->setLabel("test_user", "new_label"));
}

// --- removeEmbedding tests ---

TEST_F(AuthEngineTest, RemoveEmbeddingDeletesCorrectLabel) {
  initWithMockCamera();

  std::string vec = makeDummyVec();
  std::ofstream user_file("/tmp/linuxcampam_test_users/test_user.json");
  user_file << "{\n"
            << "  \"embeddings_rgb\": [\n"
            << "    {\"label\": \"label_to_keep\", \"data\": " << vec << "},\n"
            << "    {\"label\": \"label_to_remove\", \"data\": " << vec << "}\n"
            << "  ]\n"
            << "}";
  user_file.close();

  EXPECT_TRUE(auth_engine->removeEmbedding("test_user", "label_to_remove"));

  std::ifstream verify_file("/tmp/linuxcampam_test_users/test_user.json");
  nlohmann::json j;
  verify_file >> j;
  ASSERT_EQ(j["embeddings_rgb"].size(), 1);
  EXPECT_EQ(j["embeddings_rgb"][0]["label"], "label_to_keep");
}

TEST_F(AuthEngineTest, RemoveEmbeddingReturnsFalseForNonexistentLabel) {
  initWithMockCamera();

  writeUserWithEmbedding("test_user", "existing_label");

  EXPECT_FALSE(auth_engine->removeEmbedding("test_user", "no_such_label"));
}

TEST_F(AuthEngineTest, RemoveEmbeddingReturnsFalseForMissingUser) {
  initWithMockCamera();

  EXPECT_FALSE(auth_engine->removeEmbedding("nonexistent_user", "any_label"));
}

// --- trainUser tests ---

TEST_F(AuthEngineTest, TrainUserHandlesMissingFace) {
  auth_engine->setCameraFactory([&](const Configuration::CameraDefinition &) {
    auto mock = std::make_unique<NiceMock<MockCamera>>();
    EXPECT_CALL(*mock, capture()).WillOnce(Return(cv::Mat()));
    return mock;
  });
  EXPECT_TRUE(auth_engine->init("test_auth_config.ini"));

  std::ofstream user_file("/tmp/linuxcampam_test_users/test_user.json");
  user_file << "{}";
  user_file.close();

  EXPECT_FALSE(auth_engine->trainUser("test_user", "new_label", true));
}

TEST_F(AuthEngineTest, TrainUserRejectsInvalidUsername) {
  initWithMockCamera();

  EXPECT_FALSE(auth_engine->trainUser("../evil", "label", true));
  EXPECT_FALSE(auth_engine->trainUser("", "label", false));
}
