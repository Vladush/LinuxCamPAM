
#include "service/auth_engine.hpp"
#include "service/icamera.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <thread>
#include "json.hpp"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::NiceMock;
using ::testing::Return;

// RAII guard to clean up config file even on ASSERT failure
struct TempConfigFile {
  std::string path;
  explicit TempConfigFile(std::string p) : path(std::move(p)) {}
  ~TempConfigFile() { fs::remove(path); }
  TempConfigFile(const TempConfigFile&) = delete;
  TempConfigFile& operator=(const TempConfigFile&) = delete;
  TempConfigFile(TempConfigFile&&) = delete;
  TempConfigFile& operator=(TempConfigFile&&) = delete;
};

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

  // Forwarders for private APIs (friend access isn't inherited by TEST_F subclasses).
  static bool isUserLockedOut(AuthEngine &engine, std::string_view user) {
    return engine.isUserLockedOut(user);
  }
  static void recordAuthAttempt(AuthEngine &engine, std::string_view user,
                                bool success) {
    engine.recordAuthAttempt(user, success);
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

TEST_F(AuthEngineTest, ValidFrameTriggersFaceDetection) {
  // Return a solid grey frame to trigger the detector without failing early on empty matrix
  auth_engine->setCameraFactory([&](const Configuration::CameraDefinition &) {
    auto mock = std::make_unique<NiceMock<MockCamera>>();
    constexpr int GREY_PIXEL_VALUE = 128;
    cv::Mat grey_frame(MOCK_FRAME_HEIGHT, MOCK_FRAME_WIDTH, CV_8UC3, 
                       cv::Scalar(GREY_PIXEL_VALUE, GREY_PIXEL_VALUE, GREY_PIXEL_VALUE));
    EXPECT_CALL(*mock, capture()).WillOnce(Return(grey_frame));
    return mock;
  });
  EXPECT_TRUE(auth_engine->init("test_auth_config.ini"));

  writeUserWithEmbedding("user_valid_frame", "default");

  // Since it's just a grey frame, Yunet will detect 0 faces.
  // But this executes the detection pipeline instead of exiting early.
  AuthResult res = auth_engine->verifyUserWithDetails("user_valid_frame");
  EXPECT_FALSE(res.success);
  EXPECT_THAT(res.reason, ::testing::HasSubstr("No face detected"));
}

// --- Lockout tests ---

TEST_F(AuthEngineTest, LockoutActivatesAfterNFailures) {
  initWithMockCamera();

  ASSERT_FALSE(isUserLockedOut(*auth_engine, "alice"));
  recordAuthAttempt(*auth_engine, "alice", false);
  recordAuthAttempt(*auth_engine, "alice", false);
  EXPECT_FALSE(isUserLockedOut(*auth_engine, "alice")); // 2 < threshold of 3
  recordAuthAttempt(*auth_engine, "alice", false);
  EXPECT_TRUE(isUserLockedOut(*auth_engine, "alice")); // 3 == threshold
}

TEST_F(AuthEngineTest, LockoutBlocksVerifyWithoutModels) {
  // Ensure lockout is checked before models are loaded
  initWithMockCamera();

  recordAuthAttempt(*auth_engine, "bob", false);
  recordAuthAttempt(*auth_engine, "bob", false);
  recordAuthAttempt(*auth_engine, "bob", false);
  ASSERT_TRUE(isUserLockedOut(*auth_engine, "bob"));

  AuthResult res = auth_engine->verifyUserWithDetails("bob");
  EXPECT_FALSE(res.success);
  EXPECT_THAT(res.reason, ::testing::HasSubstr("locked"));
}

TEST_F(AuthEngineTest, SuccessfulAttemptResetsCounter) {
  initWithMockCamera();

  recordAuthAttempt(*auth_engine, "charlie", false);
  recordAuthAttempt(*auth_engine, "charlie", false);
  EXPECT_FALSE(isUserLockedOut(*auth_engine, "charlie"));

  recordAuthAttempt(*auth_engine, "charlie", true);
  EXPECT_FALSE(isUserLockedOut(*auth_engine, "charlie"));

  // Counter should be reset after success
  recordAuthAttempt(*auth_engine, "charlie", false);
  recordAuthAttempt(*auth_engine, "charlie", false);
  EXPECT_FALSE(isUserLockedOut(*auth_engine, "charlie"));
}

TEST_F(AuthEngineTest, LockoutDisabledWhenAttemptsIsZero) {
  TempConfigFile guard("/tmp/test_no_lockout.ini");
  {
    std::ofstream cfg(guard.path);
    cfg << "[Paths]\nusers_dir=/tmp/linuxcampam_test_users\n\n"
        << "[Cameras]\nnames=mock_cam\n\n"
        << "[Camera.mock_cam]\npath=/dev/video0\ntype=rgb\n\n"
        << "[Security]\nlockout_attempts=0\nlockout_duration_sec=10\n";
  }

  AuthEngine engine;
  engine.setCameraFactory([](const Configuration::CameraDefinition&) {
    return std::make_unique<NiceMock<MockCamera>>();
  });
  ASSERT_TRUE(engine.init(guard.path));

  constexpr int EXCESS_FAILURES = 100;
  for (int i = 0; i < EXCESS_FAILURES; ++i)
    recordAuthAttempt(engine, "dave", false);

  EXPECT_FALSE(isUserLockedOut(engine, "dave"));
}

TEST_F(AuthEngineTest, LockoutExpiresAfterDuration) {
  TempConfigFile guard("/tmp/test_short_lockout.ini");
  {
    std::ofstream cfg(guard.path);
    cfg << "[Paths]\nusers_dir=/tmp/linuxcampam_test_users\n\n"
        << "[Cameras]\nnames=mock_cam\n\n"
        << "[Camera.mock_cam]\npath=/dev/video0\ntype=rgb\n\n"
        << "[Security]\nlockout_attempts=3\nlockout_duration_sec=1\n";
  }

  AuthEngine engine;
  engine.setCameraFactory([](const Configuration::CameraDefinition&) {
    return std::make_unique<NiceMock<MockCamera>>();
  });
  ASSERT_TRUE(engine.init(guard.path));

  recordAuthAttempt(engine, "eve", false);
  recordAuthAttempt(engine, "eve", false);
  recordAuthAttempt(engine, "eve", false);
  ASSERT_TRUE(isUserLockedOut(engine, "eve"));

  std::this_thread::sleep_for(std::chrono::seconds(2));
  EXPECT_FALSE(isUserLockedOut(engine, "eve"));
}

TEST_F(AuthEngineTest, LockoutIsUserSpecific) {
  initWithMockCamera();

  recordAuthAttempt(*auth_engine, "frank", false);
  recordAuthAttempt(*auth_engine, "frank", false);
  recordAuthAttempt(*auth_engine, "frank", false);
  ASSERT_TRUE(isUserLockedOut(*auth_engine, "frank"));

  EXPECT_FALSE(isUserLockedOut(*auth_engine, "grace"));
}

// --- Permission tests ---

TEST_F(AuthEngineTest, SetLabelWritesFileWithSecurePermissions) {
  initWithMockCamera();

  std::string vec = makeDummyVec();
  {
    std::ofstream f("/tmp/linuxcampam_test_users/perm_user.json");
    f << "{\"_pending_rgb\": " << vec << "}";
  }

  ASSERT_TRUE(auth_engine->setLabel("perm_user", "perm_label"));

  struct stat st{};
  ASSERT_EQ(stat("/tmp/linuxcampam_test_users/perm_user.json", &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600u)
      << "setLabel must produce a 0600 file; POSIX open() fix is broken";

  // Biometric dir must be 0700.
  struct stat dir_st{};
  ASSERT_EQ(stat("/tmp/linuxcampam_test_users", &dir_st), 0);
  EXPECT_EQ(dir_st.st_mode & 0777, 0700u)
      << "writeJsonAtomic must tighten the users directory to 0700";

  // Ensure no stale tmp files are left behind.
  for (const auto &entry :
       fs::directory_iterator("/tmp/linuxcampam_test_users")) {
    EXPECT_EQ(entry.path().filename().string().find(".tmp."),
              std::string::npos)
        << "leftover temp file: " << entry.path();
  }
}
