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
class TestableCamera : public Camera {
public:
  using Camera::Camera;
  bool mock_open_success = true;
  bool mock_exposure_support = false;
  int open_count = 0;
  int single_capture_count = 0;
  int average_capture_count = 0;
  int requested_avg_frames = 0;

protected:
  bool openAndWarmup(cv::VideoCapture &temp_cap) override {
    (void)temp_cap;
    open_count++;
    if (!mock_open_success) {
      return false;
    }
    supports_manual_exposure_ = mock_exposure_support;
    return true;
  }

  cv::Mat captureSingleOnSession(cv::VideoCapture &temp_cap) override {
    (void)temp_cap;
    single_capture_count++;
    return cv::Mat(10, 10, CV_8UC1, cv::Scalar(100));
  }

  cv::Mat captureAverageOnSession(cv::VideoCapture &temp_cap, int num_frames) override {
    (void)temp_cap;
    average_capture_count++;
    requested_avg_frames = num_frames;
    return cv::Mat(10, 10, CV_8UC1, cv::Scalar(120));
  }
};
} // namespace

TEST_F(CameraTest, CaptureEnrollmentFailsWhenDeviceOpenFails) {
  TestableCamera cam("/dev/null", false, "");
  cam.mock_open_success = false;

  cv::Mat result = cam.captureEnrollment("on", true, 5);
  EXPECT_TRUE(result.empty());
  EXPECT_EQ(cam.open_count, 1);
  EXPECT_EQ(cam.single_capture_count, 0);
  EXPECT_EQ(cam.average_capture_count, 0);
}

TEST_F(CameraTest, CaptureEnrollmentExplicitHdrFallbackToAveraging) {
  TestableCamera cam("/dev/null", false, "");
  cam.mock_open_success = true;
  cam.mock_exposure_support = false;

  cv::Mat result = cam.captureEnrollment("on", true, 5);
  EXPECT_FALSE(result.empty());
  EXPECT_EQ(cam.average_capture_count, 1);
  EXPECT_EQ(cam.requested_avg_frames, 5);
  EXPECT_EQ(cam.single_capture_count, 0);
  EXPECT_FALSE(cam.supportsManualExposure());
}

TEST_F(CameraTest, CaptureEnrollmentExplicitHdrFallbackToSingleWhenAveragingDisabled) {
  TestableCamera cam("/dev/null", false, "");
  cam.mock_open_success = true;
  cam.mock_exposure_support = false;

  cv::Mat result = cam.captureEnrollment("on", false, 1);
  EXPECT_FALSE(result.empty());
  EXPECT_EQ(cam.single_capture_count, 1);
  EXPECT_EQ(cam.average_capture_count, 0);
}

TEST_F(CameraTest, CaptureEnrollmentAutoHdrFallback) {
  TestableCamera cam("/dev/null", false, "");
  cam.mock_open_success = true;
  cam.mock_exposure_support = false;

  cv::Mat result = cam.captureEnrollment("auto", true, 3);
  EXPECT_FALSE(result.empty());
  EXPECT_EQ(cam.average_capture_count, 1);
  EXPECT_EQ(cam.requested_avg_frames, 3);
  EXPECT_EQ(cam.single_capture_count, 0);
}

TEST_F(CameraTest, CaptureEnrollmentDynamicReconnectRefreshesCapability) {
  TestableCamera cam("/dev/null", false, "");
  cam.mock_open_success = true;

  // Session 1: device is supported
  cam.mock_exposure_support = true;
  cv::Mat res1 = cam.captureEnrollment("off", true, 5);
  EXPECT_FALSE(res1.empty());
  EXPECT_TRUE(cam.supportsManualExposure());

  // Session 2: device reconnects as unsupported on the same Camera instance
  cam.mock_exposure_support = false;
  cv::Mat res2 = cam.captureEnrollment("on", true, 5);
  EXPECT_FALSE(res2.empty());
  EXPECT_FALSE(cam.supportsManualExposure());
  EXPECT_EQ(cam.average_capture_count, 2);

  // Session 3: device reconnects as supported again
  cam.mock_exposure_support = true;
  cv::Mat res3 = cam.captureEnrollment("off", false, 1);
  EXPECT_FALSE(res3.empty());
  EXPECT_TRUE(cam.supportsManualExposure());
  EXPECT_EQ(cam.single_capture_count, 1);
}
