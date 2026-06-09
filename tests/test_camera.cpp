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
