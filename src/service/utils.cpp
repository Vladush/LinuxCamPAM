#include "utils.hpp"

#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace linuxcampam {

std::string classifyCameraType(const std::string &device_path) {
  FileDescriptor fd(open(device_path.c_str(), O_RDONLY));
  if (!fd.isValid())
    return "";

  struct v4l2_capability cap = {};
  if (ioctl(fd.get(), VIDIOC_QUERYCAP, &cap) < 0 ||
      !(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)) {
    return "";
  }

  bool has_grey = false, has_color = false;
  struct v4l2_fmtdesc fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  while (ioctl(fd.get(), VIDIOC_ENUM_FMT, &fmt) == 0) {
    if (fmt.pixelformat == V4L2_PIX_FMT_GREY ||
        fmt.pixelformat == V4L2_PIX_FMT_Y10 ||
        fmt.pixelformat == V4L2_PIX_FMT_Y12 ||
        fmt.pixelformat == V4L2_PIX_FMT_Y16)
      has_grey = true;
    if (fmt.pixelformat == V4L2_PIX_FMT_MJPEG ||
        fmt.pixelformat == V4L2_PIX_FMT_YUYV ||
        fmt.pixelformat == V4L2_PIX_FMT_RGB24 ||
        fmt.pixelformat == V4L2_PIX_FMT_BGR24)
      has_color = true;
    fmt.index++;
  }

  if (has_grey && !has_color)
    return "ir";
  if (has_color)
    return "rgb";
  if (has_grey)
    return "ir";
  return "generic";
}

std::vector<std::pair<std::string, std::string>> enumerateCameras() {
  std::vector<std::pair<std::string, std::string>> cameras;
  if (!fs::exists("/dev"))
    return cameras;

  for (const auto &entry : fs::directory_iterator("/dev")) {
    std::string name = entry.path().filename().string();
    if (name.rfind("video", 0) == 0) {
      std::string type = classifyCameraType(entry.path().string());
      if (!type.empty())
        cameras.push_back({entry.path().string(), type});
    }
  }
  std::sort(cameras.begin(), cameras.end());
  return cameras;
}

} // namespace linuxcampam
