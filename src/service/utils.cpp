#include "utils.hpp"

#include "constants.hpp" // For MAX_USERNAME_LENGTH
#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <linux/videodev2.h>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace linuxcampam {

// --- RealCameraBackend Implementation ---

std::vector<std::string> RealCameraBackend::getDevicePaths() const {
  std::vector<std::string> paths;
  if (!fs::exists("/dev"))
    return paths;

  for (const auto &entry : fs::directory_iterator("/dev")) {
    std::string name = entry.path().filename().string();
    if (name.rfind("video", 0) == 0) {
      paths.push_back(entry.path().string());
    }
  }
  return paths;
}

bool RealCameraBackend::isVideoCaptureDevice(const std::string &path) const {
  FileDescriptor fd(open(path.c_str(), O_RDONLY));
  if (!fd.isValid())
    return false;

  struct v4l2_capability cap = {};
  if (ioctl(fd.get(), VIDIOC_QUERYCAP, &cap) < 0)
    return false;
  return (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE);
}

std::vector<uint32_t>
RealCameraBackend::getPixelFormats(const std::string &path) const {
  std::vector<uint32_t> formats;
  FileDescriptor fd(open(path.c_str(), O_RDONLY));
  if (!fd.isValid())
    return formats;

  struct v4l2_fmtdesc fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  while (ioctl(fd.get(), VIDIOC_ENUM_FMT, &fmt) == 0) {
    formats.push_back(fmt.pixelformat);
    fmt.index++;
  }
  return formats;
}

// --- Core Logic with Dependency Injection ---

std::string classifyCameraType(const std::string &device_path,
                               const ICameraBackend &backend) {
  if (!backend.isVideoCaptureDevice(device_path)) {
    return "";
  }

  bool has_grey = false;
  bool has_color = false;
  auto formats = backend.getPixelFormats(device_path);

  for (auto fmt : formats) {
    if (fmt == V4L2_PIX_FMT_GREY || fmt == V4L2_PIX_FMT_Y10 ||
        fmt == V4L2_PIX_FMT_Y12 || fmt == V4L2_PIX_FMT_Y16)
      has_grey = true;
    if (fmt == V4L2_PIX_FMT_MJPEG || fmt == V4L2_PIX_FMT_YUYV ||
        fmt == V4L2_PIX_FMT_RGB24 || fmt == V4L2_PIX_FMT_BGR24)
      has_color = true;
  }

  if (has_grey && !has_color)
    return "ir";
  if (has_color)
    return "rgb";
  if (has_grey)
    return "ir";
  return "generic";
}

std::vector<std::pair<std::string, std::string>>
enumerateCameras(const ICameraBackend &backend) {
  std::vector<std::pair<std::string, std::string>> cameras;
  auto paths = backend.getDevicePaths();

  for (const auto &path : paths) {
    std::string type = classifyCameraType(path, backend);
    if (!type.empty()) {
      cameras.emplace_back(path, type);
    }
  }
  std::sort(cameras.begin(), cameras.end());
  return cameras;
}

// --- Default Overloads ---

std::string classifyCameraType(const std::string &device_path) {
  RealCameraBackend backend;
  return classifyCameraType(device_path, backend);
}

std::vector<std::pair<std::string, std::string>> enumerateCameras() {
  RealCameraBackend backend;
  return enumerateCameras(backend);
}

bool isValidUsername(std::string_view username) {
  if (username.empty() || username.length() > linuxcampam::MAX_USERNAME_LENGTH)
    return false;

  // Hidden files
  if (username.front() == '.')
    return false;

  // Detect Path Traversal ("..")
  // Using explicit loop or adjacent_find
  for (size_t i = 1; i < username.length(); ++i) {
    if (username[i] == '.' && username[i - 1] == '.')
      return false;
  }

  // Strict allowlist (a-z, A-Z, 0-9, _, ., -, $)
  return std::all_of(username.begin(), username.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' ||
           c == '$';
  });
}

} // namespace linuxcampam
