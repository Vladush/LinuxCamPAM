#include "../src/service/config.hpp"
#include "../src/service/utils.hpp"

#include <gtest/gtest.h>
#include <linux/videodev2.h>
#include <map>
#include <sstream>
#include <vector>

using namespace linuxcampam;

// Re-use mock backend logic
class MockConfigCameraBackend : public ICameraBackend {
public:
  std::vector<std::string> paths;
  std::map<std::string, bool> is_video;
  std::map<std::string, std::vector<uint32_t>> formats;

  std::vector<std::string> getDevicePaths() const override { return paths; }
  bool isVideoCaptureDevice(std::string_view path) const override {
    auto it = is_video.find(std::string(path));
    if (it != is_video.end())
      return it->second;
    return false;
  }
  std::vector<uint32_t> getPixelFormats(std::string_view path) const override {
    auto it = formats.find(std::string(path));
    if (it != formats.end())
      return {it->second.begin(), it->second.end()};
    return {};
  }
};

class ConfigDetectionTest : public ::testing::Test {
  // No file creating/tearing down needed anymore!
};

TEST_F(ConfigDetectionTest, DetectsIRAndRGB) {
  MockConfigCameraBackend mock;
  mock.paths = {"/dev/video0", "/dev/video1"};

  mock.is_video["/dev/video0"] = true;
  mock.formats["/dev/video0"] = {V4L2_PIX_FMT_GREY}; // IR

  mock.is_video["/dev/video1"] = true;
  mock.formats["/dev/video1"] = {V4L2_PIX_FMT_RGB24}; // RGB

  Configuration config;
  std::stringstream ss; // Empty config, triggers auto-detect
  EXPECT_TRUE(config.load(ss, &mock));

  ASSERT_EQ(config.camera_defs.size(), 2);

  EXPECT_EQ(config.camera_defs[0].id, "ir");
  EXPECT_EQ(config.camera_defs[0].path, "/dev/video0");

  EXPECT_EQ(config.camera_defs[1].id, "rgb");
  EXPECT_EQ(config.camera_defs[1].path, "/dev/video1");
}

TEST_F(ConfigDetectionTest, DetectsOnlyRGB) {
  MockConfigCameraBackend mock;
  mock.paths = {"/dev/video1"};
  mock.is_video["/dev/video1"] = true;
  mock.formats["/dev/video1"] = {V4L2_PIX_FMT_MJPEG}; // Color

  Configuration config;
  std::stringstream ss;
  EXPECT_TRUE(config.load(ss, &mock));

  ASSERT_EQ(config.camera_defs.size(), 1);
  EXPECT_EQ(config.camera_defs[0].id, "rgb");
  EXPECT_EQ(config.camera_defs[0].path, "/dev/video1");
}

TEST_F(ConfigDetectionTest, NoCamerasDetected) {
  MockConfigCameraBackend mock;
  // Empty backend

  Configuration config;
  std::stringstream ss;
  EXPECT_TRUE(config.load(ss, &mock));

  EXPECT_TRUE(config.camera_defs.empty());
}

TEST_F(ConfigDetectionTest, IgnoreAlreadyConfiguredCameras) {
  // If cameras are defined in INI, auto-detection should NOT run
  std::stringstream ss;
  ss << "[Cameras]\nnames=custom_cam\n\n[Camera.custom_cam]\npath=/dev/"
        "video99\n";

  MockConfigCameraBackend mock;
  mock.paths = {"/dev/video0"}; // Should be ignored
  mock.is_video["/dev/video0"] = true;
  mock.formats["/dev/video0"] = {V4L2_PIX_FMT_GREY};

  Configuration config;
  EXPECT_TRUE(config.load(ss, &mock));

  ASSERT_EQ(config.camera_defs.size(), 1);
  EXPECT_EQ(config.camera_defs[0].id, "custom_cam");
  EXPECT_EQ(config.camera_defs[0].path, "/dev/video99");
}
