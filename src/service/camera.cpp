#include "camera.hpp"
#include "constants.hpp"
#include "logger.hpp"
#include "utils.hpp"
#include <array>
#include <fcntl.h>
#include <filesystem>
#include <linux/videodev2.h>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/photo.hpp>
#include <spawn.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// Required for posix_spawn environment inheritance
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern char **environ;

namespace {
using linuxcampam::FileDescriptor;
}

void Camera::triggerIrEmitter() {
  log_info("[Camera] Triggering IR emitter");

  pid_t pid = 0;
  std::string cmd = "run";
  std::vector<char *> args;

  // Prepare argv for posix_spawn (requires mutable char* array)
  // We copy strings into mutable buffers, then create pointers to them.
  std::string path_str = ir_emitter_path_.string();
  std::vector<char> path_buf(path_str.begin(), path_str.end());
  path_buf.push_back('\0');

  std::vector<char> cmd_buf(cmd.begin(), cmd.end());
  cmd_buf.push_back('\0');

  args.push_back(path_buf.data());
  args.push_back(cmd_buf.data());
  args.push_back(nullptr);

  int status = posix_spawn(&pid, ir_emitter_path_.c_str(), nullptr, nullptr,
                           args.data(), environ);

  if (status == 0) {
    // Block until process completes
    if (waitpid(pid, &status, 0) != -1) {
      if (WIFEXITED(status)) {
        log_info("[Camera] IR emitter exited with code: " +
                 std::to_string(WEXITSTATUS(status)));
      } else {
        log_error("[Camera] IR emitter terminated abnormally.");
      }
    } else {
      log_error("[Camera] Failed to wait for IR emitter.");
    }
  } else {
    std::ostringstream oss;
    oss << "[Camera] posix_spawn failed for " << ir_emitter_path_ << ": "
        << std::system_category().message(status) << " (" << status << ")";
    log_error(oss.str());
  }
}

bool Camera::detectExposureSupport() {
  FileDescriptor fd_wrapper(open(device_path.c_str(), O_RDWR));
  if (!fd_wrapper.isValid())
    return false;

  struct v4l2_queryctrl queryctrl = {};
  queryctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE;
  bool supported =
      (ioctl(fd_wrapper.get(), VIDIOC_QUERYCTRL, &queryctrl) == 0) &&
      !(queryctrl.flags & V4L2_CTRL_FLAG_DISABLED);
  return supported;
}

Camera::Camera(std::string_view device_path, bool is_ir,
               const fs::path &ir_cmd_path)
    : device_path(device_path), is_ir_camera(is_ir) {
  if (ir_cmd_path.empty()) {
    ir_emitter_path_ = linuxcampam::IR_EMITTER_PATH;
  } else {
    ir_emitter_path_ = ir_cmd_path;
  }
  cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);

  if (device_path.rfind("/dev/video", 0) == 0) {
    try {
      device_id = std::stoi(std::string(device_path.substr(std::string("/dev/video").length())));
    } catch (...) {
      device_id = 0;
    }
  }

  // Detect exposure control support
  supports_manual_exposure_ = detectExposureSupport();
  if (supports_manual_exposure_) {
    log_info("[Camera] " + std::string(device_path) + " supports manual exposure");
  }
}

Camera::~Camera() {}

bool Camera::openAndWarmup(cv::VideoCapture &temp_cap) {
  // Open camera FIRST, then trigger IR emitter
  // IR state resets when camera device is released, so we must keep it open
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (temp_cap.open(device_id, cv::CAP_V4L2))
      break;
    log_warn("[Camera] Device busy. Retrying (" + std::to_string(attempt + 1) +
             "/3)...");
    std::this_thread::sleep_for(
        std::chrono::seconds(linuxcampam::CAPTURE_RETRY_DELAY_S));
  }

  if (!temp_cap.isOpened())
    return false;

  // Now trigger IR emitter while camera is open
  if (is_ir_camera) {
    triggerIrEmitter();
    std::this_thread::sleep_for(
        std::chrono::milliseconds(linuxcampam::IR_TRIGGER_DELAY_MS));
  }

  return true;
}

cv::Mat Camera::capture() {
  cv::VideoCapture temp_cap;
  if (!openAndWarmup(temp_cap)) {
    log_error("[Camera] Failed to open " + device_path);
    return cv::Mat();
  }

  cv::Mat frame;
  // Discard initial frames for auto-exposure settling
  for (int i = 0; i < linuxcampam::CAMERA_WARMUP_FRAMES; i++)
    temp_cap.read(frame); // Read and discard

  std::this_thread::sleep_for(
      std::chrono::milliseconds(linuxcampam::CAMERA_WARMUP_DELAY_MS));
  temp_cap.read(frame);

  return frame.empty() ? cv::Mat() : frame.clone();
}

cv::Mat Camera::captureAveraged(int num_frames) {
  cv::VideoCapture temp_cap;
  if (!openAndWarmup(temp_cap)) {
    log_error("[Camera] Failed to open for averaging");
    return cv::Mat();
  }

  // Warmup
  cv::Mat frame;
  for (int i = 0; i < linuxcampam::CAMERA_WARMUP_FRAMES; i++)
    temp_cap.read(frame);

  // Accumulate frames in-place
  cv::Mat sum;
  int count = 0;
  cv::Size expected_size;

  for (int i = 0; i < num_frames; i++) {
    temp_cap.read(frame);
    if (frame.empty())
      continue;

    if (count == 0) {
      expected_size = frame.size();
      frame.convertTo(sum, CV_32FC3);
      count++;
    } else if (frame.size() == expected_size) {
      cv::Mat f32;
      frame.convertTo(f32, CV_32FC3);
      cv::add(sum, f32, sum);
      count++;
    }
  }

  if (count == 0 || sum.empty())
    return cv::Mat();

  // Average
  sum /= static_cast<float>(count);

  cv::Mat result;
  sum.convertTo(result, CV_8UC3);
  log_info("[Camera] Averaged " + std::to_string(count) + " frames");
  return result;
}

cv::Mat Camera::captureHDR() {
  if (!supports_manual_exposure_) {
    log_warn("[Camera] HDR not supported, falling back to averaging");
    return captureAveraged(linuxcampam::CAMERA_AVERAGE_FRAMES);
  }

  cv::VideoCapture temp_cap;
  if (!openAndWarmup(temp_cap)) {
    log_error("[Camera] Failed to open for HDR");
    return cv::Mat();
  }

  // Warmup
  cv::Mat frame;
  for (int i = 0; i < linuxcampam::CAMERA_WARMUP_FRAMES; i++)
    temp_cap.read(frame);

  // Save original auto-exposure mode
  double original_auto_exp = temp_cap.get(cv::CAP_PROP_AUTO_EXPOSURE);

  // Disable auto-exposure (1 = manual)
  // Note: 0.25 is sometimes used in older OpenCV/V4L2 bridges,
  // but 1.0 is standard for Manual in recent versions.
  temp_cap.set(cv::CAP_PROP_AUTO_EXPOSURE, 1);

  // Capture at different exposures
  std::vector<cv::Mat> exposures;

  std::array<int, 3> exp_values = {linuxcampam::HDR_EXPOSURE_1,
                                   linuxcampam::HDR_EXPOSURE_2,
                                   linuxcampam::HDR_EXPOSURE_3};

  for (int exp : exp_values) {
    temp_cap.set(cv::CAP_PROP_EXPOSURE, exp);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(linuxcampam::HDR_SETTLE_MS));
    for (int j = 0; j < 3; j++)
      temp_cap.read(frame); // Let it settle
    if (!frame.empty())
      exposures.push_back(frame.clone());
  }

  // Restore original auto-exposure mode
  temp_cap.set(cv::CAP_PROP_AUTO_EXPOSURE, original_auto_exp);

  if (exposures.size() < 2) {
    log_error("[Camera] HDR failed, using last frame");
    return frame.empty() ? cv::Mat() : frame.clone();
  }

  // Merge using Mertens (exposure fusion, no calibration needed)
  cv::Ptr<cv::MergeMertens> merge = cv::createMergeMertens();
  cv::Mat hdr;
  merge->process(exposures, hdr);

  // Convert to 8-bit
  cv::Mat result;
  hdr.convertTo(result, CV_8U, linuxcampam::HDR_BIT_DEPTH);
  log_info("[Camera] HDR merged " + std::to_string(exposures.size()) +
           " exposures");
  return result;
}
