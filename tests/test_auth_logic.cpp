
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
  static void setPolicy(AuthEngine &engine, Configuration::AuthPolicy policy) {
    engine.config.policy = policy;
  }
  static void setTimeoutMs(AuthEngine &engine, int timeout_ms) {
    engine.config.timeout_ms = timeout_ms;
  }
  static void useModels(AuthEngine &engine, cv::Ptr<cv::FaceDetectorYN> detector,
                        cv::Ptr<cv::FaceRecognizerSF> recognizer) {
    engine.detector = std::move(detector);
    engine.recognizer = std::move(recognizer);
    engine.config.users_dir = "/tmp/linuxcampam_test_users";
    engine.config.gpu_throttle_ms = 0;
    engine.config.gpu_flush = false;
    engine.config.threshold = 0.5f;
    engine.config.lockout_attempts = 10000;
  }
  static void clearActiveCameras(AuthEngine &engine) {
    engine.active_cameras.clear();
  }
  static void addActiveCamera(AuthEngine &engine, std::unique_ptr<ICamera> cam,
                              Configuration::CameraDefinition def) {
    engine.active_cameras.push_back({std::move(cam), std::move(def)});
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

TEST_F(AuthEngineTest, LockoutBoundsValidation) {
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

  constexpr int DEFAULT_FAILURES = 5;
  for (int i = 0; i < DEFAULT_FAILURES; ++i)
    recordAuthAttempt(engine, "dave", false);

  EXPECT_TRUE(isUserLockedOut(engine, "dave"));
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

// --- Fail-closed config and policy tests ---

TEST_F(AuthEngineTest, InitFailsClosedOnMalformedConfig) {
  std::ofstream bad_cfg("bad_config.ini");
  bad_cfg << "[Auth]\nthreshold = not_a_float\n";
  bad_cfg.close();
  TempConfigFile guard("bad_config.ini");

  EXPECT_FALSE(auth_engine->init("bad_config.ini"))
      << "AuthEngine::init must fail-closed on malformed config";
}

TEST_F(AuthEngineTest, VerificationEnforcesTimeoutMs) {
  auth_engine->setCameraFactory([&](const Configuration::CameraDefinition &) {
    auto mock = std::make_unique<NiceMock<MockCamera>>();
    ON_CALL(*mock, capture()).WillByDefault([&]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      return cv::Mat::zeros(MOCK_FRAME_HEIGHT, MOCK_FRAME_WIDTH, CV_8UC3);
    });
    return mock;
  });
  ASSERT_TRUE(auth_engine->init("test_auth_config.ini"));
  setTimeoutMs(*auth_engine, 5);

  writeUserWithEmbedding("timeout_user", "default");

  AuthResult res = auth_engine->verifyUserWithDetails("timeout_user");
  EXPECT_FALSE(res.success);
  EXPECT_THAT(res.reason, ::testing::HasSubstr("Verification timeout"));
}

TEST_F(AuthEngineTest, AdaptivePolicyMandatoryCaptureFailure) {
  auth_engine->setCameraFactory([&](const Configuration::CameraDefinition &) {
    auto mock = std::make_unique<NiceMock<MockCamera>>();
    ON_CALL(*mock, capture()).WillByDefault(Return(cv::Mat()));
    return mock;
  });
  ASSERT_TRUE(auth_engine->init("test_auth_config.ini"));

  setPolicy(*auth_engine, Configuration::AuthPolicy::ADAPTIVE);
  clearActiveCameras(*auth_engine);

  Configuration::CameraDefinition ir_cam;
  ir_cam.id = "cam_ir";
  ir_cam.type = "ir";
  ir_cam.mandatory = true;
  addActiveCamera(*auth_engine, std::make_unique<NiceMock<MockCamera>>(), ir_cam);

  writeUserWithEmbedding("policy_user", "default");

  AuthResult res = auth_engine->verifyUserWithDetails("policy_user");
  EXPECT_FALSE(res.success);
  EXPECT_THAT(res.reason, ::testing::HasSubstr("Mandatory Camera cam_ir failed"));
}

TEST_F(AuthEngineTest, AdaptivePolicyMandatoryDarkFailure) {
  ASSERT_TRUE(auth_engine->init("test_auth_config.ini"));

  setPolicy(*auth_engine, Configuration::AuthPolicy::ADAPTIVE);
  clearActiveCameras(*auth_engine);

  Configuration::CameraDefinition ir_cam;
  ir_cam.id = "cam_ir";
  ir_cam.type = "ir";
  ir_cam.min_brightness = 50;
  ir_cam.mandatory = true;

  auto mock = std::make_unique<NiceMock<MockCamera>>();
  cv::Mat black_frame = cv::Mat::zeros(MOCK_FRAME_HEIGHT, MOCK_FRAME_WIDTH, CV_8UC3);
  ON_CALL(*mock, capture()).WillByDefault(Return(black_frame));
  addActiveCamera(*auth_engine, std::move(mock), ir_cam);

  writeUserWithEmbedding("policy_user", "default");

  AuthResult res = auth_engine->verifyUserWithDetails("policy_user");
  EXPECT_FALSE(res.success);
  EXPECT_THAT(res.reason, ::testing::HasSubstr("Mandatory Camera cam_ir too dark"));
}

TEST_F(AuthEngineTest, StrictPolicyFailsOnAnyDarkOrCaptureFailure) {
  ASSERT_TRUE(auth_engine->init("test_auth_config.ini"));

  setPolicy(*auth_engine, Configuration::AuthPolicy::STRICT_ALL);
  clearActiveCameras(*auth_engine);

  Configuration::CameraDefinition opt_cam;
  opt_cam.id = "cam_opt";
  opt_cam.type = "rgb";
  opt_cam.min_brightness = 50;
  opt_cam.mandatory = false;

  auto mock = std::make_unique<NiceMock<MockCamera>>();
  cv::Mat black_frame = cv::Mat::zeros(MOCK_FRAME_HEIGHT, MOCK_FRAME_WIDTH, CV_8UC3);
  ON_CALL(*mock, capture()).WillByDefault(Return(black_frame));
  addActiveCamera(*auth_engine, std::move(mock), opt_cam);

  writeUserWithEmbedding("policy_user", "default");

  AuthResult res = auth_engine->verifyUserWithDetails("policy_user");
  EXPECT_FALSE(res.success);
  EXPECT_THAT(res.reason, ::testing::HasSubstr("Camera cam_opt too dark"));
}

TEST_F(AuthEngineTest, LenientPolicyToleratesMandatoryCaptureFailure) {
  ASSERT_TRUE(auth_engine->init("test_auth_config.ini"));

  setPolicy(*auth_engine, Configuration::AuthPolicy::LENIENT_ANY);
  clearActiveCameras(*auth_engine);

  Configuration::CameraDefinition ir_cam;
  ir_cam.id = "cam_ir";
  ir_cam.type = "ir";
  ir_cam.mandatory = true;

  auto mock_ir = std::make_unique<NiceMock<MockCamera>>();
  ON_CALL(*mock_ir, capture()).WillByDefault(Return(cv::Mat())); // Capture fails
  addActiveCamera(*auth_engine, std::move(mock_ir), ir_cam);

  writeUserWithEmbedding("policy_user", "default");

  // Since IR capture fails and there are no other cameras, participants == 0
  AuthResult res = auth_engine->verifyUserWithDetails("policy_user");
  EXPECT_FALSE(res.success);
  EXPECT_EQ(res.reason, "No cameras participated");
}

// Inference doubles let policy tests exercise the real capture/brightness/data/
// detection/matching pipeline, including successful results, without face photos.
class PolicyDetector : public cv::FaceDetectorYN {
public:
  void setInputSize(const cv::Size &) override {}
  cv::Size getInputSize() override { return {8, 8}; }
  void setScoreThreshold(float) override {}
  float getScoreThreshold() override { return 0.9f; }
  void setNMSThreshold(float) override {}
  float getNMSThreshold() override { return 0.3f; }
  void setTopK(int) override {}
  int getTopK() override { return 1; }
  int detect(cv::InputArray image, cv::OutputArray faces) override {
    if (image.getMat().at<cv::Vec3b>(0, 0)[0] == 140) { faces.release(); return 0; }
    cv::Mat face = cv::Mat::ones(1, 15, CV_32F);
    face.copyTo(faces);
    return 1;
  }
};

class PolicyRecognizer : public cv::FaceRecognizerSF {
public:
  void alignCrop(cv::InputArray source, cv::InputArray, cv::OutputArray aligned) const override {
    source.getMat().copyTo(aligned);
  }
  void feature(cv::InputArray aligned, cv::OutputArray feature) override {
    cv::Mat value(1, 1, CV_32F, cv::Scalar(aligned.getMat().at<cv::Vec3b>(0, 0)[0]));
    value.copyTo(feature);
  }
  double match(cv::InputArray value, cv::InputArray, int) const override {
    return value.getMat().at<float>(0, 0) == 100.0f ? 0.9 : 0.1;
  }
};

TEST_F(AuthEngineTest, MultiCameraPolicyOutcomeMatrix) {
  enum Outcome { Match, Mismatch, NoFace, MissingEmbedding, Dark, CaptureFailure };
  using Policy = Configuration::AuthPolicy;
  for (const auto policy : {Policy::STRICT_ALL, Policy::ADAPTIVE, Policy::LENIENT_ANY}) {
    for (int required = 0; required < 4; ++required) {
      for (int first = Match; first <= CaptureFailure; ++first) {
        for (int second = Match; second <= CaptureFailure; ++second) {
          SCOPED_TRACE(::testing::Message() << "policy=" << static_cast<int>(policy)
                       << " required=" << required << " outcomes=" << first << "," << second);
          AuthEngine engine;
          useModels(engine, cv::makePtr<PolicyDetector>(), cv::makePtr<PolicyRecognizer>());
          setPolicy(engine, policy);
          nlohmann::json data;
          const int outcomes[] = {first, second};
          bool any_match = false;
          bool all_acceptable = true;
          for (int i = 0; i < 2; ++i) {
            const bool mandatory = (required & (1 << i)) != 0;
            const int outcome = outcomes[i];
            Configuration::CameraDefinition camera;
            camera.id = i == 0 ? "rgb" : "ir";
            camera.type = camera.id;
            camera.mandatory = mandatory;
            camera.min_brightness = 50;
            auto mock = std::make_unique<NiceMock<MockCamera>>();
            cv::Mat frame;
            if (outcome != CaptureFailure) {
              int value = outcome == Dark ? 0 : outcome == NoFace ? 140 : outcome == Mismatch ? 120 : 100;
              frame = cv::Mat(8, 8, CV_8UC3, cv::Scalar::all(value));
            }
            ON_CALL(*mock, capture()).WillByDefault(Return(frame));
            addActiveCamera(engine, std::move(mock), camera);
            if (outcome != MissingEmbedding)
              data["embeddings_" + camera.type] = {{{"label", "default"}, {"data", {0.1f}}}};
            any_match = any_match || outcome == Match;
            // Adaptive permits unavailable optional cameras; a participating
            // camera's mismatch/no-face/missing template still vetoes it.
            const bool optional_unavailable = !mandatory && (outcome == Dark || outcome == CaptureFailure);
            all_acceptable = all_acceptable && (outcome == Match ||
                (policy == Policy::ADAPTIVE && optional_unavailable));
          }
          { std::ofstream file("/tmp/linuxcampam_test_users/matrix.json"); file << data; }
          const bool expected = any_match && (policy == Policy::LENIENT_ANY || all_acceptable);
          EXPECT_EQ(engine.verifyUserWithDetails("matrix").success, expected);
        }
      }
    }
  }
}

TEST_F(AuthEngineTest, WorkerTimeoutsCannotResetSupervisorLockout) {
  ASSERT_TRUE(auth_engine->init("test_auth_config.ini", false));
  int calls = 0;
  auth_engine->setVerificationRunner([&](std::string_view, bool) {
    ++calls;
    return AuthResult{false, "Verification timeout", 0.0f};
  });
  for (int i = 0; i < 3; ++i) EXPECT_FALSE(auth_engine->verifyUser("worker_user"));
  EXPECT_EQ(auth_engine->verifyUserWithDetails("worker_user").reason, "User locked out");
  EXPECT_EQ(calls, 3);
}
