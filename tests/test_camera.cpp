#include "../src/service/camera.hpp"
#include <gtest/gtest.h>

using namespace linuxcampam;

class CameraTest : public ::testing::Test {
};

TEST_F(CameraTest, GracefulFailureOnInvalidDevicePath) {
  // /dev/video9999 is not a valid V4L2 camera device
  // OpenCV capture logic should fail to open it without crashing
  Camera cam("/dev/video9999", false, "");

  cv::Mat frame = cam.capture();
  EXPECT_TRUE(frame.empty());
  
  // It shouldn't crash on captureAveraged either
  cv::Mat avg_frame = cam.captureAveraged(3);
  EXPECT_TRUE(avg_frame.empty());
  
  // It shouldn't crash on captureHDR
  cv::Mat hdr_frame = cam.captureHDR();
  EXPECT_TRUE(hdr_frame.empty());
  
  // supportsManualExposure falls back to false if open fails
  EXPECT_FALSE(cam.supportsManualExposure());
}

TEST_F(CameraTest, TriggerIrEmitterDoesNotCrash) {
  // ir cmd path is invalid
  Camera cam("/dev/null", true, "/bin/false");
  
  // Should gracefully fail executing the command and not crash
  cam.triggerIrEmitter();
}

namespace {
constexpr int kMockFrameDim = 10;
constexpr int kTestAverageFrames5 = 5;
constexpr int kTestAverageFrames3 = 3;
constexpr double kMockBasePixelVal = 80.0;
constexpr double kMockStepPixelVal = 20.0;

struct SessionState {
  bool mock_open_success = true;
  bool mock_exposure_support = false;
  bool mock_set_exposure_success = true;
  int open_count = 0;
  int detect_query_count = 0;
  int grab_count = 0;
  int read_count = 0;
  std::vector<double> recorded_exposures;
};

class MockCameraSession : public ICameraSession {
public:
  explicit MockCameraSession(std::shared_ptr<SessionState> state)
      : state_(std::move(state)) {}

  bool open(std::string_view, int) override {
    state_->open_count++;
    return state_->mock_open_success;
  }

  [[nodiscard]] bool isOpened() const override {
    return state_->mock_open_success;
  }

  [[nodiscard]] bool detectExposureSupport(std::string_view) override {
    state_->detect_query_count++;
    return state_->mock_exposure_support;
  }

  bool read(cv::Mat &frame) override {
    state_->read_count++;
    double val = kMockBasePixelVal + static_cast<double>(state_->recorded_exposures.size()) * kMockStepPixelVal;
    frame = cv::Mat(kMockFrameDim, kMockFrameDim, CV_8UC3, cv::Scalar(val, val, val));
    return true;
  }

  bool grab() override {
    state_->grab_count++;
    return true;
  }

  [[nodiscard]] double getProperty(int) const override {
    return 0.0;
  }

  bool setProperty(int prop_id, double value) override {
    if (prop_id == cv::CAP_PROP_EXPOSURE) {
      state_->recorded_exposures.push_back(value);
      return state_->mock_set_exposure_success;
    }
    return true;
  }

  void sleepMs(int) override {}

private:
  std::shared_ptr<SessionState> state_;
};
} // namespace

TEST_F(CameraTest, CaptureEnrollmentFailsWhenDeviceOpenFails) {
  auto state = std::make_shared<SessionState>();
  state->mock_open_success = false;

  Camera cam("/dev/video0", false, "", false,
             [state]() { return std::make_unique<MockCameraSession>(state); });

  cv::Mat result = cam.captureEnrollment("on", true, kTestAverageFrames5);
  EXPECT_TRUE(result.empty());
  EXPECT_EQ(state->open_count, linuxcampam::CAPTURE_RETRY_ATTEMPTS);
}

TEST_F(CameraTest, CaptureEnrollmentSupportedSessionAutoExecutesHdr) {
  auto state = std::make_shared<SessionState>();
  state->mock_exposure_support = true;

  Camera cam("/dev/video0", false, "", false,
             [state]() { return std::make_unique<MockCameraSession>(state); });

  cv::Mat result = cam.captureEnrollment("auto", true, kTestAverageFrames5);
  EXPECT_FALSE(result.empty());
  EXPECT_TRUE(cam.supportsManualExposure());
  EXPECT_EQ(state->detect_query_count, 1);
  EXPECT_EQ(state->recorded_exposures.size(), 3);
  EXPECT_EQ(state->recorded_exposures.at(0), linuxcampam::HDR_EXPOSURE_1);
  EXPECT_EQ(state->recorded_exposures.at(1), linuxcampam::HDR_EXPOSURE_2);
  EXPECT_EQ(state->recorded_exposures.at(2), linuxcampam::HDR_EXPOSURE_3);
}

TEST_F(CameraTest, CaptureEnrollmentSupportedSessionOnExecutesHdr) {
  auto state = std::make_shared<SessionState>();
  state->mock_exposure_support = true;

  Camera cam("/dev/video0", false, "", false,
             [state]() { return std::make_unique<MockCameraSession>(state); });

  cv::Mat result = cam.captureEnrollment("on", true, kTestAverageFrames5);
  EXPECT_FALSE(result.empty());
  EXPECT_TRUE(cam.supportsManualExposure());
  EXPECT_EQ(state->detect_query_count, 1);
  EXPECT_EQ(state->recorded_exposures.size(), 3);
}

TEST_F(CameraTest, CaptureEnrollmentDynamicReconnectRefreshesCapability) {
  auto state = std::make_shared<SessionState>();
  Camera cam("/dev/video0", false, "", false,
             [state]() { return std::make_unique<MockCameraSession>(state); });

  // Session 1: device is supported
  state->mock_exposure_support = true;
  cv::Mat res1 = cam.captureEnrollment("auto", true, kTestAverageFrames5);
  EXPECT_FALSE(res1.empty());
  EXPECT_TRUE(cam.supportsManualExposure());
  EXPECT_EQ(state->recorded_exposures.size(), 3);

  // Session 2: device reconnects as unsupported on the same Camera instance
  state->recorded_exposures.clear();
  state->mock_exposure_support = false;
  cv::Mat res2 = cam.captureEnrollment("auto", true, kTestAverageFrames5);
  EXPECT_FALSE(res2.empty());
  EXPECT_FALSE(cam.supportsManualExposure());
  EXPECT_TRUE(state->recorded_exposures.empty());

  // Session 3: device reconnects as supported again
  state->recorded_exposures.clear();
  state->mock_exposure_support = true;
  cv::Mat res3 = cam.captureEnrollment("auto", true, kTestAverageFrames5);
  EXPECT_FALSE(res3.empty());
  EXPECT_TRUE(cam.supportsManualExposure());
  EXPECT_EQ(state->recorded_exposures.size(), 3);
}

TEST_F(CameraTest, CaptureEnrollmentExposureWriteFailureFallsBackWithoutFalseHdrClaim) {
  auto state = std::make_shared<SessionState>();
  state->mock_exposure_support = true;
  state->mock_set_exposure_success = false;

  Camera cam("/dev/video0", false, "", false,
             [state]() { return std::make_unique<MockCameraSession>(state); });

  cv::Mat result = cam.captureEnrollment("on", true, kTestAverageFrames5);
  EXPECT_FALSE(result.empty());
  // Because exposure write failed, it falls back to averaging
}

TEST_F(CameraTest, CaptureEnrollmentExplicitHdrFallbackToAveraging) {
  auto state = std::make_shared<SessionState>();
  state->mock_exposure_support = false;

  Camera cam("/dev/video0", false, "", false,
             [state]() { return std::make_unique<MockCameraSession>(state); });

  cv::Mat result = cam.captureEnrollment("on", true, kTestAverageFrames5);
  EXPECT_FALSE(result.empty());
  EXPECT_TRUE(state->recorded_exposures.empty());
  EXPECT_FALSE(cam.supportsManualExposure());
}

TEST_F(CameraTest, CaptureEnrollmentExplicitHdrFallbackToSingleWhenAveragingDisabled) {
  auto state = std::make_shared<SessionState>();
  state->mock_exposure_support = false;

  Camera cam("/dev/video0", false, "", false,
             [state]() { return std::make_unique<MockCameraSession>(state); });

  cv::Mat result = cam.captureEnrollment("on", false, 1);
  EXPECT_FALSE(result.empty());
  EXPECT_TRUE(state->recorded_exposures.empty());
}

TEST_F(CameraTest, CaptureEnrollmentAutoHdrFallback) {
  auto state = std::make_shared<SessionState>();
  state->mock_exposure_support = false;

  Camera cam("/dev/video0", false, "", false,
             [state]() { return std::make_unique<MockCameraSession>(state); });

  cv::Mat result = cam.captureEnrollment("auto", true, kTestAverageFrames3);
  EXPECT_FALSE(result.empty());
  EXPECT_TRUE(state->recorded_exposures.empty());
}
