#include "../src/service/utils.hpp"

#include <gtest/gtest.h>
#include <linux/videodev2.h>
#include <map>

using namespace linuxcampam;

class MockCameraBackend : public ICameraBackend {
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
  std::vector<uint32_t>
  getPixelFormats(std::string_view path) const override {
    auto it = formats.find(std::string(path));
    if (it != formats.end())
      return it->second;
    return {};
  }
};

TEST(CameraDetectionTest, NoDevices) {
  MockCameraBackend mock; // paths empty by default
  auto cameras = enumerateCameras(mock);
  EXPECT_TRUE(cameras.empty());
}

TEST(CameraDetectionTest, IgnoreNonVideoDevices) {
  MockCameraBackend mock;
  mock.paths = {"/dev/video0"};
  mock.is_video["/dev/video0"] = false;

  auto cameras = enumerateCameras(mock);
  EXPECT_TRUE(cameras.empty());
}

TEST(CameraDetectionTest, DetectIRCamera) {
  MockCameraBackend mock;
  mock.paths = {"/dev/video0"};
  mock.is_video["/dev/video0"] = true;
  mock.formats["/dev/video0"] = {V4L2_PIX_FMT_GREY};

  auto cameras = enumerateCameras(mock);
  ASSERT_EQ(cameras.size(), 1);
  EXPECT_EQ(cameras[0].first, "/dev/video0");
  EXPECT_EQ(cameras[0].second, "ir");
}

TEST(CameraDetectionTest, DetectRGBCamera) {
  MockCameraBackend mock;
  mock.paths = {"/dev/video1"};
  mock.is_video["/dev/video1"] = true;
  mock.formats["/dev/video1"] = {V4L2_PIX_FMT_RGB24};

  auto cameras = enumerateCameras(mock);
  ASSERT_EQ(cameras.size(), 1);
  EXPECT_EQ(cameras[0].first, "/dev/video1");
  EXPECT_EQ(cameras[0].second, "rgb");
}

TEST(CameraDetectionTest, DetectGenericCamera) {
  MockCameraBackend mock;
  mock.paths = {"/dev/video2"};
  mock.is_video["/dev/video2"] = true;
  mock.formats["/dev/video2"] = {0}; // Unknown format

  auto cameras = enumerateCameras(mock);
  ASSERT_EQ(cameras.size(), 1);
  EXPECT_EQ(cameras[0].first, "/dev/video2");
  EXPECT_EQ(cameras[0].second, "generic");
}

TEST(CameraDetectionTest, DetectCombinedCameraAsRGB) {
  // A camera that supports both Grey and RGB (e.g. webcams often do YUYV and
  // maybe generic formats, but if it has color it counts as RGB)
  MockCameraBackend mock;
  mock.paths = {"/dev/video3"};
  mock.is_video["/dev/video3"] = true;
  mock.formats["/dev/video3"] = {V4L2_PIX_FMT_GREY, V4L2_PIX_FMT_RGB24};

  auto type = classifyCameraType("/dev/video3", mock);
  EXPECT_EQ(type, "rgb");
}

TEST(CameraDetectionTest, DetectMixedDevices) {
  MockCameraBackend mock;
  mock.paths = {"/dev/video0", "/dev/video1"};

  mock.is_video["/dev/video0"] = true;
  mock.formats["/dev/video0"] = {V4L2_PIX_FMT_GREY};

  mock.is_video["/dev/video1"] = true;
  mock.formats["/dev/video1"] = {V4L2_PIX_FMT_MJPEG};

  auto cameras = enumerateCameras(mock);
  ASSERT_EQ(cameras.size(), 2);
  // enumerateCameras sorts by path
  EXPECT_EQ(cameras[0].first, "/dev/video0");
  EXPECT_EQ(cameras[0].second, "ir");
  EXPECT_EQ(cameras[1].first, "/dev/video1");
  EXPECT_EQ(cameras[1].second, "rgb");
}
