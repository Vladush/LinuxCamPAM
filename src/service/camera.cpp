#include "camera.hpp"

#include "constants.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/photo.hpp>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

void Camera::triggerIrEmitter() {
  std::cerr << "[Camera] Triggering IR emitter..." << std::endl;
  std::string cmd = ir_emitter_path_ + " run 2>&1";
  int ret = std::system(cmd.c_str());
  std::cerr << "[Camera] IR emitter returned: " << ret << std::endl;
}

bool Camera::detectExposureSupport() {
  int fd = open(device_path.c_str(), O_RDWR);
  if (fd < 0)
    return false;

  struct v4l2_queryctrl queryctrl = {};
  queryctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE;
  bool supported = (ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl) == 0) &&
                   !(queryctrl.flags & V4L2_CTRL_FLAG_DISABLED);
  close(fd);
  return supported;
}

Camera::Camera(const std::string &device_path, bool is_ir,
               const std::string &ir_cmd_path)
    : device_path(device_path), is_ir_camera(is_ir) {
  if (ir_cmd_path.empty()) {
    ir_emitter_path_ = linuxcampam::IR_EMITTER_PATH;
  } else {
    ir_emitter_path_ = ir_cmd_path;
  }
  cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);

  if (device_path.rfind("/dev/video", 0) == 0) {
    try {
      device_id = std::stoi(device_path.substr(10));
    } catch (...) {
      device_id = 0;
    }
  }

  // Detect exposure control support
  supports_manual_exposure_ = detectExposureSupport();
  if (supports_manual_exposure_) {
    std::cerr << "[Camera] " << device_path << " supports manual exposure"
              << std::endl;
  }
}

Camera::~Camera() {
  if (cap.isOpened()) {
    cap.release();
  }
}

bool Camera::openAndWarmup(cv::VideoCapture &temp_cap) {
  // Open camera FIRST, then trigger IR emitter
  // IR state resets when camera device is released, so we must keep it open
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (temp_cap.open(device_id, cv::CAP_V4L2))
      break;
    std::cerr << "[Camera] Device busy. Retrying (" << attempt + 1 << "/3)..."
              << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  if (!temp_cap.isOpened())
    return false;

  // Now trigger IR emitter while camera is open
  if (is_ir_camera) {
    triggerIrEmitter();
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
  }

  return true;
}

cv::Mat Camera::capture() {
  cv::VideoCapture temp_cap;
  if (!openAndWarmup(temp_cap)) {
    std::cerr << "[Camera] Failed to open " << device_path << std::endl;
    return cv::Mat();
  }

  cv::Mat frame;
  // Discard initial frames for auto-exposure settling
  for (int i = 0; i < 10; i++)
    temp_cap.read(frame); // Read and discard

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  temp_cap.read(frame);

  return frame.empty() ? cv::Mat() : frame.clone();
}

cv::Mat Camera::captureAveraged(int num_frames) {
  cv::VideoCapture temp_cap;
  if (!openAndWarmup(temp_cap)) {
    std::cerr << "[Camera] Failed to open for averaging" << std::endl;
    return cv::Mat();
  }

  // Warmup
  cv::Mat frame;
  for (int i = 0; i < 10; i++)
    temp_cap.read(frame);

  // Collect frames
  std::vector<cv::Mat> frames;
  cv::Size expected_size;
  for (int i = 0; i < num_frames; i++) {
    cv::Mat f;
    temp_cap.read(f);
    if (!f.empty()) {
      if (frames.empty()) {
        expected_size = f.size();
      }
      if (f.size() == expected_size) {
        cv::Mat f32;
        f.convertTo(f32, CV_32FC3);
        frames.push_back(f32);
      }
    }
  }

  if (frames.empty())
    return cv::Mat();

  // Average
  cv::Mat sum = cv::Mat::zeros(frames[0].size(), CV_32FC3);
  for (const auto &f : frames)
    sum += f;
  sum /= static_cast<float>(frames.size());

  cv::Mat result;
  sum.convertTo(result, CV_8UC3);
  std::cerr << "[Camera] Averaged " << frames.size() << " frames" << std::endl;
  return result;
}

cv::Mat Camera::captureHDR() {
  if (!supports_manual_exposure_) {
    std::cerr << "[Camera] HDR not supported, falling back to averaging"
              << std::endl;
    return captureAveraged(5);
  }

  cv::VideoCapture temp_cap;
  if (!openAndWarmup(temp_cap)) {
    std::cerr << "[Camera] Failed to open for HDR" << std::endl;
    return cv::Mat();
  }

  // Warmup
  cv::Mat frame;
  for (int i = 0; i < 10; i++)
    temp_cap.read(frame);

  // Save original auto-exposure mode
  double original_auto_exp = temp_cap.get(cv::CAP_PROP_AUTO_EXPOSURE);

  // Disable auto-exposure (1 = manual)
  // Note: 0.25 is sometimes used in older OpenCV/V4L2 bridges,
  // but 1.0 is standard for Manual in recent versions.
  temp_cap.set(cv::CAP_PROP_AUTO_EXPOSURE, 1);

  // Capture at different exposures
  std::vector<cv::Mat> exposures;
  std::vector<float> times = {0.01f, 0.05f, 0.15f}; // Relative times
  int exp_values[] = {50, 150, 400};                // Exposure values

  for (int i = 0; i < 3; i++) {
    temp_cap.set(cv::CAP_PROP_EXPOSURE, exp_values[i]);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (int j = 0; j < 3; j++)
      temp_cap.read(frame); // Let it settle
    if (!frame.empty())
      exposures.push_back(frame.clone());
  }

  // Restore original auto-exposure mode
  temp_cap.set(cv::CAP_PROP_AUTO_EXPOSURE, original_auto_exp);

  if (exposures.size() < 2) {
    std::cerr << "[Camera] HDR failed, using last frame" << std::endl;
    return frame.empty() ? cv::Mat() : frame.clone();
  }

  // Merge using Mertens (exposure fusion, no calibration needed)
  cv::Ptr<cv::MergeMertens> merge = cv::createMergeMertens();
  cv::Mat hdr;
  merge->process(exposures, hdr);

  // Convert to 8-bit
  cv::Mat result;
  hdr.convertTo(result, CV_8U, 255);
  std::cerr << "[Camera] HDR merged " << exposures.size() << " exposures"
            << std::endl;
  return result;
}
